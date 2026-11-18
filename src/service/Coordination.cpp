#include "Coordination.h"

#include "NetCompat.h"
#include "ServiceCompat.h"
#include "Utf8.h"

#ifndef _WIN32
#include <netdb.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "SecureChannel.h"

#include <chrono>
#include <cstring>

namespace thd {

namespace {

std::string Narrow(const std::wstring& w) { return utf8::FromWide(w); }

}

std::string LocalHostName() {
#ifdef _WIN32
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = ARRAYSIZE(name);
    return GetComputerNameW(name, &size) ? Narrow(name) : "unknown";
#else
    char name[256] = {};
    if (gethostname(name, sizeof(name) - 1) != 0 || !name[0]) return "unknown";
    const char* dot = std::strchr(name, '.');  // the short name, as Windows reports
    return dot ? std::string(name, static_cast<size_t>(dot - name)) : std::string(name);
#endif
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
    if (!netcompat::Startup()) {
        log_("coordination_error", "WSAStartup failed");
        return;
    }
    int backoffMs = 1000;
    while (!stopping_) {
#ifdef _WIN32
        addrinfoW hints{};
        addrinfoW* result = nullptr;
#define TH_GETADDRINFO GetAddrInfoW(host_.c_str(), std::to_wstring(port_).c_str(), &hints, &result)
#define TH_FREEADDRINFO FreeAddrInfoW(result)
        using Addr = addrinfoW;
#else
        addrinfo hints{};
        addrinfo* result = nullptr;
        const std::string hostA = Narrow(host_), portA = std::to_string(port_);
#define TH_GETADDRINFO getaddrinfo(hostA.c_str(), portA.c_str(), &hints, &result)
#define TH_FREEADDRINFO freeaddrinfo(result)
        using Addr = addrinfo;
#endif
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        SOCKET sock = INVALID_SOCKET;
        if (TH_GETADDRINFO == 0) {
            for (Addr* a = result; a && sock == INVALID_SOCKET; a = a->ai_next) {
                sock = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
                if (sock != INVALID_SOCKET && connect(sock, a->ai_addr, static_cast<int>(a->ai_addrlen)) != 0) {
                    closesocket(sock);
                    sock = INVALID_SOCKET;
                }
            }
            TH_FREEADDRINFO;
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
    netcompat::Cleanup();
}

bool CoordinationClient::Session(unsigned long long rawSock) {
    const SOCKET sock = static_cast<SOCKET>(rawSock);
    const std::string host = LocalHostName();
    securechannel::Channel channel(sock);
    std::string handshakeError;
    if (!channel.ClientHandshake(token_, host, handshakeError)) {
        log_("coordination_rejected", handshakeError);
        return false;
    }
    log_("coordination_secured", "AES-256-GCM, mutually authenticated");
    auto SendAll = [&channel](SOCKET, const std::string& line) { return channel.Send(line); };

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

        std::string line;
        const int got = channel.Receive(line, 200);
        if (got < 0) return false;  // closed, or a frame failed authentication
        if (got == 0) continue;
        {
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
