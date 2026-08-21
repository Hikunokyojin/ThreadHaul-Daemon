// StateMachine.h
//
// Owner: Divik (Actuator / Scheduling & Process Control layer)
// Spec:  specs/logistics-resource-monitor.md
//   - Throttling logic       (Section 3, Review I item 1)
//   - Escalation logic       (Section 3, Review I item 2)
//   - Recovery logic         (Section 3, Review I item 3)
//   - Per-batch independent suspend/resume (Section 3, Review I item 4)
//
// Design summary:
// A ProcessStateMachine owns the throttle/escalate/recover lifecycle for a
// SINGLE batch process. Each batch process gets its own instance, so their
// states never interfere with each other -- this satisfies the "per-batch
// independent suspend/resume" requirement. The state machine does not sample
// CPU% itself (that is Prakul's Telemetry module); it is fed a CPU% reading
// once per poll interval via Update() and reacts based on consecutive-sample
// counters, exactly as described in the spec's Section 3.3 pseudo-code.
//
// This header has no dependency on the logging module (Pranav's layer). It
// exposes a simple callback (TransitionCallback) that the integration layer
// (main.cpp) can wire up to real logging once that module exists. This keeps
// the Actuator module buildable and testable in isolation.

#pragma once

#include <windows.h>
#include <string>
#include <functional>

namespace actuator {

// The four states described in the spec's Section 3.3 diagram.
enum class ProcessState {
    Normal,
    Throttled,
    Suspended,
};

inline const wchar_t* ToString(ProcessState s) {
    switch (s) {
        case ProcessState::Normal:    return L"NORMAL";
        case ProcessState::Throttled: return L"THROTTLED";
        case ProcessState::Suspended: return L"SUSPENDED";
    }
    return L"UNKNOWN";
}

// Locked-in default thresholds from specs/logistics-resource-monitor.md.
// The Config module (Pranav) is expected to override these at runtime by
// constructing StateMachineConfig from config.json; these values are only
// the fallback defaults if no config is supplied (e.g. in unit tests).
struct StateMachineConfig {
    double   throttleThresholdPercent = 85.0;
    int      throttleConsecutiveSamples = 3;

    double   escalateThresholdPercent = 90.0;
    int      escalateConsecutiveSamples = 5;

    double   recoverThresholdPercent = 60.0;
    int      recoverConsecutiveSamples = 5;
};

// Fired whenever the state machine changes state. The integration layer can
// attach a real logger (JSON Lines + Event Log, per Pranav's module) here.
using TransitionCallback = std::function<void(
    const std::wstring& batchProcessName,
    DWORD               batchProcessId,
    ProcessState        oldState,
    ProcessState        newState,
    double              cpuPercentAtTransition)>;

class ProcessStateMachine {
public:
    ProcessStateMachine(std::wstring batchProcessName,
                         DWORD batchProcessId,
                         StateMachineConfig config,
                         TransitionCallback onTransition = nullptr);

    // Call once per poll interval (default 1s, per spec) with the current
    // CPU% of the associated CRITICAL process. Internally advances the
    // consecutive-sample counters and triggers SetPriorityClass /
    // SuspendThread / ResumeThread as required by the state machine.
    //
    // Returns true if a state transition occurred this call.
    bool Update(double criticalProcessCpuPercent);

    // Called on service shutdown (graceful shutdown, Review I item 5).
    // Resumes any suspended threads and restores NORMAL priority so the
    // batch process is never left in a degraded state after the monitor
    // itself stops running.
    void ForceRecoverForShutdown();

    ProcessState CurrentState() const { return state_; }
    const std::wstring& BatchProcessName() const { return batchProcessName_; }
    DWORD BatchProcessId() const { return batchProcessId_; }

private:
    void TransitionTo(ProcessState newState, double cpuPercent);

    std::wstring batchProcessName_;
    DWORD        batchProcessId_;
    StateMachineConfig config_;
    TransitionCallback onTransition_;

    ProcessState state_ = ProcessState::Normal;

    int consecutiveHigh_ = 0;     // samples >= throttleThresholdPercent
    int consecutiveCritical_ = 0; // samples >= escalateThresholdPercent
    int consecutiveLow_ = 0;      // samples <  recoverThresholdPercent
};

} // namespace actuator
