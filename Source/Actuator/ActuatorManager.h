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

    void AddBatchProcess(const std::wstring& processName, DWORD processId);

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
    std::unordered_map<DWORD, std::unique_ptr<ProcessStateMachine>> machines_;
};

}
