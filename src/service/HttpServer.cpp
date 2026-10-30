#include "HttpServer.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>

namespace daemon {

namespace {

constexpr size_t kMaxHeader = 16 * 1024;
constexpr size_t kMaxBody = 256 * 1024;

const char* StatusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
    }
    return "Error";
}

void SendAll(SOCKET s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n == SOCKET_ERROR) return;
        sent += static_cast<size_t>(n);
    }
}

}

std::string UrlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string HttpRequest::QueryParam(const std::string& name) const {
    size_t start = 0;
    while (start <= query.size()) {
        size_t amp = query.find('&', start);
        if (amp == std::string::npos) amp = query.size();
        const std::string pair = query.substr(start, amp - start);
        const size_t eq = pair.find('=');
        if (UrlDecode(pair.substr(0, eq)) == name) {
            return eq == std::string::npos ? "" : UrlDecode(pair.substr(eq + 1));
        }
        start = amp + 1;
    }
    return {};
}

bool HttpServer::Start(int port, std::string& error) {
    Stop();
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        error = "WSAStartup failed";
        return false;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        error = "socket() failed";
        WSACleanup();
        return false;
    }
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
               sizeof(exclusive));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);  // loopback only
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 16) != 0) {
        error = "cannot listen on 127.0.0.1:" + std::to_string(port) + " (error " +
                std::to_string(WSAGetLastError()) + "; is the port in use?)";
        closesocket(s);
        WSACleanup();
        return false;
    }
    listener_ = static_cast<unsigned long long>(s);
    port_ = port;
    stopping_ = false;
    thread_ = std::thread([this] { Loop(); });
    return true;
}

void HttpServer::Stop() {
    if (!thread_.joinable()) {
        return;
    }
    stopping_ = true;
    thread_.join();
    closesocket(static_cast<SOCKET>(listener_));
    listener_ = ~0ull;
    WSACleanup();
}

void HttpServer::Loop() {
    const SOCKET listener = static_cast<SOCKET>(listener_);
    while (!stopping_) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener, &set);
        timeval tv{0, 200 * 1000};
        if (select(0, &set, nullptr, nullptr, &tv) <= 0) {
            continue;
        }
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            continue;
        }
        DWORD timeout = 3000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                   sizeof(timeout));
        Serve(static_cast<unsigned long long>(client));
        closesocket(client);
    }
}

void HttpServer::Serve(unsigned long long rawSock) {
    const SOCKET s = static_cast<SOCKET>(rawSock);
    std::string data;
    char buf[8192];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        const int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) return;
        data.append(buf, static_cast<size_t>(n));
        headerEnd = data.find("\r\n\r\n");
        if (headerEnd == std::string::npos && data.size() > kMaxHeader) return;
    }

    HttpRequest req;
    HttpResponse res;
    const std::string head = data.substr(0, headerEnd);
    size_t lineEnd = head.find("\r\n");
    const std::string requestLine = head.substr(0, lineEnd);
    const size_t sp1 = requestLine.find(' ');
    const size_t sp2 = requestLine.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        res.status = 400;
        res.body = "{\"ok\":false,\"message\":\"bad request\"}";
    } else {
        req.method = requestLine.substr(0, sp1);
        const std::string target = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
        const size_t q = target.find('?');
        req.path = target.substr(0, q);
        if (q != std::string::npos) req.query = target.substr(q + 1);

        size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
        while (pos < head.size()) {
            size_t end = head.find("\r\n", pos);
            if (end == std::string::npos) end = head.size();
            const std::string line = head.substr(pos, end - pos);
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string name = line.substr(0, colon);
                std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                std::string value = line.substr(colon + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                req.headers[name] = value;
            }
            pos = end + 2;
        }

        size_t length = 0;
        const std::string cl = req.Header("content-length");
        if (!cl.empty()) length = static_cast<size_t>(std::strtoull(cl.c_str(), nullptr, 10));
        if (length > kMaxBody) {
            res.status = 413;
            res.body = "{\"ok\":false,\"message\":\"request too large\"}";
        } else {
            req.body = data.substr(headerEnd + 4);
            while (req.body.size() < length) {
                const int n = recv(s, buf, sizeof(buf), 0);
                if (n <= 0) return;
                req.body.append(buf, static_cast<size_t>(n));
            }
            req.body.resize(length);
            try {
                res = handler_(req);
            } catch (...) {
                res.status = 500;
                res.contentType = "application/json; charset=utf-8";
                res.body = "{\"ok\":false,\"message\":\"internal error\"}";
            }
        }
    }

    std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " + StatusText(res.status) +
                      "\r\nContent-Type: " + res.contentType +
                      "\r\nContent-Length: " + std::to_string(res.body.size()) +
                      "\r\nCache-Control: no-store"
                      "\r\nX-Content-Type-Options: nosniff"
                      "\r\nX-Frame-Options: DENY"
                      "\r\nReferrer-Policy: no-referrer"
                      "\r\nContent-Security-Policy: default-src 'self'; script-src 'self'; "
                      "style-src 'self'; img-src 'self' data:; connect-src 'self'; "
                      "frame-ancestors 'none'; base-uri 'none'; form-action 'none'"
                      "\r\nConnection: close\r\n\r\n";
    SendAll(s, out);
    SendAll(s, res.body);
}

}
