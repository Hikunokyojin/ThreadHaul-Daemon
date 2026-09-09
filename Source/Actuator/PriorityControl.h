#pragma once

#include <windows.h>
#include <string>

namespace actuator {

enum class PriorityLevel {
    Idle,
    BelowNormal,
    Normal,
};

enum class PriorityChangeResult {
    Success,
    AccessDenied,
    ProcessNotFound,
    OtherError,
};

PriorityChangeResult SetProcessPriority(DWORD processId, PriorityLevel level);

bool IsRunningElevated();

}
