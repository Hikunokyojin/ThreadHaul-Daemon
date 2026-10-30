#include "AdaptiveThresholds.h"

#include <algorithm>

namespace telemetry {

void AdaptiveThresholds::SetSettings(const AdaptiveSettings& s) {
    settings_ = s;
    while (static_cast<int>(window_.size()) > std::max(settings_.windowSamples, 1)) {
        window_.pop_front();
    }
}

void AdaptiveThresholds::AddSample(double pressurePercent) {
    window_.push_back(pressurePercent);
    while (static_cast<int>(window_.size()) > std::max(settings_.windowSamples, 1)) {
        window_.pop_front();
    }
}

void AdaptiveThresholds::Fit(double& slope, double& intercept) const {
    slope = 0.0;
    intercept = window_.empty() ? 0.0 : window_.back();
    const size_t n = window_.size();
    if (n < 3) {
        return;
    }
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; ++i) {
        const double x = static_cast<double>(i);
        sx += x;
        sy += window_[i];
        sxx += x * x;
        sxy += x * window_[i];
    }
    const double denom = n * sxx - sx * sx;
    if (denom == 0.0) {
        return;
    }
    slope = (n * sxy - sx * sy) / denom;
    intercept = (sy - slope * sx) / n;
}

double AdaptiveThresholds::Slope() const {
    double slope, intercept;
    Fit(slope, intercept);
    return slope;
}

double AdaptiveThresholds::Forecast() const {
    double slope, intercept;
    Fit(slope, intercept);
    if (window_.size() < 3) {
        return intercept;
    }
    const double lastX = static_cast<double>(window_.size() - 1);
    return intercept + slope * (lastX + settings_.horizonSamples);
}

double AdaptiveThresholds::Offset(const actuator::StateMachineConfig& base,
                                  double externalBias) const {
    // The controller's cluster bias applies even when local prediction is
    // off; the local trend term only when adaptive mode is enabled.
    double offset = std::max(externalBias, 0.0);
    const double slope = Slope();
    if (settings_.enabled && slope > 0.0 && Forecast() >= base.throttleThresholdPercent) {
        offset += slope * settings_.horizonSamples;
    }
    // Keep throttle strictly above recovery so the hysteresis band survives.
    const double room = base.throttleThresholdPercent - base.recoverThresholdPercent - 1.0;
    return std::clamp(offset, 0.0, std::max(0.0, std::min(settings_.maxAdjustPercent, room)));
}

actuator::StateMachineConfig AdaptiveThresholds::Adjust(const actuator::StateMachineConfig& base,
                                                        double externalBias) const {
    const double off = Offset(base, externalBias);
    actuator::StateMachineConfig out = base;
    out.throttleThresholdPercent -= off;
    out.escalateThresholdPercent -= off;
    return out;
}

}
