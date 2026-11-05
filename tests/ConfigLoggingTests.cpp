// Unit tests for config loading/validation and the JSON Lines logger.

#include "Config.h"
#include "JsonLinesLogger.h"
#include "LogChain.h"
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
    std::error_code ec;
    fs::remove_all(dir, ec);

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
