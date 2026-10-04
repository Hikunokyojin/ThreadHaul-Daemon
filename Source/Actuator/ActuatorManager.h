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

    void RemoveBatchProcess(DWORD processId);

    // Stable public entry point for the Telemetry layer, per INTERFACES.md
    // section 2 (CONFIRMED 2026-09-25). Telemetry calls this once per fired
    // threshold; Actuator applies evt.observedCpuPercent to every batch
    // process it currently manages. evt.criticalProcessId is not used to
    // select which process gets throttled/suspended -- see INTERFACES.md
    // section 2 for why.
    void OnThresholdEvent(const ThresholdEvent& evt);

    void UpdateAll(double criticalProcessCpuPercent);

    void ForceRecoverAllForShutdown();

    size_t ManagedCount() const { return machines_.size(); }

private:
    StateMachineConfig config_;
    TransitionCallback onTransition_;
    std::vector<std::wstring> protectedNames_;
    std::unordered_map<DWORD, std::unique_ptr<ProcessStateMachine>> machines_;
};

}
