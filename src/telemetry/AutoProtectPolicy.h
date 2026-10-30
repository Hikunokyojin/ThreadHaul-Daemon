#pragma once

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

namespace telemetry {

// System-wide automatic protection: when the whole machine is overloaded,
// lower the priority of the ordinary applications using the most CPU, and
// put them back when load returns to normal. Header-only so the decision
// logic can be unit-tested without touching real processes.
struct AutoProtectSettings {
    bool   enabled = true;
    double throttlePercent = 90.0;      // system CPU that starts throttling
    int    throttleSamples = 5;
    bool   useIdle = true;              // escalate to Idle priority if load stays extreme
    double idlePercent = 97.0;
    int    idleSamples = 10;
    double recoverPercent = 75.0;       // system CPU below which everything is restored
    int    recoverSamples = 10;
    int    maxProcesses = 3;            // at most this many processes throttled at once
    double minProcessCpuPercent = 50.0; // per-core %; lighter processes are left alone
};

enum class AutoLevel {
    Normal,    // nothing throttled
    Throttle,  // top consumers at Below Normal priority
    Idle,      // top consumers at Idle priority
};

inline const char* AutoLevelName(AutoLevel level) {
    switch (level) {
        case AutoLevel::Normal:   return "normal";
        case AutoLevel::Throttle: return "throttling";
        case AutoLevel::Idle:     return "idle";
    }
    return "normal";
}

// Consecutive-sample state machine on whole-system CPU %, with hysteresis:
// Normal -> Throttle -> Idle while overloaded, back to Normal only after the
// load has stayed below recoverPercent for recoverSamples.
class AutoProtectState {
public:
    AutoLevel Update(double systemCpuPercent, const AutoProtectSettings& s) {
        high_ = systemCpuPercent >= s.throttlePercent ? high_ + 1 : 0;
        extreme_ = systemCpuPercent >= s.idlePercent ? extreme_ + 1 : 0;
        low_ = systemCpuPercent < s.recoverPercent ? low_ + 1 : 0;
        if (level_ == AutoLevel::Normal && high_ >= s.throttleSamples) {
            level_ = AutoLevel::Throttle;
        } else if (level_ == AutoLevel::Throttle && s.useIdle && extreme_ >= s.idleSamples) {
            level_ = AutoLevel::Idle;
        } else if (level_ != AutoLevel::Normal && low_ >= s.recoverSamples) {
            level_ = AutoLevel::Normal;
        }
        return level_;
    }
    AutoLevel Level() const { return level_; }
    void Reset() { level_ = AutoLevel::Normal; high_ = extreme_ = low_ = 0; }

private:
    AutoLevel level_ = AutoLevel::Normal;
    int high_ = 0;
    int extreme_ = 0;
    int low_ = 0;
};

struct AutoCandidate {
    DWORD        pid = 0;
    std::wstring name;
    double       cpuPercent = 0.0;
};

// The heaviest eligible processes, at least minCpu each, at most `slots`.
inline std::vector<AutoCandidate> SelectAutoTargets(std::vector<AutoCandidate> eligible,
                                                    double minCpu, size_t slots) {
    eligible.erase(std::remove_if(eligible.begin(), eligible.end(),
                                  [&](const AutoCandidate& c) { return c.cpuPercent < minCpu; }),
                   eligible.end());
    std::sort(eligible.begin(), eligible.end(), [](const AutoCandidate& a, const AutoCandidate& b) {
        return a.cpuPercent != b.cpuPercent ? a.cpuPercent > b.cpuPercent : a.pid < b.pid;
    });
    if (eligible.size() > slots) eligible.resize(slots);
    return eligible;
}

}
