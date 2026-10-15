# ThreadHaul-Daemon — Interface Contract

**Status: CONFIRMED against the integrated build (2026-10-15).** Every section below matches the code in `Source/`. Changes still follow the Change Protocol (§5).

This file is the single source of truth for cross-layer interfaces. No layer invents its own field names, types, or schema — if something here doesn't match reality, this file gets fixed first, then the code.

Modules:
- Actuator/Scheduling: priority demotion, thread suspension and recovery of batch processes (`Source/Actuator`)
- Telemetry/Monitoring: process discovery, CPU/memory/I-O sampling, predictive thresholds (`Source/Telemetry`)
- Configuration/Logging/Integration: config loading and validation, logging, and the service core (`Source/Config`, `Source/Logging`, `Source/Daemon`)

---

## 1. Fixed System Constants

These are agreed project-wide. Do not hardcode duplicates in more than one layer — the Configuration layer is the single owner of these values at runtime; other layers read them from config, not from their own constants.

| Name | Value | Meaning |
|---|---|---|
| `THRESHOLD_HIGH` | 85 (%) | High CPU threshold |
| `THRESHOLD_CRITICAL` | 90 (%) | Critical CPU threshold |
| `THRESHOLD_LOW` | 60 (%) | Low/recovery CPU threshold |
| `CONSEC_HIGH` | 3 | Consecutive samples to trigger THRESHOLD_HIGH |
| `CONSEC_CRITICAL` | 5 | Consecutive samples to trigger THRESHOLD_CRITICAL |
| `CONSEC_LOW` | 5 | Consecutive samples to trigger THRESHOLD_LOW |
| `POLL_INTERVAL_MS` | 1000 | Telemetry sampling interval |

These are the defaults. At runtime the values come from `config.json` (§4); no layer hardcodes them.

---

## 2. Telemetry → Actuator Event Interface

**Status: CONFIRMED against `Source/Actuator/ActuatorManager.h`/`.cpp` (2026-09-25).**

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

**How the integrated daemon feeds the Actuator.** `Daemon::Poll` (Source/Daemon) calls `ActuatorManager::UpdateAll(pressure)` once per poll with the combined pressure of all critical processes (the highest CPU%, or memory/I-O when those triggers are enabled; see §4). That is the same input `OnThresholdEvent` forwards. `OnThresholdEvent` is kept for callers that only have threshold events, but the daemon does not call both, because that would double-count samples. Telemetry's `ThresholdTracker` (Source/Telemetry) only writes the §3.2 log lines. It resets whenever the Actuator changes state, so its `critical` event lines up with the suspension, which is counted from the throttle.

**Field renamed:** `processId` → `criticalProcessId`, to make explicit that this is the critical process's PID, not the batch process Actuator acts on (see §3.3 for the resulting log schema fix). This was ambiguous in the original `[PROPOSED]` draft and caused a real mismatch with how `ActuatorManager` is built — flagging here per the Change Protocol (§5) rather than leaving it implicit.

---

## 3. Structured Log Schema (JSON Lines)

The Logging layer defines and owns this schema centrally; the Telemetry and Actuator layers write through the logging component using these exact field names. One JSON object per line, UTF-8, no trailing comma.

### 3.1 Telemetry sample log (written every poll, by Telemetry via the logging component)

```json
{"log_type": "sample", "timestamp": "2026-09-24T14:03:21.501Z", "process_id": 4821, "cpu_percent": 42.7, "process_name": "dispatch_api.exe", "memory_mb": 182.4, "io_mb_per_sec": 0.3, "pressure_percent": 42.7, "trigger_signal": "cpu"}
```

`process_id` and `cpu_percent` are the original required fields. The rest are additions:
- `pressure_percent` is the value fed to the state machine (§2).
- `trigger_signal` names its source: `cpu`, `system_memory`, `working_set` or `io`.

`cpu_percent` uses the configured normalisation (§4 `telemetry.cpu_normalization`). Samples are skipped for the first poll after a process is (re)acquired, because there is no CPU delta yet.

### 3.2 Threshold trigger event log (written by Telemetry via the logging component, when a threshold fires)

```json
{"log_type": "threshold_event", "timestamp": "2026-09-24T14:03:24.501Z", "process_id": 4821, "threshold": "high", "consecutive_samples": 3, "cpu_percent": 87.2}
```

`threshold` is one of: `"high"`, `"critical"`, `"low"` — string, not the C++ enum value, so logs stay human-readable and language-agnostic.

### 3.3 Actuator action log (written through `logging::JsonLinesLogger::LogActuatorAction` when the Actuator changes a batch process's state)

```json
{"log_type": "actuator_action", "timestamp": "2026-09-24T14:03:24.550Z", "batch_process_id": 5190, "batch_process_name": "invoice_batch.exe", "critical_process_id": 4821, "action": "priority_lowered", "from_state": "normal", "to_state": "throttled", "cpu_percent": 87.2, "actuation_succeeded": true}
```

- `batch_process_id` / `batch_process_name`: the process the Actuator acted on (`SetProcessPriority`/`SuspendAllThreads`/`ResumeAllThreads` target).
- `critical_process_id`: the process whose CPU% triggered the transition, carried from §2's `ThresholdEvent.criticalProcessId`. `TransitionCallback` does not know it, so the integration code supplies it when writing this line.
- `cpu_percent`: the critical process's CPU% at the moment of the transition (required by the spec's logging section).
- `trigger`: `"automatic"` (state machine), `"manual"` (CLI or controller command), `"shutdown"` (service stop restores everything) or `"config_reload"` (the process name was removed from config).
- `actuation_succeeded`: `false` when the OS call did not fully take effect (`SetPriorityClass` denied or failed, not every thread suspended/resumed, or the batch process already exited). The state still advances so the machine does not retry forever; this flag shows the transition was recorded but not fully applied.

Every `actuator_action` line is also written to the Windows Application event log (source `ThreadHaulDaemon`, event IDs 1000 throttled, 1001 suspended, 1002 recovered) by `logging::EventLogWriter`.

**Real `action`/`from_state`/`to_state` values, confirmed from `ProcessStateMachine::TransitionTo` in `StateMachine.cpp`:**

| Transition | `from_state` | `to_state` | `action` |
|---|---|---|---|
| Normal → Throttled | `"normal"` | `"throttled"` | `"priority_lowered"` |
| Throttled → Suspended | `"throttled"` | `"suspended"` | `"threads_suspended"` |
| Throttled/Suspended → Normal | `"throttled"` or `"suspended"` | `"normal"` | `"recovered"` |
| Normal → Suspended (manual only) | `"normal"` | `"suspended"` | `"threads_suspended"` |
| Suspended → Throttled (manual only) | `"suspended"` | `"throttled"` | `"priority_lowered"` |

Note on the last row: a single transition out of `Suspended` back to `Normal` performs **both** a thread resume (`ResumeAllThreads`) **and** a priority restore (`SetProcessPriority(..., Normal)`) in the same `TransitionTo` call, so both effects share the single `"recovered"` action.

### 3.4 Other log kinds (written through `JsonLinesLogger::LogEvent`)

| `log_type` | When | Fields |
|---|---|---|
| `service_lifecycle` | start / stop | `event` (`started`/`stopped`), and on start: `config_path`, `config_sha256`, `logical_cores`, `cpu_normalization`, `pid` |
| `process_lifecycle` | a target appears, exits, crashes, restarts, is refused, or is protected | `role` (`critical`/`batch`), `event` (`acquired`, `exited`, `crashed`, `restarted`, `access_denied`, `protected_skipped`), `process_name`, `process_id`, optional `previous_process_id`, `exit_code`, `state` |
| `config_reload` | config file changed | `success`, `config_sha256`, and either the new `poll_interval_ms`/`high_percent`/`critical_percent`/`low_percent`/`adaptive_enabled`, or `error` |
| `config_security` | config is writable by non-admins | `warning`, `config_path` |
| `adaptive_threshold` | effective thresholds moved by ≥ 0.5 points | `offset_percent`, `slope_percent_per_sample`, `forecast_percent`, `controller_bias_percent`, `high_percent`, `critical_percent`, `low_percent` |
| `manual_override` | every CLI/controller command target | `command`, `process_id`, `process_name`, `ok`, `result` |
| `alert` | a batch process was suspended | `alert` (`batch_suspended`), `batch_process_id`, `batch_process_name`, `popup`, `webhook` |
| `self_monitor` | every `telemetry.self_monitor_interval_seconds` | `cpu_percent` (of one core), `working_set_mb`, `private_mb`, `handle_count`, `managed_batch_processes`, `tracked_critical_names` |
| `coordination` | controller connection events | `event` (`coordination_connected`, `coordination_disconnected`, `coordination_bias`, `coordination_command`, `coordination_rejected`, `coordination_error`), `detail` |

The controller (`ThreadHaulController`) writes its own `controller.jsonl` using the same `log_type` + `timestamp` conventions. Its log kinds are `controller_started`, `agent_connected`, `agent_disconnected`, `agent_rejected`, `heartbeat`, `agent_event`, `cluster_bias`, `command_sent`, `command_result` and `controller_stopped`.

### 3.5 Field naming rules (binding on all layers)

- All timestamps: ISO 8601 UTC, millisecond precision, key name always `timestamp`.
- All field names: `snake_case`, never camelCase, to match `log_type` and stay consistent regardless of which layer's C++ (which internally may use camelCase per style) writes the entry.
- Every line MUST include `log_type` and `timestamp` — these are the only two fields every consumer (grading script, review report, integration debugging) can rely on across all three log kinds.

---

## 4. Configuration File Schema

Loaded and validated by `config::LoadConfigFile` (`Source/Config/Config.cpp`); sample at `Source/Config/config.json`. Thresholds are runtime-configurable, not compile-time constants.

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
  "processes": {
    "critical":  ["critical_simulator.exe"],
    "batch":     ["batch_simulator.exe"],
    "protected": ["explorer.exe"]
  },
  "logging": {
    "log_path": "C:\\ProgramData\\ThreadHaulDaemon\\logs\\threadhaul.jsonl",
    "max_file_size_mb": 50,
    "max_rotated_files": 5
  }
}
```

Validation (any failure stops startup with a message naming the field):
- Every field is required except `processes.protected`.
- `polling.interval_ms`: integer 100–60000.
- Threshold `percent`: number, greater than 0 and at most 100. `consecutive_samples`: integer 1–100.
- `critical.percent` ≥ `high.percent`, and `low.percent` < `high.percent`.
- `processes.critical` and `processes.batch`: at least one entry each. Names are exact executable names (no paths, no wildcards), matched case-insensitively, and no name may be both critical and batch.
- `logging.max_file_size_mb`: 1–1024. `logging.max_rotated_files`: 0–100.

`processes.protected`: batch processes with these names are never throttled or suspended (`ActuatorManager::SetProtectedProcessNames`). The Actuator also always protects its own process and core Windows processes (`System`, `Registry`, `smss.exe`, `csrss.exe`, `wininit.exe`, `winlogon.exe`, `services.exe`, `lsass.exe`); config cannot remove those.

**Optional sections.** Each one can be left out entirely; the defaults are shown. Values are validated with the same rules: wrong type or out of range stops startup, naming the field.

```json
"telemetry":    { "cpu_normalization": "per_core", "log_samples": true, "self_monitor_interval_seconds": 30 },
"triggers": {
  "memory": { "enabled": false, "system_load_limit_percent": 95, "critical_working_set_limit_mb": 0 },
  "io":     { "enabled": false, "critical_io_limit_mb_per_sec": 200 }
},
"adaptive":     { "enabled": false, "window_samples": 10, "horizon_samples": 3, "max_adjust_percent": 10 },
"alerts":       { "popup": true, "webhook_url": "" },
"coordination": { "enabled": false, "controller_host": "127.0.0.1", "controller_port": 7878, "shared_token": "", "heartbeat_seconds": 5 },
"security":     { "require_protected_config": false }
```

- `telemetry.cpu_normalization`: `"per_core"` means 100% is one logical core, so a 4-thread process can read 400%. `"total"` means 100% is every logical core.
- `telemetry.self_monitor_interval_seconds`: 0–86400, where 0 turns self-monitoring off.
- `triggers.*`: each signal is converted to a percentage of its limit and goes through the same thresholds as CPU. The highest signal is used.
  - System memory: `load / system_load_limit_percent × 100`.
  - Working set: `MB / critical_working_set_limit_mb × 100`, where 0 means off.
  - I/O: `MB/s / critical_io_limit_mb_per_sec × 100`.
- `adaptive.*`: `window_samples` 3–600, `horizon_samples` 1–60, `max_adjust_percent` 0–50.
  - Lowers the high and critical thresholds while load is rising (Source/Telemetry/AdaptiveThresholds.h). The low threshold is never changed.
  - The controller's bias is added under the same cap, and applies even with `enabled: false`.
- `alerts.webhook_url`: must start with `http://` or `https://`. On suspension it receives an HTTP POST of the `actuator_action` JSON plus `"alert": "batch_suspended"`.
- `coordination.controller_port`: 1–65535. `heartbeat_seconds`: 1–3600. The wire protocol is in `Source/Daemon/Coordination.h`.
- `security.require_protected_config`: when `true`, a config file that Everyone, Users, Authenticated Users or Interactive can write is refused instead of only warned about.

**Hot reload.** The file's timestamp and size are checked every poll. A valid change is applied immediately to thresholds, process lists, the protection list, logging, alerts and coordination. Batch processes whose name was removed are restored and released, with trigger `config_reload`. An invalid change is logged as a `config_reload` failure, and the previous config stays in force.

Log rotation is size-based: when the next line would push the file past `max_file_size_mb`, it is renamed to `<log_path>.1` (older files shift to `.2`, `.3`, ...) and files beyond `max_rotated_files` are deleted.

---

## 5. Change Protocol

If any layer needs a field added/renamed/removed in sections 2–4: edit this file first, propose the change for review (for example in the pull request that needs it), get approval from the maintainers of the other affected modules, THEN change code. This file being out of sync with reality is the single most likely cause of integration failure — treat edits to it as seriously as any API contract change.
