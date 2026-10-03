#include "ActuatorManager.h"

namespace actuator {

ActuatorManager::ActuatorManager(StateMachineConfig config,
                                  TransitionCallback onTransition)
    : config_(config)
    , onTransition_(std::move(onTransition)) {}

void ActuatorManager::AddBatchProcess(const std::wstring& processName, DWORD processId) {
    if (machines_.find(processId) != machines_.end()) {
        return;
    }
    machines_[processId] = std::make_unique<ProcessStateMachine>(
        processName, processId, config_, onTransition_);
}

void ActuatorManager::RemoveBatchProcess(DWORD processId) {
    machines_.erase(processId);
}

void ActuatorManager::OnThresholdEvent(const ThresholdEvent& evt) {
    // See INTERFACES.md section 2: evt.criticalProcessId is informational
    // context only. The actual state-machine input is the observed CPU%,
    // applied to every batch process this manager already owns.
    UpdateAll(evt.observedCpuPercent);
}

void ActuatorManager::UpdateAll(double criticalProcessCpuPercent) {
    for (auto it = machines_.begin(); it != machines_.end();) {
        if (it->second->HasExited()) {
            it = machines_.erase(it);
            continue;
        }
        it->second->Update(criticalProcessCpuPercent);
        ++it;
    }
}

void ActuatorManager::ForceRecoverAllForShutdown() {
    for (auto& [pid, machine] : machines_) {
        machine->ForceRecoverForShutdown();
    }
}

}
