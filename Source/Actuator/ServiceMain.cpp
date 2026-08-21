// ServiceMain.cpp
//
// Owner: Divik (Actuator / Scheduling & Process Control layer)
// Spec:  specs/logistics-resource-monitor.md
//   - Windows Service packaging,       Review I item 6
//   - Administrator privilege handling, Review I item 7
//   - Graceful service shutdown,        Review I item 5
//
// This is the service process's entry point: registers with the Service
// Control Manager (SCM), starts a worker thread that drives the actuator
// loop, and handles SERVICE_CONTROL_STOP by resuming/restoring every
// managed batch process before the process exits.
//
// INTEGRATION NOTE (assumption, logged in build report): the real polling
// loop needs live CPU% input from Prakul's Telemetry module and target
// process names/PIDs + thresholds from Pranav's Config module. Neither
// exists yet in this repo. WorkerThreadProc below is wired to call a
// GetCriticalProcessCpuPercent() stub that must be replaced with a real
// call into the Telemetry module once it exists -- this keeps the service
// buildable and independently testable in isolation now, per the module
// ownership split.

#include <windows.h>
#include <string>
#include <vector>
#include <memory>
#include <cstdio>
#include "StateMachine.h"
#include "ActuatorManager.h"
#include "PriorityControl.h"

namespace {

constexpr wchar_t kServiceName[] = L"ThreadHaulDaemon";

SERVICE_STATUS        g_serviceStatus{};
SERVICE_STATUS_HANDLE  g_serviceStatusHandle = nullptr;
HANDLE                 g_stopEvent = nullptr;

std::unique_ptr<actuator::ActuatorManager> g_actuatorManager;

// --- STUB: replace with a real call into Prakul's Telemetry module. ---
// Returns a placeholder value so the service is independently runnable and
// testable before the Telemetry module exists. This function is the single
// integration point the team needs to rewire once Telemetry ships.
double GetCriticalProcessCpuPercent() {
    return 0.0; // placeholder: "no load" until Telemetry module is wired in
}

// --- STUB: replace with real values loaded from Pranav's Config module. ---
// Returns a placeholder single batch process so the service has something
// to manage before config.json parsing exists. This function is the single
// integration point the team needs to rewire once the Config module ships.
std::vector<std::pair<std::wstring, DWORD>> GetConfiguredBatchProcesses() {
    return {}; // placeholder: no batch processes configured yet
}

void WINAPI ServiceCtrlHandler(DWORD ctrlCode) {
    switch (ctrlCode) {
        case SERVICE_CONTROL_STOP:
            if (g_serviceStatus.dwCurrentState != SERVICE_RUNNING) {
                break;
            }
            g_serviceStatus.dwControlsAccepted = 0;
            g_serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
            g_serviceStatus.dwWin32ExitCode = 0;
            g_serviceStatus.dwCheckPoint = 4;
            SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);

            // Signal the worker loop to exit; the loop itself performs the
            // graceful-shutdown recovery (ForceRecoverAllForShutdown) before
            // this function reports SERVICE_STOPPED.
            SetEvent(g_stopEvent);
            break;
        default:
            break;
    }
}

DWORD WINAPI WorkerThreadProc(LPVOID) {
    actuator::StateMachineConfig config; // defaults per spec; Config module
                                          // (Pranav) will override these.
    g_actuatorManager = std::make_unique<actuator::ActuatorManager>(
        config,
        [](const std::wstring& name, DWORD pid, actuator::ProcessState oldState,
           actuator::ProcessState newState, double cpuPercent) {
            // INTEGRATION NOTE: this is where Pranav's JSON Lines + Windows
            // Event Log logging call belongs once that module exists.
            // Left as a no-op here so the Actuator module has no compile
            // dependency on the Logging module.
            (void)name; (void)pid; (void)oldState; (void)newState; (void)cpuPercent;
        });

    for (const auto& [name, pid] : GetConfiguredBatchProcesses()) {
        g_actuatorManager->AddBatchProcess(name, pid);
    }

    constexpr DWORD kPollIntervalMs = 1000; // 1-second polling, per spec

    while (WaitForSingleObject(g_stopEvent, kPollIntervalMs) == WAIT_TIMEOUT) {
        const double cpuPercent = GetCriticalProcessCpuPercent();
        g_actuatorManager->UpdateAll(cpuPercent);
    }

    // Graceful shutdown: resume any suspended threads / restore normal
    // priority for every managed batch process before the service exits.
    g_actuatorManager->ForceRecoverAllForShutdown();

    return 0;
}

void WINAPI ServiceMainFunc(DWORD, LPWSTR*) {
    g_serviceStatusHandle = RegisterServiceCtrlHandlerW(kServiceName, ServiceCtrlHandler);
    if (g_serviceStatusHandle == nullptr) {
        return;
    }

    ZeroMemory(&g_serviceStatus, sizeof(g_serviceStatus));
    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_serviceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_serviceStatus.dwControlsAccepted = 0;
    g_serviceStatus.dwWin32ExitCode = 0;
    g_serviceStatus.dwServiceSpecificExitCode = 0;
    g_serviceStatus.dwCheckPoint = 0;
    g_serviceStatus.dwWaitHint = 0;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stopEvent == nullptr) {
        g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
        g_serviceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
        return;
    }

    g_serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    g_serviceStatus.dwCurrentState = SERVICE_RUNNING;
    g_serviceStatus.dwCheckPoint = 0;
    g_serviceStatus.dwWaitHint = 0;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);

    HANDLE hWorker = CreateThread(nullptr, 0, WorkerThreadProc, nullptr, 0, nullptr);
    if (hWorker != nullptr) {
        WaitForSingleObject(hWorker, INFINITE);
        CloseHandle(hWorker);
    }

    CloseHandle(g_stopEvent);

    g_serviceStatus.dwControlsAccepted = 0;
    g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
    g_serviceStatus.dwWin32ExitCode = 0;
    g_serviceStatus.dwCheckPoint = 3;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    // Administrator privilege handling (Review I item 7): fail fast with a
    // clear message rather than silently failing every SetPriorityClass /
    // SuspendThread call later. The service should be installed to run as
    // LocalSystem (which is always elevated); this check mainly protects
    // against someone running the .exe directly, unelevated, for testing.
    if (!actuator::IsRunningElevated()) {
        // In a full build this would go through Pranav's logging module;
        // for the Actuator module in isolation, stderr is the simplest
        // reading that still surfaces the problem clearly.
        fwprintf(stderr,
                 L"ThreadHaulDaemon must be run with Administrator privileges "
                 L"(required for SetPriorityClass / SuspendThread on target "
                 L"processes). Re-run elevated.\n");
        return 1;
    }

    SERVICE_TABLE_ENTRYW serviceTable[] = {
        { const_cast<LPWSTR>(kServiceName), (LPSERVICE_MAIN_FUNCTIONW)ServiceMainFunc },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(serviceTable)) {
        const DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            // Not started by the SCM -- e.g. run directly from a console
            // during development. Fall back to running the worker loop
            // in the foreground so the module is testable without a full
            // service install.
            fwprintf(stdout,
                     L"Not running under the Service Control Manager; "
                     L"running in console/foreground mode for local testing. "
                     L"Press Ctrl+C to stop.\n");
            g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            WorkerThreadProc(nullptr);
            return 0;
        }
        return static_cast<int>(err);
    }

    return 0;
}
