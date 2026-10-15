#pragma once

#include <windows.h>

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "ActuatorManager.h"
#include "AdaptiveThresholds.h"
#include "Alerts.h"
#include "Config.h"
#include "ControlPipe.h"
#include "Coordination.h"
#include "Dashboard.h"
#include "EventLogWriter.h"
#include "JsonLinesLogger.h"
#include "ProcessMonitor.h"
#include "ProcessSafety.h"
#include "ThresholdTracker.h"

namespace daemon {

// The integrated monitor: Telemetry samples the critical processes, their
// combined pressure (CPU, plus memory/I-O when enabled) drives one Actuator
// state machine per batch process, and every change is logged. Also owns
// config hot-reload, adaptive thresholds, the CLI control pipe, alerting,
// self-monitoring and the coordination client.
//
// Thread model: the poll loop, the control pipe thread and the coordination
// thread all go through mutex_, so the Actuator is only ever touched by one
// thread at a time.
class Daemon {
public:
    explicit Daemon(std::wstring configPath);
    ~Daemon();

    // Loads and validates config. On failure returns false with a message
    // naming the bad field; nothing is started.
    bool Initialize(std::wstring& error);

    // Polls until stopEvent is signalled, then resumes every suspended
    // thread and restores normal priorities before returning.
    void Run(HANDLE stopEvent);

    // CLI / controller commands: "throttle|suspend|resume|release <pid|name>",
    // "status". Returns a JSON object with "ok" and "message".
    std::string HandleCommand(const std::string& line);

    // Console mode only: lets "ThreadHaulDaemon.exe stop" end the run
    // cleanly. A service is stopped through the Service Control Manager.
    void AllowRemoteStop(HANDLE stopEvent) { remoteStopEvent_ = stopEvent; }

    // ---- Dashboard API (called from the dashboard's HTTP thread) ----
    std::string DashboardStatus();                 // status + config summary + system stats
    std::string ReadLog(long long offset);         // JSON Lines entries after a byte offset
    std::string SetRole(const std::string& name, const std::string& role, bool confirmed);

    // Highest safety level among running processes that match `target`
    // (a PID or an executable name), with the matching reason. Used to demand
    // confirmation before managing Windows components and services.
    telemetry::SafetyInfo SafetyOf(const std::string& target);
    std::string ProcessRolesJson();                // lower-cased role lists + managed states

private:
    void ApplyConfig(const config::DaemonConfig& cfg, bool initial);
    void CheckConfigReload();
    bool LoadAndVerify(config::DaemonConfig& out, std::wstring& error);
    void Poll();
    void DiscoverBatchProcesses(const std::vector<telemetry::ProcessEntry>& snapshot);
    void LogSelf();
    std::string StatusJson();
    void Log(const char* type, const nlohmann::ordered_json& fields);
    void OnTransition(const std::wstring& name, DWORD pid, actuator::ProcessState from,
                      actuator::ProcessState to, double cpu, bool ok);

    std::wstring configPath_;
    FILETIME configWriteTime_{};
    ULONGLONG configSize_ = 0;

    std::recursive_mutex mutex_;
    config::DaemonConfig cfg_;
    actuator::StateMachineConfig base_;
    actuator::StateMachineConfig effective_;
    std::unique_ptr<actuator::ActuatorManager> actuator_;
    std::unique_ptr<logging::JsonLinesLogger> logger_;
    logging::EventLogWriter eventLog_;
    Alerter alerter_;
    std::unique_ptr<ControlPipeServer> pipe_;
    std::unique_ptr<CoordinationClient> coordination_;

    std::map<std::wstring, std::unique_ptr<telemetry::TrackedProcess>> critical_;
    telemetry::ThresholdTracker tracker_;
    telemetry::AdaptiveThresholds adaptive_;
    telemetry::SelfMonitor self_;
    unsigned cores_ = 1;

    std::set<DWORD> adhoc_;            // processes added by a manual command, not by config
    std::set<DWORD> protectedLogged_;  // protected batch-name matches already reported
    std::vector<telemetry::ProcessSample> lastSamples_;
    double pressure_ = 0.0;
    std::string pressureSignal_ = "cpu";
    DWORD criticalPid_ = 0;            // critical process behind the current pressure
    const char* trigger_ = "automatic";
    ULONGLONG lastSelfLog_ = 0;
    HANDLE remoteStopEvent_ = nullptr;

    // Dashboard. Started and stopped by UpdateDashboard() outside mutex_,
    // because stopping it waits for an in-flight request that may itself be
    // waiting for mutex_.
    void UpdateDashboard();
    std::string DashboardInfo();
    std::unique_ptr<Dashboard> dashboard_;
    bool dashboardDirty_ = true;
    std::string dashboardError_;
    ULONGLONG startedTick_ = 0;
    uint64_t lastSysIdle_ = 0;
    uint64_t lastSysTotal_ = 0;
};

std::wstring DefaultConfigPath(bool preferExeDirectory);

}
