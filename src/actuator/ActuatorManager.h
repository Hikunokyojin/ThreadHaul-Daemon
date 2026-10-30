#pragma once

#include "StateMachine.h"
#include "ThresholdEvent.h"
#include <unordered_map>
#include <memory>
#include <vector>

namespace actuator {

class ActuatorManager {
public:
    explicit ActuatorManager(StateMachineConfig config,
                              TransitionCallback onTransition = nullptr);

    // Returns false (and does not manage the process) if it is protected.
    bool AddBatchProcess(const std::wstring& processName, DWORD processId);

    // Replaces the configured protection list. Any currently managed process
    // that is now protected is restored to normal and dropped.
    void SetProtectedProcessNames(std::vector<std::wstring> names);

    // True for this service's own process, a fixed set of Windows processes
    // whose suspension would hang the system, and any configured name.
    // Names are compared case-insensitively against the executable name.
    bool IsProtected(const std::wstring& processName, DWORD processId) const;

    // The built-in names that are always protected, whatever the config says.
    static std::vector<std::wstring> AlwaysProtectedNames();

    void RemoveBatchProcess(DWORD processId);

    // Entry point for callers that only have threshold events: applies
    // evt.observedCpuPercent to every managed batch process (same as
    // UpdateAll). evt.criticalProcessId is informational only; it is not
    // the process that gets throttled. The service itself calls UpdateAll
    // once per poll and does not also call this, to avoid double counting.
    void OnThresholdEvent(const ThresholdEvent& evt);

    // Returns true if any managed process changed state on this sample.
    bool UpdateAll(double criticalProcessCpuPercent);

    void ForceRecoverAllForShutdown();

    // Applies new thresholds to every managed process and to any added later.
    void SetConfig(const StateMachineConfig& config);
    const StateMachineConfig& Config() const { return config_; }

    // Manual override (CLI). Returns nullptr if the PID is not managed.
    // ForceState holds the process in newState until ReleaseManualHold.
    ProcessStateMachine* Find(DWORD processId);
    bool ForceState(DWORD processId, ProcessState newState, double cpuPercent, bool hold);
    bool ReleaseManualHold(DWORD processId);

    template <typename Fn>
    void ForEach(Fn fn) const {
        for (const auto& [pid, machine] : machines_) fn(*machine);
    }

    size_t ManagedCount() const { return machines_.size(); }

private:
    StateMachineConfig config_;
    TransitionCallback onTransition_;
    std::vector<std::wstring> protectedNames_;
    std::unordered_map<DWORD, std::unique_ptr<ProcessStateMachine>> machines_;
};

}
