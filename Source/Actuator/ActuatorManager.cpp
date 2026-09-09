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

void ActuatorManager::UpdateAll(double criticalProcessCpuPercent) {
    for (auto& [pid, machine] : machines_) {
        machine->Update(criticalProcessCpuPercent);
    }
}

void ActuatorManager::ForceRecoverAllForShutdown() {
    for (auto& [pid, machine] : machines_) {
        machine->ForceRecoverForShutdown();
    }
}

}
