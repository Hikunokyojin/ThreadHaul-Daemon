#pragma once

#include <windows.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "ProcessMonitor.h"

namespace telemetry {

// What a configured executable name is pinned to. Empty fields are not checked.
struct ExpectedIdentity {
    std::wstring path;       // full image path, compared case-insensitively
    std::wstring publisher;  // signer's display name (Authenticode, embedded signature)
};

struct ObservedIdentity {
    std::wstring path;       // "" if the process could not be opened
    std::wstring publisher;  // "" if unsigned or the signature does not verify
};

// Full image path of a running process, or "".
std::wstring ProcessImagePath(DWORD pid);

// Signer of a file whose embedded Authenticode signature verifies, or "".
std::wstring FilePublisher(const std::wstring& path);

// Pure comparison, for tests. Returns "" when it matches, else the reason.
std::string IdentityMismatch(const ExpectedIdentity& want, const ObservedIdentity& got);

// Removes processes whose name is pinned but whose identity does not match,
// so they get no role. Results are cached per PID and start time, so each
// process is checked once. Thread-safe.
class IdentityFilter {
public:
    struct Rejection {
        ProcessEntry entry;
        ObservedIdentity observed;
        std::string reason;
    };

    // keys: lower-case executable names
    void SetExpected(std::map<std::wstring, ExpectedIdentity> expected);
    bool Empty() const;

    // Returns the filtered snapshot; newly rejected processes are appended
    // to `rejected` (each PID is reported only once).
    std::vector<ProcessEntry> Filter(const std::vector<ProcessEntry>& snapshot,
                                     std::vector<Rejection>& rejected);

private:
    struct Cached {
        unsigned long long startTime = 0;
        bool ok = false;
    };

    mutable std::mutex mutex_;
    std::map<std::wstring, ExpectedIdentity> expected_;
    std::map<DWORD, Cached> cache_;
};

}
