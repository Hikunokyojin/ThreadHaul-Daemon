#include "StateMachine.h"
#include "PriorityControl.h"
#include "ThreadControl.h"

namespace actuator {

ProcessStateMachine::ProcessStateMachine(std::wstring batchProcessName,
                                          DWORD batchProcessId,
                                          StateMachineConfig config,
                                          TransitionCallback onTransition)
    : batchProcessName_(std::move(batchProcessName))
    , batchProcessId_(batchProcessId)
    , config_(config)
    , onTransition_(std::move(onTransition))
    , actions_(batchProcessId) {
    processHandle_ = OpenProcess(SYNCHRONIZE, FALSE, batchProcessId_);
}

ProcessStateMachine::~ProcessStateMachine() {
    if (processHandle_ != nullptr) {
        CloseHandle(processHandle_);
    }
}

bool ProcessStateMachine::HasExited() const {
    if (processHandle_ == nullptr) {
        return false;
    }
    return WaitForSingleObject(processHandle_, 0) == WAIT_OBJECT_0;
}

bool ProcessStateMachine::Update(double criticalProcessCpuPercent) {
    const ProcessState stateBefore = state_;

    if (criticalProcessCpuPercent >= config_.throttleThresholdPercent) {
        consecutiveHigh_++;
    } else {
        consecutiveHigh_ = 0;
    }

    if (criticalProcessCpuPercent >= config_.escalateThresholdPercent) {
        consecutiveCritical_++;
    } else {
        consecutiveCritical_ = 0;
    }

    if (criticalProcessCpuPercent < config_.recoverThresholdPercent) {
        consecutiveLow_++;
    } else {
        consecutiveLow_ = 0;
    }

    if (manualHold_) {
        return false;
    }

    // --- state transitions ---
    if (state_ == ProcessState::Normal &&
        consecutiveHigh_ >= config_.throttleConsecutiveSamples) {
        TransitionTo(ProcessState::Throttled, criticalProcessCpuPercent);
    }
    else if (state_ == ProcessState::Throttled &&
             consecutiveCritical_ >= config_.escalateConsecutiveSamples) {
        TransitionTo(ProcessState::Suspended, criticalProcessCpuPercent);
    }
    else if ((state_ == ProcessState::Throttled || state_ == ProcessState::Suspended) &&
             consecutiveLow_ >= config_.recoverConsecutiveSamples) {
        TransitionTo(ProcessState::Normal, criticalProcessCpuPercent);
    }

    return state_ != stateBefore;
}

void ProcessStateMachine::TransitionTo(ProcessState newState, double cpuPercent) {
    const ProcessState oldState = state_;

    bool actuationSucceeded = true;
    lastActionError_.clear();

    if (HasExited()) {
        actuationSucceeded = false;
        lastActionError_ = "process has exited";
    } else {
        switch (newState) {
            case ProcessState::Throttled:
                // Suspended -> Throttled only happens through a manual override.
                actuationSucceeded = oldState == ProcessState::Suspended
                                         ? actions_.Unsuspend(config_.actions, lastActionError_)
                                         : actions_.Throttle(config_.actions, lastActionError_);
                break;

            case ProcessState::Suspended:
                actuationSucceeded = actions_.Suspend(config_.actions, lastActionError_);
                break;

            case ProcessState::Normal:
                // Undoes exactly what was applied, even if the policy changed since.
                actuationSucceeded = actions_.Restore(lastActionError_);
                break;
        }
    }

    state_ = newState;

    consecutiveHigh_ = 0;
    consecutiveCritical_ = 0;
    consecutiveLow_ = 0;

    if (onTransition_) {
        onTransition_(batchProcessName_, batchProcessId_, oldState, newState, cpuPercent,
                      actuationSucceeded);
    }
}

void ProcessStateMachine::ForceState(ProcessState newState, double cpuPercent, bool hold) {
    manualHold_ = hold;
    if (state_ != newState) {
        TransitionTo(newState, cpuPercent);
    }
}

void ProcessStateMachine::ForceRecoverForShutdown() {
    manualHold_ = false;
    if (state_ == ProcessState::Normal) {
        return;
    }
    TransitionTo(ProcessState::Normal, 0.0);
}

}