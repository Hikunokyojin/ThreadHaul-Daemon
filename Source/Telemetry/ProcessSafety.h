#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace telemetry {

// How risky it is to throttle or suspend a process. Used by the dashboard to
// hide or highlight processes that Windows depends on, and by the service to
// demand explicit confirmation before managing them.
enum class ProcessSafety {
    Normal,     // an ordinary application
    Service,    // runs as SYSTEM / LOCAL SERVICE / NETWORK SERVICE or in session 0
    System,     // part of Windows (image under %SystemRoot%) or access is denied
    Essential,  // Windows cannot run without it; never managed
};

struct SafetyInfo {
    ProcessSafety level = ProcessSafety::Normal;
    std::wstring  reason;     // one sentence for the user
    std::wstring  imagePath;  // empty if it could not be read
};

const char* SafetyName(ProcessSafety level);  // "normal", "service", "system", "essential"

// Case-insensitive "is `path` inside directory `dir`" (dir may end with a slash).
bool PathIsUnder(const std::wstring& path, const std::wstring& dir);

// Classifies one running process. `essentialNames` is the built-in list of
// executable names that must never be managed (ActuatorManager supplies it).
SafetyInfo ClassifyProcess(DWORD pid, const std::wstring& name,
                           const std::vector<std::wstring>& essentialNames);

}
