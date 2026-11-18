#pragma once

#include <functional>
#include <memory>
#include <string>

#include "HttpServer.h"
#include "SessionGuard.h"

namespace fleet {

// The controller's web UI: a loopback-only page of every connected server,
// with the console's commands as buttons. Same protections as the agent
// dashboard: 127.0.0.1 only, Host/Origin checks, and a per-session key in
// the X-ThreadHaul-Token header that expires after 30 min idle or 12 h,
// can be revoked (sign out), and locks the API after repeated bad keys.
//
//   GET  /                  index.html (and /fleet.js, /fleet.css) from <exe dir>\fleet
//   GET  /api/v1/fleet      agents with their last heartbeat, plus cluster state
//   GET  /api/v1/results    recent command results from agents
//   POST /api/v1/command    {"host":"<name>|all","command":"suspend","target":"app.exe"}
//   POST /api/v1/logout
struct Callbacks {
    std::function<std::string()> fleetJson;
    std::function<std::string()> resultsJson;
    // host ("all" or a name) and a console command line such as "suspend app.exe";
    // returns {"ok":..., "message":..., "sent":[hosts]}.
    std::function<std::string(const std::string& host, const std::string& command)> send;
};

class FleetWeb {
public:
    explicit FleetWeb(Callbacks callbacks);
    ~FleetWeb();

    bool Start(int port, std::string& error);
    void Stop();
    int Port() const { return port_; }
    // Link with the current session key in the fragment (issues a new key if
    // the last one expired or was revoked).
    std::string Url();

private:
    thd::HttpResponse Handle(const thd::HttpRequest& req);
    thd::HttpResponse ServeFile(const std::string& path);
    bool HostAllowed(const thd::HttpRequest& req, thd::HttpResponse& res) const;

    Callbacks callbacks_;
    std::unique_ptr<thd::HttpServer> server_;
    thd::SessionGuard session_;
    std::wstring root_;
    int port_ = 0;
};

// Pure validation of a web command, for tests. Returns "" if acceptable,
// otherwise the reason. Verbs: throttle, suspend, resume, release, exempt
// (need a PID or exe name), status (no target).
std::string ValidateCommand(const std::string& verb, const std::string& target);

}
