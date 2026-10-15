# ThreadHaul-Daemon

A Windows service that stops batch jobs from starving the real-time processes on a logistics dispatch server.

It watches one or more **critical** processes (driver tracking, route-dispatch APIs) and one or more **batch** processes (invoicing, inventory sync, archiving). When a critical process is starved, it handles each batch process independently:

1. **Throttle.** After 3 consecutive samples at or above 85%, the batch process is demoted to `IDLE_PRIORITY_CLASS`.
2. **Escalate.** If the critical process stays at or above 90% for 5 more samples, every thread of the batch process is suspended.
3. **Recover.** After 5 consecutive samples below 60%, threads are resumed and normal priority is restored.

Every threshold, process name and interval comes from `config.json` and can be changed while the service is running.

Team / module owners: Divik (Actuator), Prakul (Telemetry), Pranav (Config / Logging / Integration). Cross-module contracts are in [INTERFACES.md](INTERFACES.md). The specification is [specs/logistics-resource-monitor.md](specs/logistics-resource-monitor.md).

**Documentation:**

| Document | Read it to |
|---|---|
| [RUNNING.md](RUNNING.md) | Build, install, demonstrate and remove the service, step by step (start here if you are new) |
| [Source/Docs/ModuleGuides/](Source/Docs/ModuleGuides/) | Understand one module in depth: one PDF per team member covering its files, Windows APIs, functions and review questions. Regenerate them with `python Source\Docs\module_guides.py`. |
| [Source/Docs/literature-review.md](Source/Docs/literature-review.md) | See the research behind the design decisions |
| [INTERFACES.md](INTERFACES.md) | Look up the event struct, log schema and config schema shared between modules |

---

## Repository layout

```
build.cmd                     build everything into build\ ("build.cmd test" also runs the tests)
INTERFACES.md                 cross-layer contract: event struct, log schema, config schema
Source/
  main.cpp                    service entry point, console mode, CLI manual override
  Actuator/                   state machine, SetPriorityClass, Suspend/ResumeThread, protection list
  Telemetry/                  process discovery, CPU%/memory/I-O sampling, adaptive thresholds
  Config/                     config.json loading + validation, sample config.json
  Logging/                    JSON Lines logger with rotation, Windows Event Log writer
  Daemon/                     integration loop, hot reload, control pipe, alerts, coordination client
  Controller/                 ThreadHaulController (central coordinator for many servers)
  Simulators/                 critical_simulator and batch_simulator test apps
  Scripts/                    install.ps1, uninstall.ps1, log_viewer.py
  Tests/                      unit tests (state machine, telemetry, config/logging)
  Docs/literature-review.md   formal literature review and research gaps
third_party/nlohmann/json.hpp
```

## Build

Requirements: Windows 10/11 or Server 2016+, Visual Studio 2019 or later with "Desktop development with C++". For the log viewer: Python 3 and `pip install matplotlib`.

```bat
build.cmd
```

`build.cmd` finds Visual Studio on its own (it works from a plain prompt) and writes these to `build\`:

| Output | What it is |
|---|---|
| `ThreadHaulDaemon.exe` | the monitor (service, console mode, CLI) |
| `ThreadHaulController.exe` | central coordinator for multi-server setups |
| `critical_simulator.exe`, `batch_simulator.exe` | dummy test apps |
| `StateMachineTests.exe`, `TelemetryTests.exe`, `ConfigLoggingTests.exe` | unit tests |
| `config.json` | copy of the sample config |

The Visual Studio project `Source/Actuator/ActuatorModule.vcxproj` builds the same daemon (Debug or Release, x64).

## Run the unit tests

```bat
build.cmd test
```

There are 189 checks across three suites:
- **CPU% from kernel+user ticks:** both normalisations, clamping, zero wall time.
- **Threshold state machine:** 3/5/5 consecutive-sample rules, boundaries, escalation and recovery, manual holds, live config changes.
- **Adaptive thresholds:** slope, forecast, cap, hysteresis, controller bias.
- **Process tracking:** a missing process, then a crash detected with its exit code.
- **Config validation** and the JSON Lines log schema and rotation.

## Demo with the dummy apps (console mode)

Run each command in its own terminal, from `build\`. Run the first one elevated if you want to manage other users' processes; for these demo apps it isn't needed.

```bat
ThreadHaulDaemon.exe --console --config config.json --allow-unelevated
batch_simulator.exe --threads 2
critical_simulator.exe
```

The critical simulator's default schedule repeats this cycle:
- 8 s at 30%;
- 20 s at 100%, long enough to throttle (3 s) and then suspend (5 more s);
- 15 s at 20%, long enough to recover (5 s).

Watch the batch simulator's console:
- its priority switches to `IDLE`;
- its output stops while it is suspended;
- it prints `<- resumed after a pause` when it is resumed.

To force a crash or a restart:
- `critical_simulator.exe --load 100 --crash-after 10` exits with code 3 after 10 s. Start it again and the daemon logs `restarted` with the new PID.

Then graph the run:

```bat
python ..\Source\Scripts\log_viewer.py <log_path from config.json> --out run.png
```

The viewer prints every state change and the daemon's own CPU/memory footprint, then draws three graphs:
- critical-process CPU against the (adaptive) thresholds;
- the state timeline of each batch process;
- the daemon's own resource use.

## Install as a Windows Service

From an **elevated** PowerShell, after `build.cmd`:

```powershell
powershell -ExecutionPolicy Bypass -File Source\Scripts\install.ps1
```

The installer:
- validates `config.json` with the binary before installing anything;
- copies the binary to `%ProgramFiles%\ThreadHaulDaemon`;
- installs the config to `%ProgramData%\ThreadHaulDaemon\config.json`;
- locks the config down (see Security below);
- registers the `ThreadHaulDaemon` Application event-log source;
- creates the service: auto-start, restart on failure, only the privileges it needs;
- starts the service.

Useful flags:
- `-Account LocalService`
- `-OverwriteConfig`
- `-NoStart`

Uninstall with `Source\Scripts\uninstall.ps1`. Add `-RemoveData` to also delete the config and logs. Stopping the service always resumes suspended threads and restores priorities first.

Logs go to:
- `%ProgramData%\ThreadHaulDaemon\logs\threadhaul.jsonl`;
- Windows Event Viewer → Windows Logs → Application, source `ThreadHaulDaemon`. Event IDs: 1000 throttled, 1001 suspended, 1002 recovered, 1900 startup error.

## CLI manual override

These commands talk to the running daemon (service or console) over a local named pipe that only Administrators, SYSTEM and the daemon's own account can open:

```bat
ThreadHaulDaemon.exe throttle <pid|name>   demote to IDLE priority and hold it there
ThreadHaulDaemon.exe suspend  <pid|name>   suspend every thread and hold
ThreadHaulDaemon.exe resume   <pid|name>   resume + normal priority and hold
ThreadHaulDaemon.exe release  <pid|name>   hand the process back to automatic control
ThreadHaulDaemon.exe status                pressure, effective thresholds, every batch state
ThreadHaulDaemon.exe stop                  stop a console-mode daemon cleanly
ThreadHaulDaemon.exe validate-config [path]
```

How these behave:
- A forced state is held until `release`, so the state machine never fights the operator.
- Protected processes and configured critical processes are always refused.
- With no daemon running, `throttle`, `suspend` and `resume` act on the process directly, as a debugging fallback.

## Configuration

See [`Source/Config/config.json`](Source/Config/config.json). The full schema and validation rules are in [INTERFACES.md §4](INTERFACES.md).

**Required sections:**
- `polling`
- `thresholds` (`high`, `critical`, `low`, each with `percent` and `consecutive_samples`)
- `processes` (`critical`, `batch`, optional `protected`)
- `logging` (path, rotation size, rotated-file count)

**Optional sections:**
- `telemetry`: CPU normalisation, self-monitor interval.
- `triggers`: memory and I/O signals.
- `adaptive`: predictive thresholds.
- `alerts`: popup and webhook.
- `coordination`: controller.
- `security`: refuse a config that non-administrators can modify.

Invalid config stops startup with a message that names the field, for example `thresholds.low.percent must be below thresholds.high.percent`. While the daemon is running, edits to the file are applied on the next poll. An invalid edit is rejected and logged, and the last good config stays in force.

**CPU normalisation:** with the default `per_core`, 100% means one fully used logical core, so one busy thread reads about 100% on any machine. Set `"cpu_normalization": "total"` to make 100% mean the whole machine.

## Multi-server coordination

On a central machine:

```bat
ThreadHaulController.exe --port 7878 --token <secret> --pressure 85 --cluster-percent 50 --bias 5
```

On each dispatch server, set `"coordination": { "enabled": true, "controller_host": "<host>", "shared_token": "<secret>" }`.

Each daemon:
- connects to the controller, reconnecting with back-off;
- sends a heartbeat every few seconds;
- forwards every throttle, suspend and resume event.

The controller:
- logs everything to `controller.jsonl`;
- when at least half the servers are above 85%, tells all of them to lower their throttle thresholds by 5 points, and clears that when load drops;
- accepts operator commands such as `list`, `DISPATCH-01 suspend invoice_batch.exe` or `all release invoice_batch.exe`.

## Alerts

When a batch process is suspended, the daemon:
- shows a message box in the active console session (`alerts.popup`). This works from a Session 0 service, where toast notifications don't;
- POSTs a JSON body to `alerts.webhook_url`, if one is set. Point it at Teams or Slack, or at an email relay for email alerts.

## Security and privileges

- **Least privilege:**
  - The service is created with only `SeDebugPrivilege` (needed to open other accounts' processes) and `SeChangeNotifyPrivilege`; every other LocalSystem privilege is removed (`sc privs`).
  - `-Account LocalService` runs it with even fewer rights, but then it can only manage processes that also run as LocalService.
- **Config tamper protection:**
  - The installer removes inherited permissions from `config.json`: SYSTEM and Administrators get full control, the service account gets read access, and other users get nothing.
  - On every load the daemon checks that Everyone, Users, Authenticated Users and Interactive cannot write the file, and logs a warning if they can. With `security.require_protected_config: true` it refuses to load such a file instead.
  - Every load records the file's SHA-256 in the log, so any change is visible.
- **Protection list:** `processes.protected` plus a built-in list (`System`, `csrss.exe`, `wininit.exe`, `winlogon.exe`, `services.exe`, `lsass.exe`, `smss.exe`, `Registry`, and the daemon itself) can never be throttled or suspended.
- **Access denied:** when a target can't be opened (protected or elevated), this is logged (`access_denied` lifecycle event, or `actuation_succeeded: false`) and the daemon carries on.

## Design rationale

The full reasoning is in [Source/Docs/literature-review.md](Source/Docs/literature-review.md), which reviews 10 papers from 2023–2026 (ACM, IEEE, Elsevier, Springer, MDPI, Nature). In short:

- **Predictive thresholds instead of static ones:** 2023–2026 work (e.g. DeepScaling, IEEE/ACM ToN 2024) shows fixed CPU targets react too late. ThreadHaul fits a trend to recent load and lowers its thresholds while load is climbing, so batch work is shed before the critical process starves. The recovery threshold never moves, so it can't cause oscillation.
- **A lightweight predictor:** published predictors are deep models running on cluster control planes. A monitor on the server it protects must not add load, so ThreadHaul uses a closed-form linear fit. Self-monitoring shows about 1% of one core and about 8 MB.
- **Memory and I/O triggers:** colocation research (e.g. SODA, IEEE TC 2023) shows interference goes beyond CPU share. Memory and I/O are scaled against configured limits and share the same thresholds.
- **Graded, per-process escalation:** fine-grained core scheduling (Vessel, SOSP 2024) needs special hardware. With only OS priority classes, ThreadHaul escalates step by step (priority, then suspension) and independently for each batch process.
- **Cluster coordination:** scheduling research is moving to multi-node decisions (IEEE COMST 2024). The controller's cluster bias lets cooler servers shed load early during fleet-wide peaks.

## Verification status

**As an installed Windows Service** (Administrator, Windows 11):
- `install.ps1` validated the config, installed the program and config, locked the config down, registered the event source, and created and started the service (Automatic start, LocalSystem).
- The service answered `status` over the control pipe.
- The test programs produced the full throttle → suspend → recover cycle, with Event Log entries and the suspension pop-up.

**In console mode** (non-elevated session, Windows 11, 16 logical cores):
- All 189 unit-test checks pass.
- Both build paths compile with no compiler warnings.
- The full console-mode scenario ran:
  - automatic throttle, then suspend, then recover;
  - CLI overrides, and refusal of a protected process;
  - controller commands and the cluster bias;
  - hot reload, and rejection of an invalid edit;
  - a critical-process crash with exit code 3, and a restart under a new PID;
  - a clean stop that resumes held processes;
  - adaptive throttling ahead of the static threshold.

**Not yet verified:**
- `uninstall.ps1` passes a PowerShell syntax check but hasn't been confirmed end to end.
- The installer's `-Account LocalService` option hasn't been tried.

On machines with Smart App Control turned on, freshly built unsigned executables may occasionally be blocked ("An Application Control policy has blocked this file"). Re-running usually works, or the binaries can be code-signed.
