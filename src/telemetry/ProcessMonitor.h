#pragma once

#include <windows.h>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "CpuMath.h"

namespace telemetry {

struct ProcessEntry {
    std::wstring name;
    DWORD        pid;
};

// One CreateToolhelp32Snapshot per poll, shared by every lookup in that poll.
std::vector<ProcessEntry> SnapshotProcesses();
std::vector<DWORD> FindByName(const std::vector<ProcessEntry>& snapshot, const std::wstring& name);

struct ProcessSample {
    std::wstring name;
    DWORD  pid = 0;
    bool   valid = false;          // false on the first poll after (re)acquiring: no CPU delta yet
    double cpuPercent = 0.0;
    double workingSetMb = 0.0;
    double ioMbPerSec = 0.0;       // read + write + other bytes per second
};

enum class LifecycleKind {
    Acquired,    // first seen (startup, or a new instance)
    Exited,      // exited with code 0
    Crashed,     // exited with a non-zero code
    Restarted,   // previous instance ended and a new PID with the same name appeared
    AccessDenied // found but OpenProcess was refused; retried every poll
};

struct LifecycleEvent {
    LifecycleKind kind;
    std::wstring  name;
    DWORD         pid;
    DWORD         previousPid = 0;
    DWORD         exitCode = 0;
};

// Samples every running instance of one executable name. Handles a process
// that is not running yet (keeps looking), one that exits or crashes (its
// handle becomes signalled), and one that restarts under a new PID. Holding
// a handle also stops Windows from reusing the PID while we sample it.
class TrackedProcess {
public:
    TrackedProcess(std::wstring name, CpuNormalization normalization, unsigned logicalCores);
    ~TrackedProcess();
    TrackedProcess(const TrackedProcess&) = delete;
    TrackedProcess& operator=(const TrackedProcess&) = delete;

    void Poll(const std::vector<ProcessEntry>& snapshot, std::vector<ProcessSample>& samples,
              std::vector<LifecycleEvent>& events);

    const std::wstring& Name() const { return name_; }
    void SetNormalization(CpuNormalization n) { normalization_ = n; }

private:
    struct Instance {
        HANDLE   handle = nullptr;
        uint64_t lastCpuTicks = 0;
        uint64_t lastWallTicks = 0;
        uint64_t lastIoBytes = 0;
        bool     primed = false;
    };

    bool SampleInstance(DWORD pid, Instance& inst, ProcessSample& out);

    std::wstring name_;
    CpuNormalization normalization_;
    unsigned cores_;
    std::map<DWORD, Instance> instances_;
    std::map<DWORD, bool> deniedReported_;
    DWORD lastExitedPid_ = 0;
};

// Physical memory in use across the system, 0-100 (GlobalMemoryStatusEx).
double SystemMemoryLoadPercent();

// This process's own CPU% (per-core normalisation), working set and handle
// count, for the self-monitoring log line.
class SelfMonitor {
public:
    SelfMonitor();
    void Sample(double& cpuPercent, double& workingSetMb, double& privateMb, DWORD& handles);

private:
    uint64_t lastCpu_ = 0;
    uint64_t lastWall_ = 0;
};

}
