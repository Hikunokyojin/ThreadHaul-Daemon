// Linux alerts. A system service has no desktop to draw a message box on,
// so the "popup" channel becomes a high-priority journald entry (desktop
// journal notifiers and "journalctl -p warning" surface it). The webhook is
// an HTTP(S) POST over OpenSSL with certificate and host-name checks.

#include "Alerts.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <syslog.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <cstdlib>
#include <string>
#include <thread>

#include "Utf8.h"

namespace thd {

namespace {

struct Url {
    bool https = false;
    std::string host;
    std::string port;
    std::string path = "/";
};

bool ParseUrl(const std::string& url, Url& u) {
    std::string rest;
    if (url.rfind("https://", 0) == 0) {
        u.https = true;
        rest = url.substr(8);
    } else if (url.rfind("http://", 0) == 0) {
        rest = url.substr(7);
    } else {
        return false;
    }
    const size_t slash = rest.find('/');
    std::string authority = rest.substr(0, slash);
    if (slash != std::string::npos) u.path = rest.substr(slash);
    if (authority.empty() || authority.find('@') != std::string::npos) return false;
    if (authority.front() == '[') {  // [IPv6]:port
        const size_t close = authority.find(']');
        if (close == std::string::npos) return false;
        u.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return false;
            u.port = authority.substr(close + 2);
        }
    } else {
        const size_t colon = authority.rfind(':');
        u.host = authority.substr(0, colon);
        if (colon != std::string::npos) u.port = authority.substr(colon + 1);
    }
    if (u.port.empty()) u.port = u.https ? "443" : "80";
    for (char c : u.port) {
        if (c < '0' || c > '9') return false;
    }
    return !u.host.empty();
}

int Connect(const Url& u, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(u.host.c_str(), u.port.c_str(), &hints, &res) != 0 || !res) {
        error = "cannot resolve " + u.host;
        return -1;
    }
    int fd = -1;
    for (addrinfo* a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
        if (fd < 0) continue;
        timeval t{5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(t));
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) error = "could not connect to " + u.host + ":" + u.port;
    return fd;
}

// Plain or TLS byte stream over one connected socket.
class Stream {
public:
    explicit Stream(int fd) : fd_(fd) {}
    ~Stream() {
        if (ssl_) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
        }
        if (ctx_) SSL_CTX_free(ctx_);
        if (fd_ >= 0) close(fd_);
    }
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    bool StartTls(const std::string& host, std::string& error) {
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) return Fail("TLS setup failed", error);
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
        if (SSL_CTX_set_default_verify_paths(ctx_) != 1) return Fail("cannot load the system CA certificates", error);
        ssl_ = SSL_new(ctx_);
        if (!ssl_) return Fail("TLS setup failed", error);
        X509_VERIFY_PARAM* param = SSL_get0_param(ssl_);       // the certificate must name this host
        X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        unsigned char ip[16];
        const bool literal = inet_pton(AF_INET, host.c_str(), ip) == 1 || inet_pton(AF_INET6, host.c_str(), ip) == 1;
        const int set = literal ? X509_VERIFY_PARAM_set1_ip_asc(param, host.c_str())
                                : X509_VERIFY_PARAM_set1_host(param, host.c_str(), 0);
        if (set != 1) return Fail("bad host name", error);
        if (!literal) SSL_set_tlsext_host_name(ssl_, host.c_str());  // SNI is for names only
        SSL_set_fd(ssl_, fd_);
        if (SSL_connect(ssl_) != 1 || SSL_get_verify_result(ssl_) != X509_V_OK) {
            return Fail("TLS handshake or certificate check failed", error);
        }
        return true;
    }

    bool Write(const std::string& data) {
        size_t done = 0;
        while (done < data.size()) {
            const int n = ssl_ ? SSL_write(ssl_, data.data() + done, static_cast<int>(data.size() - done))
                               : static_cast<int>(send(fd_, data.data() + done, data.size() - done, MSG_NOSIGNAL));
            if (n <= 0) return false;
            done += static_cast<size_t>(n);
        }
        return true;
    }

    // Reads up to the end of the status line.
    std::string StatusLine() {
        std::string out;
        char c;
        while (out.size() < 512) {
            const int n = ssl_ ? SSL_read(ssl_, &c, 1) : static_cast<int>(recv(fd_, &c, 1, 0));
            if (n <= 0 || c == '\n') break;
            out += c;
        }
        return out;
    }

private:
    static bool Fail(const char* what, std::string& error) {
        error = what;
        ERR_clear_error();
        return false;
    }

    int fd_;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
};

}

void Alerter::Configure(bool popup, std::wstring webhookUrl) {
    popup_ = popup;
    webhookUrl_ = std::move(webhookUrl);
}

void Alerter::Fire(const std::wstring& title, const std::wstring& message, const std::string& jsonBody) {
    const bool popup = popup_;
    const std::wstring url = webhookUrl_;
    std::thread([popup, url, title, message, jsonBody] {
        if (popup) {
            syslog(LOG_ALERT, "%s: %s", utf8::FromWide(title).c_str(), utf8::FromWide(message).c_str());
        }
        if (!url.empty()) {
            std::wstring error;
            PostJson(url, jsonBody, error);
        }
    }).detach();
}

bool PostJson(const std::wstring& url, const std::string& body, std::wstring& error) {
    Url u;
    if (!ParseUrl(utf8::FromWide(url), u)) {
        error = L"invalid URL";
        return false;
    }
    std::string err;
    const int fd = Connect(u, err);
    if (fd < 0) {
        error = L"could not open connection: " + utf8::ToWide(err);
        return false;
    }
    Stream s(fd);
    if (u.https && !s.StartTls(u.host, err)) {
        error = L"could not open connection: " + utf8::ToWide(err);
        return false;
    }
    const std::string hostHeader = u.host.find(':') != std::string::npos ? "[" + u.host + "]" : u.host;
    const std::string request = "POST " + u.path + " HTTP/1.1\r\nHost: " + hostHeader +
                                "\r\nUser-Agent: ThreadHaulDaemon/1.0\r\nContent-Type: application/json\r\n"
                                "Content-Length: " + std::to_string(body.size()) +
                                "\r\nConnection: close\r\n\r\n" + body;
    if (!s.Write(request)) {
        error = L"request failed";
        return false;
    }
    const std::string status = s.StatusLine();  // "HTTP/1.1 204 No Content"
    const size_t sp = status.find(' ');
    const int code = sp == std::string::npos ? 0 : std::atoi(status.c_str() + sp + 1);
    if (code >= 200 && code < 300) return true;
    error = code ? L"HTTP status " + std::to_wstring(code) : L"request failed (no response)";
    return false;
}

}
