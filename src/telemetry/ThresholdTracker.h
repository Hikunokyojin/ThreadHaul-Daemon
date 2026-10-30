#pragma once

#include <vector>

#include "StateMachine.h"
#include "ThresholdEvent.h"

namespace telemetry {

// Counts consecutive samples above/below each threshold for ONE critical
// process and reports when a streak first reaches its required length, so
// the daemon can write a threshold_event log line once per streak.
// The Actuator's ProcessStateMachine keeps its own counters and makes the
// actual throttle/suspend/resume decisions; this tracker only reports.
class ThresholdTracker {
public:
    struct Fired {
        actuator::ThresholdType type;
        int consecutiveSamples;
    };

    // Returns every threshold whose streak reached its length on this sample.
    // At most one per type; High and Critical can fire on the same sample.
    std::vector<Fired> Add(double percent, const actuator::StateMachineConfig& cfg) {
        std::vector<Fired> out;
        high_ = percent >= cfg.throttleThresholdPercent ? high_ + 1 : 0;
        critical_ = percent >= cfg.escalateThresholdPercent ? critical_ + 1 : 0;
        low_ = percent < cfg.recoverThresholdPercent ? low_ + 1 : 0;
        if (high_ == cfg.throttleConsecutiveSamples) {
            out.push_back({actuator::ThresholdType::High, high_});
        }
        if (critical_ == cfg.escalateConsecutiveSamples) {
            out.push_back({actuator::ThresholdType::Critical, critical_});
        }
        if (low_ == cfg.recoverConsecutiveSamples) {
            out.push_back({actuator::ThresholdType::Low, low_});
        }
        return out;
    }

    // Called when the Actuator changes state: its ProcessStateMachine resets
    // its counters on every transition (escalation counts samples AFTER the
    // throttle), so the tracker does too and its events line up with actions.
    void Reset() { high_ = critical_ = low_ = 0; }

private:
    int high_ = 0;
    int critical_ = 0;
    int low_ = 0;
};

}
