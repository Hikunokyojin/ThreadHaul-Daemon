#include "Dashboard.h"

#include <windows.h>
#include <bcrypt.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

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
    unsigned char bytes[24];
    BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
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

Dashboard::Dashboard(Daemon& daemon) : daemon_(daemon), token_(RandomToken()) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    root_ = (std::filesystem::path(exe).parent_path() / L"dashboard").wstring();
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
    return "http://127.0.0.1:" + std::to_string(port_) + "/#token=" + token_;
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
    if (!ConstantTimeEquals(req.Header("x-threadhaul-token"), token_)) {
        return Error(401, "missing or invalid dashboard token; open the dashboard with "
                          "'ThreadHaulDaemon.exe dashboard'");
    }

    if (req.method == "GET" && req.path == "/api/status") {
        HttpResponse r;
        r.body = daemon_.DashboardStatus();
        return r;
    }
    if (req.method == "GET" && req.path == "/api/processes") {
        return Processes();
    }
    if (req.method == "GET" && req.path == "/api/log") {
        const std::string off = req.QueryParam("offset");
        long long offset = -1;
        if (!off.empty()) {
            try { offset = std::stoll(off); } catch (...) { return Error(400, "bad offset"); }
        }
        HttpResponse r;
        r.body = daemon_.ReadLog(offset);
        return r;
    }
    if (req.method == "POST" && (req.path == "/api/command" || req.path == "/api/role")) {
        if (req.Header("content-type").rfind("application/json", 0) != 0) {
            return Error(400, "Content-Type must be application/json");
        }
        const json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            return Error(400, "body must be a JSON object");
        }
        HttpResponse r;
        if (req.path == "/api/command") {
            const std::string verb = body.value("command", "");
            const std::string target = body.value("target", "");
            if (verb != "throttle" && verb != "suspend" && verb != "resume" && verb != "release") {
                return Error(400, "command must be throttle, suspend, resume or release");
            }
            if (target.empty() || target.find_first_of(" \t\r\n") != std::string::npos) {
                return Error(400, "target must be a PID or an executable name");
            }
            r.body = daemon_.HandleCommand(verb + " " + target);
        } else {
            r.body = daemon_.SetRole(body.value("name", ""), body.value("role", ""));
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
                        {"system_protected", inList(roles["builtin_protected"], key) ||
                                                 r.pid == GetCurrentProcessId()}});
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
