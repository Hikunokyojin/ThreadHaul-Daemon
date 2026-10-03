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
    , onTransition_(std::move(onTransition)) {
    processHandle_ = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                 FALSE, batchProcessId_);
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

    if (HasExited()) {
        actuationSucceeded = false;
    } else {
        switch (newState) {
            case ProcessState::Throttled:
                actuationSucceeded =
                    SetProcessPriority(batchProcessId_, PriorityLevel::Idle) ==
                    PriorityChangeResult::Success;
                break;

            case ProcessState::Suspended:
                actuationSucceeded = SuspendAllThreads(batchProcessId_).AllSucceeded();
                break;

            case ProcessState::Normal: {
                bool resumed = true;
                if (oldState == ProcessState::Suspended) {
                    resumed = ResumeAllThreads(batchProcessId_).AllSucceeded();
                }
                const bool restored =
                    SetProcessPriority(batchProcessId_, PriorityLevel::Normal) ==
                    PriorityChangeResult::Success;
                actuationSucceeded = resumed && restored;
                break;
            }
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

void ProcessStateMachine::ForceRecoverForShutdown() {
    if (state_ == ProcessState::Normal) {
        return;
    }
    TransitionTo(ProcessState::Normal, 0.0);
}

}