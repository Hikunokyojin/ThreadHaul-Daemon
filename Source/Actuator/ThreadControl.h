// ThreadControl.h
//
// Owner: Divik (Actuator / Scheduling & Process Control layer)
// Spec:  specs/logistics-resource-monitor.md
//   - Escalation logic (SuspendThread), Review I item 2
//   - Recovery logic   (ResumeThread),  Review I item 3
//
// Enumerates every thread belonging to a target process via
// CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, ...) and suspends/resumes them
// all. This mirrors the spec's description: "capture a thread-level snapshot
// of the low-priority tasks and iterate through them, calling SuspendThread".
//
// NOTE on Review II scope (not implemented here): race-condition safety for
// a thread/process that exits mid-suspend is a separate Review II item.
// This Review I version performs a best-effort suspend/resume and reports
// how many threads succeeded vs. failed, but does not yet implement retry
// or re-validation logic for threads that vanish between enumeration and
// suspension. This is called out explicitly in the assumptions log.

#pragma once

#include <windows.h>
#include <vector>

namespace actuator {

struct ThreadOpResult {
    int threadsAttempted = 0;
    int threadsSucceeded = 0;
    int threadsFailed = 0; // includes access-denied and already-exited threads

    bool AllSucceeded() const {
        return threadsAttempted > 0 && threadsFailed == 0;
    }
};

// Suspends every thread currently belonging to processId.
ThreadOpResult SuspendAllThreads(DWORD processId);

// Resumes every thread currently belonging to processId. Because
// SuspendThread/ResumeThread use a suspend COUNT rather than a boolean flag,
// this calls ResumeThread once per thread -- callers must ensure Suspend and
// Resume are called symmetrically (this state machine design guarantees
// that: a batch process only ever moves Suspended -> Normal via a single
// ForceRecoverForShutdown()/recover transition after exactly one prior
// SuspendAllThreads() call).
ThreadOpResult ResumeAllThreads(DWORD processId);

} // namespace actuator
