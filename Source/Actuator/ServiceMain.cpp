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

double GetCriticalProcessCpuPercent() {
    return 0.0;
}

std::vector<std::pair<std::wstring, DWORD>> GetConfiguredBatchProcesses() {
    return {};
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
            SetEvent(g_stopEvent);
            break;
        default:
            break;
    }
}

DWORD WINAPI WorkerThreadProc(LPVOID) {
    actuator::StateMachineConfig config;
    g_actuatorManager = std::make_unique<actuator::ActuatorManager>(
        config,
        [](const std::wstring& name, DWORD pid, actuator::ProcessState oldState,
           actuator::ProcessState newState, double cpuPercent) {
            (void)name; (void)pid; (void)oldState; (void)newState; (void)cpuPercent;
        });

    for (const auto& [name, pid] : GetConfiguredBatchProcesses()) {
        g_actuatorManager->AddBatchProcess(name, pid);
    }

    constexpr DWORD kPollIntervalMs = 1000;

    while (WaitForSingleObject(g_stopEvent, kPollIntervalMs) == WAIT_TIMEOUT) {
        const double cpuPercent = GetCriticalProcessCpuPercent();
        g_actuatorManager->UpdateAll(cpuPercent);
    }
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

}

int wmain(int argc, wchar_t* argv[]) {
    if (!actuator::IsRunningElevated()) {
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
