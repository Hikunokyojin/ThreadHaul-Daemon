#include "Coordination.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <nlohmann/json.hpp>

#include <chrono>

namespace daemon {

namespace {

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

bool SendAll(SOCKET s, const std::string& line) {
    const std::string data = line + "\n";
    size_t sent = 0;
    while (sent < data.size()) {
        const int n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n == SOCKET_ERROR) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

}

std::string LocalHostName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = ARRAYSIZE(name);
    return GetComputerNameW(name, &size) ? Narrow(name) : "unknown";
}

CoordinationClient::CoordinationClient(StatusFn status, CommandFn command, LogFn log)
    : status_(std::move(status)), command_(std::move(command)), log_(std::move(log)) {}

void CoordinationClient::Start(std::wstring host, int port, std::string token,
                               int heartbeatSeconds) {
    Stop();
    host_ = std::move(host);
    port_ = port;
    token_ = std::move(token);
    heartbeatSeconds_ = heartbeatSeconds;
    stopping_ = false;
    thread_ = std::thread([this] { Loop(); });
}

void CoordinationClient::Stop() {
    stopping_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    connected_ = false;
    bias_ = 0.0;
}

void CoordinationClient::SendEvent(const std::string& eventJson) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (outbox_.size() < 1000) {
        outbox_.push_back(eventJson);
    }
}

void CoordinationClient::Loop() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log_("coordination_error", "WSAStartup failed");
        return;
    }
    int backoffMs = 1000;
    while (!stopping_) {
        addrinfoW hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfoW* result = nullptr;
        SOCKET sock = INVALID_SOCKET;
        if (GetAddrInfoW(host_.c_str(), std::to_wstring(port_).c_str(), &hints, &result) == 0) {
            for (addrinfoW* a = result; a && sock == INVALID_SOCKET; a = a->ai_next) {
                sock = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
                if (sock != INVALID_SOCKET && connect(sock, a->ai_addr, static_cast<int>(a->ai_addrlen)) != 0) {
                    closesocket(sock);
                    sock = INVALID_SOCKET;
                }
            }
            FreeAddrInfoW(result);
        }

        if (sock != INVALID_SOCKET) {
            backoffMs = 1000;
            connected_ = true;
            log_("coordination_connected", Narrow(host_) + ":" + std::to_string(port_));
            const bool clean = Session(sock);
            closesocket(sock);
            connected_ = false;
            bias_ = 0.0;
            if (!stopping_) {
                log_("coordination_disconnected", clean ? "controller closed the connection"
                                                        : "connection lost");
            }
        }

        // Back off before reconnecting, waking early on stop.
        for (int waited = 0; waited < backoffMs && !stopping_; waited += 100) {
            Sleep(100);
        }
        backoffMs = std::min(backoffMs * 2, 30000);
    }
    WSACleanup();
}

bool CoordinationClient::Session(unsigned long long rawSock) {
    const SOCKET sock = static_cast<SOCKET>(rawSock);
    const std::string host = LocalHostName();
    nlohmann::json hello = {{"type", "hello"}, {"host", host}, {"token", token_}, {"version", 1}};
    if (!SendAll(sock, hello.dump())) return false;

    using clock = std::chrono::steady_clock;
    auto nextHeartbeat = clock::now();
    std::string inbuf;

    while (!stopping_) {
        if (clock::now() >= nextHeartbeat) {
            nlohmann::json hb = nlohmann::json::parse(status_(), nullptr, false);
            if (hb.is_discarded()) hb = nlohmann::json::object();
            hb["type"] = "heartbeat";
            hb["host"] = host;
            if (!SendAll(sock, hb.dump())) return false;
            nextHeartbeat = clock::now() + std::chrono::seconds(heartbeatSeconds_);
        }

        std::deque<std::string> pending;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            pending.swap(outbox_);
        }
        for (const std::string& e : pending) {
            nlohmann::json msg = {{"type", "event"}, {"host", host}};
            msg["event"] = nlohmann::json::parse(e, nullptr, false);
            if (!SendAll(sock, msg.dump())) return false;
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock, &readSet);
        timeval tv{0, 200 * 1000};
        const int ready = select(0, &readSet, nullptr, nullptr, &tv);
        if (ready == SOCKET_ERROR) return false;
        if (ready == 0) continue;

        char buf[4096];
        const int n = recv(sock, buf, sizeof(buf), 0);
        if (n == 0) return true;
        if (n < 0) return false;
        inbuf.append(buf, static_cast<size_t>(n));
        if (inbuf.size() > 1024 * 1024) return false;

        size_t pos;
        while ((pos = inbuf.find('\n')) != std::string::npos) {
            const std::string line = inbuf.substr(0, pos);
            inbuf.erase(0, pos + 1);
            const nlohmann::json msg = nlohmann::json::parse(line, nullptr, false);
            if (msg.is_discarded() || !msg.is_object()) continue;
            const std::string type = msg.value("type", "");
            if (type == "bias") {
                bias_ = msg.value("offset_percent", 0.0);
                log_("coordination_bias", std::to_string(bias_.load()));
            } else if (type == "command") {
                const std::string cmd = msg.value("command", "");
                log_("coordination_command", cmd);
                nlohmann::json result = nlohmann::json::parse(command_(cmd), nullptr, false);
                if (result.is_discarded()) result = nlohmann::json::object();
                result["type"] = "command_result";
                result["id"] = msg.value("id", 0);
                result["host"] = host;
                if (!SendAll(sock, result.dump())) return false;
            } else if (type == "error") {
                log_("coordination_rejected", msg.value("message", ""));
                return true;
            }
        }
    }
    return true;
}

}
