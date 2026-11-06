#include "Dashboard.h"

#include <windows.h>
#include <bcrypt.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "ActuatorManager.h"
#include "Daemon.h"

namespace daemon {

namespace {

using nlohmann::json;

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

std::string RandomToken() {
    unsigned char bytes[24] = {};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        std::abort();  // never hand out a predictable dashboard key
    }
    static const char* hex = "0123456789abcdef";
    std::string t;
    for (unsigned char b : bytes) {
        t += hex[b >> 4];
        t += hex[b & 15];
    }
    return t;
}

bool ConstantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

HttpResponse Json(int status, const json& j) {
    HttpResponse r;
    r.status = status;
    r.body = j.dump(-1, ' ', false, json::error_handler_t::replace);
    return r;
}

HttpResponse Error(int status, const std::string& message) {
    return Json(status, json{{"ok", false}, {"message", message}});
}

}

Dashboard::Dashboard(Daemon& daemon) : daemon_(daemon), session_(RandomToken) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    root_ = (std::filesystem::path(exe).parent_path() / L"dashboard").wstring();
    table_.SetEssentialNames(actuator::ActuatorManager::AlwaysProtectedNames());
    telemetry::ProcessTable::System primed;
    table_.Sample(primed);  // first sample sets the baseline for CPU deltas
}

Dashboard::~Dashboard() { Stop(); }

bool Dashboard::Start(int port, std::string& error) {
    Stop();
    auto server = std::make_unique<HttpServer>([this](const HttpRequest& r) { return Handle(r); });
    if (!server->Start(port, error)) {
        return false;
    }
    server_ = std::move(server);
    port_ = port;
    return true;
}

void Dashboard::Stop() {
    if (server_) {
        server_->Stop();
        server_.reset();
    }
}

std::string Dashboard::Url() const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/#token=" + session_.CurrentOrNewKey();
}

bool Dashboard::Authorized(const HttpRequest& req, HttpResponse& res) const {
    const std::string host = req.Header("host");
    const std::string port = std::to_string(port_);
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

HttpResponse Dashboard::Handle(const HttpRequest& req) {
    HttpResponse denied;
    if (!Authorized(req, denied)) {
        return denied;
    }
    if (req.path.rfind("/api/", 0) != 0) {
        if (req.method != "GET") return Error(405, "method not allowed");
        return ServeFile(req.path == "/" ? "/index.html" : req.path);
    }

    // Versioning: /api/v1/<route> is the stable API. The original /api/<route>
    // paths remain as deprecated aliases of v1 so existing scripts keep working.
    std::string route;
    bool legacy = false;
    if (req.path.rfind("/api/v1/", 0) == 0) {
        route = req.path.substr(7);
    } else if (req.path.size() > 6 && req.path.compare(0, 6, "/api/v") == 0 &&
               isdigit(static_cast<unsigned char>(req.path[6]))) {
        return Error(404, "unsupported API version; this service implements /api/v1");
    } else {
        route = req.path.substr(4);
        legacy = true;
    }

    // The OpenAPI description is public documentation: no session key needed
    // (the loopback, Host and Origin checks above still apply).
    if (!legacy && route == "/openapi.json") {
        if (req.method != "GET") return Error(405, "method not allowed");
        return ServeFile("/openapi.json");
    }

    HttpResponse res = Api(req, route);
    res.headers.emplace_back("API-Version", "1");
    if (legacy) {
        res.headers.emplace_back("Deprecation", "true");
        res.headers.emplace_back("Link", "</api/v1" + route + ">; rel=\"successor-version\"");
    }
    return res;
}

HttpResponse Dashboard::Api(const HttpRequest& req, const std::string& route) {
    switch (session_.Check(req.Header("x-threadhaul-token"))) {
    case SessionGuard::Result::Ok:
        break;
    case SessionGuard::Result::Locked:
        return Error(429, "too many invalid tokens; try again in a minute");
    case SessionGuard::Result::Expired:
        return Error(401, "dashboard session expired; run 'ThreadHaulDaemon.exe dashboard' "
                          "for a new link");
    default:
        return Error(401, "missing or invalid dashboard token; open the dashboard with "
                          "'ThreadHaulDaemon.exe dashboard'");
    }
    if (req.method == "POST" && route == "/logout") {
        session_.Revoke();
        return Json(200, json{{"ok", true}, {"message", "signed out"}});
    }

    if (req.method == "GET" && route == "/status") {
        HttpResponse r;
        r.body = daemon_.DashboardStatus();
        return r;
    }
    if (req.method == "GET" && route == "/processes") {
        return Processes();
    }
    if (req.method == "GET" && route == "/log") {
        const std::string off = req.QueryParam("offset");
        long long offset = -1;
        if (!off.empty()) {
            try { offset = std::stoll(off); } catch (...) { return Error(400, "bad offset"); }
        }
        HttpResponse r;
        r.body = daemon_.ReadLog(offset);
        return r;
    }
    if (req.method == "POST" && (route == "/command" || route == "/role")) {
        if (req.Header("content-type").rfind("application/json", 0) != 0) {
            return Error(400, "Content-Type must be application/json");
        }
        const json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            return Error(400, "body must be a JSON object");
        }
        HttpResponse r;
        if (route == "/command") {
            const std::string verb = body.value("command", "");
            const std::string target = body.value("target", "");
            if (verb != "throttle" && verb != "suspend" && verb != "resume" && verb != "release" &&
                verb != "exempt") {
                return Error(400, "command must be throttle, suspend, resume, release or exempt");
            }
            if (target.empty() || target.find_first_of(" \t\r\n") != std::string::npos) {
                return Error(400, "target must be a PID or an executable name");
            }
            if (verb != "release" && verb != "resume" && verb != "exempt" && !body.value("confirm", false)) {
                const telemetry::SafetyInfo s = daemon_.SafetyOf(target);
                if (s.level == telemetry::ProcessSafety::System ||
                    s.level == telemetry::ProcessSafety::Service) {
                    return Json(200, json{{"ok", false},
                                          {"needs_confirmation", true},
                                          {"safety", telemetry::SafetyName(s.level)},
                                          {"message", Narrow(s.reason)}});
                }
            }
            r.body = daemon_.HandleCommand(verb + " " + target);
        } else {
            r.body = daemon_.SetRole(body.value("name", ""), body.value("role", ""),
                                     body.value("confirm", false));
        }
        return r;
    }
    return Error(req.method == "GET" || req.method == "POST" ? 404 : 405, "unknown endpoint");
}

HttpResponse Dashboard::ServeFile(const std::string& path) {
    struct Asset { const char* path; const wchar_t* file; const char* type; };
    static const Asset assets[] = {
        {"/index.html", L"index.html", "text/html; charset=utf-8"},
        {"/app.js", L"app.js", "text/javascript; charset=utf-8"},
        {"/styles.css", L"styles.css", "text/css; charset=utf-8"},
        {"/openapi.json", L"openapi.json", "application/json; charset=utf-8"},
    };
    for (const Asset& a : assets) {
        if (path != a.path) continue;
        std::ifstream in(std::filesystem::path(root_) / a.file, std::ios::binary);
        if (!in) {
            HttpResponse r;
            r.status = 500;
            r.contentType = "text/plain; charset=utf-8";
            r.body = "Dashboard files not found in " + Narrow(root_) +
                     ". Re-run build.cmd or the installer.";
            return r;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        HttpResponse r;
        r.contentType = a.type;
        r.body = ss.str();
        return r;
    }
    HttpResponse r;
    r.status = 404;
    r.contentType = "text/plain; charset=utf-8";
    r.body = "Not found";
    return r;
}

HttpResponse Dashboard::Processes() {
    telemetry::ProcessTable::System sys;
    const auto rows = table_.Sample(sys);
    const json roles = json::parse(daemon_.ProcessRolesJson());

    // ProcessRolesJson lists every name in lower case.
    auto lower = [](std::wstring w) {
        CharLowerBuffW(w.data(), static_cast<DWORD>(w.size()));
        return Narrow(w);
    };
    auto inList = [&](const json& list, const std::string& name) {
        for (const auto& x : list) {
            if (x.get<std::string>() == name) return true;
        }
        return false;
    };

    json list = json::array();
    for (const auto& r : rows) {
        const std::string key = lower(r.name);
        std::string role = "none";
        for (const char* candidate : {"critical", "batch", "protected"}) {
            if (inList(roles[candidate], key)) {
                role = candidate;
                break;
            }
        }
        list.push_back({{"pid", r.pid},
                        {"name", Narrow(r.name)},
                        {"cpu_percent", static_cast<long long>(r.cpuPercent * 10 + 0.5) / 10.0},
                        {"memory_mb", static_cast<long long>(r.workingSetMb * 10 + 0.5) / 10.0},
                        {"accessible", r.accessible},
                        {"role", role},
                        {"state", roles["states"].value(std::to_string(r.pid), "")},
                        {"safety", telemetry::SafetyName(r.safety.level)},
                        {"safety_reason", Narrow(r.safety.reason)},
                        {"path", Narrow(r.safety.imagePath)},
                        {"system_protected", r.safety.level == telemetry::ProcessSafety::Essential ||
                                                 inList(roles["builtin_protected"], key)}});
    }
    return Json(200, json{{"ok", true},
                          {"system", {{"cpu_percent", static_cast<long long>(sys.cpuPercent * 10 + 0.5) / 10.0},
                                      {"memory_load_percent", sys.memoryLoadPercent},
                                      {"memory_used_gb", static_cast<long long>(sys.memoryUsedGb * 10 + 0.5) / 10.0},
                                      {"memory_total_gb", static_cast<long long>(sys.memoryTotalGb * 10 + 0.5) / 10.0},
                                      {"logical_cores", sys.logicalCores},
                                      {"process_count", sys.processCount}}},
                          {"processes", list}});
}

}
