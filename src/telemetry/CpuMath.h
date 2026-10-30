#pragma once

#include <windows.h>
#include <cstdint>

namespace telemetry {

// What 100% means. PerCore: 100% is one fully used logical core, so a
// process with four busy threads reads ~400%. TotalSystem: 100% is every
// logical core fully used, so the same process reads 400/cores %.
enum class CpuNormalization {
    PerCore,
    TotalSystem,
};

uint64_t FileTimeToUint64(const FILETIME& ft);

// Converts a process's CPU time delta (kernel + user, 100 ns ticks, from
// GetProcessTimes) over a wall-clock delta (100 ns ticks) into a percentage.
// Results are clamped to [0, 100 * logicalCores] for PerCore and [0, 100]
// for TotalSystem; a zero or negative wall delta returns 0.
double CpuPercentFromTicks(uint64_t processTicksDelta, uint64_t wallTicksDelta,
                           unsigned logicalCores, CpuNormalization normalization);

}
