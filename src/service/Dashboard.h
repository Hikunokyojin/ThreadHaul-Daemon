#pragma once

#include <memory>
#include <string>

#include "HttpServer.h"
#include "ProcessTable.h"

namespace daemon {

class Daemon;

// The local web dashboard: serves the static page from <exe dir>\dashboard
// and a JSON API backed by the Daemon.
//
//   GET  /                     index.html (also /app.js, /styles.css)
//   GET  /api/status           live state, thresholds, system CPU/memory
//   GET  /api/processes        every running process with CPU%, memory, role
//   GET  /api/log?offset=N     new JSON Lines entries since byte offset N
//   POST /api/command          {"command":"suspend","target":"1234"}
//   POST /api/role             {"name":"app.exe","role":"critical|batch|protected|none"}
//
// Security: loopback only; the Host header must be 127.0.0.1/localhost with
// this port (blocks DNS rebinding); a cross-site Origin is refused; and
// every /api/ call must carry the X-ThreadHaul-Token header. The token is
// random per run and is handed out only over the administrator-only
// control pipe ("ThreadHaulDaemon.exe dashboard").
class Dashboard {
public:
    explicit Dashboard(Daemon& daemon);
    ~Dashboard();

    bool Start(int port, std::string& error);
    void Stop();
    bool Running() const { return server_ != nullptr; }
    int Port() const { return port_; }
    std::string Url() const;  // includes the token in the fragment

private:
    HttpResponse Handle(const HttpRequest& req);
    HttpResponse ServeFile(const std::string& path);
    HttpResponse Processes();
    bool Authorized(const HttpRequest& req, HttpResponse& res) const;

    Daemon& daemon_;
    std::unique_ptr<HttpServer> server_;
    telemetry::ProcessTable table_;
    std::string token_;
    std::wstring root_;
    int port_ = 0;
};

}
