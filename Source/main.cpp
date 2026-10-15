// ThreadHaulDaemon entry point: Windows Service, console mode, and the CLI
// manual override. Run "ThreadHaulDaemon.exe help" for usage.

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "ActuatorManager.h"
#include "Config.h"
#include "ControlPipe.h"
#include "Daemon.h"
#include "PriorityControl.h"
#include "ThreadControl.h"
#include "ProcessMonitor.h"

namespace {

constexpr wchar_t kServiceName[] = L"ThreadHaulDaemon";

SERVICE_STATUS        g_status{};
SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
HANDLE                g_stopEvent = nullptr;
std::wstring          g_configPath;

// Opening processes that belong to other accounts needs SeDebugPrivilege.
// The installer grants the service only this (and SeChangeNotifyPrivilege);
// it is present but disabled in the token until we enable it here.
bool EnablePrivilege(const wchar_t* name) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid) &&
              AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr) &&
              GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

void SetState(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0) {
    static DWORD checkPoint = 1;
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwWaitHint = waitHint;
    g_status.dwControlsAccepted =
        state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    g_status.dwCheckPoint =
        (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkPoint++;
    SetServiceStatus(g_statusHandle, &g_status);
}

void ReportStartupError(const std::wstring& message) {
    HANDLE source = RegisterEventSourceW(nullptr, kServiceName);
    if (source != nullptr) {
        const std::wstring text = L"ThreadHaulDaemon could not start: " + message;
        LPCWSTR strings[] = {text.c_str()};
        ReportEventW(source, EVENTLOG_ERROR_TYPE, 0, 1900, nullptr, 1, 0, strings, nullptr);
        DeregisterEventSource(source);
    }
}

DWORD WINAPI ServiceCtrlHandler(DWORD control, DWORD, LPVOID, LPVOID) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        SetState(SERVICE_STOP_PENDING, NO_ERROR, 10000);
        SetEvent(g_stopEvent);
        return NO_ERROR;
    }
    return control == SERVICE_CONTROL_INTERROGATE ? NO_ERROR : ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI ServiceMainFunc(DWORD, LPWSTR*) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, ServiceCtrlHandler, nullptr);
    if (g_statusHandle == nullptr) {
        return;
    }
    SetState(SERVICE_START_PENDING, NO_ERROR, 5000);
    EnablePrivilege(SE_DEBUG_NAME);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    daemon::Daemon d(g_configPath);
    std::wstring error;
    if (g_stopEvent == nullptr || !d.Initialize(error)) {
        ReportStartupError(error.empty() ? L"internal error" : error);
        SetState(SERVICE_STOPPED, ERROR_BAD_CONFIGURATION);
        return;
    }
    SetState(SERVICE_RUNNING);
    d.Run(g_stopEvent);
    CloseHandle(g_stopEvent);
    SetState(SERVICE_STOPPED);
}

BOOL WINAPI ConsoleCtrlHandler(DWORD) {
    if (g_stopEvent != nullptr) {
        SetEvent(g_stopEvent);
        return TRUE;
    }
    return FALSE;
}

int RunConsole(bool allowUnelevated) {
    if (!actuator::IsRunningElevated() && !allowUnelevated) {
        std::fwprintf(stderr,
                      L"ThreadHaulDaemon must run as Administrator to throttle and suspend "
                      L"other users' processes. Re-run elevated, or pass --allow-unelevated to "
                      L"manage only processes you own (for local testing).\n");
        return 1;
    }
    EnablePrivilege(SE_DEBUG_NAME);
    daemon::Daemon d(g_configPath);
    std::wstring error;
    if (!d.Initialize(error)) {
        std::fwprintf(stderr, L"Invalid configuration (%ls):\n  %ls\n", g_configPath.c_str(),
                      error.c_str());
        return 2;
    }
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    d.AllowRemoteStop(g_stopEvent);
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    std::wprintf(L"ThreadHaulDaemon running in console mode.\n  config: %ls\nPress Ctrl+C to "
                 L"stop (suspended processes are resumed on exit).\n",
                 g_configPath.c_str());
    d.Run(g_stopEvent);
    std::wprintf(L"Stopped. All managed processes restored.\n");
    return 0;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += static_cast<char>(c < 128 ? c : '?');
    return s;
}

// Used when no daemon is running: performs the action directly. Same
// protection rules, but nothing is logged and nothing is held.
int DirectAction(const std::wstring& verb, const std::wstring& target) {
    actuator::ActuatorManager guard{actuator::StateMachineConfig{}};
    const auto snapshot = telemetry::SnapshotProcesses();
    int matched = 0;
    int failed = 0;
    bool numeric = !target.empty();
    for (wchar_t c : target) numeric = numeric && iswdigit(c);
    for (const auto& e : snapshot) {
        const bool match = numeric ? e.pid == std::wcstoul(target.c_str(), nullptr, 10)
                                   : CompareStringOrdinal(e.name.c_str(), -1, target.c_str(), -1,
                                                          TRUE) == CSTR_EQUAL;
        if (!match) continue;
        ++matched;
        if (guard.IsProtected(e.name, e.pid)) {
            std::wprintf(L"  %ls (PID %u): refused, protected process\n", e.name.c_str(), e.pid);
            ++failed;
            continue;
        }
        bool ok = false;
        if (verb == L"throttle") {
            ok = actuator::SetProcessPriority(e.pid, actuator::PriorityLevel::Idle) ==
                 actuator::PriorityChangeResult::Success;
        } else if (verb == L"suspend") {
            ok = actuator::SuspendAllThreads(e.pid).AllSucceeded();
        } else {
            const auto r = actuator::ResumeAllThreads(e.pid);
            ok = r.threadsFailed == 0 &&
                 actuator::SetProcessPriority(e.pid, actuator::PriorityLevel::Normal) ==
                     actuator::PriorityChangeResult::Success;
        }
        std::wprintf(L"  %ls (PID %u): %ls\n", e.name.c_str(), e.pid,
                     ok ? L"done" : L"failed (access denied? run as Administrator)");
        if (!ok) ++failed;
    }
    if (matched == 0) {
        std::wprintf(L"  no running process matches \"%ls\"\n", target.c_str());
        return 1;
    }
    return failed == 0 ? 0 : 1;
}

void PrintUsage() {
    std::wprintf(
        L"ThreadHaulDaemon - logistics server resource monitor & priority throttler\n\n"
        L"Service / monitor:\n"
        L"  ThreadHaulDaemon.exe --console [--config <path>] [--allow-unelevated]\n"
        L"      Run in the foreground (Ctrl+C to stop).\n"
        L"  ThreadHaulDaemon.exe [--config <path>]\n"
        L"      Entry point used by the Windows Service (install with scripts\\install.ps1).\n"
        L"  ThreadHaulDaemon.exe validate-config [<path>]\n\n"
        L"Manual override (talks to the running daemon; needs Administrator):\n"
        L"  ThreadHaulDaemon.exe throttle <pid|name>   demote to IDLE priority and hold\n"
        L"  ThreadHaulDaemon.exe suspend  <pid|name>   suspend every thread and hold\n"
        L"  ThreadHaulDaemon.exe resume   <pid|name>   resume + normal priority and hold\n"
        L"  ThreadHaulDaemon.exe release  <pid|name>   hand back to automatic control\n"
        L"  ThreadHaulDaemon.exe status                current pressure, thresholds, states\n"
        L"  ThreadHaulDaemon.exe stop                  stop a console-mode daemon cleanly\n"
        L"  ThreadHaulDaemon.exe dashboard             open the live web dashboard in your browser\n\n"
        L"If no daemon is running, throttle/suspend/resume act on the process directly.\n");
}

}

int wmain(int argc, wchar_t* argv[]) {
    std::wstring command;
    std::vector<std::wstring> positional;
    bool console = false;
    bool allowUnelevated = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--config" && i + 1 < argc) {
            g_configPath = argv[++i];
        } else if (a == L"--console") {
            console = true;
        } else if (a == L"--allow-unelevated") {
            allowUnelevated = true;
        } else if (a == L"help" || a == L"--help" || a == L"-h" || a == L"/?") {
            PrintUsage();
            return 0;
        } else if (command.empty()) {
            command = a;
        } else {
            positional.push_back(a);
        }
    }

    if (command == L"throttle" || command == L"suspend" || command == L"resume" ||
        command == L"release" || command == L"status" || command == L"stop") {
        const std::wstring target = positional.empty() ? L"" : positional[0];
        std::string response;
        const std::string request = Narrow(command + (target.empty() ? L"" : L" " + target));
        if (daemon::SendControlRequest(request, response)) {
            std::printf("%s\n", response.c_str());
            return response.find("\"ok\":true") != std::string::npos ? 0 : 1;
        }
        if (command == L"status" || command == L"release" || command == L"stop") {
            std::printf("%s\n", response.c_str());
            return 1;
        }
        if (target.empty()) {
            PrintUsage();
            return 1;
        }
        std::printf("Daemon not reachable (%s); acting directly:\n", response.c_str());
        return DirectAction(command, target);
    }

    if (command == L"dashboard") {
        std::string response;
        if (!daemon::SendControlRequest("dashboard", response)) {
            std::printf("Cannot open the dashboard: %s\n", response.c_str());
            return 1;
        }
        const std::string key = "\"url\":\"";
        const size_t at = response.find(key);
        if (at == std::string::npos) {
            std::printf("%s\n", response.c_str());
            return 1;
        }
        const std::string url = response.substr(at + key.size(),
                                                response.find('"', at + key.size()) - at - key.size());
        const std::wstring wurl(url.begin(), url.end());
        std::printf("Opening %s\n(the link contains this session's access key; do not share it)\n",
                    url.substr(0, url.find('#')).c_str());
        ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
    }

    if (command == L"validate-config") {
        const std::wstring path = !positional.empty() ? positional[0]
                                  : !g_configPath.empty() ? g_configPath
                                                          : daemon::DefaultConfigPath(true);
        try {
            config::LoadConfigFile(path);
            std::wprintf(L"OK: %ls is valid.\n", path.c_str());
            return 0;
        } catch (const config::ConfigError& e) {
            std::printf("INVALID: %s\n", e.what());
            return 2;
        }
    }

    if (!command.empty()) {
        std::fwprintf(stderr, L"Unknown command \"%ls\".\n\n", command.c_str());
        PrintUsage();
        return 1;
    }

    if (g_configPath.empty()) {
        // The SCM starts services in System32, so only console runs look
        // next to the executable first.
        g_configPath = daemon::DefaultConfigPath(console || GetConsoleWindow() != nullptr);
    }
    if (console) {
        return RunConsole(allowUnelevated);
    }

    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(kServiceName), ServiceMainFunc},
        {nullptr, nullptr},
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            // Started from a prompt rather than by the SCM.
            return RunConsole(allowUnelevated);
        }
        return static_cast<int>(GetLastError());
    }
    return 0;
}
