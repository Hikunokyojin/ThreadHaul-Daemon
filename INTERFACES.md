# ThreadHaul-Daemon — Interface Contract

**Status: DRAFT — sections marked `[PROPOSED]` are not yet confirmed against Divik's actual Actuator code and MUST be verified before Prakul or Pranav build against them.**

This file is the single source of truth for cross-layer interfaces. No layer invents its own field names, types, or schema — if something here doesn't match reality, this file gets fixed first, then the code.

Owners:
- Actuator/Scheduling — Divik
- Telemetry/Monitoring — Prakul
- Configuration/Logging/Integration — Pranav

---

## 1. Fixed System Constants

These are agreed project-wide. Do not hardcode duplicates in more than one layer — Pranav's config layer is the single owner of these values at runtime; other layers read them from config, not from their own constants.

| Name | Value | Meaning |
|---|---|---|
| `THRESHOLD_HIGH` | 85 (%) | High CPU threshold |
| `THRESHOLD_CRITICAL` | 90 (%) | Critical CPU threshold |
| `THRESHOLD_LOW` | 60 (%) | Low/recovery CPU threshold |
| `CONSEC_HIGH` | 3 | Consecutive samples to trigger THRESHOLD_HIGH |
| `CONSEC_CRITICAL` | 5 | Consecutive samples to trigger THRESHOLD_CRITICAL |
| `CONSEC_LOW` | 5 | Consecutive samples to trigger THRESHOLD_LOW |
| `POLL_INTERVAL_MS` | 1000 | Telemetry sampling interval |

**Open question for the team:** do these stay as compile-time constants (simplest, matches Divik's existing code) or move into Pranav's config file as runtime-configurable values? Decide before Pranav builds the config loader — this is Pranav's call to drive, but Divik and Prakul need to agree since it affects their headers too.

---

## 2. Telemetry → Actuator Event Interface

**Status: CONFIRMED against `Source/Actuator/ActuatorManager.h`/`.cpp` (Divik, 2026-09-25).**

Real shape, matching the code as built:

```cpp
// shared/ThresholdEvent.h — confirmed against Source/Actuator

#pragma once
#include <cstdint>
#include <chrono>

enum class ThresholdType : uint8_t {
    High     = 0,  // 85%, 3 consecutive samples
    Critical = 1,  // 90%, 5 consecutive samples
    Low      = 2   // 60%, 5 consecutive samples (recovery)
};

struct ThresholdEvent {
    uint32_t   criticalProcessId;                          // the CRITICAL process the % was sampled from — see note below
    ThresholdType type;                                     // which threshold fired
    double     observedCpuPercent;                          // actual sampled value at trigger time
    std::chrono::system_clock::time_point timestamp;        // when the trigger condition was met
};

// Real consumer-side signature exposed by Actuator:
// void ActuatorManager::OnThresholdEvent(const ThresholdEvent& evt);
//
// IMPORTANT — how Actuator uses this event: ActuatorManager does not act on
// a single named process from the event. It applies evt.observedCpuPercent
// (the critical process's CPU%) to EVERY batch process it currently manages
// (added via AddBatchProcess(name, pid) at startup/config time), via
// UpdateAll(evt.observedCpuPercent) internally. So OnThresholdEvent's PID
// is informational/logging context only — it is NOT the PID that gets
// throttled/suspended. The actual affected PID(s) are the batch PIDs
// Actuator already owns. See §3.3's `batch_process_id` field, added below,
// for how that distinction shows up in logs.
```

**Field renamed:** `processId` → `criticalProcessId`, to make explicit that this is the critical process's PID, not the batch process Actuator acts on (see §3.3 for the resulting log schema fix). This was ambiguous in the original `[PROPOSED]` draft and caused a real mismatch with how `ActuatorManager` is built — flagging here per the Change Protocol (§5) rather than leaving it implicit.

---

## 3. Structured Log Schema (JSON Lines)

Owner: Pranav defines and owns this schema centrally; Prakul and Divik's layers write through Pranav's logging component using these exact field names. One JSON object per line, UTF-8, no trailing comma.

### 3.1 Telemetry sample log (written every poll, by Telemetry via Pranav's logger)

```json
{"log_type": "sample", "timestamp": "2026-09-24T14:03:21.501Z", "process_id": 4821, "cpu_percent": 42.7}
```

### 3.2 Threshold trigger event log (written by Telemetry via Pranav's logger, when a threshold fires)

```json
{"log_type": "threshold_event", "timestamp": "2026-09-24T14:03:24.501Z", "process_id": 4821, "threshold": "high", "consecutive_samples": 3, "cpu_percent": 87.2}
```

`threshold` is one of: `"high"`, `"critical"`, `"low"` — string, not the C++ enum value, so logs stay human-readable and language-agnostic.

### 3.3 Actuator action log (written by Actuator via Pranav's logger, when it acts on an event)

**Status: CONFIRMED against `Source/Actuator/StateMachine.cpp` (Divik, 2026-09-25).**

```json
{"log_type": "actuator_action", "timestamp": "2026-09-24T14:03:24.550Z", "batch_process_id": 5190, "batch_process_name": "invoice_batch.exe", "critical_process_id": 4821, "action": "priority_lowered", "from_state": "normal", "to_state": "throttled", "actuation_succeeded": true}
```

**`[PROPOSED]` field added 2026-10-03 — needs Pranav's and Prakul's ack per §5 before it is binding:** `actuation_succeeded` (boolean). `StateMachine`'s `TransitionCallback` now ends with a `bool actuationSucceeded` parameter. It is `false` when the OS call did not fully take effect: `SetPriorityClass` was denied or failed, not every thread of the batch process could be suspended or resumed, or the batch process had already exited. The state still advances so the machine does not retry forever; the flag lets the log show that a transition was recorded but not fully applied. Pranav's logger should write it through as `actuation_succeeded`.

**Fields changed from the original placeholder — flagged per §5 Change Protocol:**
- `process_id` → split into `batch_process_id` (the PID Actuator actually acted on — this is what `SetProcessPriority`/`SuspendAllThreads`/`ResumeAllThreads` were called on) and `critical_process_id` (the PID whose CPU% triggered the transition, carried through from §2's `ThresholdEvent.criticalProcessId`). The original single `process_id` was ambiguous about which PID it meant; `StateMachine`'s real `TransitionCallback` only has the batch PID/name plus the CPU% at transition — it does not itself know the critical process's PID, so Pranav's/Prakul's logging wiring will need to thread `criticalProcessId` through from the originating `ThresholdEvent` when writing this line.
- `batch_process_name` added — `StateMachine::TransitionCallback` already carries the batch process's name (`std::wstring`), and it's cheap/useful to log alongside the PID.

**Real `action`/`from_state`/`to_state` values, confirmed from `ProcessStateMachine::TransitionTo` in `StateMachine.cpp`:**

| Transition | `from_state` | `to_state` | `action` |
|---|---|---|---|
| Normal → Throttled | `"normal"` | `"throttled"` | `"priority_lowered"` |
| Throttled → Suspended | `"throttled"` | `"suspended"` | `"threads_suspended"` |
| Throttled/Suspended → Normal | `"throttled"` or `"suspended"` | `"normal"` | `"recovered"` |

Note on the last row: a single transition out of `Suspended` back to `Normal` performs **both** a thread resume (`ResumeAllThreads`) **and** a priority restore (`SetProcessPriority(..., Normal)`) in the same `TransitionTo` call — there's only one `action` field, so both effects are represented by the single `"recovered"` value rather than two separate log lines. Flagging this as a deliberate simplification for team sign-off, not a silent guess — if per-effect granularity is needed later, this would need either a second log line or an `effects: []` array added to the schema.

### 3.4 Field naming rules (binding on all layers)

- All timestamps: ISO 8601 UTC, millisecond precision, key name always `timestamp`.
- All field names: `snake_case`, never camelCase, to match `log_type` and stay consistent regardless of which layer's C++ (which internally may use camelCase per style) writes the entry.
- Every line MUST include `log_type` and `timestamp` — these are the only two fields every consumer (grading script, review report, integration debugging) can rely on across all three log kinds.

**Action item — Divik:** confirm/replace the `action`, `from_state`, `to_state` values in 3.3 against your real StateMachine.

---

## 4. Configuration File Schema

Owner: Pranav. Consumed by all three layers at startup (each layer reads only the section it needs).

```json
{
  "polling": {
    "interval_ms": 1000
  },
  "thresholds": {
    "high":     { "percent": 85, "consecutive_samples": 3 },
    "critical": { "percent": 90, "consecutive_samples": 5 },
    "low":      { "percent": 60, "consecutive_samples": 5 }
  },
  "logging": {
    "log_path": "C:\\ThreadHaulDaemon\\logs\\threadhaul.jsonl",
    "max_file_size_mb": 50
  }
}
```

**Open question:** rotation policy (size-based above, vs. daily) — Pranav's call, flag it as a milestone decision rather than guessing.

---

## 5. Change Protocol

If any layer needs a field added/renamed/removed in sections 2–4: edit this file first, post the diff to the team (or whatever your team's communication channel is), get at minimum a thumbs-up from the other two owners, THEN change code. This file being out of sync with reality is the single most likely cause of integration failure at the end of this project — treat edits to it as seriously as you'd treat an API contract change on a real team.
