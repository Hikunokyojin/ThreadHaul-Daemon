#pragma once

#include <windows.h>

#include <string>

#include "PriorityControl.h"

namespace actuator {

// What "throttle" and "suspend" do to a batch process. The defaults are the
// v2.0 behaviour: Idle priority, then freeze every thread.
enum class IoPriority { Unchanged, Low, VeryLow };
enum class SuspendMode { Freeze, CpuRate };

struct ActionPolicy {
    // Throttle stage: every option that is set is applied together.
    PriorityLevel throttlePriority = PriorityLevel::Idle;
    IoPriority    throttleIo = IoPriority::Unchanged;
    int           throttleAffinityCores = 0;    // 0 = off; else confine to this many logical cores
    int           throttleCpuRatePercent = 0;   // 0 = off; else Job Object hard CPU cap

    // Suspend stage: freeze threads, or squeeze to a tiny CPU share (the
    // process keeps running, so sockets and timers survive).
    SuspendMode   suspendMode = SuspendMode::Freeze;
    int           suspendCpuRatePercent = 1;    // for SuspendMode::CpuRate

    bool operator==(const ActionPolicy& o) const {
        return throttlePriority == o.throttlePriority && throttleIo == o.throttleIo &&
               throttleAffinityCores == o.throttleAffinityCores &&
               throttleCpuRatePercent == o.throttleCpuRatePercent && suspendMode == o.suspendMode &&
               suspendCpuRatePercent == o.suspendCpuRatePercent;
    }
};

// Applies and undoes the actions on one process. It remembers what it
// actually changed (original affinity, I/O priority, the job it created),
// so Restore() undoes exactly that, even if the policy changed meanwhile.
// Every function returns true only if every requested change succeeded;
// `error` names the first one that failed.
class ProcessActions {
public:
    explicit ProcessActions(DWORD pid) : pid_(pid) {}
    ~ProcessActions();  // lifts any CPU cap so the job cannot outlive us
    ProcessActions(const ProcessActions&) = delete;
    ProcessActions& operator=(const ProcessActions&) = delete;

    bool Throttle(const ActionPolicy& p, std::string& error);         // Normal -> Throttled
    bool Suspend(const ActionPolicy& p, std::string& error);          // Throttled -> Suspended
    bool Unsuspend(const ActionPolicy& p, std::string& error);        // Suspended -> Throttled (manual)
    bool Restore(std::string& error);                                 // any -> Normal

    bool Frozen() const { return frozen_; }

private:
    bool SetCpuRate(int percent, std::string& error);  // 0 lifts the cap

    DWORD pid_;
    HANDLE job_ = nullptr;
    int cpuRate_ = 0;               // cap currently in force (percent), 0 = none
    bool frozen_ = false;
    bool affinityChanged_ = false;
    DWORD_PTR originalAffinity_ = 0;
    bool ioChanged_ = false;
    ULONG originalIo_ = 2;          // IoPriorityNormal
};

// Low-level helpers, exposed for tests.
bool GetIoPriority(DWORD pid, ULONG& priority);    // 0 very low, 1 low, 2 normal
bool SetIoPriority(DWORD pid, ULONG priority);
// Mask with the `cores` highest-numbered bits of `available` (0 if none).
DWORD_PTR HighestCores(DWORD_PTR available, int cores);

}
