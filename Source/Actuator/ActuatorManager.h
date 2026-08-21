// ActuatorManager.h
//
// Owner: Divik (Actuator / Scheduling & Process Control layer)
// Spec:  specs/logistics-resource-monitor.md
//   - Per-batch-process independent suspend/resume, Review I item 4
//
// Owns one ProcessStateMachine per batch process, keyed by PID. This is what
// actually delivers "independent" behaviour: each batch process reacts to
// the critical process's load on its own state machine instance, so one
// batch process being suspended has no effect on another batch process's
// state.
//
// ASSUMPTION (logged in build report): Review I assumes a single critical
// process feeding all batch state machines the same CPU% reading each poll.
// Multi-critical-process support (multiple independent CPU% streams routed
// to different batch groups) is a Telemetry-layer (Prakul) Review I item;
// once that module exposes per-critical-process readings, AddBatchProcess
// below can be extended to associate a specific critical-process id with
// each batch state machine. That extension is out of scope for this file.

#pragma once

#include "StateMachine.h"
#include <unordered_map>
#include <memory>
#include <vector>

namespace actuator {

class ActuatorManager {
public:
    explicit ActuatorManager(StateMachineConfig config,
                              TransitionCallback onTransition = nullptr);

    // Registers a batch process to be independently managed. Safe to call
    // multiple times for different batch processes; each gets its own
    // ProcessStateMachine instance.
    void AddBatchProcess(const std::wstring& processName, DWORD processId);

    // Removes a batch process from management (e.g. it exited). Does not
    // attempt to resume/restore priority -- if the process is gone there is
    // nothing left to restore.
    void RemoveBatchProcess(DWORD processId);

    // Feeds the latest critical-process CPU% reading to every managed
    // batch process's state machine. Call once per poll interval.
    void UpdateAll(double criticalProcessCpuPercent);

    // Graceful shutdown (Review I item 5): resumes/restores every managed
    // batch process before the service exits.
    void ForceRecoverAllForShutdown();

    size_t ManagedCount() const { return machines_.size(); }

private:
    StateMachineConfig config_;
    TransitionCallback onTransition_;
    // unique_ptr wrapper avoids requiring ProcessStateMachine to be
    // copyable/movable just to satisfy unordered_map's storage model.
    std::unordered_map<DWORD, std::unique_ptr<ProcessStateMachine>> machines_;
};

} // namespace actuator
