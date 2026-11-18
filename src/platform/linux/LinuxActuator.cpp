// Linux implementation of PriorityControl.h, ThreadControl.h and Actions.h.
//
//   Windows                      Linux
//   priority class Idle/BelowN.  nice 19 / nice 10 on every thread
//   suspend all threads          SIGSTOP / SIGCONT (the whole process)
//   I/O priority very low/low    ioprio class idle / best-effort level 7
//   affinity mask                sched_setaffinity on every thread
//   Job Object CPU hard cap      cgroup v2 cpu.max in /sys/fs/cgroup/threadhaul
//
// Nice, I/O priority and affinity are per thread on Linux, so each is applied
// to every thread in /proc/<pid>/task; threads created later inherit them.

#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "Actions.h"
#include "LinuxProc.h"
#include "PriorityControl.h"
#include "ThreadControl.h"

namespace actuator {

using namespace linuxproc;

namespace {

constexpr int kIoprioWhoProcess = 1;
constexpr int kIoprioClassShift = 13;
constexpr int kIoprioClassBe = 2;
constexpr int kIoprioClassIdle = 3;

int IoprioGet(int tid) { return static_cast<int>(syscall(SYS_ioprio_get, kIoprioWhoProcess, tid)); }
int IoprioSet(int tid, int value) {
    return static_cast<int>(syscall(SYS_ioprio_set, kIoprioWhoProcess, tid, value));
}

std::string Errno(const char* what) { return std::string(what) + " failed (" + std::strerror(errno) + ")"; }

int PopCount(DWORD_PTR v) {
    int n = 0;
    for (; v; v &= v - 1) ++n;
    return n;
}

constexpr const char* kCgroupRoot = "/sys/fs/cgroup";
constexpr const char* kCgroupParent = "/sys/fs/cgroup/threadhaul";

std::string CapGroup(DWORD pid) { return std::string(kCgroupParent) + "/pid-" + std::to_string(pid); }

// The process's cgroup v2 path ("/user.slice/..."), from the "0::" line.
std::string CurrentCgroup(DWORD pid) {
    std::istringstream s(ReadText("/proc/" + std::to_string(pid) + "/cgroup"));
    for (std::string line; std::getline(s, line);) {
        if (line.compare(0, 3, "0::") == 0) return line.substr(3);
    }
    return {};
}

bool EnsureCapParent(std::string& error) {
    if (ReadText(std::string(kCgroupRoot) + "/cgroup.controllers").find("cpu") == std::string::npos) {
        error = "cgroup v2 with the cpu controller is required for CPU caps";
        return false;
    }
    mkdir(kCgroupParent, 0755);  // EEXIST is fine
    // The cpu controller must be enabled on the path down to our groups.
    WriteText(std::string(kCgroupRoot) + "/cgroup.subtree_control", "+cpu");
    if (!WriteText(std::string(kCgroupParent) + "/cgroup.subtree_control", "+cpu")) {
        error = Errno("enabling the cpu controller under /sys/fs/cgroup/threadhaul");
        return false;
    }
    return true;
}

}

// ---------------------------------------------------------------- priority

PriorityChangeResult SetProcessPriority(DWORD processId, PriorityLevel level) {
    const int nice = level == PriorityLevel::Idle ? 19 : level == PriorityLevel::BelowNormal ? 10 : 0;
    const std::vector<int> threads = Threads(static_cast<int>(processId));
    if (threads.empty()) return PriorityChangeResult::ProcessNotFound;
    PriorityChangeResult result = PriorityChangeResult::Success;
    for (int tid : threads) {
        if (setpriority(PRIO_PROCESS, static_cast<id_t>(tid), nice) != 0 && errno != ESRCH) {
            result = errno == EPERM || errno == EACCES ? PriorityChangeResult::AccessDenied
                                                       : PriorityChangeResult::OtherError;
        }
    }
    return result;
}

bool IsRunningElevated() { return geteuid() == 0; }

// ---------------------------------------------------------------- freeze

ThreadOpResult SuspendAllThreads(DWORD processId) {
    // SIGSTOP stops every thread at once and cannot be caught or ignored.
    ThreadOpResult r;
    r.threadsAttempted = 1;
    if (kill(static_cast<pid_t>(processId), SIGSTOP) == 0) r.threadsSucceeded = 1;
    else r.threadsFailed = 1;
    return r;
}

ThreadOpResult ResumeAllThreads(DWORD processId) {
    ThreadOpResult r;
    r.threadsAttempted = 1;
    if (kill(static_cast<pid_t>(processId), SIGCONT) == 0) r.threadsSucceeded = 1;
    else r.threadsFailed = 1;
    return r;
}

// ---------------------------------------------------------------- I/O priority

// Same scale as Windows: 0 very low, 1 low, 2 normal.
bool GetIoPriority(DWORD pid, ULONG& priority) {
    const int v = IoprioGet(static_cast<int>(pid));
    if (v < 0) return false;
    const int cls = v >> kIoprioClassShift, data = v & ((1 << kIoprioClassShift) - 1);
    priority = cls == kIoprioClassIdle ? 0 : (cls == kIoprioClassBe && data == 7) ? 1 : 2;
    return true;
}

bool SetIoPriority(DWORD pid, ULONG priority) {
    // Normal (2) returns to "none": the kernel then derives it from the nice value.
    const int value = priority == 0 ? (kIoprioClassIdle << kIoprioClassShift)
                    : priority == 1 ? ((kIoprioClassBe << kIoprioClassShift) | 7)
                                    : 0;
    const std::vector<int> threads = Threads(static_cast<int>(pid));
    bool ok = !threads.empty();
    for (int tid : threads) {
        if (IoprioSet(tid, value) != 0 && errno != ESRCH) ok = false;
    }
    return ok;
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

namespace {

bool GetAffinity(DWORD pid, DWORD_PTR& mask) {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(static_cast<pid_t>(pid), sizeof(set), &set) != 0) return false;
    mask = 0;
    for (int i = 0; i < static_cast<int>(sizeof(DWORD_PTR) * 8); ++i) {
        if (CPU_ISSET(i, &set)) mask |= static_cast<DWORD_PTR>(1) << i;
    }
    return true;
}

bool SetAffinity(DWORD pid, DWORD_PTR mask) {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < static_cast<int>(sizeof(DWORD_PTR) * 8); ++i) {
        if (mask & (static_cast<DWORD_PTR>(1) << i)) CPU_SET(i, &set);
    }
    const std::vector<int> threads = Threads(static_cast<int>(pid));
    bool ok = !threads.empty();
    for (int tid : threads) {
        if (sched_setaffinity(tid, sizeof(set), &set) != 0 && errno != ESRCH) ok = false;
    }
    return ok;
}

}

// ---------------------------------------------------------------- actions

ProcessActions::~ProcessActions() {
    if (job_) {
        std::string ignored;
        SetCpuRate(0, ignored);  // never leave a cap behind
    }
}

// percent is of the whole machine, as on Windows: cpu.max allows
// percent * cores / 100 CPUs' worth of time per 100 ms period.
bool ProcessActions::SetCpuRate(int percent, std::string& error) {
    if (percent == cpuRate_) return true;
    const std::string group = CapGroup(pid_);
    if (percent > 0 && !job_) {
        if (!EnsureCapParent(error)) return false;
        originalCgroup_ = CurrentCgroup(pid_);
        if (originalCgroup_.empty()) {
            error = "reading the process's cgroup failed";
            return false;
        }
        if (mkdir(group.c_str(), 0755) != 0 && errno != EEXIST) {
            error = Errno(("creating " + group).c_str());
            return false;
        }
        if (!WriteText(group + "/cgroup.procs", std::to_string(pid_))) {
            error = Errno("moving the process into its cap cgroup");
            rmdir(group.c_str());
            return false;
        }
        job_ = reinterpret_cast<HANDLE>(1);  // "the process is in our cgroup"
    }
    if (!job_) return true;  // lifting a cap that was never set
    if (percent > 0) {
        const unsigned long long quota =
            std::max(1000ull, static_cast<unsigned long long>(percent) * LogicalCores() * 1000ull);
        if (!WriteText(group + "/cpu.max", std::to_string(quota) + " 100000")) {
            error = Errno("writing cpu.max");
            return false;
        }
        cpuRate_ = percent;
        return true;
    }
    // Lift: put the process back where it was and remove our group. If the
    // original group refuses it, lift the limit in place instead.
    const bool movedBack = WriteText(std::string(kCgroupRoot) + originalCgroup_ + "/cgroup.procs", std::to_string(pid_));
    if (movedBack || ReadText("/proc/" + std::to_string(pid_) + "/stat").empty()) {
        rmdir(group.c_str());
        job_ = nullptr;
    } else if (!WriteText(group + "/cpu.max", "max 100000")) {
        error = Errno("lifting cpu.max");
        return false;
    }
    cpuRate_ = 0;
    return true;
}

bool ProcessActions::Throttle(const ActionPolicy& p, std::string& error) {
    bool ok = true;
    auto fail = [&](const std::string& e) {
        if (ok) error = e;
        ok = false;
    };

    if (SetProcessPriority(pid_, p.throttlePriority) != PriorityChangeResult::Success) {
        fail("setpriority failed (permission denied or process gone)");
    }
    if (p.throttleIo != IoPriority::Unchanged) {
        ULONG current = 2;
        if (!ioChanged_ && GetIoPriority(pid_, current)) originalIo_ = current;
        if (SetIoPriority(pid_, p.throttleIo == IoPriority::VeryLow ? 0 : 1)) ioChanged_ = true;
        else fail("setting I/O priority failed");
    }
    if (p.throttleAffinityCores > 0) {
        DWORD_PTR procMask = 0;
        if (GetAffinity(pid_, procMask)) {
            const DWORD_PTR original = affinityChanged_ ? originalAffinity_ : procMask;
            const DWORD_PTR target = HighestCores(original, p.throttleAffinityCores);
            if (PopCount(target) < PopCount(original)) {
                if (SetAffinity(pid_, target)) {
                    if (!affinityChanged_) originalAffinity_ = procMask;
                    affinityChanged_ = true;
                } else {
                    fail(Errno("sched_setaffinity"));
                }
            }
        } else {
            fail(Errno("reading the CPU affinity"));
        }
    }
    std::string e;
    if (!SetCpuRate(p.throttleCpuRatePercent, e)) fail(e);
    return ok;
}

bool ProcessActions::Suspend(const ActionPolicy& p, std::string& error) {
    if (p.suspendMode == SuspendMode::CpuRate) return SetCpuRate(p.suspendCpuRatePercent, error);
    frozen_ = true;
    if (!SuspendAllThreads(pid_).AllSucceeded()) {
        error = Errno("SIGSTOP");
        return false;
    }
    return true;
}

bool ProcessActions::Unsuspend(const ActionPolicy& p, std::string& error) {
    bool ok = true;
    if (frozen_) {
        ok = ResumeAllThreads(pid_).AllSucceeded();
        if (!ok) error = Errno("SIGCONT");
        frozen_ = false;
    }
    std::string e;
    if (!SetCpuRate(p.throttleCpuRatePercent, e)) {
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
        if (!ResumeAllThreads(pid_).AllSucceeded()) fail(Errno("SIGCONT"));
        frozen_ = false;
    }
    std::string e;
    if (!SetCpuRate(0, e)) fail(e);
    if (affinityChanged_) {
        if (!SetAffinity(pid_, originalAffinity_)) fail(Errno("restoring the CPU affinity"));
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
