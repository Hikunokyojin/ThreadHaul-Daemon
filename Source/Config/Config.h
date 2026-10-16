#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace config {

struct ThresholdSpec {
    double percent = 0.0;
    int    consecutiveSamples = 0;
};

// Runtime configuration loaded from config.json. Schema: INTERFACES.md section 4.
struct DaemonConfig {
    int pollIntervalMs = 1000;

    ThresholdSpec high{85.0, 3};
    ThresholdSpec critical{90.0, 5};
    ThresholdSpec low{60.0, 5};

    std::vector<std::wstring> criticalProcesses;
    std::vector<std::wstring> batchProcesses;
    std::vector<std::wstring> protectedProcesses;

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
};

// Thrown for unreadable, malformed, incomplete or out-of-range config. The
// message names the offending field, e.g. "thresholds.high.percent must be...".
class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

DaemonConfig ParseConfig(const std::string& jsonText);
DaemonConfig LoadConfigFile(const std::wstring& path);

// Returns jsonText with `processName` moved to the given role: "critical",
// "batch", "protected", or "none" (removed from all three lists). Names are
// matched case-insensitively; key order and every other setting are kept.
// The result is validated with ParseConfig, so an invalid outcome throws
// ConfigError and the caller's file is never written.
std::string SetProcessRole(const std::string& jsonText, const std::string& processName,
                           const std::string& role);

}
