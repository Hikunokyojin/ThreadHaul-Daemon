// PriorityControl.h
//
// Owner: Divik (Actuator / Scheduling & Process Control layer)
// Spec:  specs/logistics-resource-monitor.md
//   - Throttling logic (SetPriorityClass), Review I item 1
//   - Administrator privilege handling,    Review I item 7
//
// Thin, testable wrapper around the Win32 priority-class API. Kept separate
// from StateMachine.h so the state machine's transition logic can be unit
// tested (Review II item, Pranav's module) without needing real OS handles.

#pragma once

#include <windows.h>
#include <string>

namespace actuator {

enum class PriorityLevel {
    Idle,       // maps to IDLE_PRIORITY_CLASS
    BelowNormal,// maps to BELOW_NORMAL_PRIORITY_CLASS
    Normal,     // maps to NORMAL_PRIORITY_CLASS
};

// Result of a priority-change attempt. Distinguishing AccessDenied from a
// generic failure matters: an AccessDenied result on a protected process is
// an expected, handled case (Review II item, Prakul's module), not a bug.
enum class PriorityChangeResult {
    Success,
    AccessDenied,
    ProcessNotFound,
    OtherError,
};

// Opens the target process with PROCESS_SET_INFORMATION and applies the
// requested priority class. Does not throw; failures are reported via the
// return value so the state machine can decide how to react (e.g. log and
// continue, rather than crash the service).
PriorityChangeResult SetProcessPriority(DWORD processId, PriorityLevel level);

// Returns true if the current process is running with Administrator
// privileges. The service should check this at startup: without it,
// SetProcessPriority and the thread-suspend calls in ThreadControl will
// fail with AccessDenied on most target processes.
bool IsRunningElevated();

} // namespace actuator
