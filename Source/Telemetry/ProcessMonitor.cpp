#include "ProcessMonitor.h"

#include <psapi.h>
#include <tlhelp32.h>

namespace telemetry {

namespace {

uint64_t NowTicks() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return FileTimeToUint64(ft);
}

bool SameName(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

constexpr double kMb = 1024.0 * 1024.0;

}

std::vector<ProcessEntry> SnapshotProcesses() {
    std::vector<ProcessEntry> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return out;
    }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            out.push_back({pe.szExeFile, pe.th32ProcessID});
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

std::vector<DWORD> FindByName(const std::vector<ProcessEntry>& snapshot, const std::wstring& name) {
    std::vector<DWORD> pids;
    for (const ProcessEntry& e : snapshot) {
        if (SameName(e.name, name)) {
            pids.push_back(e.pid);
        }
    }
    return pids;
}

TrackedProcess::TrackedProcess(std::wstring name, CpuNormalization normalization,
                               unsigned logicalCores)
    : name_(std::move(name)), normalization_(normalization), cores_(logicalCores) {}

TrackedProcess::~TrackedProcess() {
    for (auto& [pid, inst] : instances_) {
        CloseHandle(inst.handle);
    }
}

void TrackedProcess::Poll(const std::vector<ProcessEntry>& snapshot,
                          std::vector<ProcessSample>& samples,
                          std::vector<LifecycleEvent>& events) {
    // Drop instances that have exited since the last poll.
    for (auto it = instances_.begin(); it != instances_.end();) {
        if (WaitForSingleObject(it->second.handle, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(it->second.handle, &code);
            events.push_back({code == 0 ? LifecycleKind::Exited : LifecycleKind::Crashed, name_,
                              it->first, 0, code});
            lastExitedPid_ = it->first;
            CloseHandle(it->second.handle);
            it = instances_.erase(it);
        } else {
            ++it;
        }
    }

    // Pick up instances that started (or restarted under a new PID).
    for (DWORD pid : FindByName(snapshot, name_)) {
        if (instances_.count(pid) != 0) {
            continue;
        }
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (h == nullptr) {
            // Protected or elevated process we cannot open: report once, keep trying.
            if (GetLastError() == ERROR_ACCESS_DENIED && !deniedReported_[pid]) {
                deniedReported_[pid] = true;
                events.push_back({LifecycleKind::AccessDenied, name_, pid});
            }
            continue;
        }
        deniedReported_.erase(pid);
        const bool restart = instances_.empty() && lastExitedPid_ != 0 && lastExitedPid_ != pid;
        events.push_back({restart ? LifecycleKind::Restarted : LifecycleKind::Acquired, name_, pid,
                          restart ? lastExitedPid_ : 0});
        lastExitedPid_ = 0;
        Instance inst;
        inst.handle = h;
        instances_.emplace(pid, inst);
    }

    for (auto& [pid, inst] : instances_) {
        ProcessSample s;
        s.name = name_;
        s.pid = pid;
        if (SampleInstance(pid, inst, s)) {
            samples.push_back(s);
        }
    }
}

bool TrackedProcess::SampleInstance(DWORD pid, Instance& inst, ProcessSample& out) {
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(inst.handle, &created, &exited, &kernel, &user)) {
        return false;
    }
    const uint64_t cpu = FileTimeToUint64(kernel) + FileTimeToUint64(user);
    const uint64_t wall = NowTicks();

    IO_COUNTERS io{};
    uint64_t ioBytes = 0;
    if (GetProcessIoCounters(inst.handle, &io)) {
        ioBytes = io.ReadTransferCount + io.WriteTransferCount + io.OtherTransferCount;
    }

    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(inst.handle, &pmc, sizeof(pmc))) {
        out.workingSetMb = pmc.WorkingSetSize / kMb;
    }

    if (inst.primed && wall > inst.lastWallTicks) {
        const uint64_t wallDelta = wall - inst.lastWallTicks;
        out.cpuPercent = CpuPercentFromTicks(cpu - inst.lastCpuTicks, wallDelta, cores_,
                                             normalization_);
        const double seconds = wallDelta / 1e7;
        out.ioMbPerSec = ioBytes >= inst.lastIoBytes
                             ? (ioBytes - inst.lastIoBytes) / kMb / seconds
                             : 0.0;
        out.valid = true;
    }
    inst.lastCpuTicks = cpu;
    inst.lastWallTicks = wall;
    inst.lastIoBytes = ioBytes;
    inst.primed = true;
    (void)pid;
    return true;
}

double SystemMemoryLoadPercent() {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) {
        return 0.0;
    }
    return static_cast<double>(ms.dwMemoryLoad);
}

SelfMonitor::SelfMonitor() {
    double c, w, p;
    DWORD h;
    Sample(c, w, p, h);
}

void SelfMonitor::Sample(double& cpuPercent, double& workingSetMb, double& privateMb,
                         DWORD& handles) {
    FILETIME created, exited, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    const uint64_t cpu = FileTimeToUint64(kernel) + FileTimeToUint64(user);
    const uint64_t wall = NowTicks();
    cpuPercent = lastWall_ == 0 ? 0.0
                                : CpuPercentFromTicks(cpu - lastCpu_, wall - lastWall_, 1,
                                                      CpuNormalization::PerCore);
    lastCpu_ = cpu;
    lastWall_ = wall;

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                         sizeof(pmc));
    workingSetMb = pmc.WorkingSetSize / kMb;
    privateMb = pmc.PrivateUsage / kMb;
    handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
}

}
