#pragma once

#include "StateMachine.h"
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

    void UpdateAll(double criticalProcessCpuPercent);

    void ForceRecoverAllForShutdown();

    size_t ManagedCount() const { return machines_.size(); }

private:
    StateMachineConfig config_;
    TransitionCallback onTransition_;
    std::unordered_map<DWORD, std::unique_ptr<ProcessStateMachine>> machines_;
};

}
