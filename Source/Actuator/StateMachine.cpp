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
    , onTransition_(std::move(onTransition)) {}

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

    switch (newState) {
        case ProcessState::Throttled:
            SetProcessPriority(batchProcessId_, PriorityLevel::Idle);
            break;

        case ProcessState::Suspended:
            SuspendAllThreads(batchProcessId_);
            break;

        case ProcessState::Normal:
            if (oldState == ProcessState::Suspended) {
                ResumeAllThreads(batchProcessId_);
            }
            SetProcessPriority(batchProcessId_, PriorityLevel::Normal);
            break;
    }

    state_ = newState;

    consecutiveHigh_ = 0;
    consecutiveCritical_ = 0;
    consecutiveLow_ = 0;

    if (onTransition_) {
        onTransition_(batchProcessName_, batchProcessId_, oldState, newState, cpuPercent);
    }
}

void ProcessStateMachine::ForceRecoverForShutdown() {
    if (state_ == ProcessState::Normal) {
        return;
    }
    TransitionTo(ProcessState::Normal, 0.0);
}

}