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

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
