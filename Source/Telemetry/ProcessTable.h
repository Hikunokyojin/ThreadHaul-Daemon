#pragma once

#include <windows.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "CpuMath.h"

namespace telemetry {

// Whole-machine view used by the dashboard's process picker: every running
// process with CPU% (per logical core) and working set, plus system-wide CPU
// and memory. CPU% is measured between two consecutive calls, so the first
// call reports 0% for everything. Thread-safe.
class ProcessTable {
public:
    struct Row {
        DWORD        pid = 0;
        std::wstring name;
        double       cpuPercent = 0.0;   // 100 = one logical core
        double       workingSetMb = 0.0;
        bool         accessible = true;  // false if OpenProcess was refused
    };

    struct System {
        double cpuPercent = 0.0;         // whole machine, 0-100
        double memoryLoadPercent = 0.0;
        double memoryTotalGb = 0.0;
        double memoryUsedGb = 0.0;
        unsigned logicalCores = 1;
        unsigned processCount = 0;
    };

    std::vector<Row> Sample(System& system);

private:
    struct Prev {
        uint64_t cpu = 0;
        uint64_t wall = 0;
    };

    std::mutex mutex_;
    std::map<DWORD, Prev> prev_;
    uint64_t lastIdle_ = 0;
    uint64_t lastTotal_ = 0;
};

}
