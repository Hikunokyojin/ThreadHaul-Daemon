#include "AutoProtect.h"

#include <nlohmann/json.hpp>

#include "CpuMath.h"
#include "Utf8.h"

#ifndef _WIN32
#include <poll.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>

#include "LinuxProc.h"
#endif

namespace thd {

namespace {

int Rank(DWORD cls) {
    switch (cls) {
        case IDLE_PRIORITY_CLASS:         return 0;
        case BELOW_NORMAL_PRIORITY_CLASS: return 1;
        case NORMAL_PRIORITY_CLASS:       return 2;
        case ABOVE_NORMAL_PRIORITY_CLASS: return 3;
        case HIGH_PRIORITY_CLASS:         return 4;
        case REALTIME_PRIORITY_CLASS:     return 5;
    }
    return 2;
}

std::string Narrow(const std::wstring& w) { return utf8::FromWide(w); }

double Round1(double x) { return static_cast<long long>(x * 10.0 + 0.5) / 10.0; }

// A pinned process: Windows holds a handle, Linux a pidfd. Either way the
// PID cannot be mistaken for a new process before it is restored.
#ifdef _WIN32
HANDLE Pin(DWORD pid) {
    return OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
}
bool Exited(HANDLE h) { return WaitForSingleObject(h, 0) == WAIT_OBJECT_0; }
void Unpin(HANDLE h) { CloseHandle(h); }
DWORD GetClass(HANDLE h, DWORD) { return GetPriorityClass(h); }
bool SetClass(HANDLE h, DWORD, DWORD cls) { return SetPriorityClass(h, cls) != FALSE; }
#else
int Fd(HANDLE h) { return static_cast<int>(reinterpret_cast<intptr_t>(h)) - 1; }
HANDLE Pin(DWORD pid) {
    const long fd = syscall(SYS_pidfd_open, static_cast<pid_t>(pid), 0);
    return fd < 0 ? nullptr : reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd + 1));
}
bool Exited(HANDLE h) {
    pollfd p{Fd(h), POLLIN, 0};  // a pidfd becomes readable when the process exits
    return poll(&p, 1, 0) > 0;
}
void Unpin(HANDLE h) { close(Fd(h)); }
int NiceOf(DWORD pid) {
    errno = 0;
    const int n = getpriority(PRIO_PROCESS, static_cast<id_t>(pid));
    return errno == 0 ? n : 0;
}
DWORD ClassOfNice(int n) {
    if (n >= 19) return IDLE_PRIORITY_CLASS;
    if (n >= 5) return BELOW_NORMAL_PRIORITY_CLASS;
    if (n > -5) return NORMAL_PRIORITY_CLASS;
    if (n > -15) return ABOVE_NORMAL_PRIORITY_CLASS;
    return HIGH_PRIORITY_CLASS;
}
DWORD GetClass(HANDLE, DWORD pid) { return ClassOfNice(NiceOf(pid)); }
// nice is per thread on Linux, so every thread is set.
bool SetNice(DWORD pid, int nice) {
    bool any = false, failed = false;
    for (int tid : linuxproc::Threads(static_cast<int>(pid))) {
        if (setpriority(PRIO_PROCESS, static_cast<id_t>(tid), nice) == 0) any = true;
        else if (errno != ESRCH) failed = true;
    }
    return any && !failed;
}
bool SetClass(HANDLE, DWORD pid, DWORD cls) {
    return SetNice(pid, cls == IDLE_PRIORITY_CLASS ? 19 : cls == BELOW_NORMAL_PRIORITY_CLASS ? 10 : 0);
}
#endif

}

const char* PriorityClassName(DWORD cls) {
    switch (cls) {
        case IDLE_PRIORITY_CLASS:         return "idle";
        case BELOW_NORMAL_PRIORITY_CLASS: return "below normal";
        case NORMAL_PRIORITY_CLASS:       return "normal";
        case ABOVE_NORMAL_PRIORITY_CLASS: return "above normal";
        case HIGH_PRIORITY_CLASS:         return "high";
        case REALTIME_PRIORITY_CLASS:     return "realtime";
    }
    return "unknown";
}

AutoProtector::AutoProtector(LogFn log) : log_(std::move(log)) {
    SampleSystemCpu();
}

AutoProtector::~AutoProtector() {
    RestoreAll("service stopping");
}

void AutoProtector::Configure(const telemetry::AutoProtectSettings& settings,
                              std::vector<std::wstring> essential) {
    settings_ = settings;
    table_.SetEssentialNames(std::move(essential));
    if (!settings_.enabled) {
        RestoreAll("auto-protect disabled");
        state_.Reset();
    }
}

double AutoProtector::SampleSystemCpu() {
    FILETIME idleFt, kernelFt, userFt;
    if (!GetSystemTimes(&idleFt, &kernelFt, &userFt)) return systemCpu_;
    const uint64_t idle = telemetry::FileTimeToUint64(idleFt);
    const uint64_t total = telemetry::FileTimeToUint64(kernelFt) + telemetry::FileTimeToUint64(userFt);
    if (lastTotal_ != 0 && total > lastTotal_) {
        const double busy = 1.0 - static_cast<double>(idle - lastIdle_) /
                                      static_cast<double>(total - lastTotal_);
        systemCpu_ = busy < 0 ? 0 : (busy > 1 ? 100.0 : busy * 100.0);
    }
    lastIdle_ = idle;
    lastTotal_ = total;
    return systemCpu_;
}

void AutoProtector::Tick(const ExcludeFn& exclude) {
    const double cpu = SampleSystemCpu();
    if (!settings_.enabled) return;

    // Forget processes that have exited (their priority no longer matters).
    for (auto it = throttled_.begin(); it != throttled_.end();) {
        if (Exited(it->second.handle)) {
            Unpin(it->second.handle);
            it = throttled_.erase(it);
        } else {
            ++it;
        }
    }

    const telemetry::AutoLevel before = state_.Level();
    const telemetry::AutoLevel level = state_.Update(cpu, settings_);
    if (level != before) {
        log_("level", nullptr, 0, 0,
             std::string(telemetry::AutoLevelName(before)) + " -> " + telemetry::AutoLevelName(level) +
                 " at " + std::to_string(static_cast<int>(cpu + 0.5)) + "% system CPU");
    }

    if (level == telemetry::AutoLevel::Normal) {
        RestoreAll("system CPU back below " + std::to_string(static_cast<int>(settings_.recoverPercent)) + "%");
        // Keep per-process CPU deltas warm while load is elevated, so the
        // right processes can be chosen the moment protection starts.
        if (cpu >= settings_.recoverPercent) {
            telemetry::ProcessTable::System sys;
            table_.Sample(sys);
        }
        return;
    }

    telemetry::ProcessTable::System sys;
    const std::vector<telemetry::ProcessTable::Row> rows = table_.Sample(sys);

    // Drop exemptions for processes that are gone.
    for (auto it = exempt_.begin(); it != exempt_.end();) {
        const bool alive = std::any_of(rows.begin(), rows.end(),
                                       [&](const auto& r) { return r.pid == *it; });
        it = alive ? std::next(it) : exempt_.erase(it);
    }

    const DWORD target = level == telemetry::AutoLevel::Idle ? IDLE_PRIORITY_CLASS
                                                             : BELOW_NORMAL_PRIORITY_CLASS;

    // Escalate processes already throttled.
    for (auto& [pid, e] : throttled_) {
        if (Rank(e.currentClass) > Rank(target) && SetClass(e.handle, e.pid, target)) {
            log_("lowered_to_idle", &e, e.currentClass, target,
                 "system CPU stayed at or above " + std::to_string(static_cast<int>(settings_.idlePercent)) + "%");
            e.currentClass = target;
        }
    }

    // Add the heaviest eligible processes, up to the limit.
    if (static_cast<int>(throttled_.size()) < settings_.maxProcesses) {
        std::vector<telemetry::AutoCandidate> eligible;
        for (const auto& r : rows) {
            if (!r.accessible || r.safety.level != telemetry::ProcessSafety::Normal) continue;
            if (throttled_.count(r.pid) || exempt_.count(r.pid)) continue;
            if (exclude(r.name, r.pid)) continue;
            eligible.push_back({r.pid, r.name, r.cpuPercent});
        }
        const auto chosen = telemetry::SelectAutoTargets(
            eligible, settings_.minProcessCpuPercent,
            static_cast<size_t>(settings_.maxProcesses) - throttled_.size());
        for (const auto& c : chosen) {
            for (const auto& r : rows) {
                if (r.pid == c.pid) {
                    Lower(r, target);
                    break;
                }
            }
        }
    }
}

bool AutoProtector::Lower(const telemetry::ProcessTable::Row& row, DWORD targetClass) {
    HANDLE h = Pin(row.pid);
    if (h == nullptr) return false;
#ifndef _WIN32
    const int originalNice = NiceOf(row.pid);
#endif
    const DWORD original = GetClass(h, row.pid);
    if (original == 0 || Rank(original) <= Rank(targetClass) || !SetClass(h, row.pid, targetClass)) {
        Unpin(h);  // already low enough, or not allowed
        return false;
    }
    Entry e;
    e.pid = row.pid;
    e.name = row.name;
    e.handle = h;
    e.originalClass = original;
#ifndef _WIN32
    e.originalNice = originalNice;
#endif
    e.currentClass = targetClass;
    e.cpuAtAction = row.cpuPercent;
    e.sinceTick = GetTickCount64();
    const auto& stored = throttled_.emplace(row.pid, e).first->second;
    log_("throttled", &stored, original, targetClass,
         "using " + std::to_string(static_cast<int>(row.cpuPercent + 0.5)) + "% CPU while the system was at " +
             std::to_string(static_cast<int>(systemCpu_ + 0.5)) + "%");
    return true;
}

void AutoProtector::Restore(std::map<DWORD, Entry>::iterator it, const std::string& reason) {
    Entry& e = it->second;
    if (!Exited(e.handle)) {
#ifdef _WIN32
        SetPriorityClass(e.handle, e.originalClass);
#else
        SetNice(e.pid, e.originalNice);
#endif
        log_("restored", &e, e.currentClass, e.originalClass, reason);
    }
    Unpin(e.handle);
    throttled_.erase(it);
}

void AutoProtector::RestoreAll(const std::string& reason) {
    while (!throttled_.empty()) {
        Restore(throttled_.begin(), reason);
    }
}

bool AutoProtector::Exempt(DWORD pid, const std::wstring& name) {
    const auto it = throttled_.find(pid);
    if (it != throttled_.end()) {
        Restore(it, "restored and exempted by an administrator");
    }
    exempt_.insert(pid);
    Entry e;
    e.pid = pid;
    e.name = name;
    log_("exempted", &e, 0, 0, "excluded from auto-protect until it exits");
    return true;
}

std::map<DWORD, std::string> AutoProtector::States() const {
    std::map<DWORD, std::string> out;
    for (const auto& [pid, e] : throttled_) {
        out[pid] = std::string("auto: ") + PriorityClassName(e.currentClass);
    }
    return out;
}

std::string AutoProtector::StatusJson() const {
    using nlohmann::ordered_json;
    ordered_json j;
    j["enabled"] = settings_.enabled;
    j["level"] = telemetry::AutoLevelName(state_.Level());
    j["system_cpu_percent"] = Round1(systemCpu_);
    j["throttle_at_percent"] = settings_.throttlePercent;
    j["idle_at_percent"] = settings_.useIdle ? settings_.idlePercent : 0.0;
    j["recover_below_percent"] = settings_.recoverPercent;
    j["max_processes"] = settings_.maxProcesses;
    ordered_json list = ordered_json::array();
    const ULONGLONG now = GetTickCount64();
    for (const auto& [pid, e] : throttled_) {
        list.push_back({{"pid", pid},
                        {"name", Narrow(e.name)},
                        {"original_priority", PriorityClassName(e.originalClass)},
                        {"current_priority", PriorityClassName(e.currentClass)},
                        {"cpu_at_action", Round1(e.cpuAtAction)},
                        {"seconds", (now - e.sinceTick) / 1000}});
    }
    j["processes"] = list;
    ordered_json ex = ordered_json::array();
    for (DWORD pid : exempt_) ex.push_back(pid);
    j["exempt_pids"] = ex;
    return j.dump();
}

}
