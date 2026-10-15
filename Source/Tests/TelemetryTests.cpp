// Unit tests for the Telemetry layer: CPU%-from-ticks conversion, the
// consecutive-sample threshold tracker, and predictive/adaptive thresholds.

#include "AdaptiveThresholds.h"
#include "CpuMath.h"
#include "ProcessMonitor.h"
#include "ThresholdTracker.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace telemetry;

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

bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

constexpr uint64_t kSecond = 10'000'000;  // 100 ns ticks

// ---- CPU% from ticks ----

void PerCoreOneBusyCore() {
    Check(Near(CpuPercentFromTicks(kSecond, kSecond, 8, CpuNormalization::PerCore), 100.0),
          __func__, "1 s of CPU in 1 s = 100% of one core");
}

void PerCoreHalfCore() {
    Check(Near(CpuPercentFromTicks(kSecond / 2, kSecond, 8, CpuNormalization::PerCore), 50.0),
          __func__, "0.5 s of CPU in 1 s = 50%");
}

void PerCoreMultiThreadExceeds100() {
    Check(Near(CpuPercentFromTicks(4 * kSecond, kSecond, 8, CpuNormalization::PerCore), 400.0),
          __func__, "4 busy cores = 400% per-core");
}

void TotalSystemDividesByCores() {
    Check(Near(CpuPercentFromTicks(4 * kSecond, kSecond, 8, CpuNormalization::TotalSystem), 50.0),
          __func__, "4 of 8 cores = 50% of the system");
    Check(Near(CpuPercentFromTicks(kSecond, kSecond, 4, CpuNormalization::TotalSystem), 25.0),
          __func__, "1 of 4 cores = 25% of the system");
}

void ClampsToPhysicalMaximum() {
    // Timer skew can make CPU time exceed wall time slightly.
    Check(Near(CpuPercentFromTicks(20 * kSecond, kSecond, 8, CpuNormalization::PerCore), 800.0),
          __func__, "per-core clamps at 100 * cores");
    Check(Near(CpuPercentFromTicks(20 * kSecond, kSecond, 8, CpuNormalization::TotalSystem), 100.0),
          __func__, "total clamps at 100");
}

void ZeroWallTimeIsZero() {
    Check(CpuPercentFromTicks(kSecond, 0, 8, CpuNormalization::PerCore) == 0.0, __func__,
          "no wall time elapsed returns 0");
    Check(CpuPercentFromTicks(0, kSecond, 8, CpuNormalization::PerCore) == 0.0, __func__,
          "idle process returns 0");
}

void ZeroCoresTreatedAsOne() {
    Check(Near(CpuPercentFromTicks(kSecond, kSecond, 0, CpuNormalization::TotalSystem), 100.0),
          __func__, "0 cores does not divide by zero");
}

void FileTimeConversion() {
    FILETIME ft{0x89ABCDEF, 0x01234567};
    Check(FileTimeToUint64(ft) == 0x0123456789ABCDEFull, __func__, "high/low words combined");
}

// ---- Threshold tracker ----

actuator::StateMachineConfig Defaults() { return {}; }

void TrackerFiresHighOnThirdSample() {
    ThresholdTracker t;
    Check(t.Add(86, Defaults()).empty(), __func__, "sample 1");
    Check(t.Add(86, Defaults()).empty(), __func__, "sample 2");
    const auto fired = t.Add(86, Defaults());
    Check(fired.size() == 1 && fired[0].type == actuator::ThresholdType::High &&
              fired[0].consecutiveSamples == 3,
          __func__, "sample 3 fires high");
    Check(t.Add(86, Defaults()).empty(), __func__, "fires once per streak");
    t.Reset();
    t.Add(86, Defaults());
    t.Add(86, Defaults());
    Check(!t.Add(86, Defaults()).empty(), __func__, "fires again after Reset");
}

void TrackerBreaksStreak() {
    ThresholdTracker t;
    t.Add(90, Defaults());
    t.Add(90, Defaults());
    t.Add(70, Defaults());
    Check(t.Add(90, Defaults()).empty(), __func__, "a dip resets the high count");
}

void TrackerFiresCriticalOnFifth() {
    ThresholdTracker t;
    int criticalAt = 0;
    for (int i = 1; i <= 6; ++i) {
        for (const auto& f : t.Add(95, Defaults())) {
            if (f.type == actuator::ThresholdType::Critical) criticalAt = i;
        }
    }
    Check(criticalAt == 5, __func__, "critical fires on the 5th consecutive >= 90% sample");
}

void TrackerFiresLow() {
    ThresholdTracker t;
    int lowAt = 0;
    for (int i = 1; i <= 5; ++i) {
        for (const auto& f : t.Add(59.9, Defaults())) {
            if (f.type == actuator::ThresholdType::Low) lowAt = i;
        }
    }
    Check(lowAt == 5, __func__, "low fires on the 5th consecutive < 60% sample");
    ThresholdTracker u;
    for (int i = 0; i < 5; ++i) u.Add(60.0, Defaults());
    Check(u.Add(60.0, Defaults()).empty(), __func__, "exactly 60% is not below 60%");
}

// ---- Adaptive thresholds ----

AdaptiveSettings Enabled() {
    AdaptiveSettings s;
    s.enabled = true;
    s.windowSamples = 5;
    s.horizonSamples = 3;
    s.maxAdjustPercent = 10;
    return s;
}

void AdaptiveDisabledIsStatic() {
    AdaptiveThresholds a;  // disabled
    for (double v : {20, 40, 60, 80}) a.AddSample(v);
    const auto c = a.Adjust(Defaults());
    Check(c.throttleThresholdPercent == 85 && c.escalateThresholdPercent == 90, __func__,
          "disabled: thresholds unchanged");
    Check(a.Adjust(Defaults(), 5.0).throttleThresholdPercent == 80, __func__,
          "disabled: controller bias still applies");
}

void SlopeOfLinearRamp() {
    AdaptiveThresholds a(Enabled());
    for (double v : {50, 55, 60, 65, 70}) a.AddSample(v);
    Check(Near(a.Slope(), 5.0), __func__, "slope of +5 per sample");
    Check(Near(a.Forecast(), 85.0), __func__, "forecast 3 samples ahead of 70 is 85");
}

void RisingLoadLowersThresholds() {
    AdaptiveThresholds a(Enabled());
    for (double v : {50, 55, 60, 65, 70}) a.AddSample(v);
    const auto c = a.Adjust(Defaults());
    // slope 5 * horizon 3 = 15, capped at maxAdjust 10.
    Check(Near(c.throttleThresholdPercent, 75.0), __func__, "throttle lowered by the 10-point cap");
    Check(Near(c.escalateThresholdPercent, 80.0), __func__, "escalate lowered by the same amount");
    Check(Near(c.recoverThresholdPercent, 60.0), __func__, "recovery threshold never moves");
    Check(c.throttleConsecutiveSamples == 3, __func__, "sample counts unchanged");
}

void GentleRiseAdjustsProportionally() {
    AdaptiveThresholds a(Enabled());
    for (double v : {80, 81, 82, 83, 84}) a.AddSample(v);
    // slope 1, forecast 87 >= 85, offset 1 * 3 = 3.
    Check(Near(a.Adjust(Defaults()).throttleThresholdPercent, 82.0), __func__, "offset = slope * horizon");
}

void RisingButFarBelowDoesNotAdjust() {
    AdaptiveThresholds a(Enabled());
    for (double v : {10, 12, 14, 16, 18}) a.AddSample(v);
    Check(Near(a.Adjust(Defaults()).throttleThresholdPercent, 85.0), __func__,
          "forecast 24% is far below 85%: no change");
}

void FlatOrFallingDoesNotAdjust() {
    AdaptiveThresholds flat(Enabled());
    for (int i = 0; i < 5; ++i) flat.AddSample(88);
    Check(Near(flat.Adjust(Defaults()).throttleThresholdPercent, 85.0), __func__, "flat load");
    AdaptiveThresholds falling(Enabled());
    for (double v : {99, 95, 91, 87, 83}) falling.AddSample(v);
    Check(Near(falling.Adjust(Defaults()).throttleThresholdPercent, 85.0), __func__, "falling load");
}

void NeedsThreeSamples() {
    AdaptiveThresholds a(Enabled());
    a.AddSample(10);
    a.AddSample(90);
    Check(a.Slope() == 0.0, __func__, "two samples give no trend");
}

void WindowDropsOldSamples() {
    AdaptiveThresholds a(Enabled());
    for (double v : {100, 0, 0, 0, 0, 0}) a.AddSample(v);
    Check(Near(a.Slope(), 0.0), __func__, "the 100 fell out of the 5-sample window");
}

void ControllerBiasAddsUnderCap() {
    AdaptiveThresholds a(Enabled());
    for (int i = 0; i < 5; ++i) a.AddSample(40);
    Check(Near(a.Adjust(Defaults(), 5.0).throttleThresholdPercent, 80.0), __func__,
          "cluster bias of 5 lowers throttle to 80");
    Check(Near(a.Adjust(Defaults(), 50.0).throttleThresholdPercent, 75.0), __func__,
          "bias is capped at max_adjust_percent");
    Check(Near(a.Adjust(Defaults(), -5.0).throttleThresholdPercent, 85.0), __func__,
          "negative bias ignored");
}

void KeepsHysteresisBand() {
    AdaptiveSettings s = Enabled();
    s.maxAdjustPercent = 50;
    AdaptiveThresholds a(s);
    for (double v : {0, 30, 60, 90, 100}) a.AddSample(v);
    const auto c = a.Adjust(Defaults());
    Check(c.throttleThresholdPercent > c.recoverThresholdPercent, __func__,
          "throttle stays above recovery even with a huge offset");
    Check(Near(c.throttleThresholdPercent, 61.0), __func__, "limited to low + 1");
}

// ---- Process discovery (real OS) ----

void FindsOwnProcessByName() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring exe = path;
    exe = exe.substr(exe.find_last_of(L"\\/") + 1);
    const auto snap = SnapshotProcesses();
    bool found = false;
    for (DWORD pid : FindByName(snap, exe)) found = found || pid == GetCurrentProcessId();
    Check(found, __func__, "this test process is found by its executable name");

    std::wstring upper = exe;
    for (auto& ch : upper) ch = static_cast<wchar_t>(towupper(ch));
    Check(!FindByName(snap, upper).empty(), __func__, "name match is case-insensitive");
    Check(FindByName(snap, L"no_such_process_xyz.exe").empty(), __func__, "missing name -> none");
}

void TracksMissingThenRunningProcess() {
    TrackedProcess missing(L"no_such_process_xyz.exe", CpuNormalization::PerCore, 4);
    std::vector<ProcessSample> samples;
    std::vector<LifecycleEvent> events;
    missing.Poll(SnapshotProcesses(), samples, events);
    Check(samples.empty() && events.empty(), __func__, "absent process: no samples, no crash");

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring exe = path;
    exe = exe.substr(exe.find_last_of(L"\\/") + 1);
    TrackedProcess self(exe, CpuNormalization::PerCore, 4);
    self.Poll(SnapshotProcesses(), samples, events);
    Check(!events.empty() && events[0].kind == LifecycleKind::Acquired, __func__,
          "first sight reports Acquired");
    Check(!samples.empty() && !samples[0].valid, __func__, "first sample has no CPU delta yet");
    samples.clear();
    volatile double x = 0;
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < 200) x += 1.0;
    self.Poll(SnapshotProcesses(), samples, events);
    Check(!samples.empty() && samples[0].valid && samples[0].cpuPercent > 10.0, __func__,
          "busy loop shows up as CPU usage");
    Check(!samples.empty() && samples[0].workingSetMb > 0.0, __func__, "working set sampled");
}

void DetectsExitAndCrash() {
    // Start a child that exits with code 3 and watch it as a tracked process.
    wchar_t cmd[] = L"cmd.exe /c exit 3";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        nullptr, nullptr, &si, &pi)) {
        Check(false, __func__, "could not start cmd.exe");
        return;
    }
    TrackedProcess t(L"cmd.exe", CpuNormalization::PerCore, 4);
    std::vector<ProcessSample> samples;
    std::vector<LifecycleEvent> events;
    t.Poll(SnapshotProcesses(), samples, events);
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, 5000);
    events.clear();
    t.Poll(SnapshotProcesses(), samples, events);
    bool crashed = false;
    for (const auto& e : events) {
        crashed = crashed || (e.kind == LifecycleKind::Crashed && e.pid == pi.dwProcessId && e.exitCode == 3);
    }
    Check(crashed, __func__, "non-zero exit reported as Crashed with its exit code");
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

}

int main() {
    PerCoreOneBusyCore();
    PerCoreHalfCore();
    PerCoreMultiThreadExceeds100();
    TotalSystemDividesByCores();
    ClampsToPhysicalMaximum();
    ZeroWallTimeIsZero();
    ZeroCoresTreatedAsOne();
    FileTimeConversion();

    TrackerFiresHighOnThirdSample();
    TrackerBreaksStreak();
    TrackerFiresCriticalOnFifth();
    TrackerFiresLow();

    AdaptiveDisabledIsStatic();
    SlopeOfLinearRamp();
    RisingLoadLowersThresholds();
    GentleRiseAdjustsProportionally();
    RisingButFarBelowDoesNotAdjust();
    FlatOrFallingDoesNotAdjust();
    NeedsThreeSamples();
    WindowDropsOldSamples();
    ControllerBiasAddsUnderCap();
    KeepsHysteresisBand();

    FindsOwnProcessByName();
    TracksMissingThenRunningProcess();
    DetectsExitAndCrash();

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
