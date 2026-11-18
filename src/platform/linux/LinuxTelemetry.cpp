// Linux implementation of the telemetry functions declared in
// ProcessMonitor.h, ProcessTable.h, ProcessSafety.h and ProcessMatch.h.
// Everything is read from /proc; times are converted to the 100 ns units the
// shared CpuMath code expects.

#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <set>

#include "LinuxProc.h"
#include "ProcessMatch.h"
#include "ProcessMonitor.h"
#include "ProcessSafety.h"
#include "ProcessTable.h"

namespace telemetry {

using namespace linuxproc;

namespace {

constexpr double kMb = 1024.0 * 1024.0;

uint64_t CpuTime100ns(const Stat& st) { return TicksTo100ns(st.utime + st.stime); }

double RssMb(int pid) {
    std::istringstream s(ReadText("/proc/" + std::to_string(pid) + "/statm"));
    uint64_t size = 0, resident = 0;
    if (!(s >> size >> resident)) return 0.0;
    return static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE)) / kMb;
}

// Bytes read and written through any file descriptor, like Windows' transfer
// counters. Needs the same user or root; 0 when unreadable.
uint64_t IoBytes(int pid) {
    std::istringstream s(ReadText("/proc/" + std::to_string(pid) + "/io"));
    uint64_t total = 0;
    for (std::string key; s >> key;) {
        uint64_t v = 0;
        s >> v;
        if (key == "rchar:" || key == "wchar:") total += v;
    }
    return total;
}

struct MemInfo {
    double totalKb = 0, availableKb = 0;
};

MemInfo ReadMemInfo() {
    MemInfo m;
    std::istringstream s(ReadText("/proc/meminfo"));
    for (std::string key; s >> key;) {
        double v = 0;
        s >> v;
        if (key == "MemTotal:") m.totalKb = v;
        else if (key == "MemAvailable:") m.availableKb = v;
        std::string unit;
        std::getline(s, unit);
    }
    return m;
}

bool IsKernelThread(int pid, const Stat& st) { return pid == 2 || st.ppid == 2; }

// Daemons a Linux system cannot run without, in addition to ActuatorManager's list.
const std::set<std::string>& LinuxEssentials() {
    static const std::set<std::string> names = {
        "init", "systemd", "systemd-journald", "systemd-logind", "systemd-udevd",
        "systemd-networkd", "systemd-resolved", "dbus-daemon", "dbus-broker", "sshd",
        "Xorg", "Xwayland", "gnome-shell", "kwin_wayland", "kwin_x11", "gdm", "sddm"};
    return names;
}

uid_t ProcessUid(int pid) {
    const std::string uids = StatusField(pid, "Uid");
    return uids.empty() ? static_cast<uid_t>(-1) : static_cast<uid_t>(std::strtoul(uids.c_str(), nullptr, 10));
}

}

// ---------------------------------------------------------------- snapshot

std::vector<ProcessEntry> SnapshotProcesses() {
    std::vector<ProcessEntry> out;
    for (int pid : NumericEntries("/proc")) {
        Stat st;
        if (!ReadStat(pid, st) || st.state == 'Z') continue;
        out.push_back({Widen(ProcessName(pid, st.comm)), static_cast<DWORD>(pid)});
    }
    return out;
}

std::vector<DWORD> FindByName(const std::vector<ProcessEntry>& snapshot, const std::wstring& name) {
    // Linux file names are case-sensitive.
    std::vector<DWORD> pids;
    for (const ProcessEntry& e : snapshot) {
        if (e.name == name) pids.push_back(e.pid);
    }
    return pids;
}

// ---------------------------------------------------------------- tracked process

TrackedProcess::TrackedProcess(std::wstring name, CpuNormalization normalization,
                               unsigned logicalCores)
    : name_(std::move(name)), normalization_(normalization), cores_(logicalCores) {}

TrackedProcess::~TrackedProcess() = default;

void TrackedProcess::Poll(const std::vector<ProcessEntry>& snapshot,
                          std::vector<ProcessSample>& samples,
                          std::vector<LifecycleEvent>& events) {
    // An instance has ended when its /proc entry is gone, is a zombie, or now
    // belongs to a different process (PID reuse). Linux gives the exit code
    // only to the parent, so endings are reported as Exited with code 0.
    for (auto it = instances_.begin(); it != instances_.end();) {
        Stat st;
        const bool alive = ReadStat(static_cast<int>(it->first), st) && st.state != 'Z' &&
                           st.starttime == it->second.startTime;
        if (!alive) {
            events.push_back({LifecycleKind::Exited, name_, it->first, 0, 0});
            lastExitedPid_ = it->first;
            it = instances_.erase(it);
        } else {
            ++it;
        }
    }

    for (DWORD pid : FindByName(snapshot, name_)) {
        if (instances_.count(pid) != 0) continue;
        Stat st;
        if (!ReadStat(static_cast<int>(pid), st)) continue;  // ended since the snapshot
        const bool restart = instances_.empty() && lastExitedPid_ != 0 && lastExitedPid_ != pid;
        events.push_back({restart ? LifecycleKind::Restarted : LifecycleKind::Acquired, name_, pid,
                          restart ? lastExitedPid_ : 0});
        lastExitedPid_ = 0;
        Instance inst;
        inst.startTime = st.starttime;
        instances_.emplace(pid, inst);
    }

    for (auto& [pid, inst] : instances_) {
        ProcessSample s;
        s.name = name_;
        s.pid = pid;
        if (SampleInstance(pid, inst, s)) samples.push_back(s);
    }
}

bool TrackedProcess::SampleInstance(DWORD pid, Instance& inst, ProcessSample& out) {
    Stat st;
    if (!ReadStat(static_cast<int>(pid), st)) return false;
    const uint64_t cpu = CpuTime100ns(st);
    const uint64_t wall = Now100ns();
    const uint64_t ioBytes = IoBytes(static_cast<int>(pid));
    out.workingSetMb = RssMb(static_cast<int>(pid));

    if (inst.primed && wall > inst.lastWallTicks) {
        const uint64_t wallDelta = wall - inst.lastWallTicks;
        out.cpuPercent = CpuPercentFromTicks(cpu >= inst.lastCpuTicks ? cpu - inst.lastCpuTicks : 0,
                                             wallDelta, cores_, normalization_);
        const double seconds = wallDelta / 1e7;
        out.ioMbPerSec = ioBytes >= inst.lastIoBytes ? (ioBytes - inst.lastIoBytes) / kMb / seconds : 0.0;
        out.valid = true;
    }
    inst.lastCpuTicks = cpu;
    inst.lastWallTicks = wall;
    inst.lastIoBytes = ioBytes;
    inst.primed = true;
    return true;
}

double SystemMemoryLoadPercent() {
    const MemInfo m = ReadMemInfo();
    return m.totalKb > 0 ? 100.0 * (m.totalKb - m.availableKb) / m.totalKb : 0.0;
}

SelfMonitor::SelfMonitor() {
    double c, w, p;
    DWORD h;
    Sample(c, w, p, h);
}

void SelfMonitor::Sample(double& cpuPercent, double& workingSetMb, double& privateMb, DWORD& handles) {
    const int self = static_cast<int>(getpid());
    Stat st;
    const uint64_t cpu = ReadStat(self, st) ? CpuTime100ns(st) : lastCpu_;
    const uint64_t wall = Now100ns();
    cpuPercent = lastWall_ == 0 ? 0.0
                                : CpuPercentFromTicks(cpu - lastCpu_, wall - lastWall_, 1,
                                                      CpuNormalization::PerCore);
    lastCpu_ = cpu;
    lastWall_ = wall;
    workingSetMb = RssMb(self);
    privateMb = std::strtod(StatusField(self, "RssAnon").c_str(), nullptr) / 1024.0;
    handles = static_cast<DWORD>(NumericEntries("/proc/self/fd").size());
}

// ---------------------------------------------------------------- process table

std::vector<ProcessTable::Row> ProcessTable::Sample(System& system) {
    std::lock_guard<std::mutex> lock(mutex_);
    system.logicalCores = LogicalCores();

    // System CPU from the aggregate "cpu" line: idle + iowait vs. everything.
    {
        std::istringstream s(ReadText("/proc/stat"));
        std::string label;
        s >> label;
        uint64_t v[10] = {};
        for (uint64_t& x : v) s >> x;
        const uint64_t idle = v[3] + v[4];
        uint64_t total = 0;
        for (int i = 0; i < 8; ++i) total += v[i];  // guest time is already in user
        if (lastTotal_ != 0 && total > lastTotal_) {
            const double busy = 1.0 - static_cast<double>(idle - lastIdle_) /
                                          static_cast<double>(total - lastTotal_);
            system.cpuPercent = std::clamp(busy * 100.0, 0.0, 100.0);
        }
        lastIdle_ = idle;
        lastTotal_ = total;
    }

    const MemInfo m = ReadMemInfo();
    if (m.totalKb > 0) {
        system.memoryLoadPercent = 100.0 * (m.totalKb - m.availableKb) / m.totalKb;
        system.memoryTotalGb = m.totalKb / (1024.0 * 1024.0);
        system.memoryUsedGb = (m.totalKb - m.availableKb) / (1024.0 * 1024.0);
    }

    const uint64_t now = Now100ns();
    std::vector<Row> rows;
    std::map<DWORD, Prev> next;
    for (int pid : NumericEntries("/proc")) {
        Stat st;
        if (!ReadStat(pid, st) || st.state == 'Z') continue;
        if (IsKernelThread(pid, st)) continue;  // kernel threads are not applications
        Row r;
        r.pid = static_cast<DWORD>(pid);
        r.name = Widen(ProcessName(pid, st.comm));
        r.safety = ClassifyProcess(r.pid, r.name, essential_);
        const uint64_t cpu = CpuTime100ns(st);
        const auto it = prev_.find(r.pid);
        if (it != prev_.end() && now > it->second.wall && cpu >= it->second.cpu) {
            r.cpuPercent = CpuPercentFromTicks(cpu - it->second.cpu, now - it->second.wall,
                                               system.logicalCores, CpuNormalization::PerCore);
        }
        next[r.pid] = {cpu, now};
        r.workingSetMb = RssMb(pid);
        rows.push_back(r);
    }
    prev_.swap(next);
    system.processCount = static_cast<unsigned>(rows.size());
    return rows;
}

// ---------------------------------------------------------------- safety

const char* SafetyName(ProcessSafety level) {
    switch (level) {
        case ProcessSafety::Service: return "service";
        case ProcessSafety::System: return "system";
        case ProcessSafety::Essential: return "essential";
        default: return "normal";
    }
}

bool PathIsUnder(const std::wstring& path, const std::wstring& dir) {
    if (dir.empty() || path.size() <= dir.size()) return false;
    std::wstring base = dir;
    if (base.back() != L'/') base += L'/';
    return path.size() >= base.size() && path.compare(0, base.size(), base) == 0;
}

SafetyInfo ClassifyProcess(DWORD pid, const std::wstring& name,
                           const std::vector<std::wstring>& essentialNames) {
    SafetyInfo info;
    Stat st;
    const bool haveStat = ReadStat(static_cast<int>(pid), st);
    if (pid <= 2 || (haveStat && IsKernelThread(static_cast<int>(pid), st))) {
        info.level = ProcessSafety::Essential;
        info.reason = L"Linux kernel or init process.";
        return info;
    }
    if (pid == static_cast<DWORD>(getpid())) {
        info.level = ProcessSafety::Essential;
        info.reason = L"This is the ThreadHaul service itself.";
        return info;
    }
    const std::string narrow = Narrow(name);
    const bool listed = LinuxEssentials().count(narrow) != 0 ||
                        std::find(essentialNames.begin(), essentialNames.end(), name) != essentialNames.end();
    if (listed) {
        info.level = ProcessSafety::Essential;
        info.reason = L"Essential to the system: pausing or slowing it can freeze the machine or lock you out.";
        return info;
    }

    const std::string exe = ExePath(static_cast<int>(pid));
    info.imagePath = Widen(exe);
    if (exe.empty()) {
        info.level = ProcessSafety::System;
        info.reason = L"Linux restricts access to this process; it usually belongs to the system or another user.";
        return info;
    }
    const uid_t uid = ProcessUid(static_cast<int>(pid));
    const std::string cgroup = ReadText("/proc/" + std::to_string(pid) + "/cgroup");
    for (const wchar_t* dir : {L"/usr/lib/systemd", L"/lib/systemd", L"/usr/libexec", L"/usr/sbin", L"/sbin"}) {
        if (uid == 0 && PathIsUnder(info.imagePath, dir)) {
            info.level = ProcessSafety::System;
            info.reason = L"Part of the operating system (a root process installed under a system directory).";
            return info;
        }
    }
    if (uid < 1000 || cgroup.find("/system.slice/") != std::string::npos) {
        info.level = ProcessSafety::Service;
        info.reason = L"Runs as a system service or system account.";
        return info;
    }
    info.reason = L"An ordinary application.";
    return info;
}

// ---------------------------------------------------------------- matching facts

unsigned long long ProcessStartTime(DWORD pid) {
    Stat st;
    return ReadStat(static_cast<int>(pid), st) ? st.starttime : 0;
}

std::wstring ProcessCommandLine(DWORD pid) {
    std::string raw = ReadText("/proc/" + std::to_string(pid) + "/cmdline");
    while (!raw.empty() && raw.back() == '\0') raw.pop_back();
    std::replace(raw.begin(), raw.end(), '\0', ' ');
    return Widen(raw);
}

std::wstring ProcessUser(DWORD pid) {
    const uid_t uid = ProcessUid(static_cast<int>(pid));
    if (uid == static_cast<uid_t>(-1)) return L"";
    passwd pw{};
    passwd* result = nullptr;
    char buf[4096];
    if (getpwuid_r(uid, &pw, buf, sizeof(buf), &result) == 0 && result) return Widen(pw.pw_name);
    return Widen(std::to_string(uid));
}

// The main process of a systemd unit: the oldest process whose cgroup is the
// unit's own ("<name>" or "<name>.service").
DWORD ServiceProcessId(const std::wstring& serviceName) {
    std::string unit = Narrow(serviceName);
    if (unit.find('.') == std::string::npos) unit += ".service";
    const std::string suffix = "/" + unit + "\n";
    DWORD best = 0;
    uint64_t bestStart = UINT64_MAX;
    for (int pid : NumericEntries("/proc")) {
        const std::string cg = ReadText("/proc/" + std::to_string(pid) + "/cgroup");
        if (cg.size() < suffix.size() || cg.compare(cg.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        Stat st;
        if (ReadStat(pid, st) && st.state != 'Z' && st.starttime < bestStart) {
            bestStart = st.starttime;
            best = static_cast<DWORD>(pid);
        }
    }
    return best;
}

}
