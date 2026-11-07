#include "ProcessMatch.h"

#include <algorithm>
#include <cwctype>
#include <memory>

namespace telemetry {

namespace {

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return std::towlower(c); });
    return s;
}

bool Same(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

unsigned long long StartTime(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;
    FILETIME c{}, e{}, k{}, u{};
    unsigned long long t = 0;
    if (GetProcessTimes(h, &c, &e, &k, &u)) {
        t = (static_cast<unsigned long long>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
    }
    CloseHandle(h);
    return t;
}

// UNICODE_STRING as returned by ProcessCommandLineInformation.
struct UnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

}

std::wstring ProcessCommandLine(DWORD pid) {
    // ProcessCommandLineInformation (60) works with limited query rights on
    // Windows 8.1 and later; resolved at run time like the I/O priority call.
    using NtQueryFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<NtQueryFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess")));
    if (!query) return L"";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    ULONG len = 0;
    query(h, 60, nullptr, 0, &len);
    std::wstring out;
    if (len >= sizeof(UnicodeString) && len < (1u << 20)) {
        std::unique_ptr<BYTE[]> buf(new BYTE[len]);
        if (query(h, 60, buf.get(), len, &len) >= 0) {
            const auto* us = reinterpret_cast<const UnicodeString*>(buf.get());
            if (us->Buffer && us->Length > 0) out.assign(us->Buffer, us->Length / sizeof(wchar_t));
        }
    }
    CloseHandle(h);
    return out;
}

std::wstring ProcessUser(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    HANDLE token = nullptr;
    std::wstring out;
    if (OpenProcessToken(h, TOKEN_QUERY, &token)) {
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> buf(size);
        if (size > 0 && GetTokenInformation(token, TokenUser, buf.data(), size, &size)) {
            PSID sid = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
            wchar_t name[256], domain[256];
            DWORD n = ARRAYSIZE(name), d = ARRAYSIZE(domain);
            SID_NAME_USE use;
            if (LookupAccountSidW(nullptr, sid, name, &n, domain, &d, &use)) {
                out = std::wstring(domain) + L"\\" + name;
            }
        }
        CloseHandle(token);
    }
    CloseHandle(h);
    return out;
}

DWORD ServiceProcessId(const std::wstring& serviceName) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return 0;
    DWORD pid = 0;
    SC_HANDLE svc = OpenServiceW(scm, serviceName.c_str(), SERVICE_QUERY_STATUS);
    if (svc) {
        SERVICE_STATUS_PROCESS ssp{};
        DWORD needed = 0;
        if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp),
                                 &needed) &&
            ssp.dwCurrentState != SERVICE_STOPPED) {
            pid = ssp.dwProcessId;
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return pid;
}

bool ConditionMatches(const MatchCondition& c, DWORD pid, const ProcessFacts& facts, DWORD servicePid) {
    if (!c.commandLineContains.empty()) {
        if (facts.commandLine.empty() ||
            Lower(facts.commandLine).find(Lower(c.commandLineContains)) == std::wstring::npos) {
            return false;
        }
    }
    if (!c.user.empty()) {
        if (facts.user.empty()) return false;
        if (c.user.find(L'\\') != std::wstring::npos) {
            if (!Same(c.user, facts.user)) return false;
        } else {
            const size_t slash = facts.user.find(L'\\');
            const std::wstring bare = slash == std::wstring::npos ? facts.user : facts.user.substr(slash + 1);
            if (!Same(c.user, bare)) return false;
        }
    }
    if (!c.service.empty()) {
        if (servicePid == 0 || servicePid != pid) return false;
    }
    return true;
}

void RoleMatcher::Configure(Role role, const std::vector<std::wstring>& names,
                            const std::map<std::wstring, std::vector<MatchCondition>>& rules) {
    RoleData& d = Data(role);
    d.names.clear();
    for (const auto& n : names) d.names.insert(Lower(n));
    d.rules.clear();
    for (const auto& [n, alts] : rules) d.rules[Lower(n)] = alts;
    cache_.clear();
}

void RoleMatcher::BeginPoll(const std::vector<ProcessEntry>& snapshot) {
    servicePids_.clear();
    for (RoleData* d : {&critical_, &batch_}) {
        for (const auto& [n, alts] : d->rules) {
            (void)n;
            for (const auto& c : alts) {
                if (!c.service.empty()) {
                    const std::wstring key = Lower(c.service);
                    if (!servicePids_.count(key)) servicePids_[key] = ServiceProcessId(c.service);
                }
            }
        }
    }
    std::set<DWORD> alive;
    for (const auto& e : snapshot) alive.insert(e.pid);
    for (auto it = cache_.begin(); it != cache_.end();) {
        it = alive.count(it->first) ? std::next(it) : cache_.erase(it);
    }
}

const ProcessFacts& RoleMatcher::Facts(DWORD pid) {
    const unsigned long long start = StartTime(pid);
    auto it = cache_.find(pid);
    if (it == cache_.end() || it->second.startTime != start) {
        Cached c;
        c.startTime = start;
        c.facts.commandLine = ProcessCommandLine(pid);
        c.facts.user = ProcessUser(pid);
        it = cache_.insert_or_assign(pid, std::move(c)).first;
    }
    return it->second.facts;
}

bool RoleMatcher::Member(Role role, DWORD pid, const std::wstring& name) {
    RoleData& d = Data(role);
    const std::wstring key = Lower(name);
    if (!d.names.count(key)) return false;
    const auto r = d.rules.find(key);
    if (r == d.rules.end()) return true;  // no conditions: every instance
    bool needFacts = false;
    for (const auto& c : r->second) needFacts = needFacts || !c.commandLineContains.empty() || !c.user.empty();
    static const ProcessFacts kNone;
    const ProcessFacts& facts = needFacts ? Facts(pid) : kNone;
    for (const auto& c : r->second) {
        const auto sp = c.service.empty() ? servicePids_.end() : servicePids_.find(Lower(c.service));
        const DWORD servicePid = sp == servicePids_.end() ? 0 : sp->second;
        if (ConditionMatches(c, pid, facts, servicePid)) return true;
    }
    return false;
}

std::vector<ProcessEntry> RoleMatcher::Filter(const std::vector<ProcessEntry>& snapshot, Role role) {
    std::vector<ProcessEntry> out;
    for (const auto& e : snapshot) {
        if (Member(role, e.pid, e.name)) out.push_back(e);
    }
    return out;
}

}
