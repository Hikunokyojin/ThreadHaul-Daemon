#pragma once

#include <deque>

#include "StateMachine.h"

namespace telemetry {

struct AdaptiveSettings {
    bool   enabled = false;
    int    windowSamples = 10;      // samples used for the trend fit
    int    horizonSamples = 3;      // how far ahead the forecast looks
    double maxAdjustPercent = 10.0; // largest amount thresholds may be lowered
};

// Predictive threshold adjustment. Fits a least-squares line to the last
// windowSamples pressure readings and forecasts horizonSamples ahead. When
// load is rising and the forecast crosses the throttle threshold, the
// throttle and escalate thresholds are lowered by the forecast rise
// (slope * horizon), capped at maxAdjustPercent, so the batch is demoted
// before the critical process is starved instead of after. Flat or falling
// load leaves the configured thresholds untouched, and the recovery
// threshold is never moved, so adaptation cannot make recovery easier.
//
// externalBias (from the distributed controller during cluster-wide
// pressure) is added to the offset under the same cap, and applies even
// when local prediction is disabled.
class AdaptiveThresholds {
public:
    explicit AdaptiveThresholds(AdaptiveSettings settings = {}) : settings_(settings) {}

    void SetSettings(const AdaptiveSettings& s);
    void AddSample(double pressurePercent);
    void Reset() { window_.clear(); }

    double Slope() const;          // percent per sample; 0 until 3 samples
    double Forecast() const;       // fitted value horizonSamples ahead
    double Offset(const actuator::StateMachineConfig& base, double externalBias) const;

    actuator::StateMachineConfig Adjust(const actuator::StateMachineConfig& base,
                                        double externalBias = 0.0) const;

private:
    void Fit(double& slope, double& intercept) const;

    AdaptiveSettings settings_;
    std::deque<double> window_;
};

}
