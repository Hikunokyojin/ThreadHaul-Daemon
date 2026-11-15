#pragma once

#include <windows.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "ProcessMonitor.h"

namespace telemetry {

// Tells instances of the same executable apart, so that for example one
// java.exe can be critical and another batch. A rule's conditions must all
// hold; a name's rules are alternatives. A name with no rules matches every
// instance (the v2.0 behaviour).
struct MatchCondition {
    std::wstring commandLineContains;  // case-insensitive substring of the full command line
    std::wstring user;                 // "DOMAIN\\user" (exact) or "user" (any domain), case-insensitive
    std::wstring service;              // the process must host this Windows service
};

// What we know about one process; empty strings mean "could not be read".
struct ProcessFacts {
    std::wstring commandLine;
    std::wstring user;                 // "DOMAIN\\user"
};

std::wstring ProcessCommandLine(DWORD pid);
std::wstring ProcessUser(DWORD pid);
DWORD ServiceProcessId(const std::wstring& serviceName);  // 0 if stopped or unknown

// Pure check, for tests. servicePid is the PID that hosts the rule's service (or 0).
bool ConditionMatches(const MatchCondition& c, DWORD pid, const ProcessFacts& facts, DWORD servicePid);

enum class Role { Critical, Batch };

class RoleMatcher {
public:
    // names: every exe name configured for the role. rules: lower-case name ->
    // alternatives; names absent from rules match every instance.
    void Configure(Role role, const std::vector<std::wstring>& names,
                   const std::map<std::wstring, std::vector<MatchCondition>>& rules);

    // Call once per poll before Filter/Member: refreshes service PIDs and
    // forgets processes that have exited.
    void BeginPoll(const std::vector<ProcessEntry>& snapshot);

    // Snapshot entries that belong to the role.
    std::vector<ProcessEntry> Filter(const std::vector<ProcessEntry>& snapshot, Role role);

    // True if this instance belongs to the role.
    bool Member(Role role, DWORD pid, const std::wstring& name);

private:
    struct RoleData {
        std::set<std::wstring> names;  // lower-case
        std::map<std::wstring, std::vector<MatchCondition>> rules;
    };
    struct Cached {
        unsigned long long startTime = 0;
        ProcessFacts facts;
    };

    const ProcessFacts& Facts(DWORD pid);
    RoleData& Data(Role r) { return r == Role::Critical ? critical_ : batch_; }

    RoleData critical_, batch_;
    std::map<std::wstring, DWORD> servicePids_;  // lower-case service -> pid
    std::map<DWORD, Cached> cache_;
};

}
