// StateMachine.cpp
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

    // --- consecutive-sample counters, mirroring spec Section 3.3 ---
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

    // Apply the actual Win32 side effect for this transition.
    switch (newState) {
        case ProcessState::Throttled:
            SetProcessPriority(batchProcessId_, PriorityLevel::Idle);
            break;

        case ProcessState::Suspended:
            SuspendAllThreads(batchProcessId_);
            break;

        case ProcessState::Normal:
            // Recovering from Suspended requires resuming threads first;
            // recovering from Throttled only requires restoring priority.
            // Calling ResumeAllThreads when nothing was suspended is a
            // harmless no-op (zero threads attempted only if the process
            // has no matching snapshot entries -- otherwise it's a benign
            // extra resume call, which is safe here because Suspend/Resume
            // are only ever invoked in this single symmetric pairing).
            if (oldState == ProcessState::Suspended) {
                ResumeAllThreads(batchProcessId_);
            }
            SetProcessPriority(batchProcessId_, PriorityLevel::Normal);
            break;
    }

    state_ = newState;

    // Reset the counters that drove this transition so the next state
    // starts counting fresh (prevents an immediately-repeated transition
    // off stale counter values).
    consecutiveHigh_ = 0;
    consecutiveCritical_ = 0;
    consecutiveLow_ = 0;

    if (onTransition_) {
        onTransition_(batchProcessName_, batchProcessId_, oldState, newState, cpuPercent);
    }
}

void ProcessStateMachine::ForceRecoverForShutdown() {
    if (state_ == ProcessState::Normal) {
        return; // nothing to restore
    }
    // Reuses the same Normal-transition logic (resumes threads if needed,
    // restores NORMAL priority), satisfying the "graceful shutdown" item:
    // no batch process should be left throttled/suspended after the
    // service itself stops.
    TransitionTo(ProcessState::Normal, 0.0);
}

} // namespace actuator
