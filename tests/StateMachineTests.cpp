// Unit tests for the throttle/escalate/recover state machine.
// Uses a PID that does not exist, so no real process is ever touched: every
// OS call fails harmlessly and the tests observe state transitions through
// the TransitionCallback.

#include "StateMachine.h"
#include "ActuatorManager.h"
#include <cstdio>
#include <vector>

using namespace actuator;

namespace {

constexpr DWORD kFakePid = 999999;

struct Transition {
    ProcessState from;
    ProcessState to;
    double cpu;
    bool actuationSucceeded;
};

struct Recorder {
    std::vector<Transition> log;
    TransitionCallback Callback() {
        return [this](const std::wstring&, DWORD, ProcessState from, ProcessState to,
                      double cpu, bool ok) { log.push_back({from, to, cpu, ok}); };
    }
};

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* test, const char* what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        std::printf("FAIL  %s: %s\n", test, what);
    }
}

void Feed(ProcessStateMachine& m, std::initializer_list<double> samples) {
    for (double s : samples) m.Update(s);
}

void StartsNormal() {
    ProcessStateMachine m(L"batch.exe", kFakePid, {});
    Check(m.CurrentState() == ProcessState::Normal, __func__, "initial state is Normal");
}

void ThrottlesAfterThreeHighSamples() {
    Recorder r;
    ProcessStateMachine m(L"batch.exe", kFakePid, {}, r.Callback());
    Feed(m, {86, 87});
    Check(m.CurrentState() == ProcessState::Normal, __func__, "2 high samples: still Normal");
    Check(m.Update(88), __func__, "3rd high sample reports a change");
    Check(m.CurrentState() == ProcessState::Throttled, __func__, "3rd high sample: Throttled");
    Check(r.log.size() == 1, __func__, "exactly one transition");
    Check(r.log[0].from == ProcessState::Normal && r.log[0].to == ProcessState::Throttled,
          __func__, "transition Normal->Throttled");
    Check(r.log[0].cpu == 88, __func__, "CPU% at transition is the triggering sample");
}

void HighSamplesMustBeConsecutive() {
    ProcessStateMachine m(L"batch.exe", kFakePid, {});
    Feed(m, {90, 90, 50, 90, 90});
    Check(m.CurrentState() == ProcessState::Normal, __func__, "a dip resets the high counter");
}

void ThresholdBoundaries() {
    ProcessStateMachine high(L"batch.exe", kFakePid, {});
    Feed(high, {85.0, 85.0, 85.0});
    Check(high.CurrentState() == ProcessState::Throttled, __func__, "exactly 85% counts as high");

    ProcessStateMachine below(L"batch.exe", kFakePid, {});
    Feed(below, {84.99, 84.99, 84.99});
    Check(below.CurrentState() == ProcessState::Normal, __func__, "84.99% does not count");

    ProcessStateMachine low(L"batch.exe", kFakePid, {});
    Feed(low, {90, 90, 90});
    Feed(low, {60.0, 60.0, 60.0, 60.0, 60.0});
    Check(low.CurrentState() == ProcessState::Throttled, __func__,
          "exactly 60% does not count as low");
}

void EscalatesAfterFiveCriticalSamples() {
    Recorder r;
    ProcessStateMachine m(L"batch.exe", kFakePid, {}, r.Callback());
    Feed(m, {95, 95, 95});
    Check(m.CurrentState() == ProcessState::Throttled, __func__, "throttled first");
    // Counters reset on transition, so the 5 critical samples start fresh here.
    Feed(m, {95, 95, 95, 95});
    Check(m.CurrentState() == ProcessState::Throttled, __func__, "4 critical: still Throttled");
    m.Update(95);
    Check(m.CurrentState() == ProcessState::Suspended, __func__, "5th critical: Suspended");
    Check(r.log.size() == 2 && r.log[1].to == ProcessState::Suspended, __func__,
          "transition Throttled->Suspended recorded");
}

void HighButNotCriticalDoesNotEscalate() {
    ProcessStateMachine m(L"batch.exe", kFakePid, {});
    Feed(m, {87, 87, 87, 87, 87, 87, 87, 87, 87, 87});
    Check(m.CurrentState() == ProcessState::Throttled, __func__, "87% never suspends");
}

void RecoversFromThrottled() {
    Recorder r;
    ProcessStateMachine m(L"batch.exe", kFakePid, {}, r.Callback());
    Feed(m, {90, 90, 90});
    Feed(m, {40, 40, 40, 40});
    Check(m.CurrentState() == ProcessState::Throttled, __func__, "4 low: still Throttled");
    m.Update(40);
    Check(m.CurrentState() == ProcessState::Normal, __func__, "5th low: Normal");
    Check(r.log.back().from == ProcessState::Throttled, __func__, "recovered from Throttled");
}

void RecoversFromSuspended() {
    Recorder r;
    ProcessStateMachine m(L"batch.exe", kFakePid, {}, r.Callback());
    Feed(m, {95, 95, 95, 95, 95, 95, 95, 95});
    Check(m.CurrentState() == ProcessState::Suspended, __func__, "suspended first");
    Feed(m, {10, 10, 10, 10, 10});
    Check(m.CurrentState() == ProcessState::Normal, __func__, "5 low: Normal");
    Check(r.log.back().from == ProcessState::Suspended, __func__, "recovered from Suspended");
}

void LowSamplesMustBeConsecutive() {
    ProcessStateMachine m(L"batch.exe", kFakePid, {});
    Feed(m, {90, 90, 90});
    Feed(m, {40, 40, 40, 40, 70, 40, 40, 40, 40});
    Check(m.CurrentState() == ProcessState::Throttled, __func__, "a spike resets the low counter");
}

void NormalIgnoresLowSamples() {
    Recorder r;
    ProcessStateMachine m(L"batch.exe", kFakePid, {}, r.Callback());
    Feed(m, {5, 5, 5, 5, 5, 5, 5});
    Check(m.CurrentState() == ProcessState::Normal && r.log.empty(), __func__,
          "low samples in Normal fire no transition");
}

void ForceRecoverForShutdown() {
    Recorder r;
    ProcessStateMachine m(L"batch.exe", kFakePid, {}, r.Callback());
    m.ForceRecoverForShutdown();
    Check(r.log.empty(), __func__, "no-op when already Normal");

    Feed(m, {95, 95, 95, 95, 95, 95, 95, 95});
    m.ForceRecoverForShutdown();
    Check(m.CurrentState() == ProcessState::Normal, __func__, "Suspended -> Normal on shutdown");
    Check(r.log.back().from == ProcessState::Suspended, __func__, "shutdown transition recorded");
}

void ReportsFailedActuationForMissingProcess() {
    Recorder r;
    ProcessStateMachine m(L"batch.exe", kFakePid, {}, r.Callback());
    Feed(m, {90, 90, 90});
    Check(!r.log.empty() && !r.log[0].actuationSucceeded, __func__,
          "actuation on a nonexistent PID is reported as failed");
}

// The one test that uses a real process: a short-lived ping child. It checks
// the SYNCHRONIZE-only handle can tell a running process from an exited one,
// which is what keeps a recycled PID from being acted on.
void DetectsExitOfRealProcess() {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    wchar_t cmd[] = L"ping -n 3 127.0.0.1";
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        Check(false, __func__, "could not start ping child");
        return;
    }
    ProcessStateMachine m(L"PING.EXE", pi.dwProcessId, {});
    Check(!m.HasExited(), __func__, "running process is not reported as exited");
    WaitForSingleObject(pi.hProcess, 10000);
    Check(m.HasExited(), __func__, "exited process is reported as exited");
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

void CustomConfigIsRespected() {
    StateMachineConfig cfg;
    cfg.throttleThresholdPercent = 50;
    cfg.throttleConsecutiveSamples = 1;
    ProcessStateMachine m(L"batch.exe", kFakePid, cfg);
    m.Update(55);
    Check(m.CurrentState() == ProcessState::Throttled, __func__, "1 sample at 55% with 50%/1 config");
}

void ManagerForwardsThresholdEvents() {
    Recorder r;
    ActuatorManager mgr({}, r.Callback());
    mgr.AddBatchProcess(L"batch.exe", kFakePid);
    ThresholdEvent evt{4821, ThresholdType::High, 90.0, std::chrono::system_clock::now()};
    mgr.OnThresholdEvent(evt);
    mgr.OnThresholdEvent(evt);
    mgr.OnThresholdEvent(evt);
    Check(r.log.size() == 1 && r.log[0].to == ProcessState::Throttled, __func__,
          "3 events at 90% throttle the managed batch process");
}

void ManualHoldBlocksAutomaticTransitions() {
    Recorder r;
    ProcessStateMachine m(L"batch.exe", kFakePid, {}, r.Callback());
    m.ForceState(ProcessState::Suspended, 10.0, true);
    Check(m.CurrentState() == ProcessState::Suspended, __func__, "forced straight to Suspended");
    Check(m.IsManuallyHeld(), __func__, "hold flag set");
    Feed(m, {10, 10, 10, 10, 10, 10});
    Check(m.CurrentState() == ProcessState::Suspended, __func__, "low samples ignored while held");
    m.ReleaseManualHold();
    Feed(m, {10, 10, 10, 10, 10});
    Check(m.CurrentState() == ProcessState::Normal, __func__, "recovers after release");
}

void ForceStateFromAnyState() {
    Recorder r;
    ProcessStateMachine m(L"batch.exe", kFakePid, {}, r.Callback());
    m.ForceState(ProcessState::Throttled, 0, true);
    m.ForceState(ProcessState::Suspended, 0, true);
    m.ForceState(ProcessState::Throttled, 0, true);
    m.ForceState(ProcessState::Normal, 0, false);
    Check(r.log.size() == 4, __func__, "four forced transitions recorded");
    Check(r.log.size() == 4 && r.log[2].from == ProcessState::Suspended &&
              r.log[2].to == ProcessState::Throttled,
          __func__, "Suspended -> Throttled allowed manually");
    m.ForceState(ProcessState::Normal, 0, false);
    Check(r.log.size() == 4, __func__, "forcing the current state is a no-op");
}

void SetConfigKeepsCounters() {
    ProcessStateMachine m(L"batch.exe", kFakePid, {});
    Feed(m, {82, 82});
    StateMachineConfig lower;
    lower.throttleThresholdPercent = 80;
    m.SetConfig(lower);
    m.Update(82);
    Check(m.CurrentState() == ProcessState::Normal, __func__,
          "samples below the old threshold were not counted retroactively");
    Feed(m, {82, 82});
    Check(m.CurrentState() == ProcessState::Throttled, __func__, "new threshold applies");
}

void ManagerSetConfigAndForce() {
    Recorder r;
    ActuatorManager mgr({}, r.Callback());
    mgr.AddBatchProcess(L"batch.exe", kFakePid);
    StateMachineConfig c;
    c.throttleConsecutiveSamples = 1;
    mgr.SetConfig(c);
    mgr.UpdateAll(86);
    Check(r.log.size() == 1, __func__, "manager pushes config to existing machines");
    Check(mgr.ForceState(kFakePid, ProcessState::Normal, 0, true), __func__, "force on managed pid");
    Check(!mgr.ForceState(12345678, ProcessState::Normal, 0, true), __func__, "unknown pid refused");
    Check(mgr.Find(kFakePid)->IsManuallyHeld(), __func__, "held");
    Check(mgr.ReleaseManualHold(kFakePid) && !mgr.Find(kFakePid)->IsManuallyHeld(), __func__, "released");
}

void ProtectedProcessesNeverManaged() {
    ActuatorManager mgr({});
    mgr.SetProtectedProcessNames({L"explorer.exe"});
    Check(!mgr.AddBatchProcess(L"EXPLORER.EXE", kFakePid), __func__, "configured name, any case");
    Check(!mgr.AddBatchProcess(L"csrss.exe", kFakePid), __func__, "built-in protection");
    Check(!mgr.AddBatchProcess(L"batch.exe", GetCurrentProcessId()), __func__, "own process");
    Check(mgr.ManagedCount() == 0, __func__, "nothing managed");
}

void HighestCoresMask() {
    Check(HighestCores(0xF, 2) == 0xC, __func__, "two highest of four cores");
    Check(HighestCores(0xA, 1) == 0x8, __func__, "skips cores the process cannot use");
    Check(HighestCores(0x3, 5) == 0x3, __func__, "asking for more cores than exist keeps them all");
    Check(HighestCores(0, 3) == 0, __func__, "no cores available");
}

int Bits(DWORD_PTR v) {
    int n = 0;
    for (; v; v &= v - 1) ++n;
    return n;
}

// Runs every action against a real child process we started (so we have the
// rights), checks each with the OS's own query calls, then checks the restore.
void ActionsOnRealProcess() {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    wchar_t cmd[] = L"ping -n 60 127.0.0.1";
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Check(false, __func__, "could not start ping child");
        return;
    }
    DWORD_PTR origMask = 0, sysMask = 0;
    GetProcessAffinityMask(pi.hProcess, &origMask, &sysMask);
    ULONG origIo = 99;
    Check(GetIoPriority(pi.dwProcessId, origIo) && origIo == 2, __func__, "child starts at normal I/O priority");

    StateMachineConfig cfg;
    cfg.actions.throttlePriority = PriorityLevel::BelowNormal;
    cfg.actions.throttleIo = IoPriority::VeryLow;
    cfg.actions.throttleAffinityCores = 1;
    cfg.actions.throttleCpuRatePercent = 20;
    cfg.actions.suspendMode = SuspendMode::CpuRate;
    cfg.actions.suspendCpuRatePercent = 2;
    Recorder rec;
    ProcessStateMachine m(L"PING.EXE", pi.dwProcessId, cfg, rec.Callback());

    m.ForceState(ProcessState::Throttled, 0, true);
    Check(!rec.log.empty() && rec.log.back().actuationSucceeded, __func__, "throttle actions all succeeded");
    if (!m.LastActionError().empty()) std::printf("      (%s)\n", m.LastActionError().c_str());
    Check(GetPriorityClass(pi.hProcess) == BELOW_NORMAL_PRIORITY_CLASS, __func__, "priority is Below Normal");
    ULONG io = 99;
    Check(GetIoPriority(pi.dwProcessId, io) && io == 0, __func__, "I/O priority is very low");
    DWORD_PTR mask = 0;
    GetProcessAffinityMask(pi.hProcess, &mask, &sysMask);
    Check(Bits(origMask) < 2 || Bits(mask) == 1, __func__, "confined to one core");
    Check(Bits(origMask) < 2 || (mask & origMask) == mask, __func__, "the core is one it could already use");
    BOOL inJob = FALSE;
    Check(IsProcessInJob(pi.hProcess, nullptr, &inJob) && inJob, __func__, "placed in a CPU-rate job");

    m.ForceState(ProcessState::Suspended, 0, true);
    Check(rec.log.back().actuationSucceeded, __func__, "cpu_rate suspend succeeded");
    Check(WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT, __func__, "process keeps running under cpu_rate suspend");

    m.ForceState(ProcessState::Throttled, 0, true);
    Check(rec.log.back().actuationSucceeded, __func__, "back to throttled from cpu_rate suspend");

    // Change the policy before recovering: restore must still undo what was applied.
    m.SetConfig(StateMachineConfig{});
    m.ForceState(ProcessState::Normal, 0, true);
    Check(rec.log.back().actuationSucceeded, __func__, "restore succeeded");
    if (!m.LastActionError().empty()) std::printf("      (%s)\n", m.LastActionError().c_str());
    Check(GetPriorityClass(pi.hProcess) == NORMAL_PRIORITY_CLASS, __func__, "priority restored");
    Check(GetIoPriority(pi.dwProcessId, io) && io == origIo, __func__, "I/O priority restored");
    GetProcessAffinityMask(pi.hProcess, &mask, &sysMask);
    Check(mask == origMask, __func__, "affinity restored exactly");

    // Default policy: freeze and thaw.
    m.ForceState(ProcessState::Suspended, 0, true);
    Check(rec.log.back().actuationSucceeded, __func__, "freeze succeeded");
    m.ForceState(ProcessState::Normal, 0, true);
    Check(rec.log.back().actuationSucceeded, __func__, "thaw succeeded");

    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

void GroupsHaveIndependentPolicies() {
    StateMachineConfig tight;            // group 0: throttle at 85
    StateMachineConfig loose;            // group 1: throttle only at 98
    loose.throttleThresholdPercent = 98;
    loose.escalateThresholdPercent = 99;
    loose.recoverThresholdPercent = 50;
    ActuatorManager mgr(tight);
    mgr.SetGroupConfigs({tight, loose});
    mgr.AddBatchProcess(L"a.exe", kFakePid, 0);
    mgr.AddBatchProcess(L"b.exe", kFakePid + 1, 1);
    mgr.AddBatchProcess(L"c.exe", kFakePid + 2, 7);  // unknown group -> default policy
    Check(mgr.GroupOf(kFakePid + 1) == 1 && mgr.GroupOf(kFakePid + 2) == 7, __func__, "group recorded");
    Check(mgr.Find(kFakePid + 1)->Config().throttleThresholdPercent == 98, __func__, "group policy applied");
    Check(mgr.Find(kFakePid + 2)->Config().throttleThresholdPercent == 85, __func__, "unknown group uses default policy");

    // Same 90% pressure in both groups: only the tight group throttles.
    for (int i = 0; i < 3; ++i) mgr.UpdateGroups({90, 90}, {true, true});
    Check(mgr.Find(kFakePid)->CurrentState() == ProcessState::Throttled, __func__, "group 0 throttles at 90");
    Check(mgr.Find(kFakePid + 1)->CurrentState() == ProcessState::Normal, __func__, "group 1 ignores 90");

    // Group 1's own pressure, not group 0's, drives it.
    for (int i = 0; i < 3; ++i) mgr.UpdateGroups({10, 99}, {true, true});
    Check(mgr.Find(kFakePid + 1)->CurrentState() == ProcessState::Throttled, __func__, "group 1 throttles on its pressure");

    // Inactive group: its machines are not updated (no recovery on a missing sample).
    for (int i = 0; i < 10; ++i) mgr.UpdateGroups({10, 0}, {true, false});
    Check(mgr.Find(kFakePid + 1)->CurrentState() == ProcessState::Throttled, __func__, "inactive group untouched");
    Check(mgr.Find(kFakePid)->CurrentState() == ProcessState::Normal, __func__, "active group recovers");

    StateMachineConfig looser = loose;
    looser.throttleThresholdPercent = 97;
    mgr.SetGroupConfigs({tight, looser});
    Check(mgr.Find(kFakePid + 1)->Config().throttleThresholdPercent == 97, __func__, "SetGroupConfigs re-applies");
    mgr.SetConfig(loose);
    Check(mgr.Find(kFakePid)->Config().throttleThresholdPercent == 98 &&
              mgr.Find(kFakePid + 1)->Config().throttleThresholdPercent == 97,
          __func__, "SetConfig changes only the default group");
}

void ActionErrorsAreReported() {
    StateMachineConfig cfg;
    cfg.actions.throttleCpuRatePercent = 10;
    ProcessStateMachine m(L"missing.exe", kFakePid, cfg);
    m.ForceState(ProcessState::Throttled, 0, true);
    Check(!m.LastActionError().empty(), __func__, "a failed action names the reason");
}

}

int main() {
    StartsNormal();
    ThrottlesAfterThreeHighSamples();
    HighSamplesMustBeConsecutive();
    ThresholdBoundaries();
    EscalatesAfterFiveCriticalSamples();
    HighButNotCriticalDoesNotEscalate();
    RecoversFromThrottled();
    RecoversFromSuspended();
    LowSamplesMustBeConsecutive();
    NormalIgnoresLowSamples();
    ForceRecoverForShutdown();
    ReportsFailedActuationForMissingProcess();
    DetectsExitOfRealProcess();
    CustomConfigIsRespected();
    ManagerForwardsThresholdEvents();
    ManualHoldBlocksAutomaticTransitions();
    ForceStateFromAnyState();
    SetConfigKeepsCounters();
    ManagerSetConfigAndForce();
    ProtectedProcessesNeverManaged();
    HighestCoresMask();
    ActionsOnRealProcess();
    ActionErrorsAreReported();
    GroupsHaveIndependentPolicies();

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
