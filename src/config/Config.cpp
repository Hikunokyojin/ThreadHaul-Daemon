#include "Config.h"
#include "Secrets.h"

#include <windows.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace config {

namespace {

using nlohmann::json;

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) {
        throw ConfigError("config contains text that is not valid UTF-8");
    }
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                        static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n,
                        nullptr, nullptr);
    return s;
}

const json& Field(const json& obj, const char* key, const std::string& path) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        throw ConfigError("missing required field: " + path);
    }
    return *it;
}

const json& Object(const json& obj, const char* key, const std::string& path) {
    const json& v = Field(obj, key, path);
    if (!v.is_object()) {
        throw ConfigError(path + " must be an object");
    }
    return v;
}

int Integer(const json& obj, const char* key, const std::string& path, int min, int max) {
    const json& v = Field(obj, key, path);
    if (!v.is_number_integer()) {
        throw ConfigError(path + " must be an integer");
    }
    const long long x = v.get<long long>();
    if (x < min || x > max) {
        throw ConfigError(path + " must be between " + std::to_string(min) + " and " +
                          std::to_string(max) + ", got " + std::to_string(x));
    }
    return static_cast<int>(x);
}

double Percent(const json& obj, const char* key, const std::string& path) {
    const json& v = Field(obj, key, path);
    if (!v.is_number()) {
        throw ConfigError(path + " must be a number");
    }
    const double x = v.get<double>();
    if (!(x > 0.0 && x <= 100.0)) {
        throw ConfigError(path + " must be greater than 0 and at most 100");
    }
    return x;
}

std::vector<std::wstring> NameList(const json& v, const std::string& path) {
    if (!v.is_array()) {
        throw ConfigError(path + " must be an array of process names");
    }
    std::vector<std::wstring> names;
    for (size_t i = 0; i < v.size(); ++i) {
        const std::string item = path + "[" + std::to_string(i) + "]";
        if (!v[i].is_string()) {
            throw ConfigError(item + " must be a string");
        }
        const std::string name = v[i].get<std::string>();
        if (name.empty()) {
            throw ConfigError(item + " must not be empty");
        }
        if (name.find_first_of("\\/") != std::string::npos) {
            throw ConfigError(item + " must be an executable name such as \"app.exe\", not a path");
        }
        names.push_back(Utf8ToWide(name));
    }
    return names;
}

ThresholdSpec Threshold(const json& thresholds, const char* key) {
    const std::string path = std::string("thresholds.") + key;
    const json& t = Object(thresholds, key, path);
    ThresholdSpec spec;
    spec.percent = Percent(t, "percent", path + ".percent");
    spec.consecutiveSamples =
        Integer(t, "consecutive_samples", path + ".consecutive_samples", 1, 100);
    return spec;
}

bool OptBool(const json& obj, const char* key, const std::string& path, bool def) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return def;
    }
    if (!it->is_boolean()) {
        throw ConfigError(path + " must be true or false");
    }
    return it->get<bool>();
}

int OptInteger(const json& obj, const char* key, const std::string& path, int min, int max,
               int def) {
    return obj.contains(key) ? Integer(obj, key, path, min, max) : def;
}

double OptNumber(const json& obj, const char* key, const std::string& path, double min,
                 double max, double def) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return def;
    }
    if (!it->is_number()) {
        throw ConfigError(path + " must be a number");
    }
    const double x = it->get<double>();
    if (x < min || x > max) {
        throw ConfigError(path + " must be between " + std::to_string(min) + " and " +
                          std::to_string(max));
    }
    return x;
}

// Decrypts a "dpapi:" value; plain values pass through.
std::string Secret(const std::string& value, const char* path) {
    std::string plain, error;
    if (!UnprotectSecret(value, plain, error)) {
        throw ConfigError(std::string(path) + ": " + error);
    }
    return plain;
}

std::string OptString(const json& obj, const char* key, const std::string& path,
                      const std::string& def) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return def;
    }
    if (!it->is_string()) {
        throw ConfigError(path + " must be a string");
    }
    return it->get<std::string>();
}

const json* OptObject(const json& obj, const char* key, const std::string& path) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return nullptr;
    }
    if (!it->is_object()) {
        throw ConfigError(path + " must be an object");
    }
    return &*it;
}

bool SameName(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

}

DaemonConfig ParseConfig(const std::string& jsonText) {
    json root;
    try {
        root = json::parse(jsonText);
    } catch (const json::exception& e) {
        // parse_error, and out_of_range for numbers such as 1e309: both are
        // bad input and must surface as ConfigError, never escape uncaught.
        throw ConfigError(std::string("config is not valid JSON: ") + e.what());
    }
    if (!root.is_object()) {
        throw ConfigError("config must be a JSON object");
    }

    DaemonConfig cfg;

    cfg.pollIntervalMs =
        Integer(Object(root, "polling", "polling"), "interval_ms", "polling.interval_ms", 100, 60000);

    const json& thresholds = Object(root, "thresholds", "thresholds");
    cfg.high = Threshold(thresholds, "high");
    cfg.critical = Threshold(thresholds, "critical");
    cfg.low = Threshold(thresholds, "low");
    if (cfg.critical.percent < cfg.high.percent) {
        throw ConfigError("thresholds.critical.percent must be at least thresholds.high.percent");
    }
    if (cfg.low.percent >= cfg.high.percent) {
        throw ConfigError("thresholds.low.percent must be below thresholds.high.percent");
    }

    const json& processes = Object(root, "processes", "processes");
    cfg.criticalProcesses = NameList(Field(processes, "critical", "processes.critical"),
                                     "processes.critical");
    cfg.batchProcesses = NameList(Field(processes, "batch", "processes.batch"), "processes.batch");
    // Empty lists are allowed: the service then monitors nothing until
    // processes are assigned (for example from the dashboard).
    const auto prot = processes.find("protected");
    if (prot != processes.end()) {
        cfg.protectedProcesses = NameList(*prot, "processes.protected");
    }
    for (const std::wstring& c : cfg.criticalProcesses) {
        for (const std::wstring& b : cfg.batchProcesses) {
            if (SameName(c, b)) {
                throw ConfigError("\"" + WideToUtf8(c) +
                                  "\" is listed in both processes.critical and processes.batch");
            }
        }
    }

    const json& logging = Object(root, "logging", "logging");
    const json& logPath = Field(logging, "log_path", "logging.log_path");
    if (!logPath.is_string() || logPath.get<std::string>().empty()) {
        throw ConfigError("logging.log_path must be a non-empty string");
    }
    cfg.logPath = Utf8ToWide(logPath.get<std::string>());
    cfg.maxFileSizeMb =
        Integer(logging, "max_file_size_mb", "logging.max_file_size_mb", 1, 1024);
    cfg.maxRotatedFiles =
        Integer(logging, "max_rotated_files", "logging.max_rotated_files", 0, 100);

    if (const json* t = OptObject(root, "telemetry", "telemetry")) {
        const std::string norm =
            OptString(*t, "cpu_normalization", "telemetry.cpu_normalization", "per_core");
        if (norm != "per_core" && norm != "total") {
            throw ConfigError("telemetry.cpu_normalization must be \"per_core\" or \"total\"");
        }
        cfg.cpuPerCore = norm == "per_core";
        cfg.logSamples = OptBool(*t, "log_samples", "telemetry.log_samples", true);
        cfg.selfMonitorIntervalSec = OptInteger(*t, "self_monitor_interval_seconds",
                                                "telemetry.self_monitor_interval_seconds", 0,
                                                86400, 30);
    }

    if (const json* tr = OptObject(root, "triggers", "triggers")) {
        if (const json* m = OptObject(*tr, "memory", "triggers.memory")) {
            cfg.memoryTriggerEnabled = OptBool(*m, "enabled", "triggers.memory.enabled", false);
            cfg.systemMemoryLimitPercent =
                OptNumber(*m, "system_load_limit_percent",
                          "triggers.memory.system_load_limit_percent", 1, 100, 95);
            cfg.criticalWorkingSetLimitMb =
                OptNumber(*m, "critical_working_set_limit_mb",
                          "triggers.memory.critical_working_set_limit_mb", 0, 1048576, 0);
        }
        if (const json* io = OptObject(*tr, "io", "triggers.io")) {
            cfg.ioTriggerEnabled = OptBool(*io, "enabled", "triggers.io.enabled", false);
            cfg.criticalIoLimitMbPerSec =
                OptNumber(*io, "critical_io_limit_mb_per_sec",
                          "triggers.io.critical_io_limit_mb_per_sec", 0.1, 100000, 200);
        }
    }

    if (const json* a = OptObject(root, "adaptive", "adaptive")) {
        cfg.adaptiveEnabled = OptBool(*a, "enabled", "adaptive.enabled", false);
        cfg.adaptiveWindowSamples =
            OptInteger(*a, "window_samples", "adaptive.window_samples", 3, 600, 10);
        cfg.adaptiveHorizonSamples =
            OptInteger(*a, "horizon_samples", "adaptive.horizon_samples", 1, 60, 3);
        cfg.adaptiveMaxAdjustPercent =
            OptNumber(*a, "max_adjust_percent", "adaptive.max_adjust_percent", 0, 50, 10);
    }

    if (const json* al = OptObject(root, "alerts", "alerts")) {
        cfg.alertPopup = OptBool(*al, "popup", "alerts.popup", true);
        const std::string url =
            Secret(OptString(*al, "webhook_url", "alerts.webhook_url", ""), "alerts.webhook_url");
        if (!url.empty() && url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
            throw ConfigError("alerts.webhook_url must start with http:// or https://");
        }
        cfg.alertWebhookUrl = Utf8ToWide(url);
    }

    if (const json* id = OptObject(root, "identity", "identity")) {
        for (const auto& [name, spec] : id->items()) {
            const std::string where = "identity." + name;
            if (name.size() < 5 || name.find_first_of("\\/") != std::string::npos ||
                _stricmp(name.c_str() + name.size() - 4, ".exe") != 0) {
                throw ConfigError(where + ": key must be an executable name such as app.exe");
            }
            if (!spec.is_object()) throw ConfigError(where + " must be an object");
            DaemonConfig::PinnedIdentity p;
            p.name = Utf8ToWide(name);
            p.path = Utf8ToWide(OptString(spec, "path", where + ".path", ""));
            p.publisher = Utf8ToWide(OptString(spec, "publisher", where + ".publisher", ""));
            if (p.path.empty() && p.publisher.empty()) {
                throw ConfigError(where + " needs a path, a publisher, or both");
            }
            if (!p.path.empty() && (p.path.size() < 3 || p.path[1] != L':' || p.path[2] != L'\\')) {
                if (p.path.rfind(L"\\\\", 0) != 0) {
                    throw ConfigError(where + ".path must be a full path such as C:\\\\Apps\\\\app.exe");
                }
            }
            cfg.identities.push_back(std::move(p));
        }
    }

    if (const json* c = OptObject(root, "coordination", "coordination")) {
        cfg.coordinationEnabled = OptBool(*c, "enabled", "coordination.enabled", false);
        cfg.controllerHost = Utf8ToWide(
            OptString(*c, "controller_host", "coordination.controller_host", "127.0.0.1"));
        cfg.controllerPort =
            OptInteger(*c, "controller_port", "coordination.controller_port", 1, 65535, 7878);
        cfg.sharedToken = Secret(OptString(*c, "shared_token", "coordination.shared_token", ""),
                                 "coordination.shared_token");
        cfg.heartbeatSeconds =
            OptInteger(*c, "heartbeat_seconds", "coordination.heartbeat_seconds", 1, 3600, 5);
        if (cfg.coordinationEnabled && cfg.controllerHost.empty()) {
            throw ConfigError("coordination.controller_host must not be empty when enabled");
        }
        if (cfg.coordinationEnabled && cfg.sharedToken.size() < 16) {
            throw ConfigError("coordination.shared_token must be at least 16 characters when enabled");
        }
    }

    if (const json* sec = OptObject(root, "security", "security")) {
        cfg.requireProtectedConfig = OptBool(*sec, "require_protected_config",
                                             "security.require_protected_config", false);
    }

    if (const json* a = OptObject(root, "auto_protect", "auto_protect")) {
        const std::string p = "auto_protect.";
        cfg.autoEnabled = OptBool(*a, "enabled", p + "enabled", true);
        cfg.autoThrottlePercent = OptNumber(*a, "throttle_at_percent", p + "throttle_at_percent", 10, 100, 90);
        cfg.autoThrottleSamples = OptInteger(*a, "throttle_samples", p + "throttle_samples", 1, 600, 5);
        cfg.autoUseIdle = OptBool(*a, "use_idle", p + "use_idle", true);
        cfg.autoIdlePercent = OptNumber(*a, "idle_at_percent", p + "idle_at_percent", 10, 100, 97);
        cfg.autoIdleSamples = OptInteger(*a, "idle_samples", p + "idle_samples", 1, 600, 10);
        cfg.autoRecoverPercent = OptNumber(*a, "recover_below_percent", p + "recover_below_percent", 1, 100, 75);
        cfg.autoRecoverSamples = OptInteger(*a, "recover_samples", p + "recover_samples", 1, 600, 10);
        cfg.autoMaxProcesses = OptInteger(*a, "max_processes", p + "max_processes", 1, 50, 3);
        cfg.autoMinProcessCpuPercent =
            OptNumber(*a, "min_process_cpu_percent", p + "min_process_cpu_percent", 1, 6400, 50);
        if (cfg.autoRecoverPercent >= cfg.autoThrottlePercent) {
            throw ConfigError("auto_protect.recover_below_percent must be below auto_protect.throttle_at_percent");
        }
        if (cfg.autoUseIdle && cfg.autoIdlePercent < cfg.autoThrottlePercent) {
            throw ConfigError("auto_protect.idle_at_percent must be at least auto_protect.throttle_at_percent");
        }
    }

    if (const json* d = OptObject(root, "dashboard", "dashboard")) {
        cfg.dashboardEnabled = OptBool(*d, "enabled", "dashboard.enabled", true);
        cfg.dashboardPort = OptInteger(*d, "port", "dashboard.port", 1024, 65535, 8787);
    }

    if (const auto sig = root.find("signals"); sig != root.end()) {
        if (!sig->is_array()) throw ConfigError("signals must be an array");
        if (sig->size() > 16) throw ConfigError("signals: at most 16 signals");
        for (size_t i = 0; i < sig->size(); ++i) {
            const std::string where = "signals[" + std::to_string(i) + "]";
            const json& s = (*sig)[i];
            if (!s.is_object()) throw ConfigError(where + " must be an object");
            DaemonConfig::SignalConfig sc;
            sc.type = OptString(s, "type", where + ".type", "");
            if (sc.type != "gpu" && sc.type != "disk_queue" && sc.type != "network" && sc.type != "counter") {
                throw ConfigError(where + ".type must be gpu, disk_queue, network or counter");
            }
            sc.name = OptString(s, "name", where + ".name", sc.type == "counter" ? "" : sc.type);
            if (sc.name.empty() || sc.name.size() > 40 ||
                sc.name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
                throw ConfigError(where + ".name must be 1-40 characters of a-z, 0-9 and _");
            }
            for (const auto& other : cfg.signals) {
                if (other.name == sc.name) throw ConfigError(where + ".name \"" + sc.name + "\" is used twice");
            }
            if (!s.contains("limit")) throw ConfigError(where + ".limit is required");
            sc.limit = OptNumber(s, "limit", where + ".limit", 1e-9, 1e15, 1.0);
            sc.aggregate = OptString(s, "aggregate", where + ".aggregate", "sum");
            if (sc.aggregate != "sum" && sc.aggregate != "max") {
                throw ConfigError(where + ".aggregate must be sum or max");
            }
            sc.instanceContains = Utf8ToWide(OptString(s, "instance_contains", where + ".instance_contains", ""));
            if (sc.type == "counter") {
                sc.counter = Utf8ToWide(OptString(s, "counter", where + ".counter", ""));
                if (sc.counter.size() < 4 || sc.counter[0] != L'\\' || sc.counter.find(L'\\', 1) == std::wstring::npos) {
                    throw ConfigError(where + ".counter must be a counter path such as \\Processor(_Total)\\% Processor Time");
                }
                sc.scale = OptNumber(s, "scale", where + ".scale", 1e-15, 1e15, 1.0);
            } else if (s.contains("counter") || s.contains("scale")) {
                throw ConfigError(where + ": counter and scale apply only to type \"counter\"");
            }
            cfg.signals.push_back(std::move(sc));
        }
    }

    if (const json* m = OptObject(root, "metrics", "metrics")) {
        cfg.metricsEnabled = OptBool(*m, "enabled", "metrics.enabled", false);
        cfg.metricsToken = Secret(OptString(*m, "token", "metrics.token", ""), "metrics.token");
        if (!cfg.metricsToken.empty() && cfg.metricsToken.size() < 16) {
            throw ConfigError("metrics.token must be at least 16 characters (or empty for no token)");
        }
        if (cfg.metricsToken.find_first_of(" \t\r\n") != std::string::npos) {
            throw ConfigError("metrics.token must not contain spaces or line breaks");
        }
    }

    return cfg;
}

std::string SetProcessRole(const std::string& jsonText, const std::string& processName,
                           const std::string& role) {
    using ordered = nlohmann::ordered_json;
    if (role != "critical" && role != "batch" && role != "protected" && role != "none") {
        throw ConfigError("role must be critical, batch, protected or none");
    }
    if (processName.empty() || processName.find_first_of("\\/") != std::string::npos) {
        throw ConfigError("process name must be an executable name such as \"app.exe\"");
    }
    ordered root;
    try {
        root = ordered::parse(jsonText);
    } catch (const nlohmann::json::exception& e) {  // parse_error and out_of_range (e.g. 1e309)
        throw ConfigError(std::string("config is not valid JSON: ") + e.what());
    }
    if (!root.is_object() || !root.contains("processes") || !root["processes"].is_object()) {
        throw ConfigError("config has no processes object");
    }
    ordered& processes = root["processes"];
    const std::wstring wanted = Utf8ToWide(processName);
    for (const char* list : {"critical", "batch", "protected"}) {
        if (!processes.contains(list)) {
            continue;
        }
        ordered& names = processes[list];
        if (!names.is_array()) {
            throw ConfigError(std::string("processes.") + list + " must be an array");
        }
        ordered kept = ordered::array();
        for (const auto& n : names) {
            if (!(n.is_string() && SameName(Utf8ToWide(n.get<std::string>()), wanted))) {
                kept.push_back(n);
            }
        }
        names = kept;
    }
    if (role != "none") {
        if (!processes.contains(role)) {
            processes[role] = ordered::array();
        }
        processes[role].push_back(processName);
    }
    std::string out = root.dump(2);
    out += "\n";
    ParseConfig(out);  // throws ConfigError if the result is invalid
    return out;
}

DaemonConfig LoadConfigFile(const std::wstring& path) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) {
        throw ConfigError("cannot open config file: " + WideToUtf8(path));
    }
    std::ostringstream contents;
    contents << in.rdbuf();
    return ParseConfig(contents.str());
}

}
