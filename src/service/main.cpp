// ThreadHaulDaemon entry point: Windows Service (or a systemd service on
// Linux), console mode, and the CLI manual override. Run
// "ThreadHaulDaemon.exe help" (Linux: "threadhauld help") for usage.

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

#include <clocale>
#include <cstdarg>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <thread>
#endif

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "ActuatorManager.h"
#include "Config.h"
#include "ControlPipe.h"
#include "Daemon.h"
#include "LogChain.h"
#include "Secrets.h"
#include "SelfCheck.h"
#include "PriorityControl.h"
#include "ThreadControl.h"
#include "ProcessMonitor.h"
#include "Utf8.h"

namespace {

HANDLE                g_stopEvent = nullptr;
std::wstring          g_configPath;

#ifndef _WIN32
// Wide output that works next to narrow printf: glibc fixes a stream's
// width on first use, so format to a wide buffer and write UTF-8.
int WPrintf(FILE* f, const wchar_t* fmt, ...) {
    std::wstring buf(8192, L'\0');
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vswprintf(buf.data(), buf.size(), fmt, ap);
    va_end(ap);
    if (n < 0) return n;
    buf.resize(static_cast<size_t>(n));
    std::fputs(utf8::FromWide(buf).c_str(), f);
    return n;
}
#define wprintf(...) WPrintf(stdout, __VA_ARGS__)
#define fwprintf WPrintf

// systemd readiness/stopping notices (sd_notify), without libsystemd.
void Notify(const char* state) {
    const char* path = std::getenv("NOTIFY_SOCKET");
    if (!path || (path[0] != '/' && path[0] != '@')) return;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const size_t len = std::strlen(path);
    if (len >= sizeof(addr.sun_path)) return;
    std::memcpy(addr.sun_path, path, len);
    if (addr.sun_path[0] == '@') addr.sun_path[0] = '\0';  // abstract namespace
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    sendto(fd, state, std::strlen(state), MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&addr),
           static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + len));
    close(fd);
}

// SIGTERM (systemctl stop), SIGINT (Ctrl+C) and SIGHUP set the stop event.
// Blocked before any thread starts, so only this waiter receives them.
void BlockStopSignals(sigset_t& set) {
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

void WaitForStopSignal(sigset_t set) {
    std::thread([set] {
        int sig = 0;
        sigwait(&set, &sig);
        Notify("STOPPING=1");
        SetEvent(g_stopEvent);
    }).detach();
}
#endif

#ifdef _WIN32
constexpr wchar_t kServiceName[] = L"ThreadHaulDaemon";

SERVICE_STATUS        g_status{};
SERVICE_STATUS_HANDLE g_statusHandle = nullptr;

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

    thd::Daemon d(g_configPath);
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
#else
bool EnablePrivilege(const wchar_t*) { return true; }  // root already has what it needs
#define SE_DEBUG_NAME L""

// Linux service mode: systemd runs the daemon in the foreground; errors go
// to stderr, which journald records.
int RunLinuxService() {
    sigset_t set;
    BlockStopSignals(set);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    WaitForStopSignal(set);
    thd::Daemon d(g_configPath);
    std::wstring error;
    if (!d.Initialize(error)) {
        fwprintf(stderr, L"ThreadHaulDaemon could not start: %ls\n", error.empty() ? L"internal error" : error.c_str());
        Notify("STATUS=configuration rejected");
        return 2;
    }
    Notify("READY=1\nSTATUS=monitoring");
    d.Run(g_stopEvent);
    return 0;
}
#endif

int RunConsole(bool allowUnelevated) {
    if (!actuator::IsRunningElevated() && !allowUnelevated) {
        fwprintf(stderr,
#ifdef _WIN32
                      L"ThreadHaulDaemon must run as Administrator to throttle and suspend "
#else
                      L"ThreadHaulDaemon must run as root to throttle and suspend "
#endif
                      L"other users' processes. Re-run elevated, or pass --allow-unelevated to "
                      L"manage only processes you own (for local testing).\n");
        return 1;
    }
    EnablePrivilege(SE_DEBUG_NAME);
#ifndef _WIN32
    sigset_t set;
    BlockStopSignals(set);  // before the daemon starts its threads
#endif
    thd::Daemon d(g_configPath);
    std::wstring error;
    if (!d.Initialize(error)) {
        fwprintf(stderr, L"Invalid configuration (%ls):\n  %ls\n", g_configPath.c_str(),
                      error.c_str());
        return 2;
    }
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    d.AllowRemoteStop(g_stopEvent);
#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#else
    WaitForStopSignal(set);
#endif
    wprintf(L"ThreadHaulDaemon running in console mode.\n  config: %ls\nPress Ctrl+C to "
                 L"stop (suspended processes are resumed on exit).\n",
                 g_configPath.c_str());
    d.Run(g_stopEvent);
    wprintf(L"Stopped. All managed processes restored.\n");
    return 0;
}

std::string Narrow(const std::wstring& w) { return utf8::FromWide(w); }

bool SameName(const std::wstring& a, const std::wstring& b) {
#ifdef _WIN32
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    }
    return true;
#endif
}

bool Exists(const std::wstring& path) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(path), ec);
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
                                   : SameName(e.name, target);
        if (!match) continue;
        ++matched;
        if (guard.IsProtected(e.name, e.pid)) {
            wprintf(L"  %ls (PID %u): refused, protected process\n", e.name.c_str(), e.pid);
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
        wprintf(L"  %ls (PID %u): %ls\n", e.name.c_str(), e.pid,
                     ok ? L"done" : L"failed (access denied? run as Administrator or root)");
        if (!ok) ++failed;
    }
    if (matched == 0) {
        wprintf(L"  no running process matches \"%ls\"\n", target.c_str());
        return 1;
    }
    return failed == 0 ? 0 : 1;
}

void PrintUsage() {
    std::wstring u(
        L"ThreadHaulDaemon - logistics server resource monitor & priority throttler\n\n"
        L"Service / monitor:\n"
        L"  ThreadHaulDaemon.exe --console [--config <path>] [--allow-unelevated]\n"
        L"      Run in the foreground (Ctrl+C to stop).\n"
        L"  ThreadHaulDaemon.exe [--config <path>]\n"
        L"      Entry point used by the Windows Service (install with scripts\\install.ps1).\n"
        L"  ThreadHaulDaemon.exe validate-config [<path>]\n"
        L"  ThreadHaulDaemon.exe verify-log [<log path>]  check the log's hash chain for tampering\n"
        L"  ThreadHaulDaemon.exe selfcheck [<config>]  audit this installation's security settings\n"
        L"  ThreadHaulDaemon.exe protect-secret        encrypt a token or webhook URL for the config\n"
        L"      (reads the secret from input; paste the printed dpapi:... value into the config)\n\n"
        L"Manual override (talks to the running daemon; needs Administrator):\n"
        L"  ThreadHaulDaemon.exe throttle <pid|name>   demote to IDLE priority and hold\n"
        L"  ThreadHaulDaemon.exe suspend  <pid|name>   suspend every thread and hold\n"
        L"  ThreadHaulDaemon.exe resume   <pid|name>   resume + normal priority and hold\n"
        L"  ThreadHaulDaemon.exe release  <pid|name>   hand back to automatic control\n"
        L"  ThreadHaulDaemon.exe exempt   <pid|name>   undo auto-protect and exclude it until it exits\n"
        L"  ThreadHaulDaemon.exe status                current pressure, thresholds, states\n"
        L"  ThreadHaulDaemon.exe stop                  stop a console-mode daemon cleanly\n"
        L"  ThreadHaulDaemon.exe dashboard             open the live web dashboard in your browser\n\n"
        L"If no daemon is running, throttle/suspend/resume act on the process directly.\n");
#ifndef _WIN32
    auto replaceAll = [&u](const std::wstring& from, const std::wstring& to) {
        for (size_t at = u.find(from); at != std::wstring::npos; at = u.find(from, at + to.size())) {
            u.replace(at, from.size(), to);
        }
    };
    replaceAll(L"ThreadHaulDaemon.exe", L"threadhauld");
    replaceAll(L"Entry point used by the Windows Service (install with scripts\\install.ps1)",
               L"Entry point used by the systemd service (threadhaul.service)");
    replaceAll(L"dpapi:...", L"sealed:...");
    replaceAll(L"needs Administrator", L"needs root");
#endif
    std::fputs(Narrow(u).c_str(), stdout);
}

}

int Main(int argc, wchar_t* argv[]) {
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
        command == L"release" || command == L"status" || command == L"stop" ||
        command == L"exempt") {
        const std::wstring target = positional.empty() ? L"" : positional[0];
        std::string response;
        const std::string request = Narrow(command + (target.empty() ? L"" : L" " + target));
        if (thd::SendControlRequest(request, response)) {
            std::printf("%s\n", response.c_str());
            return response.find("\"ok\":true") != std::string::npos ? 0 : 1;
        }
        if (command == L"status" || command == L"release" || command == L"stop" ||
            command == L"exempt") {
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
        if (!thd::SendControlRequest("dashboard", response)) {
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
#ifdef _WIN32
        const std::wstring wurl(url.begin(), url.end());
        std::printf("Opening %s\n(the link contains this session's access key; do not share it)\n",
                    url.substr(0, url.find('#')).c_str());
        ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
        // This command runs as root, which should not start a browser: print the link.
        std::printf("Open this link in a browser on this machine:\n  %s\n"
                    "(it contains this session's access key; do not share it)\n", url.c_str());
#endif
        return 0;
    }

    if (command == L"validate-config") {
        const std::wstring path = !positional.empty() ? positional[0]
                                  : !g_configPath.empty() ? g_configPath
                                                          : thd::DefaultConfigPath(true);
        try {
            config::LoadConfigFile(path);
            wprintf(L"OK: %ls is valid.\n", path.c_str());
            return 0;
        } catch (const config::ConfigError& e) {
            std::printf("INVALID: %s\n", e.what());
            return 2;
        }
    }

    if (command == L"selfcheck") {
        return thd::RunSelfCheck(!positional.empty() ? positional[0]
                                    : !g_configPath.empty() ? g_configPath
                                                            : thd::DefaultConfigPath(true));
    }

    if (command == L"protect-secret") {
        // Read from stdin, not the command line, so the secret does not land
        // in shell history or the process list. Input is not echoed.
#ifdef _WIN32
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        const bool tty = GetConsoleMode(in, &mode) != 0;
        if (tty) {
            std::fprintf(stderr, "Secret (input hidden): ");
            SetConsoleMode(in, mode & ~ENABLE_ECHO_INPUT);
        }
#else
        termios mode{};
        const bool tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &mode) == 0;
        if (tty) {
            std::fprintf(stderr, "Secret (input hidden): ");
            termios quiet = mode;
            quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
            tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
        }
#endif
        std::string secret;
        std::getline(std::cin, secret);
        if (tty) {
#ifdef _WIN32
            SetConsoleMode(in, mode);
#else
            tcsetattr(STDIN_FILENO, TCSANOW, &mode);
#endif
            std::fprintf(stderr, "\n");
        }
        if (!secret.empty() && secret.back() == '\r') secret.pop_back();
        if (secret.empty()) {
            std::fprintf(stderr, "No secret given.\n");
            return 1;
        }
        const std::string out = config::ProtectSecret(secret);
#ifdef _WIN32
        SecureZeroMemory(secret.data(), secret.size());
#else
        {
            volatile char* p = secret.data();
            for (size_t i = 0; i < secret.size(); ++i) p[i] = 0;
        }
#endif
        if (out.empty()) {
            std::fprintf(stderr, "Encryption failed (error %lu).\n", static_cast<unsigned long>(GetLastError()));
            return 1;
        }
        std::printf("%s\n", out.c_str());
        std::fprintf(stderr, "Paste this as the value in the config file. It only decrypts on this computer.\n");
        return 0;
    }

    if (command == L"verify-log") {
        std::wstring log;
        if (!positional.empty()) {
            log = positional[0];
        } else {
            const std::wstring cfg = !g_configPath.empty() ? g_configPath : thd::DefaultConfigPath(true);
            try {
                log = config::LoadConfigFile(cfg).logPath;
            } catch (const config::ConfigError& e) {
                std::printf("Cannot read the log path from the config: %s\nPass the log file: "
                            "ThreadHaulDaemon.exe verify-log <path>\n", e.what());
                return 2;
            }
        }
        // Oldest rotated file first, so each file's first line can be
        // checked against the end of the one before it.
        std::vector<std::wstring> files;
        for (int i = 99; i >= 1; --i) {
            const std::wstring f = log + L"." + std::to_wstring(i);
            if (Exists(f)) files.push_back(f);
        }
        if (Exists(log)) files.push_back(log);
        if (files.empty()) {
            wprintf(L"No log found at %ls\n", log.c_str());
            return 2;
        }
        bool ok = true;
        std::string expected;
        for (const std::wstring& f : files) {
            std::ifstream in(std::filesystem::path(f), std::ios::binary);
            const logging::ChainReport r = logging::VerifyChain(in, expected);
            if (r.Ok()) {
                wprintf(L"OK      %ls: %zu lines (%zu chained, %zu from before chaining)\n",
                             f.c_str(), r.lines, r.chained, r.unchained);
            } else {
                ok = false;
                std::printf("BROKEN  %s: line %zu: %s\n", Narrow(f).c_str(), r.brokenLine,
                            r.message.c_str());
            }
            expected = logging::LastLineHash(f);
        }
        std::printf(ok ? "Log chain intact.\n"
                       : "Log chain BROKEN: the log was edited, or lines were inserted or removed.\n");
        return ok ? 0 : 3;
    }

    if (!command.empty()) {
        fwprintf(stderr, L"Unknown command \"%ls\".\n\n", command.c_str());
        PrintUsage();
        return 1;
    }

    if (g_configPath.empty()) {
        // The SCM starts services in System32, so only console runs look
        // next to the executable first.
#ifdef _WIN32
        g_configPath = thd::DefaultConfigPath(console || GetConsoleWindow() != nullptr);
#else
        g_configPath = thd::DefaultConfigPath(console);
#endif
    }
    if (console) {
        return RunConsole(allowUnelevated);
    }
#ifndef _WIN32
    return RunLinuxService();
#else

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
#endif
}

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) { return Main(argc, argv); }
#else
int main(int argc, char* argv[]) {
    std::setlocale(LC_CTYPE, "C.UTF-8");  // wide formatting of non-ASCII text
    std::vector<std::wstring> args;
    for (int i = 0; i < argc; ++i) args.push_back(utf8::ToWide(argv[i]));
    std::vector<wchar_t*> ptrs;
    for (auto& a : args) ptrs.push_back(a.data());
    ptrs.push_back(nullptr);
    return Main(argc, ptrs.data());
}
#endif
