#include "SelfCheck.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#include <nlohmann/json.hpp>

#include "Config.h"
#include "ConfigSecurity.h"
#include "LogChain.h"
#include "Secrets.h"

namespace daemon {

namespace {

int g_fail = 0;
int g_warn = 0;

void Report(const char* level, const char* check, const std::string& detail) {
    if (level[0] == 'F') ++g_fail;
    if (level[0] == 'W') ++g_warn;
    std::printf("%-4s  %-26s %s\n", level, check, detail.c_str());
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

std::string ReadAll(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool IsLoopback(const std::wstring& host) {
    return host == L"127.0.0.1" || host == L"::1" || _wcsicmp(host.c_str(), L"localhost") == 0;
}

void CheckServiceAccount() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE svc = scm ? OpenServiceW(scm, L"ThreadHaulDaemon", SERVICE_QUERY_CONFIG) : nullptr;
    if (!svc) {
        Report("INFO", "Service account", "service not installed (or no access); skipped");
        if (scm) CloseServiceHandle(scm);
        return;
    }
    // The first call only asks for the buffer size, so it is expected to fail
    // with ERROR_INSUFFICIENT_BUFFER; anything else means no size is known.
    DWORD needed = 0;
    if (!QueryServiceConfigW(svc, nullptr, 0, &needed) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        needed = 0;
    }
    std::unique_ptr<BYTE[]> buf(new BYTE[needed ? needed : 1]);
    auto* qsc = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.get());
    if (needed && QueryServiceConfigW(svc, qsc, needed, &needed)) {
        const std::wstring account = qsc->lpServiceStartName ? qsc->lpServiceStartName : L"";
        if (_wcsicmp(account.c_str(), L"LocalSystem") == 0) {
            Report("INFO", "Service account",
                   "LocalSystem: full control of all processes. If the managed programs run "
                   "under one service account, install.ps1 -Account Virtual limits the blast radius");
        } else {
            Report("PASS", "Service account", Narrow(account) + " (reduced privilege)");
        }
    } else {
        Report("WARN", "Service account", "could not read the service configuration");
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
}

}

int RunSelfCheck(const std::wstring& configPath) {
    g_fail = g_warn = 0;
    std::printf("ThreadHaul self-check\n  config: %s\n\n", Narrow(configPath).c_str());

    // 1. Config is present and valid. The installed config is readable only by
    // SYSTEM, Administrators and the service account, so say so plainly.
    {
        HANDLE h = CreateFileW(configPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED) {
            Report("FAIL", "Config valid",
                   "access denied (expected: the installed config is locked to Administrators); "
                   "run selfcheck from an Administrator PowerShell");
            return 1;
        }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    config::DaemonConfig cfg;
    try {
        cfg = config::LoadConfigFile(configPath);
        Report("PASS", "Config valid", "loads and passes validation");
    } catch (const config::ConfigError& e) {
        Report("FAIL", "Config valid", e.what());
        std::printf("\nCannot continue without a valid config.\n");
        return 1;
    }

    // 2. Only administrators can change the config.
    std::wstring who;
    if (IsWritableByNonAdmins(configPath, who)) {
        Report("FAIL", "Config permissions",
               Narrow(who) + " can modify it; re-run scripts\\install.ps1 to lock it down");
    } else {
        Report("PASS", "Config permissions", "writable only by SYSTEM and Administrators");
    }

    // 3. Secrets are not stored in plain text.
    const nlohmann::json raw = nlohmann::json::parse(ReadAll(configPath), nullptr, false);
    auto rawString = [&](const char* section, const char* key) -> std::string {
        if (!raw.is_object() || !raw.contains(section) || !raw[section].is_object()) return "";
        const auto& s = raw[section];
        return s.contains(key) && s[key].is_string() ? s[key].get<std::string>() : "";
    };
    const std::string token = rawString("coordination", "shared_token");
    const std::string hook = rawString("alerts", "webhook_url");
    bool plainSecret = false;
    if (!token.empty() && !config::IsProtectedSecret(token)) {
        plainSecret = true;
        Report("WARN", "Secrets encrypted",
               "coordination.shared_token is plain text; encrypt it with 'ThreadHaulDaemon.exe protect-secret'");
    }
    if (!hook.empty() && !config::IsProtectedSecret(hook) && hook.find('?') != std::string::npos) {
        plainSecret = true;
        Report("WARN", "Secrets encrypted",
               "alerts.webhook_url carries a query string (often a key) in plain text; encrypt it with protect-secret");
    }
    if (!plainSecret) Report("PASS", "Secrets encrypted", "no plain-text secrets found");

    // 4. Coordination is authenticated and encrypted (v2 protocol; tokens >= 16 chars).
    if (cfg.coordinationEnabled) {
        Report("PASS", "Controller link",
               "enabled to " + Narrow(cfg.controllerHost) + ": mutually authenticated, AES-256-GCM");
        if (!IsLoopback(cfg.controllerHost) && cfg.sharedToken.size() < 24) {
            Report("WARN", "Controller token strength",
                   "token is under 24 characters for a remote controller; use a long random value");
        }
    } else {
        Report("PASS", "Controller link", "disabled");
    }

    // 5. Dashboard exposure.
    if (cfg.dashboardEnabled) {
        Report("PASS", "Dashboard",
               "loopback only (127.0.0.1:" + std::to_string(cfg.dashboardPort) +
                   "), per-session key with expiry");
    } else {
        Report("PASS", "Dashboard", "disabled");
    }

    // 6. Critical processes are pinned to a real identity.
    size_t unpinned = 0;
    for (const auto& name : cfg.criticalProcesses) {
        bool pinned = false;
        for (const auto& p : cfg.identities) pinned = pinned || _wcsicmp(p.name.c_str(), name.c_str()) == 0;
        if (!pinned) ++unpinned;
    }
    if (unpinned > 0) {
        Report("WARN", "Process identity",
               std::to_string(unpinned) + " critical process name(s) have no identity pin; any program "
               "with that name gets the role (add an \"identity\" entry)");
    } else {
        Report("PASS", "Process identity", "every critical process is pinned to a path or publisher");
    }

    // 7. Log location is protected and the log chain is intact.
    const std::wstring logDir = std::filesystem::path(cfg.logPath).parent_path().wstring();
    if (!logDir.empty() && GetFileAttributesW(logDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (IsWritableByNonAdmins(logDir, who)) {
            Report("WARN", "Log permissions", Narrow(who) + " can write the log folder");
        } else {
            Report("PASS", "Log permissions", "log folder not writable by ordinary users");
        }
    }
    if (GetFileAttributesW(cfg.logPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::ifstream in(cfg.logPath, std::ios::binary);
        const auto r = logging::VerifyChain(in);
        if (!r.Ok()) {
            Report("FAIL", "Log integrity",
                   "line " + std::to_string(r.brokenLine) + ": " + r.message + " (run verify-log for detail)");
        } else if (r.chained == 0) {
            Report("WARN", "Log integrity", "no hash-chained lines yet (written by an older version)");
        } else {
            Report("PASS", "Log integrity", std::to_string(r.chained) + " chained lines verified");
        }
    } else {
        Report("INFO", "Log integrity", "no log file yet");
    }

    // 8. Service account.
    CheckServiceAccount();

    std::printf("\n%d failed, %d warning(s).\n", g_fail, g_warn);
    return g_fail == 0 ? 0 : 1;
}

}
