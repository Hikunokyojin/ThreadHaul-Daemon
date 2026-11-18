#include "FleetWeb.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "SecureChannel.h"

namespace fleet {

namespace {

using nlohmann::json;

daemon::HttpResponse Json(int status, const json& j) {
    daemon::HttpResponse r;
    r.status = status;
    r.body = j.dump(-1, ' ', false, json::error_handler_t::replace);
    return r;
}

daemon::HttpResponse Error(int status, const std::string& message) {
    return Json(status, json{{"ok", false}, {"message", message}});
}

std::string NewKey() { return securechannel::ToHex(securechannel::RandomBytes(24)); }

}

std::string ValidateCommand(const std::string& verb, const std::string& target) {
    if (verb == "status") {
        return target.empty() ? "" : "status takes no target";
    }
    if (verb != "throttle" && verb != "suspend" && verb != "resume" && verb != "release" && verb != "exempt") {
        return "command must be throttle, suspend, resume, release, exempt or status";
    }
    if (target.empty() || target.size() > 260 || target.find_first_of(" \t\r\n\"") != std::string::npos ||
        target.find_first_of("\\/") != std::string::npos) {
        return "target must be a PID or an executable name";
    }
    return "";
}

FleetWeb::FleetWeb(Callbacks callbacks) : callbacks_(std::move(callbacks)), session_(NewKey) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    root_ = (std::filesystem::path(exe).parent_path() / L"fleet").wstring();
}

FleetWeb::~FleetWeb() { Stop(); }

bool FleetWeb::Start(int port, std::string& error) {
    Stop();
    auto server = std::make_unique<daemon::HttpServer>([this](const daemon::HttpRequest& r) { return Handle(r); });
    if (!server->Start(port, error)) return false;
    server_ = std::move(server);
    port_ = port;
    return true;
}

void FleetWeb::Stop() {
    if (server_) {
        server_->Stop();
        server_.reset();
    }
}

std::string FleetWeb::Url() {
    return "http://127.0.0.1:" + std::to_string(port_) + "/#token=" + session_.CurrentOrNewKey();
}

bool FleetWeb::HostAllowed(const daemon::HttpRequest& req, daemon::HttpResponse& res) const {
    const std::string port = std::to_string(port_);
    const std::string host = req.Header("host");
    if (host != "127.0.0.1:" + port && host != "localhost:" + port) {
        res = Error(403, "requests must use http://127.0.0.1:" + port);
        return false;
    }
    const std::string origin = req.Header("origin");
    if (!origin.empty() && origin != "http://127.0.0.1:" + port && origin != "http://localhost:" + port) {
        res = Error(403, "cross-origin requests are not allowed");
        return false;
    }
    return true;
}

daemon::HttpResponse FleetWeb::Handle(const daemon::HttpRequest& req) {
    daemon::HttpResponse denied;
    if (!HostAllowed(req, denied)) return denied;
    if (req.path.rfind("/api/", 0) != 0) {
        if (req.method != "GET") return Error(405, "method not allowed");
        return ServeFile(req.path == "/" ? "/index.html" : req.path);
    }
    if (req.path.rfind("/api/v1/", 0) != 0) return Error(404, "this controller implements /api/v1");
    const std::string route = req.path.substr(7);

    switch (session_.Check(req.Header("x-threadhaul-token"))) {
        case daemon::SessionGuard::Result::Ok: break;
        case daemon::SessionGuard::Result::Locked:
            return Error(429, "too many invalid tokens; try again in a minute");
        case daemon::SessionGuard::Result::Expired:
            return Error(401, "session expired; type 'web' in the controller console for a new link");
        default:
            return Error(401, "missing or invalid token; type 'web' in the controller console for a link");
    }

    daemon::HttpResponse res;
    res.headers.emplace_back("API-Version", "1");
    if (req.method == "GET" && route == "/fleet") {
        res.body = callbacks_.fleetJson();
    } else if (req.method == "GET" && route == "/results") {
        res.body = callbacks_.resultsJson();
    } else if (req.method == "POST" && route == "/logout") {
        session_.Revoke();
        res = Json(200, json{{"ok", true}, {"message", "signed out"}});
    } else if (req.method == "POST" && route == "/command") {
        if (req.Header("content-type").rfind("application/json", 0) != 0) {
            return Error(400, "Content-Type must be application/json");
        }
        const json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) return Error(400, "body must be a JSON object");
        const std::string host = body.value("host", "");
        const std::string verb = body.value("command", "");
        const std::string target = body.value("target", "");
        if (host.empty() || host.find_first_of(" \t\r\n") != std::string::npos) {
            return Error(400, "host must be an agent name or \"all\"");
        }
        const std::string why = ValidateCommand(verb, target);
        if (!why.empty()) return Error(400, why);
        res.body = callbacks_.send(host, target.empty() ? verb : verb + " " + target);
    } else {
        return Error(req.method == "GET" || req.method == "POST" ? 404 : 405, "unknown endpoint");
    }
    return res;
}

daemon::HttpResponse FleetWeb::ServeFile(const std::string& path) {
    struct Asset { const char* path; const wchar_t* file; const char* type; };
    static const Asset assets[] = {
        {"/index.html", L"index.html", "text/html; charset=utf-8"},
        {"/fleet.js", L"fleet.js", "text/javascript; charset=utf-8"},
        {"/fleet.css", L"fleet.css", "text/css; charset=utf-8"},
    };
    for (const Asset& a : assets) {
        if (path != a.path) continue;
        std::ifstream in(std::filesystem::path(root_) / a.file, std::ios::binary);
        daemon::HttpResponse r;
        if (!in) {
            r.status = 500;
            r.contentType = "text/plain; charset=utf-8";
            r.body = "Fleet UI files not found next to ThreadHaulController.exe (fleet\\). Re-run build.cmd or the installer.";
            return r;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        r.contentType = a.type;
        r.body = ss.str();
        return r;
    }
    daemon::HttpResponse r;
    r.status = 404;
    r.contentType = "text/plain; charset=utf-8";
    r.body = "Not found";
    return r;
}

}
