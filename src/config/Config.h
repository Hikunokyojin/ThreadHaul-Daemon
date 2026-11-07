#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace config {

struct ThresholdSpec {
    double percent = 0.0;
    int    consecutiveSamples = 0;
};

// Runtime configuration loaded from config.json. Every field and its valid
// range is documented in README.md (Configuration) and enforced by ParseConfig.
struct DaemonConfig {
    int pollIntervalMs = 1000;

    ThresholdSpec high{85.0, 3};
    ThresholdSpec critical{90.0, 5};
    ThresholdSpec low{60.0, 5};

    std::vector<std::wstring> criticalProcesses;
    std::vector<std::wstring> batchProcesses;
    std::vector<std::wstring> protectedProcesses;

    // Optional per-instance conditions for critical/batch names, from object
    // entries such as {"name":"java.exe","command_line_contains":"batch.jar"}.
    // Key: lower-case exe name. A name with no entry here matches every
    // instance. Conditions within a rule must all hold; rules are alternatives.
    struct MatchRule {
        std::wstring commandLineContains;
        std::wstring user;      // "DOMAIN\\user" or "user"
        std::wstring service;   // service name, e.g. "wuauserv"
    };
    std::map<std::wstring, std::vector<MatchRule>> criticalRules;
    std::map<std::wstring, std::vector<MatchRule>> batchRules;

    // "identity": {"app.exe": {"path": "C:\\...\\app.exe", "publisher": "Contoso Ltd"}}
    // A process with a pinned name gets its role only if it matches.
    struct PinnedIdentity {
        std::wstring name;
        std::wstring path;
        std::wstring publisher;
    };
    std::vector<PinnedIdentity> identities;

    std::wstring logPath;
    int maxFileSizeMb = 50;
    int maxRotatedFiles = 5;

    // ---- Optional sections (defaults apply when absent) ----

    // telemetry.cpu_normalization: "per_core" (100% = one logical core) or
    // "total" (100% = all logical cores).
    bool cpuPerCore = true;
    bool logSamples = true;              // telemetry.log_samples
    int  selfMonitorIntervalSec = 30;    // telemetry.self_monitor_interval_seconds (0 = off)

    // triggers.*: non-CPU signals expressed as a percentage of a limit, so
    // they share the CPU thresholds (85% of a limit throttles, and so on).
    bool   memoryTriggerEnabled = false;
    double systemMemoryLimitPercent = 95.0;  // triggers.memory.system_load_limit_percent
    double criticalWorkingSetLimitMb = 0.0;  // triggers.memory.critical_working_set_limit_mb (0 = off)
    bool   ioTriggerEnabled = false;
    double criticalIoLimitMbPerSec = 200.0;  // triggers.io.critical_io_limit_mb_per_sec

    // adaptive.*
    bool   adaptiveEnabled = false;
    int    adaptiveWindowSamples = 10;
    int    adaptiveHorizonSamples = 3;
    double adaptiveMaxAdjustPercent = 10.0;

    // alerts.*
    bool         alertPopup = true;          // alerts.popup (message box in the active session)
    std::wstring alertWebhookUrl;            // alerts.webhook_url (HTTP POST of a JSON body)

    // coordination.*
    bool         coordinationEnabled = false;
    std::wstring controllerHost = L"127.0.0.1";
    int          controllerPort = 7878;
    std::string  sharedToken;
    int          heartbeatSeconds = 5;

    // security.require_protected_config: refuse to load a config file that
    // non-administrators can modify.
    bool requireProtectedConfig = false;

    // auto_protect.*: system-wide protection. When total CPU is overloaded,
    // the heaviest ordinary applications are lowered in priority and later
    // restored. On unless the section sets "enabled": false.
    bool   autoEnabled = true;
    double autoThrottlePercent = 90.0;
    int    autoThrottleSamples = 5;
    bool   autoUseIdle = true;
    double autoIdlePercent = 97.0;
    int    autoIdleSamples = 10;
    double autoRecoverPercent = 75.0;
    int    autoRecoverSamples = 10;
    int    autoMaxProcesses = 3;
    double autoMinProcessCpuPercent = 50.0;

    // dashboard.*: local web dashboard served on 127.0.0.1 only.
    bool dashboardEnabled = true;
    int  dashboardPort = 8787;

    // "signals": extra pressure sources from performance counters.
    struct SignalConfig {
        std::string name;                // unique, [a-z0-9_]
        std::string type;                // gpu | disk_queue | network | counter
        std::wstring counter;            // type counter only: English PDH path
        std::string aggregate = "sum";   // sum | max over wildcard instances
        std::wstring instanceContains;   // optional instance-name filter
        double scale = 1.0;              // type counter only
        double limit = 0.0;              // value that counts as 100% pressure
    };
    std::vector<SignalConfig> signals;

    // "actions": what throttle and suspend do to a batch process.
    struct ActionsConfig {
        std::string throttlePriority = "idle";   // idle | below_normal
        std::string throttleIoPriority = "unchanged";  // unchanged | low | very_low
        int throttleAffinityCores = 0;           // 0 = off
        int throttleCpuRatePercent = 0;          // 0 = off (Job Object hard cap, % of the machine)
        std::string suspendMode = "freeze";      // freeze | cpu_rate
        int suspendCpuRatePercent = 1;           // for cpu_rate
    } actions;

    // "groups": extra independent policies. Group i in this list is policy
    // index i + 1; index 0 is the top-level ("default") policy. Their process
    // names are also merged into criticalProcesses/batchProcesses (and rules).
    struct GroupConfig {
        std::string name;
        std::vector<std::wstring> critical;
        std::vector<std::wstring> batch;
        ThresholdSpec high, critical_, low;   // critical_ = the "critical" threshold
        ActionsConfig actions;
    };
    std::vector<GroupConfig> groups;
    std::map<std::wstring, int> criticalGroupOf;  // lower-case name -> policy index (absent = 0)
    std::map<std::wstring, int> batchGroupOf;

    // "schedules": time windows that override one group's policy. The first
    // schedule that matches the local time wins for its group.
    struct ScheduleConfig {
        std::string name;
        int group = 0;                 // policy index (0 = default)
        unsigned daysMask = 0;         // bit 0 = Sunday ... bit 6 = Saturday
        int fromMinute = 0;            // 0..1439
        int toMinute = 0;              // 1..1440; below fromMinute = runs past midnight
        bool off = false;              // mode "off": never throttle during the window
        // Effective policy for the window (the group's, with overrides applied).
        ThresholdSpec high, critical_, low;
        ActionsConfig actions;
    };
    std::vector<ScheduleConfig> schedules;

    // Prometheus endpoint at http://127.0.0.1:<dashboard port>/metrics.
    bool metricsEnabled = false;
    std::string metricsToken;  // if set, scrapes need "Authorization: Bearer <token>"
};

// Thrown for unreadable, malformed, incomplete or out-of-range config. The
// message names the offending field, e.g. "thresholds.high.percent must be...".
class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

DaemonConfig ParseConfig(const std::string& jsonText);
DaemonConfig LoadConfigFile(const std::wstring& path);

// Index into cfg.schedules of the schedule active for `group` at a local
// weekday (0 = Sunday) and minute of the day (0..1439), or -1 for none.
// A window that runs past midnight belongs to the day it starts on.
int ActiveSchedule(const DaemonConfig& cfg, int group, int weekday, int minuteOfDay);

// Returns jsonText with `processName` moved to the given role: "critical",
// "batch", "protected", or "none" (removed from all three lists). Names are
// matched case-insensitively; key order and every other setting are kept.
// The result is validated with ParseConfig, so an invalid outcome throws
// ConfigError and the caller's file is never written.
std::string SetProcessRole(const std::string& jsonText, const std::string& processName,
                           const std::string& role);

}
