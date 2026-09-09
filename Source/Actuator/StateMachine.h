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
    double              cpuPercentAtTransition)>;

class ProcessStateMachine {
public:
    ProcessStateMachine(std::wstring batchProcessName,
                         DWORD batchProcessId,
                         StateMachineConfig config,
                         TransitionCallback onTransition = nullptr);
    bool Update(double criticalProcessCpuPercent);
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

    int consecutiveHigh_ = 0;
    int consecutiveCritical_ = 0;
    int consecutiveLow_ = 0;
};

}