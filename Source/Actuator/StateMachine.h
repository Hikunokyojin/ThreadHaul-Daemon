#pragma once

#include <windows.h>
#include <string>
#include <functional>

namespace actuator {

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
struct StateMachineConfig {
    double   throttleThresholdPercent = 85.0;
    int      throttleConsecutiveSamples = 3;

    double   escalateThresholdPercent = 90.0;
    int      escalateConsecutiveSamples = 5;

    double   recoverThresholdPercent = 60.0;
    int      recoverConsecutiveSamples = 5;
};
using TransitionCallback = std::function<void(
    const std::wstring& batchProcessName,
    DWORD               batchProcessId,
    ProcessState        oldState,
    ProcessState        newState,
    double              cpuPercentAtTransition,
    bool                actuationSucceeded)>;

class ProcessStateMachine {
public:
    ProcessStateMachine(std::wstring batchProcessName,
                         DWORD batchProcessId,
                         StateMachineConfig config,
                         TransitionCallback onTransition = nullptr);
    ~ProcessStateMachine();
    ProcessStateMachine(const ProcessStateMachine&) = delete;
    ProcessStateMachine& operator=(const ProcessStateMachine&) = delete;

    bool Update(double criticalProcessCpuPercent);
    void ForceRecoverForShutdown();

    // Replaces the thresholds (config hot-reload, adaptive adjustment).
    // Consecutive-sample counters are kept so a reload does not reset a
    // trigger that is already in progress.
    void SetConfig(const StateMachineConfig& config) { config_ = config; }
    const StateMachineConfig& Config() const { return config_; }

    // Manual override (CLI). Moves straight to newState, performing the same
    // OS calls as an automatic transition. While held, Update() still counts
    // samples but never changes state; ReleaseManualHold() hands control back.
    void ForceState(ProcessState newState, double cpuPercent, bool hold);
    void ReleaseManualHold() { manualHold_ = false; }
    bool IsManuallyHeld() const { return manualHold_; }

    // True once the managed process has exited. While we hold a handle to it
    // Windows cannot recycle its PID, so false also means the PID still refers
    // to the process we started managing. If the handle could not be opened at
    // construction the exit state is unknown and this returns false.
    bool HasExited() const;

    ProcessState CurrentState() const { return state_; }
    const std::wstring& BatchProcessName() const { return batchProcessName_; }
    DWORD BatchProcessId() const { return batchProcessId_; }

private:
    void TransitionTo(ProcessState newState, double cpuPercent);

    std::wstring batchProcessName_;
    DWORD        batchProcessId_;
    HANDLE       processHandle_ = nullptr;
    StateMachineConfig config_;
    TransitionCallback onTransition_;

    ProcessState state_ = ProcessState::Normal;
    bool manualHold_ = false;

    int consecutiveHigh_ = 0;
    int consecutiveCritical_ = 0;
    int consecutiveLow_ = 0;
};

}