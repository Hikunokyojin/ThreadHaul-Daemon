// Unit tests for config loading/validation and the JSON Lines logger.

#include "Config.h"
#include "JsonLinesLogger.h"
#include "LogChain.h"
#include "Metrics.h"
#include "Secrets.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* test, const std::string& what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        std::printf("FAIL  %s: %s\n", test, what.c_str());
    }
}

const char* kValidConfig = R"({
  "polling": { "interval_ms": 1000 },
  "thresholds": {
    "high":     { "percent": 85, "consecutive_samples": 3 },
    "critical": { "percent": 90, "consecutive_samples": 5 },
    "low":      { "percent": 60, "consecutive_samples": 5 }
  },
  "processes": {
    "critical": ["dispatch_api.exe"],
    "batch": ["invoice_batch.exe", "archiver.exe"],
    "protected": ["explorer.exe"]
  },
  "logging": {
    "log_path": "C:\\logs\\threadhaul.jsonl",
    "max_file_size_mb": 50,
    "max_rotated_files": 5
  }
})";

// Returns the error message, or "" if the config was accepted.
std::string ParseError(const nlohmann::json& cfg) {
    try {
        config::ParseConfig(cfg.dump());
        return "";
    } catch (const config::ConfigError& e) {
        return e.what();
    }
}

void ExpectRejected(const char* test, nlohmann::json cfg, const std::string& mustMention) {
    const std::string err = ParseError(cfg);
    Check(!err.empty(), test, "config with bad " + mustMention + " was accepted");
    Check(err.find(mustMention) != std::string::npos, test,
          "error should name " + mustMention + ", got: " + err);
}

void ParsesValidConfig() {
    const config::DaemonConfig c = config::ParseConfig(kValidConfig);
    Check(c.pollIntervalMs == 1000, __func__, "interval");
    Check(c.high.percent == 85 && c.high.consecutiveSamples == 3, __func__, "high threshold");
    Check(c.critical.percent == 90 && c.critical.consecutiveSamples == 5, __func__, "critical");
    Check(c.low.percent == 60 && c.low.consecutiveSamples == 5, __func__, "low threshold");
    Check(c.criticalProcesses == std::vector<std::wstring>{L"dispatch_api.exe"}, __func__,
          "critical processes");
    Check(c.batchProcesses.size() == 2 && c.batchProcesses[1] == L"archiver.exe", __func__,
          "batch processes");
    Check(c.protectedProcesses == std::vector<std::wstring>{L"explorer.exe"}, __func__,
          "protected processes");
    Check(c.logPath == L"C:\\logs\\threadhaul.jsonl", __func__, "log path");
    Check(c.maxFileSizeMb == 50 && c.maxRotatedFiles == 5, __func__, "rotation settings");
}

void ProtectedListIsOptional() {
    nlohmann::json cfg = nlohmann::json::parse(kValidConfig);
    cfg["processes"].erase("protected");
    Check(ParseError(cfg).empty(), __func__, "config without processes.protected rejected");
}

void RejectsMalformedJson() {
    std::string err;
    try {
        config::ParseConfig("{ \"polling\": ");
    } catch (const config::ConfigError& e) {
        err = e.what();
    }
    Check(err.find("not valid JSON") != std::string::npos, __func__, "got: " + err);
}

void RejectsMissingFields() {
    const nlohmann::json base = nlohmann::json::parse(kValidConfig);
    auto without = [&](const nlohmann::json::json_pointer& ptr) {
        nlohmann::json cfg = base;
        cfg[ptr.parent_pointer()].erase(ptr.back());
        return cfg;
    };
    ExpectRejected(__func__, without("/polling/interval_ms"_json_pointer), "polling.interval_ms");
    ExpectRejected(__func__, without("/thresholds/low"_json_pointer), "thresholds.low");
    ExpectRejected(__func__, without("/processes/batch"_json_pointer), "processes.batch");
    ExpectRejected(__func__, without("/logging/log_path"_json_pointer), "logging.log_path");
}

void RejectsOutOfRangeValues() {
    const nlohmann::json base = nlohmann::json::parse(kValidConfig);
    auto with = [&](const nlohmann::json::json_pointer& ptr, nlohmann::json value) {
        nlohmann::json cfg = base;
        cfg[ptr] = std::move(value);
        return cfg;
    };
    ExpectRejected(__func__, with("/polling/interval_ms"_json_pointer, 10), "polling.interval_ms");
    ExpectRejected(__func__, with("/polling/interval_ms"_json_pointer, "1000"), "polling.interval_ms");
    ExpectRejected(__func__, with("/thresholds/high/percent"_json_pointer, 120), "thresholds.high.percent");
    ExpectRejected(__func__, with("/thresholds/low/percent"_json_pointer, 0), "thresholds.low.percent");
    ExpectRejected(__func__, with("/thresholds/critical/consecutive_samples"_json_pointer, 0),
                   "thresholds.critical.consecutive_samples");
    ExpectRejected(__func__, with("/logging/max_file_size_mb"_json_pointer, 0), "logging.max_file_size_mb");
}

void RejectsInconsistentThresholds() {
    const nlohmann::json base = nlohmann::json::parse(kValidConfig);
    nlohmann::json criticalBelowHigh = base;
    criticalBelowHigh["thresholds"]["critical"]["percent"] = 80;
    ExpectRejected(__func__, criticalBelowHigh, "thresholds.critical.percent");

    nlohmann::json lowNotBelowHigh = base;
    lowNotBelowHigh["thresholds"]["low"]["percent"] = 85;
    ExpectRejected(__func__, lowNotBelowHigh, "thresholds.low.percent");
}

void RejectsBadProcessLists() {
    const nlohmann::json base = nlohmann::json::parse(kValidConfig);
    nlohmann::json emptyLists = base;
    emptyLists["processes"]["batch"] = nlohmann::json::array();
    emptyLists["processes"]["critical"] = nlohmann::json::array();
    Check(ParseError(emptyLists).empty(), __func__, "empty critical/batch lists are allowed");

    nlohmann::json pathName = base;
    pathName["processes"]["batch"] = {"C:\\apps\\invoice_batch.exe"};
    ExpectRejected(__func__, pathName, "processes.batch[0]");

    nlohmann::json overlap = base;
    overlap["processes"]["batch"] = {"DISPATCH_API.EXE"};
    ExpectRejected(__func__, overlap, "both processes.critical and processes.batch");
}

void RejectsMissingFile() {
    std::string err;
    try {
        config::LoadConfigFile(L"C:\\definitely\\not\\here\\config.json");
    } catch (const config::ConfigError& e) {
        err = e.what();
    }
    Check(err.find("cannot open config file") != std::string::npos, __func__, "got: " + err);
}

std::vector<nlohmann::json> ReadLines(const fs::path& file) {
    std::vector<nlohmann::json> lines;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(nlohmann::json::parse(line));
    }
    return lines;
}

bool LooksLikeIsoTimestamp(const std::string& ts) {
    // 2026-09-24T14:03:21.501Z
    return ts.size() == 24 && ts[4] == '-' && ts[10] == 'T' && ts[19] == '.' && ts[23] == 'Z';
}

void LogsAllThreeKindsWithSchemaFields(const fs::path& dir) {
    const fs::path file = dir / "schema.jsonl";
    {
        logging::JsonLinesLogger log(file.wstring(), 1024 * 1024, 3);
        Check(log.LogSample(4821, 42.5), __func__, "LogSample write");
        Check(log.LogThresholdEvent(4821, actuator::ThresholdType::High, 3, 87.2), __func__,
              "LogThresholdEvent write");
        Check(log.LogActuatorAction(5190, L"invoice_batch.exe", 4821,
                                    actuator::ProcessState::Throttled,
                                    actuator::ProcessState::Suspended, 93.0, false),
              __func__, "LogActuatorAction write");
    }
    const auto lines = ReadLines(file);
    Check(lines.size() == 3, __func__, "expected 3 lines");
    if (lines.size() != 3) {
        return;
    }
    for (const auto& l : lines) {
        Check(l.contains("log_type") && l.contains("timestamp"), __func__,
              "every line has log_type and timestamp");
        Check(LooksLikeIsoTimestamp(l.value("timestamp", "")), __func__,
              "timestamp format: " + l.value("timestamp", ""));
    }
    Check(lines[0]["log_type"] == "sample" && lines[0]["process_id"] == 4821 &&
              lines[0]["cpu_percent"] == 42.5,
          __func__, "sample fields");
    Check(lines[1]["log_type"] == "threshold_event" && lines[1]["threshold"] == "high" &&
              lines[1]["consecutive_samples"] == 3,
          __func__, "threshold_event fields");
    const auto& a = lines[2];
    Check(a["log_type"] == "actuator_action" && a["batch_process_id"] == 5190 &&
              a["batch_process_name"] == "invoice_batch.exe" && a["critical_process_id"] == 4821 &&
              a["action"] == "threads_suspended" && a["from_state"] == "throttled" &&
              a["to_state"] == "suspended" && a["cpu_percent"] == 93.0 &&
              a["actuation_succeeded"] == false,
          __func__, "actuator_action fields: " + a.dump());
}

void AppendsToExistingFile(const fs::path& dir) {
    const fs::path file = dir / "append.jsonl";
    {
        logging::JsonLinesLogger log(file.wstring(), 1024 * 1024, 3);
        log.LogSample(1, 1.0);
    }
    {
        logging::JsonLinesLogger log(file.wstring(), 1024 * 1024, 3);
        log.LogSample(2, 2.0);
    }
    Check(ReadLines(file).size() == 2, __func__, "second logger appends instead of truncating");
}

void RotatesBySizeAndCapsFileCount(const fs::path& dir) {
    const fs::path file = dir / "rotate.jsonl";
    constexpr uint64_t kMaxBytes = 300;
    {
        logging::JsonLinesLogger log(file.wstring(), kMaxBytes, 2);
        for (int i = 0; i < 40; ++i) {
            log.LogSample(1000 + i, 50.0);
        }
    }
    Check(fs::exists(file), __func__, "current log exists");
    Check(fs::exists(file.string() + ".1"), __func__, ".1 exists");
    Check(fs::exists(file.string() + ".2"), __func__, ".2 exists");
    Check(!fs::exists(file.string() + ".3"), __func__, "no .3 beyond max_rotated_files");
    Check(fs::file_size(file) <= kMaxBytes, __func__, "current log within size limit");

    const auto newest = ReadLines(file);
    Check(!newest.empty() && newest.back()["process_id"] == 1039, __func__,
          "last line written is in the current file");
}

void CreatesMissingLogDirectory(const fs::path& dir) {
    const fs::path file = dir / "nested" / "deeper" / "log.jsonl";
    {
        logging::JsonLinesLogger log(file.wstring(), 1024, 1);
        Check(log.LogSample(7, 7.0), __func__, "write into a directory that did not exist");
    }
    Check(fs::exists(file), __func__, "log file created");
}

void StateAndActionNames() {
    using actuator::ProcessState;
    Check(std::string(logging::ActionName(ProcessState::Throttled)) == "priority_lowered" &&
              std::string(logging::ActionName(ProcessState::Suspended)) == "threads_suspended" &&
              std::string(logging::ActionName(ProcessState::Normal)) == "recovered",
          __func__, "action names match the documented log schema");
    Check(std::string(logging::StateName(ProcessState::Normal)) == "normal", __func__, "state name");
}

void OptionalSectionsDefault() {
    const config::DaemonConfig c = config::ParseConfig(kValidConfig);
    Check(c.cpuPerCore && !c.adaptiveEnabled && !c.coordinationEnabled && !c.memoryTriggerEnabled &&
              !c.ioTriggerEnabled && c.alertPopup && !c.requireProtectedConfig,
          __func__, "defaults when optional sections are absent");
}

void ParsesOptionalSections() {
    nlohmann::json cfg = nlohmann::json::parse(kValidConfig);
    cfg["telemetry"] = {{"cpu_normalization", "total"}, {"self_monitor_interval_seconds", 10}};
    cfg["triggers"] = {{"memory", {{"enabled", true}, {"system_load_limit_percent", 90}}},
                       {"io", {{"enabled", true}, {"critical_io_limit_mb_per_sec", 50}}}};
    cfg["adaptive"] = {{"enabled", true}, {"window_samples", 8}, {"max_adjust_percent", 7.5}};
    cfg["alerts"] = {{"popup", false}, {"webhook_url", "https://example.invalid/hook"}};
    cfg["coordination"] = {{"enabled", true}, {"controller_host", "ctl"}, {"controller_port", 9000},
                           {"shared_token", "0123456789abcdef-secret"}};
    cfg["security"] = {{"require_protected_config", true}};
    const config::DaemonConfig c = config::ParseConfig(cfg.dump());
    Check(!c.cpuPerCore && c.selfMonitorIntervalSec == 10, __func__, "telemetry");
    Check(c.memoryTriggerEnabled && c.systemMemoryLimitPercent == 90, __func__, "memory trigger");
    Check(c.ioTriggerEnabled && c.criticalIoLimitMbPerSec == 50, __func__, "io trigger");
    Check(c.adaptiveEnabled && c.adaptiveWindowSamples == 8 && c.adaptiveMaxAdjustPercent == 7.5,
          __func__, "adaptive");
    Check(!c.alertPopup && c.alertWebhookUrl == L"https://example.invalid/hook", __func__, "alerts");
    Check(c.coordinationEnabled && c.controllerHost == L"ctl" && c.controllerPort == 9000, __func__,
          "coordination");
    Check(c.requireProtectedConfig, __func__, "security");
}

void RejectsBadOptionalSections() {
    const nlohmann::json base = nlohmann::json::parse(kValidConfig);
    auto with = [&](const char* key, nlohmann::json value) {
        nlohmann::json cfg = base;
        cfg[key] = std::move(value);
        return cfg;
    };
    ExpectRejected(__func__, with("telemetry", {{"cpu_normalization", "cores"}}),
                   "telemetry.cpu_normalization");
    ExpectRejected(__func__, with("adaptive", {{"enabled", "yes"}}), "adaptive.enabled");
    ExpectRejected(__func__, with("adaptive", {{"window_samples", 1}}), "adaptive.window_samples");
    ExpectRejected(__func__, with("alerts", {{"webhook_url", "ftp://x"}}), "alerts.webhook_url");
    ExpectRejected(__func__, with("coordination", {{"controller_port", 70000}}),
                   "coordination.controller_port");
    ExpectRejected(__func__, with("triggers", {{"io", {{"critical_io_limit_mb_per_sec", 0}}}}),
                   "triggers.io.critical_io_limit_mb_per_sec");
    ExpectRejected(__func__, with("security", 5), "security");
    ExpectRejected(__func__, with("coordination", {{"enabled", true}, {"shared_token", "short"}}),
                   "coordination.shared_token");
}

void LogsGenericEventsAndTrigger(const fs::path& dir) {
    const fs::path path = dir / "events.jsonl";
    {
        logging::JsonLinesLogger log(path.wstring(), 1024 * 1024, 2);
        log.LogEvent("self_monitor", nlohmann::ordered_json{{"cpu_percent", 0.4}, {"log_type", "spoof"}});
        log.LogActuatorAction(5190, L"invoice_batch.exe", 4821, actuator::ProcessState::Throttled,
                              actuator::ProcessState::Suspended, 93.0, true, "manual");
    }
    std::ifstream in(path);
    std::string l1, l2;
    std::getline(in, l1);
    std::getline(in, l2);
    const auto j1 = nlohmann::json::parse(l1);
    const auto j2 = nlohmann::json::parse(l2);
    Check(j1["log_type"] == "self_monitor" && j1.contains("timestamp") && j1["cpu_percent"] == 0.4,
          __func__, "generic event keeps log_type and timestamp");
    Check(l1.rfind("{\"log_type\"", 0) == 0, __func__, "log_type is the first key");
    Check(j2["trigger"] == "manual" && j2["action"] == "threads_suspended", __func__,
          "actuator action carries trigger");
}

void ParsesAutoProtectSection() {
    Check(config::ParseConfig(kValidConfig).autoEnabled, __func__, "on when the section is absent");
    nlohmann::json cfg = nlohmann::json::parse(kValidConfig);
    cfg["auto_protect"] = {{"enabled", true}, {"throttle_at_percent", 85}, {"max_processes", 5},
                           {"recover_below_percent", 70}, {"idle_at_percent", 95}};
    const auto c = config::ParseConfig(cfg.dump());
    Check(c.autoThrottlePercent == 85 && c.autoMaxProcesses == 5 && c.autoRecoverPercent == 70,
          __func__, "values");
    cfg["auto_protect"] = {{"recover_below_percent", 95}};
    ExpectRejected(__func__, cfg, "auto_protect.recover_below_percent");
    cfg["auto_protect"] = {{"throttle_at_percent", 90}, {"idle_at_percent", 80}};
    ExpectRejected(__func__, cfg, "auto_protect.idle_at_percent");
    cfg["auto_protect"] = {{"max_processes", 0}};
    ExpectRejected(__func__, cfg, "auto_protect.max_processes");
}

void ParsesDashboardSection() {
    Check(config::ParseConfig(kValidConfig).dashboardEnabled &&
              config::ParseConfig(kValidConfig).dashboardPort == 8787,
          __func__, "dashboard defaults");
    nlohmann::json cfg = nlohmann::json::parse(kValidConfig);
    cfg["dashboard"] = {{"enabled", false}, {"port", 9000}};
    const auto c = config::ParseConfig(cfg.dump());
    Check(!c.dashboardEnabled && c.dashboardPort == 9000, __func__, "dashboard values");
    cfg["dashboard"] = {{"port", 80}};
    ExpectRejected(__func__, cfg, "dashboard.port");
}

void SetsProcessRoles() {
    const std::string base = kValidConfig;
    std::string out = config::SetProcessRole(base, "chrome.exe", "critical");
    auto c = config::ParseConfig(out);
    Check(c.criticalProcesses.size() == 2 && c.criticalProcesses[1] == L"chrome.exe", __func__,
          "added to critical");

    out = config::SetProcessRole(out, "CHROME.EXE", "batch");
    c = config::ParseConfig(out);
    Check(c.criticalProcesses.size() == 1 && c.batchProcesses.size() == 3, __func__,
          "moved from critical to batch, matched case-insensitively");

    out = config::SetProcessRole(out, "chrome.exe", "none");
    c = config::ParseConfig(out);
    Check(c.batchProcesses.size() == 2, __func__, "role none removes it");

    out = config::SetProcessRole(out, "dispatch_api.exe", "none");
    c = config::ParseConfig(out);
    Check(c.criticalProcesses.empty(), __func__, "last critical process can be removed");

    out = config::SetProcessRole(out, "notepad.exe", "protected");
    c = config::ParseConfig(out);
    Check(c.protectedProcesses.size() == 2, __func__, "added to protected");

    // The dashboard's rewrite keeps the editor schema pointer, and bad
    // numbers are a ConfigError rather than an escaping JSON exception.
    const std::string withSchema =
        "{\"$schema\":\"https://example.invalid/s.json\"," + base.substr(base.find('{') + 1);
    const std::string kept = config::SetProcessRole(withSchema, "a.exe", "batch");
    Check(kept.find("\"$schema\"") != std::string::npos && kept.find("\"$schema\"") < kept.find("\"polling\""),
          __func__, "$schema kept first");
    bool configError = false;
    try {
        config::SetProcessRole("{\"processes\":{\"critical\":[],\"batch\":[]},\"x\":1e309}", "a.exe", "batch");
    } catch (const config::ConfigError&) {
        configError = true;
    } catch (...) {
    }
    Check(configError, __func__, "out-of-range number reported as ConfigError");

    Check(out.find("\"polling\"") < out.find("\"thresholds\"") &&
              out.find("\"thresholds\"") < out.find("\"processes\""),
          __func__, "key order preserved");

    bool threw = false;
    try { config::SetProcessRole(base, "x.exe", "boss"); } catch (const config::ConfigError&) { threw = true; }
    Check(threw, __func__, "unknown role rejected");
    threw = false;
    try { config::SetProcessRole(base, "C:\\x.exe", "batch"); } catch (const config::ConfigError&) { threw = true; }
    Check(threw, __func__, "path rejected");
}

std::vector<std::string> RawLines(const fs::path& file) {
    std::vector<std::string> out;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) out.push_back(line);
    return out;
}

logging::ChainReport VerifyText(const std::vector<std::string>& lines,
                                const std::string& expectedFirst = "") {
    std::string text;
    for (const auto& l : lines) text += l + "\n";
    std::istringstream in(text);
    return logging::VerifyChain(in, expectedFirst);
}

void HashChainDetectsTampering(const fs::path& dir) {
    const char* t = "HashChainDetectsTampering";
    const fs::path file = dir / "chain.jsonl";
    {
        // Lines from before chaining existed, then a chained logger appends.
        std::ofstream old(file, std::ios::binary);
        old << "{\"log_type\":\"lifecycle\",\"n\":1}\n{\"log_type\":\"lifecycle\",\"n\":2}\n";
    }
    for (int run = 0; run < 2; ++run) {  // two loggers: the chain continues across restarts
        logging::JsonLinesLogger log(file.wstring(), 1024 * 1024, 3);
        for (int i = 0; i < 3; ++i) log.LogSample(100 + i, 1.0);
    }
    auto lines = RawLines(file);
    Check(lines.size() == 8, t, "8 lines written");
    if (lines.size() != 8) return;
    auto r = VerifyText(lines);
    Check(r.Ok() && r.chained == 6 && r.unchained == 2, t, "intact chain verifies: " + r.message);

    auto edited = lines;
    const size_t pos = edited[4].find("\"process_id\":102");
    Check(pos != std::string::npos, t, "found line to edit");
    if (pos != std::string::npos) edited[4].replace(pos, 16, "\"process_id\":999");
    r = VerifyText(edited);
    Check(!r.Ok() && r.brokenLine == 6, t, "edited line breaks the next link");

    auto removed = lines;
    removed.erase(removed.begin() + 5);
    Check(VerifyText(removed).brokenLine == 6, t, "removed line breaks the chain");

    auto stripped = lines;
    stripped[6] = "{\"log_type\":\"sample\"}";
    Check(VerifyText(stripped).brokenLine == 7, t, "line without prev_hash after the chain started");

    Check(VerifyText({"not json"}).brokenLine == 1, t, "garbage line rejected");
}

void HashChainSpansRotation(const fs::path& dir) {
    const char* t = "HashChainSpansRotation";
    const fs::path file = dir / "chainrot.jsonl";
    {
        logging::JsonLinesLogger log(file.wstring(), 400, 3);
        for (int i = 0; i < 12; ++i) log.LogSample(2000 + i, 5.0);
    }
    const std::string older = file.string() + ".1";
    Check(fs::exists(older), t, "rotated file exists");
    const std::string link = logging::LastLineHash(fs::path(older).wstring());
    Check(VerifyText(RawLines(file), link).Ok(), t, "current file links to the end of .1");
    Check(!VerifyText(RawLines(file), logging::kGenesisHash).Ok(), t,
          "wrong predecessor is detected");
}

void ProtectedSecretsDecrypt() {
    const char* t = "ProtectedSecretsDecrypt";
    const std::string token = "0123456789abcdef-secret";
    const std::string enc = config::ProtectSecret(token);
    Check(config::IsProtectedSecret(enc) && enc.find(token) == std::string::npos, t,
          "encrypted value has the prefix and hides the plaintext");
    std::string plain, err;
    Check(config::UnprotectSecret(enc, plain, err) && plain == token, t, "round trip");
    Check(config::UnprotectSecret("plain-value", plain, err) && plain == "plain-value", t,
          "plain values pass through");
    Check(!config::UnprotectSecret("dpapi:!!!", plain, err), t, "bad base64 rejected");
    Check(!config::UnprotectSecret("dpapi:QUJDREVGR0g=", plain, err), t, "non-DPAPI blob rejected");

    nlohmann::json cfg = nlohmann::json::parse(kValidConfig);
    cfg["coordination"] = {{"enabled", true}, {"controller_host", "ctl"}, {"shared_token", enc}};
    cfg["alerts"] = {{"webhook_url", config::ProtectSecret("https://example.invalid/hook?k=1")}};
    const auto c = config::ParseConfig(cfg.dump());
    Check(c.sharedToken == token, t, "config decrypts shared_token");
    Check(c.alertWebhookUrl == L"https://example.invalid/hook?k=1", t, "config decrypts webhook_url");

    cfg["coordination"]["shared_token"] = "dpapi:QUJDREVGR0g=";
    ExpectRejected(t, cfg, "coordination.shared_token");
}

void ParsesIdentitySection() {
    nlohmann::json cfg = nlohmann::json::parse(kValidConfig);
    cfg["identity"] = {{"dispatch_api.exe", {{"path", "C:\\Apps\\dispatch_api.exe"}, {"publisher", "Contoso Ltd"}}},
                       {"invoice_batch.exe", {{"publisher", "Contoso Ltd"}}}};
    const auto c = config::ParseConfig(cfg.dump());
    Check(c.identities.size() == 2 && c.identities[0].name == L"dispatch_api.exe" &&
              c.identities[0].path == L"C:\\Apps\\dispatch_api.exe",
          __func__, "identity entries");
    auto with = [&](nlohmann::json id) { nlohmann::json x = nlohmann::json::parse(kValidConfig); x["identity"] = id; return x; };
    ExpectRejected(__func__, with({{"notanexe", {{"publisher", "X"}}}}), "identity.notanexe");
    ExpectRejected(__func__, with({{"a.exe", nlohmann::json::object()}}), "identity.a.exe");
    ExpectRejected(__func__, with({{"a.exe", {{"path", "relative\\a.exe"}}}}), "identity.a.exe.path");
}

void ParsesMetricsSection() {
    Check(!config::ParseConfig(kValidConfig).metricsEnabled, __func__, "metrics off by default");
    nlohmann::json cfg = nlohmann::json::parse(kValidConfig);
    cfg["metrics"] = {{"enabled", true}, {"token", "scrape-token-0123456789"}};
    const auto c = config::ParseConfig(cfg.dump());
    Check(c.metricsEnabled && c.metricsToken == "scrape-token-0123456789", __func__, "metrics values");
    cfg["metrics"] = {{"enabled", true}, {"token", "short"}};
    ExpectRejected(__func__, cfg, "metrics.token");
    cfg["metrics"] = {{"enabled", true}, {"token", "has a space in it 0123"}};
    ExpectRejected(__func__, cfg, "metrics.token");
}

void SchedulesSelectByTime() {
    nlohmann::json cfg = nlohmann::json::parse(kValidConfig);
    const nlohmann::json weekdays = {"mon", "tue", "wed", "thu", "fri"};
    cfg["groups"] = nlohmann::json::array({{{"name", "reports"}, {"critical", {"sqlservr.exe"}}, {"batch", {"etl.exe"}}}});
    cfg["schedules"] = nlohmann::json::array({
        {{"name", "business"}, {"days", weekdays}, {"from", "08:00"}, {"to", "18:00"},
         {"thresholds", {{"high", {{"percent", 70}, {"consecutive_samples", 3}}}}}},
        {{"name", "nightly"}, {"days", weekdays}, {"from", "22:00"}, {"to", "06:00"}, {"mode", "off"}},
        {{"name", "lunch"}, {"days", weekdays}, {"from", "12:00"}, {"to", "13:00"}, {"mode", "off"}},
        {{"name", "reports_weekend"}, {"group", "reports"}, {"days", {"sat", "sun"}}, {"from", "00:00"}, {"to", "24:00"},
         {"actions", {{"suspend", {{"mode", "cpu_rate"}, {"cpu_rate_percent", 5}}}}}}});
    const auto c = config::ParseConfig(cfg.dump());
    Check(c.schedules.size() == 4 && c.schedules[3].group == 1, __func__, "schedules parsed, group resolved");
    Check(c.schedules[0].high.percent == 70 && c.schedules[0].critical_.percent == c.critical.percent,
          __func__, "override applied on top of the group's policy");
    Check(c.schedules[3].actions.suspendMode == "cpu_rate" && c.schedules[3].high.percent == c.high.percent,
          __func__, "group schedule inherits the group's thresholds");
    auto at = [&](int group, int day, const char* hhmm) {
        const int m = ((hhmm[0] - '0') * 10 + (hhmm[1] - '0')) * 60 + (hhmm[3] - '0') * 10 + (hhmm[4] - '0');
        const int i = config::ActiveSchedule(c, group, day, m);
        return i < 0 ? std::string("none") : c.schedules[i].name;
    };
    // Weekdays: 0 = Sunday ... 6 = Saturday.
    Check(at(0, 1, "09:30") == "business", __func__, "Monday morning: business hours");
    Check(at(0, 1, "17:59") == "business" && at(0, 1, "18:00") == "none", __func__, "window end is exclusive");
    Check(at(0, 3, "12:30") == "business", __func__, "first matching schedule wins (business before lunch)");
    Check(at(0, 1, "23:15") == "nightly" && at(0, 2, "05:59") == "nightly", __func__, "overnight window spans midnight");
    Check(at(0, 6, "05:00") == "nightly", __func__, "Friday night's window runs into Saturday morning");
    Check(at(0, 1, "05:00") == "none", __func__, "Monday early morning: Sunday is not a listed day");
    Check(at(0, 6, "10:00") == "none", __func__, "weekend daytime: no default-group schedule");
    Check(at(1, 0, "00:00") == "reports_weekend" && at(1, 6, "23:59") == "reports_weekend", __func__,
          "00:00-24:00 covers the whole day");
    Check(at(1, 1, "09:30") == "none", __func__, "schedules only apply to their own group");
    Check(c.schedules[1].off, __func__, "mode off parsed");

    auto with = [&](nlohmann::json s) {
        nlohmann::json x = cfg;
        x["schedules"] = nlohmann::json::array({s});
        return x;
    };
    const nlohmann::json base = {{"name", "s"}, {"days", {"mon"}}, {"from", "08:00"}, {"to", "09:00"}, {"mode", "off"}};
    auto edit = [&](const char* key, nlohmann::json v) { nlohmann::json s = base; s[key] = v; return with(s); };
    ExpectRejected(__func__, edit("from", "8:00"), "schedules[0].from must be a time HH:MM");
    ExpectRejected(__func__, edit("from", "24:00"), "schedules[0].from must be a time");
    ExpectRejected(__func__, edit("to", "25:00"), "schedules[0].to must be a time");
    ExpectRejected(__func__, edit("to", "08:00"), "from and to are the same");
    ExpectRejected(__func__, edit("days", {"monday"}), "schedules[0].days");
    ExpectRejected(__func__, edit("days", nlohmann::json::array()), "schedules[0].days");
    ExpectRejected(__func__, edit("group", "nope"), "is not a defined group");
    ExpectRejected(__func__, edit("mode", "pause"), "schedules[0].mode");
    nlohmann::json noop = base;
    noop.erase("mode");
    ExpectRejected(__func__, with(noop), "changes nothing");
    nlohmann::json offWithThresholds = base;
    offWithThresholds["thresholds"] = {{"high", {{"percent", 70}, {"consecutive_samples", 3}}}};
    ExpectRejected(__func__, with(offWithThresholds), "no effect with mode \"off\"");
    nlohmann::json badLevels = base;
    badLevels.erase("mode");
    badLevels["thresholds"] = {{"low", {{"percent", 95}, {"consecutive_samples", 3}}}};
    ExpectRejected(__func__, with(badLevels), "low.percent must be below high.percent");
}

void ParsesGroups() {
    Check(config::ParseConfig(kValidConfig).groups.empty(), __func__, "no groups by default");
    nlohmann::json cfg = nlohmann::json::parse(kValidConfig);
    cfg["actions"] = {{"throttle", {{"priority", "below_normal"}}}};
    cfg["groups"] = nlohmann::json::array({
        {{"name", "reports"}, {"critical", {"sqlservr.exe"}}, {"batch", {"report_builder.exe", "etl.exe"}},
         {"thresholds", {{"high", {{"percent", 95}, {"consecutive_samples", 5}}},
                         {"critical", {{"percent", 97}, {"consecutive_samples", 5}}}}},
         {"actions", {{"suspend", {{"mode", "cpu_rate"}, {"cpu_rate_percent", 2}}}}}},
        {{"name", "web"}, {"critical", {{{"name", "java.exe"}, {"command_line_contains", "api.jar"}}}},
         {"batch", {{{"name", "java.exe"}, {"command_line_contains", "crawl.jar"}}}}}});
    const auto c = config::ParseConfig(cfg.dump());
    Check(c.groups.size() == 2 && c.groups[0].name == "reports", __func__, "two groups");
    const auto& r = c.groups[0];
    Check(r.high.percent == 95 && r.high.consecutiveSamples == 5 && r.critical_.percent == 97 &&
              r.low.percent == c.low.percent,
          __func__, "partial threshold override inherits the rest");
    nlohmann::json bad = cfg;
    bad["groups"][0]["thresholds"].erase("critical");
    ExpectRejected(__func__, bad, "critical.percent must be at least high.percent");
    Check(r.actions.throttlePriority == "below_normal" && r.actions.suspendMode == "cpu_rate", __func__,
          "actions override layers on top-level actions");
    Check(c.groups[1].actions.suspendMode == "freeze", __func__, "group without actions inherits top-level");
    Check(c.criticalGroupOf.at(L"sqlservr.exe") == 1 && c.batchGroupOf.at(L"etl.exe") == 1 &&
              c.criticalGroupOf.at(L"java.exe") == 2 && c.batchGroupOf.at(L"java.exe") == 2,
          __func__, "names map to their group");
    Check(!c.criticalGroupOf.count(L"dispatch_api.exe"), __func__, "top-level names are group 0");
    bool merged = false;
    for (const auto& n : c.batchProcesses) merged = merged || n == L"report_builder.exe";
    Check(merged && c.batchRules.count(L"java.exe"), __func__, "group names and rules merged into the role lists");

    auto with = [&](nlohmann::json groups) {
        nlohmann::json x = nlohmann::json::parse(kValidConfig);
        x["groups"] = groups;
        return x;
    };
    const nlohmann::json ok = {{"name", "g"}, {"critical", {"x.exe"}}, {"batch", {"y.exe"}}};
    ExpectRejected(__func__, with({{{"name", "default"}, {"critical", {"x.exe"}}, {"batch", {"y.exe"}}}}), "not \"default\"");
    ExpectRejected(__func__, with({ok, ok}), "used twice");
    ExpectRejected(__func__, with({{{"name", "g"}, {"critical", nlohmann::json::array()}, {"batch", {"y.exe"}}}}),
                   "at least one critical and one batch");
    ExpectRejected(__func__, with({{{"name", "g"}, {"critical", {"dispatch_api.exe"}}, {"batch", {"y.exe"}}}}),
                   "already used by group \"default\"");
    ExpectRejected(__func__, with({ok, {{"name", "h"}, {"critical", {"z.exe"}}, {"batch", {"Y.EXE"}}}}),
                   "already used by group \"g\"");
    ExpectRejected(__func__, with({{{"name", "g"}, {"critical", {"x.exe"}}, {"batch", {"y.exe"}},
                                    {"thresholds", {{"low", {{"percent", 99}, {"consecutive_samples", 1}}}}}}}),
                   "low.percent must be below high.percent");
    ExpectRejected(__func__, with({{{"name", "g"}, {"critical", {"x.exe"}}, {"batch", {"y.exe"}},
                                    {"actions", {{"suspend", {{"mode", "kill"}}}}}}}),
                   "groups[0].actions.suspend.mode");
}

void ParsesMatchRules() {
    nlohmann::json cfg = nlohmann::json::parse(kValidConfig);
    cfg["processes"]["critical"] = {"dispatch_api.exe",
                                    {{"name", "java.exe"}, {"command_line_contains", "api-server.jar"}}};
    cfg["processes"]["batch"] = {"invoice_batch.exe",
                                 {{"name", "java.exe"}, {"command_line_contains", "nightly-export.jar"}},
                                 {{"name", "JAVA.exe"}, {"user", "svc_reports"}},
                                 {{"name", "svchost.exe"}, {"service", "wuauserv"}}};
    const auto c = config::ParseConfig(cfg.dump());
    Check(c.criticalProcesses.size() == 2 && c.batchProcesses.size() == 3, __func__,
          "names merged case-insensitively (java.exe listed twice in batch)");
    Check(c.criticalRules.count(L"java.exe") && c.criticalRules.at(L"java.exe").size() == 1 &&
              c.criticalRules.at(L"java.exe")[0].commandLineContains == L"api-server.jar",
          __func__, "critical rule");
    Check(c.batchRules.count(L"java.exe") && c.batchRules.at(L"java.exe").size() == 2 &&
              c.batchRules.at(L"java.exe")[1].user == L"svc_reports",
          __func__, "two alternative batch rules for java.exe");
    Check(c.batchRules.at(L"svchost.exe")[0].service == L"wuauserv", __func__, "service rule");
    Check(!c.criticalRules.count(L"dispatch_api.exe"), __func__, "plain names have no rules");

    nlohmann::json plain = cfg;
    plain["processes"]["batch"].push_back("svchost.exe");
    Check(!config::ParseConfig(plain.dump()).batchRules.count(L"svchost.exe"), __func__,
          "a plain name overrides its rules: every instance");

    nlohmann::json both = nlohmann::json::parse(kValidConfig);
    both["processes"]["critical"] = {"java.exe"};
    both["processes"]["batch"] = {{{"name", "java.exe"}, {"command_line_contains", "x"}}};
    ExpectRejected(__func__, both, "only allowed when both entries have match conditions");

    auto withBatch = [&](nlohmann::json entry) {
        nlohmann::json x = nlohmann::json::parse(kValidConfig);
        x["processes"]["batch"] = nlohmann::json::array({entry});
        return x;
    };
    ExpectRejected(__func__, withBatch({{"name", "a.exe"}}), "needs command_line_contains, user or service");
    ExpectRejected(__func__, withBatch({{"command_line_contains", "x"}}), "processes.batch[0].name is required");
    ExpectRejected(__func__, withBatch({{"name", "a.exe"}, {"cmdline", "x"}}), "unknown key");
    ExpectRejected(__func__, withBatch({{"name", "C:\\a.exe"}, {"user", "x"}}), "not a path");
    ExpectRejected(__func__, withBatch({{"name", "a.exe"}, {"user", ""}}), "must be a non-empty string");
    nlohmann::json prot = nlohmann::json::parse(kValidConfig);
    prot["processes"]["protected"] = {{{"name", "a.exe"}, {"user", "x"}}};
    ExpectRejected(__func__, prot, "processes.protected[0] must be a string");
}

void ParsesActionsSection() {
    const auto d = config::ParseConfig(kValidConfig).actions;
    Check(d.throttlePriority == "idle" && d.throttleIoPriority == "unchanged" && d.throttleAffinityCores == 0 &&
              d.throttleCpuRatePercent == 0 && d.suspendMode == "freeze",
          __func__, "defaults reproduce v2.0 behaviour");
    nlohmann::json cfg = nlohmann::json::parse(kValidConfig);
    cfg["actions"] = {{"throttle", {{"priority", "below_normal"}, {"io_priority", "very_low"},
                                    {"affinity_cores", 2}, {"cpu_rate_percent", 25}}},
                      {"suspend", {{"mode", "cpu_rate"}, {"cpu_rate_percent", 3}}}};
    const auto a = config::ParseConfig(cfg.dump()).actions;
    Check(a.throttlePriority == "below_normal" && a.throttleIoPriority == "very_low" && a.throttleAffinityCores == 2 &&
              a.throttleCpuRatePercent == 25 && a.suspendMode == "cpu_rate" && a.suspendCpuRatePercent == 3,
          __func__, "all action fields");
    auto with = [&](nlohmann::json act) {
        nlohmann::json x = nlohmann::json::parse(kValidConfig);
        x["actions"] = act;
        return x;
    };
    ExpectRejected(__func__, with({{"throttle", {{"priority", "realtime"}}}}), "actions.throttle.priority");
    ExpectRejected(__func__, with({{"throttle", {{"io_priority", "high"}}}}), "actions.throttle.io_priority");
    ExpectRejected(__func__, with({{"throttle", {{"cpu_rate_percent", 150}}}}), "actions.throttle.cpu_rate_percent");
    ExpectRejected(__func__, with({{"suspend", {{"mode", "kill"}}}}), "actions.suspend.mode");
    ExpectRejected(__func__,
                   with({{"throttle", {{"cpu_rate_percent", 5}}}, {"suspend", {{"mode", "cpu_rate"}, {"cpu_rate_percent", 10}}}}),
                   "must be below actions.throttle.cpu_rate_percent");
}

void ParsesSignalsSection() {
    Check(config::ParseConfig(kValidConfig).signals.empty(), __func__, "no signals by default");
    nlohmann::json cfg = nlohmann::json::parse(kValidConfig);
    cfg["signals"] = nlohmann::json::array({
        {{"type", "gpu"}, {"limit", 90}},
        {{"type", "disk_queue"}, {"limit", 4}, {"name", "disk"}},
        {{"type", "counter"}, {"name", "sql_waits"}, {"counter", "\\SQLServer:Wait Statistics(*)\\Waits in progress"},
         {"aggregate", "max"}, {"instance_contains", "Lock"}, {"scale", 0.5}, {"limit", 100}}});
    const auto c = config::ParseConfig(cfg.dump());
    Check(c.signals.size() == 3 && c.signals[0].name == "gpu" && c.signals[0].limit == 90, __func__,
          "built-in name defaults to its type");
    Check(c.signals.size() == 3 && c.signals[1].name == "disk" && c.signals[2].aggregate == "max" &&
              c.signals[2].scale == 0.5 && c.signals[2].instanceContains == L"Lock",
          __func__, "custom counter fields");
    auto with = [&](nlohmann::json s) {
        nlohmann::json x = nlohmann::json::parse(kValidConfig);
        x["signals"] = nlohmann::json::array({s});
        return x;
    };
    ExpectRejected(__func__, with({{"type", "teleport"}, {"limit", 1}}), "signals[0].type");
    ExpectRejected(__func__, with({{"type", "gpu"}}), "signals[0].limit");
    ExpectRejected(__func__, with({{"type", "gpu"}, {"limit", 0}}), "signals[0].limit");
    ExpectRejected(__func__, with({{"type", "counter"}, {"limit", 1}}), "signals[0].name");
    ExpectRejected(__func__, with({{"type", "counter"}, {"name", "x"}, {"counter", "no-slash"}, {"limit", 1}}),
                   "signals[0].counter");
    ExpectRejected(__func__, with({{"type", "gpu"}, {"name", "Bad Name"}, {"limit", 1}}), "signals[0].name");
    ExpectRejected(__func__, with({{"type", "gpu"}, {"limit", 1}, {"scale", 2}}), "signals[0]");
    nlohmann::json dup = nlohmann::json::parse(kValidConfig);
    dup["signals"] = nlohmann::json::array({{{"type", "gpu"}, {"limit", 1}}, {{"type", "gpu"}, {"limit", 2}}});
    ExpectRejected(__func__, dup, "used twice");
}

void PrometheusTextFormat() {
    const char* t = "PrometheusTextFormat";
    const nlohmann::json status = nlohmann::json::parse(R"({
        "ok": true, "pressure_percent": 92.5, "offset_percent": 5,
        "thresholds": {"high": 85, "critical": 90, "low": 60},
        "coordination_connected": false,
        "critical": [{"name": "dispatch_api.exe", "pid": 4821, "cpu_percent": 92.5}],
        "batch": [{"name": "inv\"oice.exe", "pid": 5190, "state": "throttled", "manual_hold": false}],
        "service": {"uptime_seconds": 120, "working_set_mb": 2},
        "system": {"cpu_percent": 71.3, "memory_load_percent": 40, "memory_used_gb": 1, "memory_total_gb": 16, "logical_cores": 8},
        "auto_protect": {"enabled": true, "level": "throttling", "processes": [{"pid": 1}]},
        "groups": [{"name": "default", "active": true, "pressure_percent": 92.5, "thresholds": {"high": 85, "critical": 90, "low": 60}},
                   {"name": "reports", "active": false, "pressure_percent": 0, "thresholds": {"high": 95, "critical": 97, "low": 60}}],
        "signals": [{"name": "gpu", "ok": true, "value": 45, "pressure_percent": 50},
                    {"name": "broken", "ok": false, "error": "counter not found"}]
    })");
    daemon::MetricCounters counters;
    counters.actions[{"priority_lowered", "automatic"}] = 3;
    counters.logEvents["sample"] = 42;
    const std::string text = daemon::PrometheusText(status, counters);
    auto has = [&](const std::string& s) { return text.find(s) != std::string::npos; };
    Check(has("# TYPE threadhaul_up gauge\nthreadhaul_up 1\n"), t, "up gauge");
    Check(has("threadhaul_pressure_percent 92.5\n"), t, "pressure");
    Check(has("threadhaul_threshold_percent{level=\"critical\"} 90\n"), t, "threshold label");
    Check(has("threadhaul_critical_process_cpu_percent{name=\"dispatch_api.exe\",pid=\"4821\"} 92.5\n"), t,
          "critical cpu with labels");
    Check(has("threadhaul_batch_processes{state=\"throttled\"} 1\n") &&
              has("threadhaul_batch_processes{state=\"suspended\"} 0\n"),
          t, "batch counts include zero states");
    Check(has("name=\"inv\\\"oice.exe\""), t, "quote in label escaped");
    Check(has("threadhaul_system_memory_total_bytes 17179869184\n"), t, "GiB converted to bytes");
    Check(has("threadhaul_service_working_set_bytes 2097152\n"), t, "MB converted to bytes");
    Check(has("threadhaul_auto_protect_level{level=\"throttling\"} 1\n") &&
              has("threadhaul_auto_protect_level{level=\"idle\"} 0\n"),
          t, "auto-protect level state set");
    Check(has("# TYPE threadhaul_actions_total counter\n") &&
              has("threadhaul_actions_total{action=\"priority_lowered\",trigger=\"automatic\"} 3\n"),
          t, "action counter");
    Check(has("threadhaul_log_events_total{log_type=\"sample\"} 42\n"), t, "log event counter");
    Check(has("threadhaul_signal_value{signal=\"gpu\"} 45\n") &&
              has("threadhaul_signal_pressure_percent{signal=\"gpu\"} 50\n") &&
              has("threadhaul_signal_up{signal=\"broken\"} 0\n") &&
              !has("threadhaul_signal_value{signal=\"broken\"}"),
          t, "signal metrics; broken signal only reports up=0");
    Check(has("threadhaul_group_pressure_percent{group=\"default\"} 92.5\n") &&
              has("threadhaul_group_threshold_percent{group=\"reports\",level=\"high\"} 95\n") &&
              has("threadhaul_group_active{group=\"reports\"} 0\n"),
          t, "group metrics");
    Check(text.back() == '\n' && !has("\r"), t, "LF line endings, ends with newline");
    Check(daemon::PrometheusLabel("a\\b\nc") == "a\\\\b\\nc", t, "backslash and newline escaped");

    const std::string minimal = daemon::PrometheusText(nlohmann::json::object(), {});
    Check(minimal.find("threadhaul_up 1") != std::string::npos &&
              minimal.find("threadhaul_pressure_percent") == std::string::npos,
          t, "missing fields are skipped, not invented");
}

}

int main() {
    ParsesValidConfig();
    ProtectedListIsOptional();
    RejectsMalformedJson();
    RejectsMissingFields();
    RejectsOutOfRangeValues();
    RejectsInconsistentThresholds();
    RejectsBadProcessLists();
    RejectsMissingFile();
    ParsesDashboardSection();
    ParsesAutoProtectSection();
    SetsProcessRoles();

    const fs::path dir =
        fs::temp_directory_path() / ("threadhaul_tests_" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(dir);
    LogsAllThreeKindsWithSchemaFields(dir);
    AppendsToExistingFile(dir);
    RotatesBySizeAndCapsFileCount(dir);
    CreatesMissingLogDirectory(dir);
    StateAndActionNames();
    OptionalSectionsDefault();
    ParsesOptionalSections();
    RejectsBadOptionalSections();
    LogsGenericEventsAndTrigger(dir);
    HashChainDetectsTampering(dir);
    HashChainSpansRotation(dir);
    ProtectedSecretsDecrypt();
    ParsesIdentitySection();
    ParsesMetricsSection();
    ParsesSignalsSection();
    ParsesActionsSection();
    ParsesMatchRules();
    ParsesGroups();
    SchedulesSelectByTime();
    PrometheusTextFormat();
    std::error_code ec;
    fs::remove_all(dir, ec);

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
