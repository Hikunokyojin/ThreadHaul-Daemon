#include "ProcessTable.h"

#include <psapi.h>

#include "ProcessMonitor.h"

namespace telemetry {

std::vector<ProcessTable::Row> ProcessTable::Sample(System& system) {
    std::lock_guard<std::mutex> lock(mutex_);

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    system.logicalCores = si.dwNumberOfProcessors;

    // System CPU from idle vs. kernel+user time (kernel time includes idle).
    FILETIME idleFt, kernelFt, userFt;
    if (GetSystemTimes(&idleFt, &kernelFt, &userFt)) {
        const uint64_t idle = FileTimeToUint64(idleFt);
        const uint64_t total = FileTimeToUint64(kernelFt) + FileTimeToUint64(userFt);
        if (lastTotal_ != 0 && total > lastTotal_) {
            const double busy = 1.0 - static_cast<double>(idle - lastIdle_) /
                                          static_cast<double>(total - lastTotal_);
            system.cpuPercent = busy < 0 ? 0 : (busy > 1 ? 100 : busy * 100.0);
        }
        lastIdle_ = idle;
        lastTotal_ = total;
    }

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        system.memoryLoadPercent = ms.dwMemoryLoad;
        system.memoryTotalGb = ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
        system.memoryUsedGb = (ms.ullTotalPhys - ms.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
    }

    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    const uint64_t now = FileTimeToUint64(nowFt);

    std::vector<Row> rows;
    std::map<DWORD, Prev> next;
    for (const ProcessEntry& e : SnapshotProcesses()) {
        Row r;
        r.pid = e.pid;
        r.name = e.name;
        if (e.pid == 0) {  // System Idle Process: not a real process
            continue;
        }
        r.safety = ClassifyProcess(e.pid, e.name, essential_);
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.pid);
        if (h == nullptr) {
            r.accessible = false;
            rows.push_back(r);
            continue;
        }
        FILETIME c, x, k, u;
        if (GetProcessTimes(h, &c, &x, &k, &u)) {
            const uint64_t cpu = FileTimeToUint64(k) + FileTimeToUint64(u);
            const auto it = prev_.find(e.pid);
            if (it != prev_.end() && now > it->second.wall && cpu >= it->second.cpu) {
                r.cpuPercent = CpuPercentFromTicks(cpu - it->second.cpu, now - it->second.wall,
                                                   system.logicalCores, CpuNormalization::PerCore);
            }
            next[e.pid] = {cpu, now};
        }
        PROCESS_MEMORY_COUNTERS pmc{};
        if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) {
            r.workingSetMb = pmc.WorkingSetSize / (1024.0 * 1024.0);
        }
        CloseHandle(h);
        rows.push_back(r);
    }
    prev_.swap(next);
    system.processCount = static_cast<unsigned>(rows.size());
    return rows;
}

}
