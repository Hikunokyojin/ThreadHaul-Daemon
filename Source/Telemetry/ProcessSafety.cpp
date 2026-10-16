#include "ProcessSafety.h"

#include <vector>

namespace telemetry {

namespace {

bool SameName(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring WindowsDirectory() {
    wchar_t buf[MAX_PATH] = {};
    const UINT n = GetSystemWindowsDirectoryW(buf, MAX_PATH);
    return n > 0 && n < MAX_PATH ? std::wstring(buf, n) : L"C:\\Windows";
}

// Returns "SYSTEM", "LOCAL SERVICE", "NETWORK SERVICE" or empty.
std::wstring ServiceAccount(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
        return {};
    }
    std::wstring result;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buf(size);
    if (size > 0 && GetTokenInformation(token, TokenUser, buf.data(), size, &size)) {
        PSID sid = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
        struct Known { WELL_KNOWN_SID_TYPE type; const wchar_t* name; };
        const Known known[] = {
            {WinLocalSystemSid, L"SYSTEM"},
            {WinLocalServiceSid, L"LOCAL SERVICE"},
            {WinNetworkServiceSid, L"NETWORK SERVICE"},
        };
        for (const Known& k : known) {
            BYTE wk[SECURITY_MAX_SID_SIZE];
            DWORD wkSize = sizeof(wk);
            if (CreateWellKnownSid(k.type, nullptr, wk, &wkSize) && EqualSid(sid, wk)) {
                result = k.name;
                break;
            }
        }
    }
    CloseHandle(token);
    return result;
}

}

const char* SafetyName(ProcessSafety level) {
    switch (level) {
        case ProcessSafety::Normal:    return "normal";
        case ProcessSafety::Service:   return "service";
        case ProcessSafety::System:    return "system";
        case ProcessSafety::Essential: return "essential";
    }
    return "normal";
}

bool PathIsUnder(const std::wstring& path, const std::wstring& dir) {
    if (dir.empty() || path.size() <= dir.size()) {
        return false;
    }
    std::wstring base = dir;
    if (base.back() != L'\\' && base.back() != L'/') {
        base += L'\\';
    }
    if (path.size() < base.size()) {
        return false;
    }
    const std::wstring head = path.substr(0, base.size());
    return CompareStringOrdinal(head.c_str(), -1, base.c_str(), -1, TRUE) == CSTR_EQUAL;
}

SafetyInfo ClassifyProcess(DWORD pid, const std::wstring& name,
                           const std::vector<std::wstring>& essentialNames) {
    SafetyInfo info;
    if (pid <= 4) {
        info.level = ProcessSafety::Essential;
        info.reason = L"Windows kernel process.";
        return info;
    }
    if (pid == GetCurrentProcessId()) {
        info.level = ProcessSafety::Essential;
        info.reason = L"This is the ThreadHaul service itself.";
        return info;
    }
    for (const std::wstring& essential : essentialNames) {
        if (SameName(name, essential)) {
            info.level = ProcessSafety::Essential;
            info.reason = L"Essential to Windows: pausing or slowing it can freeze or crash the system.";
            return info;
        }
    }

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) {
        info.level = ProcessSafety::System;
        info.reason = L"Windows restricts access to this process; it is usually a protected system or "
                      L"security component.";
        return info;
    }

    wchar_t path[MAX_PATH * 2];
    DWORD len = ARRAYSIZE(path);
    if (QueryFullProcessImageNameW(h, 0, path, &len)) {
        info.imagePath.assign(path, len);
    }
    const std::wstring account = ServiceAccount(h);
    CloseHandle(h);

    DWORD session = 0;
    const bool haveSession = ProcessIdToSessionId(pid, &session) != FALSE;

    if (!info.imagePath.empty() && PathIsUnder(info.imagePath, WindowsDirectory())) {
        info.level = ProcessSafety::System;
        info.reason = L"Part of Windows (" + info.imagePath +
                      L"). Slowing or pausing it can make the desktop or other programs unstable.";
    } else if (!account.empty()) {
        info.level = ProcessSafety::Service;
        info.reason = L"Background service running as " + account +
                      L". Other programs or Windows features may depend on it.";
    } else if (haveSession && session == 0) {
        info.level = ProcessSafety::Service;
        info.reason = L"Runs in the Windows services session. Other programs may depend on it.";
    }
    return info;
}

}
