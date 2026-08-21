// PriorityControl.cpp
#include "PriorityControl.h"

namespace actuator {

namespace {

DWORD ToWin32PriorityClass(PriorityLevel level) {
    switch (level) {
        case PriorityLevel::Idle:        return IDLE_PRIORITY_CLASS;
        case PriorityLevel::BelowNormal: return BELOW_NORMAL_PRIORITY_CLASS;
        case PriorityLevel::Normal:      return NORMAL_PRIORITY_CLASS;
    }
    return NORMAL_PRIORITY_CLASS;
}

} // namespace

PriorityChangeResult SetProcessPriority(DWORD processId, PriorityLevel level) {
    HANDLE hProcess = OpenProcess(PROCESS_SET_INFORMATION, FALSE, processId);
    if (hProcess == nullptr) {
        const DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            return PriorityChangeResult::AccessDenied;
        }
        if (err == ERROR_INVALID_PARAMETER) {
            // Typically means the PID no longer exists (process exited).
            return PriorityChangeResult::ProcessNotFound;
        }
        return PriorityChangeResult::OtherError;
    }

    const BOOL ok = SetPriorityClass(hProcess, ToWin32PriorityClass(level));
    const DWORD lastError = ok ? 0 : GetLastError();
    CloseHandle(hProcess);

    if (ok) {
        return PriorityChangeResult::Success;
    }
    if (lastError == ERROR_ACCESS_DENIED) {
        return PriorityChangeResult::AccessDenied;
    }
    return PriorityChangeResult::OtherError;
}

bool IsRunningElevated() {
    BOOL isElevated = FALSE;
    HANDLE hToken = nullptr;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD returnedSize = 0;
    if (GetTokenInformation(hToken, TokenElevation, &elevation,
                             sizeof(elevation), &returnedSize)) {
        isElevated = elevation.TokenIsElevated;
    }

    CloseHandle(hToken);
    return isElevated != FALSE;
}

} // namespace actuator
