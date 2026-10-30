#include "CpuMath.h"

#include <algorithm>

namespace telemetry {

uint64_t FileTimeToUint64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

double CpuPercentFromTicks(uint64_t processTicksDelta, uint64_t wallTicksDelta,
                           unsigned logicalCores, CpuNormalization normalization) {
    if (wallTicksDelta == 0) {
        return 0.0;
    }
    const unsigned cores = logicalCores == 0 ? 1 : logicalCores;
    const double perCore = 100.0 * static_cast<double>(processTicksDelta) /
                           static_cast<double>(wallTicksDelta);
    if (normalization == CpuNormalization::TotalSystem) {
        return std::clamp(perCore / cores, 0.0, 100.0);
    }
    return std::clamp(perCore, 0.0, 100.0 * cores);
}

}
