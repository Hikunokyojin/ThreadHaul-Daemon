// Linux engine tests: run the real /proc telemetry and the real actions
// against child processes. CPU-cap checks need root (cgroup writes) and are
// skipped otherwise.

#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "Actions.h"
#include "LinuxProc.h"
#include "ProcessMatch.h"
#include "ProcessMonitor.h"
#include "ProcessSafety.h"
#include "ProcessTable.h"
#include "StateMachine.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* test, const std::string& what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        std::printf("FAIL  %s: %s\n", test, what.c_str());
    }
}

// A child that spins, so it uses one core.
pid_t SpawnSpinner() {
    const pid_t pid = fork();
    if (pid == 0) {
        volatile unsigned long x = 0;
        for (;;) x = x + 1;
    }
    return pid;
}

void Kill(pid_t pid) {
    kill(pid, SIGCONT);
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
}

void Sleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

char State(pid_t pid) {
    linuxproc::Stat st;
    return linuxproc::ReadStat(pid, st) ? st.state : '?';
}

int Nice(pid_t pid) {
    errno = 0;
    return getpriority(PRIO_PROCESS, static_cast<id_t>(pid));
}

void Helpers() {
    const char* t = "Helpers";
    const std::wstring cafe{L'c', L'a', L'f', static_cast<wchar_t>(0xE9)};
    const std::string cafeUtf8{'c', 'a', 'f', static_cast<char>(0xC3), static_cast<char>(0xA9)};
    const wchar_t bad = static_cast<wchar_t>(0xFFFD);
    Check(linuxproc::Widen(cafeUtf8) == cafe, t, "UTF-8 decode");
    Check(linuxproc::Narrow(cafe) == cafeUtf8, t, "UTF-8 encode");
    Check(linuxproc::Widen(std::string{'a', static_cast<char>(0xFF)}) == std::wstring{L'a', bad}, t,
          "invalid byte replaced");
    Check(linuxproc::Widen(std::string(1, static_cast<char>(0xC3))) == std::wstring(1, bad), t,
          "truncated sequence replaced");
    Check(telemetry::PathIsUnder(L"/usr/sbin/cron", L"/usr/sbin") &&
              !telemetry::PathIsUnder(L"/usr/sbinx/a", L"/usr/sbin"),
          t, "path containment");
}

void Telemetry() {
    const char* t = "Telemetry";
    const pid_t child = SpawnSpinner();
    const std::wstring self = linuxproc::Widen(linuxproc::ProcessName(getpid(), ""));
    const auto snap = telemetry::SnapshotProcesses();
    Check(!telemetry::FindByName(snap, self).empty(), t, "snapshot finds this test by executable name");

    telemetry::TrackedProcess tracked(self, telemetry::CpuNormalization::PerCore, linuxproc::LogicalCores());
    std::vector<telemetry::ProcessSample> samples;
    std::vector<telemetry::LifecycleEvent> events;
    tracked.Poll(snap, samples, events);
    Check(!events.empty() && events[0].kind == telemetry::LifecycleKind::Acquired, t, "instances acquired");
    Sleep(500);
    samples.clear();
    events.clear();
    tracked.Poll(telemetry::SnapshotProcesses(), samples, events);
    double childCpu = -1;
    for (const auto& s : samples) {
        if (s.pid == static_cast<DWORD>(child) && s.valid) childCpu = s.cpuPercent;
    }
    Check(childCpu > 50.0, t, "spinning child above 50% of a core (got " + std::to_string(childCpu) + ")");

    Kill(child);
    samples.clear();
    events.clear();
    tracked.Poll(telemetry::SnapshotProcesses(), samples, events);
    bool exited = false;
    for (const auto& e : events) {
        exited = exited || (e.pid == static_cast<DWORD>(child) && e.kind == telemetry::LifecycleKind::Exited);
    }
    Check(exited, t, "exit detected");

    telemetry::ProcessTable table;
    telemetry::ProcessTable::System sys;
    table.Sample(sys);
    Sleep(200);
    const auto rows = table.Sample(sys);
    Check(!rows.empty() && sys.processCount == rows.size(), t, "process table lists processes");
    Check(sys.memoryTotalGb > 0 && sys.memoryLoadPercent > 0 && sys.memoryLoadPercent < 100, t, "memory figures");
    Check(sys.cpuPercent >= 0 && sys.cpuPercent <= 100 && sys.logicalCores >= 1, t, "system CPU figures");
    Check(telemetry::SystemMemoryLoadPercent() > 0, t, "memory load");

    double c, w, p;
    DWORD h;
    telemetry::SelfMonitor selfMon;
    selfMon.Sample(c, w, p, h);
    Check(w > 0 && h >= 3, t, "self monitor reads memory and open files");
}

void Safety() {
    const char* t = "Safety";
    using telemetry::ProcessSafety;
    Check(telemetry::ClassifyProcess(1, L"systemd", {}).level == ProcessSafety::Essential, t, "PID 1 essential");
    Check(telemetry::ClassifyProcess(getpid(), L"x", {}).level == ProcessSafety::Essential, t, "self essential");
    const pid_t child = SpawnSpinner();
    Check(telemetry::ClassifyProcess(child, L"sshd", {}).level == ProcessSafety::Essential, t, "sshd essential");
    Check(telemetry::ClassifyProcess(child, L"app", {L"app"}).level == ProcessSafety::Essential, t,
          "configured essential name");
    const auto level = telemetry::ClassifyProcess(child, L"app", {}).level;
    // Under a systemd service (a CI runner) or a system account the child is
    // correctly a "service"; from a login session it is an ordinary application.
    const bool serviceContext = getuid() < 1000 ||
        linuxproc::ReadText("/proc/self/cgroup").find("/system.slice/") != std::string::npos;
    Check(level == (serviceContext ? ProcessSafety::Service : ProcessSafety::Normal), t,
          std::string("own child is ") + (serviceContext ? "a service" : "an ordinary application"));
    Kill(child);
    Check(std::string(telemetry::SafetyName(ProcessSafety::Service)) == "service", t, "safety names");
}

void Matching() {
    const char* t = "Matching";
    const DWORD me = static_cast<DWORD>(getpid());
    Check(telemetry::ProcessCommandLine(me).find(L"test") != std::wstring::npos, t, "command line read");
    Check(!telemetry::ProcessUser(me).empty(), t, "user name read");
    telemetry::MatchCondition c;
    c.user = telemetry::ProcessUser(me);
    telemetry::ProcessFacts f{telemetry::ProcessCommandLine(me), telemetry::ProcessUser(me)};
    Check(telemetry::ConditionMatches(c, me, f, 0), t, "user condition matches");
    c.user = L"someone-else";
    Check(!telemetry::ConditionMatches(c, me, f, 0), t, "other user does not match");
    Check(telemetry::ServiceProcessId(L"no-such-unit-threadhaul") == 0, t, "unknown unit has no PID");
}

void Actions() {
    const char* t = "Actions";
    using namespace actuator;
    const pid_t child = SpawnSpinner();
    Sleep(50);
    {
        ProcessActions a(static_cast<DWORD>(child));
        ActionPolicy p;
        p.throttlePriority = PriorityLevel::Idle;
        p.throttleIo = IoPriority::VeryLow;
        p.throttleAffinityCores = linuxproc::LogicalCores() > 1 ? 1 : 0;
        std::string err;
        Check(a.Throttle(p, err), t, "throttle: " + err);
        Check(Nice(child) == 19, t, "nice 19 after throttle");
        ULONG io = 2;
        Check(GetIoPriority(child, io) && io == 0, t, "I/O priority idle after throttle");
        if (p.throttleAffinityCores) {
            cpu_set_t set;
            CPU_ZERO(&set);
            sched_getaffinity(child, sizeof(set), &set);
            Check(CPU_COUNT(&set) == 1, t, "confined to one core");
        }
        Check(a.Suspend(p, err) && a.Frozen(), t, "suspend: " + err);
        Sleep(50);
        Check(State(child) == 'T', t, "process stopped");
        Check(a.Unsuspend(p, err) && !a.Frozen(), t, "unsuspend: " + err);
        Sleep(50);
        Check(State(child) == 'R' || State(child) == 'S', t, "process running again");
        const bool restored = a.Restore(err);
        // Lowering nice back to 0 needs root (or CAP_SYS_NICE).
        if (getuid() == 0) {
            Check(restored, t, "restore: " + err);
            Check(Nice(child) == 0, t, "nice 0 after restore");
        }
        Check(GetIoPriority(child, io) && io == 2, t, "I/O priority normal after restore");
        if (p.throttleAffinityCores) {
            cpu_set_t set;
            CPU_ZERO(&set);
            sched_getaffinity(child, sizeof(set), &set);
            Check(CPU_COUNT(&set) == static_cast<int>(linuxproc::LogicalCores()), t, "affinity restored");
        }
    }
    Check(HighestCores(0xF, 2) == 0xC, t, "highest cores chosen");
    Kill(child);
}

void StateMachineExit() {
    const char* t = "StateMachineExit";
    const pid_t child = SpawnSpinner();
    actuator::ProcessStateMachine sm(L"spin", static_cast<DWORD>(child), actuator::StateMachineConfig{});
    Check(!sm.HasExited(), t, "running child not exited");
    Kill(child);
    Check(sm.HasExited(), t, "killed child exited");
}

void CpuCap() {
    const char* t = "CpuCap";
    if (getuid() != 0) {
        std::printf("skip  CpuCap (needs root)\n");
        return;
    }
    using namespace actuator;
    const pid_t child = SpawnSpinner();
    const std::string group = "/sys/fs/cgroup/threadhaul/pid-" + std::to_string(child);
    const unsigned cores = linuxproc::LogicalCores();
    {
        ProcessActions a(static_cast<DWORD>(child));
        ActionPolicy p;
        p.throttlePriority = PriorityLevel::Normal;
        p.throttleCpuRatePercent = 1;  // 1% of the machine
        std::string err;
        Check(a.Throttle(p, err), t, "cap applied: " + err);
        Check(linuxproc::ReadText(group + "/cgroup.procs").find(std::to_string(child)) != std::string::npos, t,
              "process moved into its cgroup");
        Check(linuxproc::ReadText(group + "/cpu.max") == std::to_string(cores * 1000) + " 100000\n", t,
              "cpu.max set");
        telemetry::TrackedProcess tp(linuxproc::Widen(linuxproc::ProcessName(child, "")),
                                     telemetry::CpuNormalization::PerCore, cores);
        std::vector<telemetry::ProcessSample> s;
        std::vector<telemetry::LifecycleEvent> e;
        tp.Poll(telemetry::SnapshotProcesses(), s, e);
        Sleep(1000);
        s.clear();
        tp.Poll(telemetry::SnapshotProcesses(), s, e);
        double cpu = 100;
        for (const auto& x : s) {
            if (x.pid == static_cast<DWORD>(child) && x.valid) cpu = x.cpuPercent;
        }
        Check(cpu < cores * 1.0 + 5, t, "capped child stays near its cap (got " + std::to_string(cpu) + "%)");
        Check(a.Restore(err), t, "cap lifted: " + err);
        Check(access(group.c_str(), F_OK) != 0, t, "cap cgroup removed");
    }
    Kill(child);
}

}

int main() {
    Helpers();
    Telemetry();
    Safety();
    Matching();
    Actions();
    StateMachineExit();
    CpuCap();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
