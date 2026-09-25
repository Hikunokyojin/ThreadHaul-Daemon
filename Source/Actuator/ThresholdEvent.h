#pragma once

// Shared cross-layer struct, confirmed in INTERFACES.md section 2.
// Lives here (Actuator's folder) because ActuatorManager::OnThresholdEvent
// is the consumer that fixes this struct's real shape; Telemetry
// (Prakul) should include this exact header rather than redefining it.
// If the team later adds a Source/Shared/ folder, this file should move
// there without changing its contents -- flag that as a team decision,
// not something to do unilaterally.

#include <cstdint>
#include <chrono>

namespace actuator {

enum class ThresholdType : uint8_t {
    High     = 0,  // 85%, 3 consecutive samples
    Critical = 1,  // 90%, 5 consecutive samples
    Low      = 2   // 60%, 5 consecutive samples (recovery)
};

struct ThresholdEvent {
    uint32_t criticalProcessId;                             // the CRITICAL process the % was sampled from (informational -- see ActuatorManager::OnThresholdEvent)
    ThresholdType type;                                      // which threshold fired
    double observedCpuPercent;                                // actual sampled value at trigger time
    std::chrono::system_clock::time_point timestamp;          // when the trigger condition was met
};

}
