#include "Actions.h"

#include "ThreadControl.h"

namespace actuator {

namespace {

// I/O priority has no documented API for other processes; this is the same
// NtSetInformationProcess(ProcessIoPriority) call that Process Explorer and
// Process Lasso use. Resolved at run time so nothing links against ntdll.lib.
constexpr ULONG kProcessIoPriority = 33;
using NtQueryFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using NtSetFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG);

template <typename Fn>
Fn Ntdll(const char* name) {
    HMODULE h = GetModuleHandleW(L"ntdll.dll");
    return h ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(h, name))) : nullptr;
}

std::string LastError(const char* what) {
    return std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")";
}

int PopCount(DWORD_PTR v) {
    int n = 0;
    for (; v; v &= v - 1) ++n;
    return n;
}

}

bool GetIoPriority(DWORD pid, ULONG& priority) {
    static const auto query = Ntdll<NtQueryFn>("NtQueryInformationProcess");
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h || !query) {
        if (h) CloseHandle(h);
        return false;
    }
    ULONG value = 2, len = 0;
    const LONG st = query(h, kProcessIoPriority, &value, sizeof(value), &len);
    CloseHandle(h);
    if (st < 0) return false;
    priority = value;
    return true;
}

bool SetIoPriority(DWORD pid, ULONG priority) {
    static const auto set = Ntdll<NtSetFn>("NtSetInformationProcess");
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    if (!h || !set) {
        if (h) CloseHandle(h);
        return false;
    }
    const LONG st = set(h, kProcessIoPriority, &priority, sizeof(priority));
    CloseHandle(h);
    return st >= 0;
}

DWORD_PTR HighestCores(DWORD_PTR available, int cores) {
    DWORD_PTR out = 0;
    for (int bit = static_cast<int>(sizeof(DWORD_PTR) * 8) - 1; bit >= 0 && cores > 0; --bit) {
        const DWORD_PTR b = static_cast<DWORD_PTR>(1) << bit;
        if (available & b) {
            out |= b;
            --cores;
        }
    }
    return out;
}

ProcessActions::~ProcessActions() {
    if (job_) {
        std::string ignored;
        SetCpuRate(0, ignored);  // a job lives as long as its process; never leave a cap behind
        CloseHandle(job_);
    }
}

bool ProcessActions::SetCpuRate(int percent, std::string& error) {
    if (percent == cpuRate_) return true;
    if (percent > 0 && !job_) {
        job_ = CreateJobObjectW(nullptr, nullptr);
        if (!job_) {
            error = LastError("CreateJobObject");
            return false;
        }
        HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, pid_);
        const bool assigned = h && AssignProcessToJobObject(job_, h);
        if (!assigned) error = LastError("AssignProcessToJobObject");
        if (h) CloseHandle(h);
        if (!assigned) {
            CloseHandle(job_);
            job_ = nullptr;
            return false;
        }
    }
    if (!job_) return true;  // lifting a cap that was never set
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION info{};
    if (percent > 0) {
        info.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        info.CpuRate = static_cast<DWORD>(percent) * 100;  // units of 1/100 percent of the machine
    }
    if (!SetInformationJobObject(job_, JobObjectCpuRateControlInformation, &info, sizeof(info))) {
        error = LastError("SetInformationJobObject(cpu rate)");
        return false;
    }
    cpuRate_ = percent;
    return true;
}

bool ProcessActions::Throttle(const ActionPolicy& p, std::string& error) {
    bool ok = true;
    auto fail = [&](const std::string& e) {
        if (ok) error = e;
        ok = false;
    };

    if (SetProcessPriority(pid_, p.throttlePriority) != PriorityChangeResult::Success) {
        fail("SetPriorityClass failed (access denied or process gone)");
    }

    if (p.throttleIo != IoPriority::Unchanged) {
        ULONG current = 2;
        if (!ioChanged_ && GetIoPriority(pid_, current)) originalIo_ = current;
        if (SetIoPriority(pid_, p.throttleIo == IoPriority::VeryLow ? 0 : 1)) {
            ioChanged_ = true;
        } else {
            fail("setting I/O priority failed");
        }
    }

    if (p.throttleAffinityCores > 0) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION, FALSE, pid_);
        DWORD_PTR procMask = 0, sysMask = 0;
        if (h && GetProcessAffinityMask(h, &procMask, &sysMask)) {
            const DWORD_PTR original = affinityChanged_ ? originalAffinity_ : procMask;
            const DWORD_PTR target = HighestCores(original, p.throttleAffinityCores);
            if (PopCount(target) < PopCount(original)) {  // only if it actually narrows the set
                if (SetProcessAffinityMask(h, target)) {
                    if (!affinityChanged_) originalAffinity_ = procMask;
                    affinityChanged_ = true;
                } else {
                    fail(LastError("SetProcessAffinityMask"));
                }
            }
        } else {
            fail(LastError("reading the CPU affinity"));
        }
        if (h) CloseHandle(h);
    }

    std::string e;
    if (!SetCpuRate(p.throttleCpuRatePercent, e)) fail(e);
    return ok;
}

bool ProcessActions::Suspend(const ActionPolicy& p, std::string& error) {
    if (p.suspendMode == SuspendMode::CpuRate) {
        return SetCpuRate(p.suspendCpuRatePercent, error);
    }
    if (!SuspendAllThreads(pid_).AllSucceeded()) {
        error = "suspending one or more threads failed";
        frozen_ = true;  // some threads may be suspended; Resume undoes those
        return false;
    }
    frozen_ = true;
    return true;
}

bool ProcessActions::Unsuspend(const ActionPolicy& p, std::string& error) {
    bool ok = true;
    if (frozen_) {
        ok = ResumeAllThreads(pid_).AllSucceeded();
        if (!ok) error = "resuming one or more threads failed";
        frozen_ = false;
    }
    std::string e;
    if (!SetCpuRate(p.throttleCpuRatePercent, e)) {  // back to the throttle-stage cap (or none)
        if (ok) error = e;
        ok = false;
    }
    return ok;
}

bool ProcessActions::Restore(std::string& error) {
    bool ok = true;
    auto fail = [&](const std::string& e) {
        if (ok) error = e;
        ok = false;
    };
    if (frozen_) {
        if (!ResumeAllThreads(pid_).AllSucceeded()) fail("resuming one or more threads failed");
        frozen_ = false;
    }
    std::string e;
    if (!SetCpuRate(0, e)) fail(e);
    if (affinityChanged_) {
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid_);
        if (!h || !SetProcessAffinityMask(h, originalAffinity_)) fail(LastError("restoring the CPU affinity"));
        if (h) CloseHandle(h);
        affinityChanged_ = false;
    }
    if (ioChanged_) {
        if (!SetIoPriority(pid_, originalIo_)) fail("restoring I/O priority failed");
        ioChanged_ = false;
    }
    if (SetProcessPriority(pid_, PriorityLevel::Normal) != PriorityChangeResult::Success) {
        fail("restoring normal priority failed");
    }
    return ok;
}

}
