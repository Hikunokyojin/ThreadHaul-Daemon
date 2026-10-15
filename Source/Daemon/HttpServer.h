#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>

namespace daemon {

struct HttpRequest {
    std::string method;
    std::string path;    // without the query string
    std::string query;   // text after '?', undecoded
    std::map<std::string, std::string> headers;  // names lower-cased
    std::string body;

    std::string Header(const std::string& lowerName) const {
        const auto it = headers.find(lowerName);
        return it == headers.end() ? std::string() : it->second;
    }
    std::string QueryParam(const std::string& name) const;
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json; charset=utf-8";
    std::string body;
};

// Minimal HTTP/1.1 server for the local dashboard. Listens on 127.0.0.1 only
// (never on an external interface), handles one request per connection with
// "Connection: close", and caps request size. Every response carries
// no-store caching and a strict Content-Security-Policy.
class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    explicit HttpServer(Handler handler) : handler_(std::move(handler)) {}
    ~HttpServer() { Stop(); }

    // Returns false (with a reason) if the port cannot be bound.
    bool Start(int port, std::string& error);
    void Stop();
    int Port() const { return port_; }

private:
    void Loop();
    void Serve(unsigned long long sock);

    Handler handler_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    unsigned long long listener_ = ~0ull;
    int port_ = 0;
};

std::string UrlDecode(const std::string& s);

}
