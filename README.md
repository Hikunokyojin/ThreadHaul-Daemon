# ThreadHaul-Daemon

**Keep latency-critical Windows workloads responsive by automatically throttling and pausing background batch jobs when CPU runs short.**

ThreadHaul-Daemon is a lightweight Windows Service, written in C++17 against the standard Win32 API. It watches the processes you mark as *critical*. When they are starved of CPU, it steps in against the processes you mark as *batch*: first it lowers their priority, then, if that isn't enough, it suspends them. Once load returns to normal, it restores them automatically. Every decision is recorded in a structured log and in the Windows Event Log.

---

## Contents

- [The problem](#the-problem)
- [How it solves it](#how-it-solves-it)
- [Features](#features)
- [How it works](#how-it-works)
- [Requirements](#requirements)
- [Quick start](#quick-start)
- [Building from source](#building-from-source)
- [Installing as a Windows Service](#installing-as-a-windows-service)
- [Using the service](#using-the-service)
- [Web dashboard](#web-dashboard)
- [Configuration](#configuration)
- [Logs and monitoring](#logs-and-monitoring)
- [Multi-server deployments](#multi-server-deployments)
- [Security model](#security-model)
- [Troubleshooting](#troubleshooting)
- [Project layout](#project-layout)
- [Running the tests](#running-the-tests)
- [Known limitations](#known-limitations)
- [Contributing](#contributing)

---

## The problem

Servers rarely run one thing. A dispatch or order-routing server, for example, often runs a real-time API that drivers and customers depend on next to batch work: invoice generation, inventory synchronisation, report building, archiving.

Most of the time this is fine. During peak load, though, the batch work competes for the same CPU cores. The Windows scheduler treats both kinds of work as equally important unless told otherwise, so the latency-critical service slows down at exactly the moment it matters most. The usual workarounds all have drawbacks:

| Workaround | Drawback |
|---|---|
| Schedule batch jobs off-peak | Peaks are not always predictable, and some batch work cannot wait until night |
| Permanently lower batch priority | Batch jobs run slowly even when the server is idle |
| Run batch work on separate machines | Extra hardware and operational cost |
| Kill batch jobs by hand during incidents | Slow, error-prone, and loses work in progress |

## How it solves it

ThreadHaul-Daemon makes the decision continuously and automatically, based on what the critical workload is actually experiencing:

1. **Measure.** Once per second, it samples the CPU usage of each critical process. Memory and I/O pressure can optionally be included.
2. **Throttle.** If a critical process stays at or above 85% for 3 consecutive seconds, each batch process is demoted to *Idle* priority. Batch jobs then only run on CPU time that nothing else needs.
3. **Suspend.** If the critical process is still at or above 90% for 5 more seconds, every thread of each batch process is suspended. The work is paused, not lost.
4. **Recover.** Once the critical process stays below 60% for 5 consecutive seconds, batch processes are resumed and returned to normal priority.

Batch work runs at full speed whenever the server has capacity, and steps aside the moment it doesn't. All thresholds, durations and process names are configurable and can be changed without restarting the service.

## Features

**Core control**
- Graded response: priority demotion first, thread suspension only when needed, automatic recovery after that.
- Each batch process has its own state machine, so one job can be suspended while another is only throttled.
- Multiple critical and multiple batch processes, matched by executable name (case-insensitive), including multiple instances of the same name.
- Optional **predictive thresholds**: when load is rising quickly, the thresholds are lowered temporarily so batch work is shed *before* the critical process is starved.
- Optional **memory and I/O triggers** alongside CPU.

**Operations**
- Runs as a native Windows Service: automatic start, restart on failure, clean shutdown.
- **Live configuration reload.** Edits to `config.json` apply within one poll interval. Invalid edits are rejected, and the service keeps running on the last valid configuration.
- **Web dashboard** on `127.0.0.1`:
  - live statistics and charts;
  - a list of every running process, where you assign roles (critical, batch, protected) with no config editing;
  - throttle, suspend, resume and release buttons;
  - a searchable, filterable activity log with raw-JSON view and CSV export.
- **Manual override CLI.** Throttle, suspend, resume or release any batch process on demand, and query live status.
- **Structured JSON Lines log** with size-based rotation, plus **Windows Event Log** entries.
- **Alerts** when a process is suspended: a desktop pop-up and/or an HTTP webhook (Teams, Slack, or an email relay).
- **Self-monitoring.** The service logs its own CPU and memory use, typically about 1% of one core and under 10 MB.
- Optional **central controller** for fleets of servers, with cluster-wide coordination.

**Safety**
- A protection list that can never be throttled or suspended. Core Windows processes and the service itself are always protected.
- Handles targets that aren't running yet, that crash, or that restart under a new process ID.
- Guards against process-ID reuse, and against a process exiting while it is suspended.
- Least-privilege service account, a tamper-protected configuration file, and an administrator-only control channel.
- Stopping the service always restores every process it has touched.

## How it works

```
                 every poll interval (default 1 s)
 +----------------------+   pressure %   +------------------------+   Win32 calls   +-----------------+
 |      Telemetry       | -------------> |        Actuator        | --------------> |  batch process  |
 | find critical procs, |                | one state machine per  |  SetPriority-   |  (e.g. invoice  |
 | sample CPU/mem/I-O   |                | batch process          |  Class, Suspend-|   generation)   |
 +----------------------+                +------------------------+  /ResumeThread  +-----------------+
            |                                        | every state change
            v                                        v
 +-----------------------------------------------------------------------------------------------+
 |  Service core: configuration + live reload, JSON Lines log, Event Log, alerts,                 |
 |  control pipe (CLI), optional controller connection                                           |
 +-----------------------------------------------------------------------------------------------+
```

**Telemetry.** Finds processes by executable name with a Toolhelp snapshot. CPU usage is derived from `GetProcessTimes` (kernel + user time) between two samples. Memory comes from `GetProcessMemoryInfo`, and I/O from `GetProcessIoCounters`.

CPU is normalised **per core** by default: 100% means one fully used logical core, so a threshold means the same thing on a 4-core and a 64-core machine. Whole-system normalisation is available as an option.

**Actuator.** Each batch process moves through three states:

```
            >= high% for N samples                >= critical% for M samples
  NORMAL  ------------------------->  THROTTLED  ---------------------------->  SUSPENDED
    ^        (SetPriorityClass IDLE)      |          (SuspendThread on all threads)    |
    |                                     |  < low% for K samples                      |
    +-------------------------------------+--------------------------------------------+
                         (ResumeThread + SetPriorityClass NORMAL)
```

Escalation counts samples *after* throttling, so suspension only happens when demotion has demonstrably not been enough. Recovery uses a lower threshold than throttling. That gap (hysteresis) stops a process from flapping between states.

**Predictive thresholds (optional).** A least-squares trend is fitted over the last few pressure samples. If load is rising and is forecast to cross the throttle threshold, the throttle and suspend thresholds are lowered by the forecast rise, up to a configurable cap. The recovery threshold never moves, so prediction can't cause oscillation. The calculation is a handful of arithmetic operations per second, so the cost is negligible.

## Requirements

| Requirement | Notes |
|---|---|
| Windows 10, Windows 11, or Windows Server 2016 or later (x64) | |
| Administrator rights | To install the service, and to manage processes owned by other accounts |
| Visual Studio 2019 or later with the **Desktop development with C++** workload | Build only. The free Community edition is sufficient. |
| Python 3 with `matplotlib` | Optional, only for the log-graphing script |

There are no other dependencies. The JSON library ([nlohmann/json](https://github.com/nlohmann/json)) is bundled in `third_party/`.

## Quick start

These commands go from a fresh clone to a running service. Use an **Administrator PowerShell** opened in the repository folder.

```powershell
.\build.cmd                                                        # 1. compile
powershell -ExecutionPolicy Bypass -File .\Source\Scripts\install.ps1   # 2. install and start the service
& "C:\Program Files\ThreadHaulDaemon\ThreadHaulDaemon.exe" status   # 3. confirm it responds
```

To see it in action, start the bundled test programs in two other PowerShell windows (from the repository folder):

```powershell
.\build\batch_simulator.exe --threads 2     # window 2: a CPU-hungry "batch job"
.\build\critical_simulator.exe              # window 3: a "critical service" that periodically hits 100%
```

Within a few seconds of each full-load phase, the batch window's output changes:
- it shows `priority IDLE` (throttled);
- then it stops printing (suspended);
- then it reports `resumed after a pause` (recovered).

To choose the real programs you want managed, open the dashboard and assign roles on its **Processes** tab:

```powershell
& "C:\Program Files\ThreadHaulDaemon\ThreadHaulDaemon.exe" dashboard
```

For a detailed walkthrough with expected output at every step, see **[RUNNING.md](RUNNING.md)**.

## Building from source

```powershell
.\build.cmd          # build everything into .\build
.\build.cmd test     # build, then run the unit tests
```

`build.cmd` locates Visual Studio automatically via `vswhere`, so it works from any prompt. It produces:

| Output | Purpose |
|---|---|
| `ThreadHaulDaemon.exe` | The service, console mode and command-line tool |
| `ThreadHaulController.exe` | Optional central controller for multiple servers |
| `critical_simulator.exe`, `batch_simulator.exe` | Test workloads for demonstrations and validation |
| `StateMachineTests.exe`, `TelemetryTests.exe`, `ConfigLoggingTests.exe` | Unit tests |
| `config.json` | A copy of the sample configuration |

The compiler output is written to `build\build.log`. A Visual Studio project is also provided at `Source/Actuator/ActuatorModule.vcxproj` (Debug/Release, x64).

## Installing as a Windows Service

From an **Administrator PowerShell** in the repository folder:

```powershell
powershell -ExecutionPolicy Bypass -File .\Source\Scripts\install.ps1
```

The installer:

1. Validates the configuration file and stops if it is invalid.
2. Replaces any previous installation. The old service is stopped cleanly, which restores any processes it was managing.
3. Copies the program to `C:\Program Files\ThreadHaulDaemon\`.
4. Installs the configuration to `C:\ProgramData\ThreadHaulDaemon\config.json`. An existing configuration is kept unless you pass `-OverwriteConfig`.
5. Restricts the configuration file so only SYSTEM and Administrators can change it.
6. Registers the `ThreadHaulDaemon` source in the Windows Application event log.
7. Creates the service: automatic start, restart on failure, and only the privileges it needs.
8. Starts the service.

| Option | Effect |
|---|---|
| `-OverwriteConfig` | Replace an existing installed `config.json` with the one from the repository |
| `-Account LocalService` | Run under the lower-privilege LocalService account. It can then only manage processes that also run as LocalService. |
| `-NoStart` | Install without starting |
| `-BinDir <path>` / `-ConfigSource <path>` | Install from a different build folder or configuration file |

**Service management**

```powershell
Get-Service ThreadHaulDaemon        # check state
Stop-Service ThreadHaulDaemon       # stops after restoring every managed process
Start-Service ThreadHaulDaemon
Restart-Service ThreadHaulDaemon
```

**Uninstalling**

```powershell
powershell -ExecutionPolicy Bypass -File .\Source\Scripts\uninstall.ps1              # keep configuration and logs
powershell -ExecutionPolicy Bypass -File .\Source\Scripts\uninstall.ps1 -RemoveData  # remove everything
```

### Running without installing

For evaluation, or on machines where you cannot install a service, the same engine runs in the foreground:

```powershell
.\build\ThreadHaulDaemon.exe --console --config .\build\config.json
```

Add `--allow-unelevated` to run without administrator rights. In that mode it can only manage processes started by your own user account. Press **Ctrl+C**, or run `ThreadHaulDaemon.exe stop`, to stop. Stopping always restores managed processes.

> The sample configuration logs to `C:\ProgramData\...`, which a standard user can't write to. When running unelevated, set `logging.log_path` to a writable location, such as `"threadhaul.jsonl"`.

## Using the service

The executable doubles as a command-line tool that talks to the running service (or console instance) over a local, administrator-only named pipe.

```powershell
$thd = "C:\Program Files\ThreadHaulDaemon\ThreadHaulDaemon.exe"

& $thd status                          # current pressure, effective thresholds, every managed process
& $thd throttle invoice_batch.exe      # demote to Idle priority, and hold
& $thd suspend  invoice_batch.exe      # suspend all threads, and hold
& $thd resume   invoice_batch.exe      # resume at normal priority, and hold
& $thd release  invoice_batch.exe      # return to automatic control
& $thd dashboard                       # open the web dashboard in your browser
& $thd validate-config C:\ProgramData\ThreadHaulDaemon\config.json
& $thd help
```

- Targets can be an executable name (all instances) or a process ID.
- Manual actions are **held** until `release`, so the automatic logic never fights an operator decision.
- Processes on the protection list, and processes configured as critical, are always refused.
- If no service is running, `throttle`, `suspend` and `resume` act on the process directly. This is useful for testing.

`status` returns a single JSON object, which makes it easy to use from scripts or monitoring agents:

```json
{"ok":true,"pressure_percent":97.3,"pressure_signal":"cpu","critical_process_id":4821,
 "offset_percent":0.0,"thresholds":{"high":85.0,"critical":90.0,"low":60.0},
 "coordination_connected":false,
 "critical":[{"name":"dispatch_api.exe","pid":4821,"cpu_percent":97.3}],
 "batch":[{"name":"invoice_batch.exe","pid":5190,"state":"throttled","manual_hold":false}]}
```

## Web dashboard

The service includes a web dashboard served at `http://127.0.0.1:8787` (loopback only). Open it from an Administrator prompt:

```powershell
& "C:\Program Files\ThreadHaulDaemon\ThreadHaulDaemon.exe" dashboard
```

This asks the running service for a per-session access key over the administrator-only control channel, then opens your default browser with that key.

| Tab | What it provides |
|---|---|
| **Overview** | Critical pressure against the effective thresholds; system CPU and memory; batch process counts by state; service uptime and footprint. Also a five-minute live chart, with periods when a batch process was suspended shaded, and per-process Throttle / Suspend / Resume / Release buttons. |
| **Processes** | Every running process, with CPU % (per logical core) and memory, sortable and searchable. Set each program's role (**Critical**, **Batch**, **Protected** or **Unassigned**). The change is written to `config.json` and applied immediately, and the file's permissions are preserved. Windows system processes and the service itself are shown as *system protected* and can't be assigned. |
| **Activity log** | The JSON Lines log rendered as a readable table, newest first, colour-coded by severity. Filter by event type, search text and time range; click a row for the pretty-printed raw JSON; export the filtered rows as CSV. New entries stream in live. |

**HTTP API.** The page is a thin client over a small JSON API, which you can also script against. Every request needs the `X-ThreadHaul-Token` header.

| Method and path | Purpose |
|---|---|
| `GET /api/status` | The `status` object plus service details, the configured process lists, and system CPU and memory |
| `GET /api/processes` | Every running process with CPU %, memory, role and managed state |
| `GET /api/log?offset=N` | Log entries after byte offset `N`. `-1` returns the tail of the file. The response includes the next offset and a `reset` flag set after log rotation. |
| `POST /api/command` | `{"command":"suspend","target":"<pid or exe name>"}`. Commands are `throttle`, `suspend`, `resume` and `release`. |
| `POST /api/role` | `{"name":"<exe name>","role":"critical"}`. Roles are `critical`, `batch`, `protected` and `none`. |

To change the port or turn the dashboard off, set `dashboard.port` or `dashboard.enabled` in `config.json`.

## Configuration

The installed service reads `C:\ProgramData\ThreadHaulDaemon\config.json`. Edit it from an elevated editor, for example `notepad C:\ProgramData\ThreadHaulDaemon\config.json` in an Administrator PowerShell. Changes apply within one poll interval; no restart is needed.

### Minimal configuration

```json
{
  "polling":    { "interval_ms": 1000 },
  "thresholds": {
    "high":     { "percent": 85, "consecutive_samples": 3 },
    "critical": { "percent": 90, "consecutive_samples": 5 },
    "low":      { "percent": 60, "consecutive_samples": 5 }
  },
  "processes": {
    "critical":  ["dispatch_api.exe"],
    "batch":     ["invoice_batch.exe", "inventory_sync.exe"],
    "protected": ["explorer.exe"]
  },
  "logging": {
    "log_path": "C:\\ProgramData\\ThreadHaulDaemon\\logs\\threadhaul.jsonl",
    "max_file_size_mb": 50,
    "max_rotated_files": 5
  }
}
```

### Reference

| Setting | Description | Default / range |
|---|---|---|
| `polling.interval_ms` | Time between samples | 1000 (100–60000) |
| `thresholds.high` | Throttle when pressure ≥ `percent` for `consecutive_samples` | 85%, 3 |
| `thresholds.critical` | Suspend when pressure ≥ `percent` for `consecutive_samples` after throttling | 90%, 5 |
| `thresholds.low` | Recover when pressure < `percent` for `consecutive_samples` | 60%, 5 |
| `processes.critical` | Executable names to protect. May be empty; assign them from the dashboard. | |
| `processes.batch` | Executable names that may be throttled or suspended. May be empty. | |
| `processes.protected` | Executable names that must never be touched | optional |
| `logging.log_path` | JSON Lines log file | |
| `logging.max_file_size_mb` / `max_rotated_files` | Rotation policy | 50 MB, 5 files |
| `telemetry.cpu_normalization` | `per_core` (100% = one core) or `total` (100% = whole machine) | `per_core` |
| `telemetry.log_samples` | Write a log line for every sample | `true` |
| `telemetry.self_monitor_interval_seconds` | How often the service logs its own footprint (0 = off) | 30 |
| `triggers.memory.enabled` | Include memory pressure | `false` |
| `triggers.memory.system_load_limit_percent` | System memory load treated as 100% pressure | 95 |
| `triggers.memory.critical_working_set_limit_mb` | Critical-process working set treated as 100% (0 = off) | 0 |
| `triggers.io.enabled` / `critical_io_limit_mb_per_sec` | Include I/O throughput of the critical process | `false`, 200 |
| `adaptive.enabled` | Predictive thresholds | `false` |
| `adaptive.window_samples` / `horizon_samples` / `max_adjust_percent` | Trend window, forecast distance, maximum lowering | 10, 3, 10 |
| `alerts.popup` | Desktop pop-up when a process is suspended | `true` |
| `alerts.webhook_url` | HTTP(S) endpoint that receives a JSON POST on suspension | empty |
| `coordination.*` | Central controller connection (see below) | disabled |
| `security.require_protected_config` | Refuse a configuration file that non-administrators can modify | `false` |
| `dashboard.enabled` / `dashboard.port` | Web dashboard on `127.0.0.1` | `true`, 8787 (1024–65535) |

**Validation rules:**
- Every required field must be present and within range.
- `low` must be below `high`, and `high` must not exceed `critical`.
- A name can't be both critical and batch.
- Names must be executable names, not paths.

An invalid file stops the service at startup, with a message naming the field (also written to the Event Log). The same file edited while the service is running is rejected, and the previous configuration stays active.

**Memory and I/O triggers** are each expressed as a percentage of their configured limit. They go through the same thresholds as CPU, and the highest signal wins. For example, with `system_load_limit_percent` set to 95, a system memory load of 81% counts as 85% pressure.

## Logs and monitoring

### JSON Lines log

Each line of the log file is one complete JSON object, written in this form: `log_type`, then `timestamp` (ISO 8601 UTC, milliseconds), then snake_case fields. The file rotates by size.

```json
{"log_type":"actuator_action","timestamp":"2026-10-15T07:27:41.749Z","batch_process_id":5190,"batch_process_name":"invoice_batch.exe","critical_process_id":4821,"action":"threads_suspended","from_state":"throttled","to_state":"suspended","cpu_percent":100.0,"actuation_succeeded":true,"trigger":"automatic"}
```

| `log_type` | Written when |
|---|---|
| `actuator_action` | A batch process changes state. `trigger` is `automatic`, `manual`, `shutdown` or `config_reload`. |
| `sample` | Every poll, per critical process: CPU, memory, I/O and combined pressure |
| `threshold_event` | A threshold streak completes |
| `process_lifecycle` | A target appears, exits, crashes (with its exit code), restarts under a new ID, or is refused |
| `config_reload` / `config_security` | The configuration changed or was rejected, or the configuration file's permissions are too open |
| `adaptive_threshold` | Predictive thresholds moved |
| `manual_override` | A CLI or controller command was executed |
| `alert` | A suspension alert was sent |
| `self_monitor` | Periodically: the service's own CPU, memory and handle count |
| `service_lifecycle` | The service started or stopped. The start entry includes the configuration file's SHA-256. |
| `dashboard` / `dashboard_action` | The dashboard started (or failed to start), or a role was changed from the dashboard |

Follow it live:

```powershell
Get-Content C:\ProgramData\ThreadHaulDaemon\logs\threadhaul.jsonl -Tail 20 -Wait
```

### Graphs

```powershell
python -m pip install matplotlib
python .\Source\Scripts\log_viewer.py C:\ProgramData\ThreadHaulDaemon\logs\threadhaul.jsonl --out activity.png
```

This prints a table of every state change and the service's resource footprint, then renders three charts:
- critical-process load against the (possibly adaptive) thresholds;
- each batch process's state over time;
- the service's own CPU and memory use.

### Windows Event Log

Source **ThreadHaulDaemon** in the *Application* log:

| Event ID | Meaning |
|---|---|
| 1000 | Batch process throttled |
| 1001 | Batch process suspended (warning) |
| 1002 | Batch process recovered |
| 1900 | Service failed to start. The message gives the reason. |

```powershell
Get-EventLog -LogName Application -Source ThreadHaulDaemon -Newest 10
```

## Multi-server deployments

For fleets of servers, an optional controller collects state from every node and coordinates them.

**On the controller host:**

```powershell
.\build\ThreadHaulController.exe --port 7878 --token <shared-secret> --pressure 85 --cluster-percent 50 --bias 5
```

**On each server**, in `config.json`:

```json
"coordination": {
  "enabled": true,
  "controller_host": "controller.example.internal",
  "controller_port": 7878,
  "shared_token": "<shared-secret>",
  "heartbeat_seconds": 5
}
```

Each service connects with the shared token, then:
- sends a heartbeat (pressure, thresholds, batch states) every few seconds;
- forwards every state change.

It reconnects automatically if the controller is unavailable, and keeps protecting its own server in the meantime.

When at least `--cluster-percent` of servers are above `--pressure`, the controller sends every server a **bias** that lowers their throttle thresholds by `--bias` points. Servers that are not yet starved then start shedding batch load early, which reduces pressure across the fleet. The bias is cleared when the cluster calms down.

Operators can type commands at the controller console:

```
list                                     every connected server and its batch processes
web-01 suspend invoice_batch.exe         act on one server
all release invoice_batch.exe            act on every server
quit
```

The controller writes its own JSON Lines log (`controller.jsonl` by default). Allow the chosen TCP port through the controller host's firewall.

## Security model

- **Least privilege.**
  - The service is installed with every privilege removed except `SeDebugPrivilege`, needed to open processes owned by other accounts, and `SeChangeNotifyPrivilege`.
  - The installer's `-Account LocalService` option runs it under a lower-privilege account where that is sufficient.
- **Protected configuration.**
  - The installer removes inherited permissions from `config.json`: SYSTEM and Administrators can modify it, the service account can read it, and other users have no access.
  - At every load the service checks that Everyone, Users, Authenticated Users and Interactive cannot write the file, and logs a warning if they can. With `security.require_protected_config` set to `true` it refuses to load such a file instead.
  - The file's SHA-256 is logged at start-up and on every reload.
- **Dashboard.** The dashboard:
  - listens on `127.0.0.1` only;
  - requires a random per-session access key, handed out only over the administrator-only control channel, on every API call;
  - rejects requests whose `Host` header isn't `127.0.0.1`/`localhost` (DNS rebinding);
  - rejects cross-site `Origin` headers and non-JSON POST bodies, which blocks cross-site request forgery;
  - serves only its own three static files;
  - sends a strict Content-Security-Policy.

  All page content is rendered as text, so log contents can't inject markup.
- **Administrator-only control channel.** The CLI uses a local named pipe (`\\.\pipe\ThreadHaulDaemon`). Its access list admits only SYSTEM, Administrators and the service's own account, and remote clients are rejected.
- **Controller authentication.** Nodes must present the shared token. Run the controller on a trusted network segment; the protocol does not encrypt traffic.
- **Protection list.** The service itself, `System`, `Registry`, `smss.exe`, `csrss.exe`, `wininit.exe`, `winlogon.exe`, `services.exe` and `lsass.exe` are always protected, in addition to `processes.protected`.
- **Graceful failure.** If a process can't be opened (access denied, or the process has exited), the outcome is logged with `actuation_succeeded: false` and the service carries on.

## Troubleshooting

| Symptom | Resolution |
|---|---|
| `... is not recognized` / `No such file or directory` | Run commands from the repository folder. Paths containing spaces must be quoted. |
| `Run this script from an elevated (Administrator) PowerShell` | Open PowerShell with **Run as administrator**. |
| `Visual Studio not found` | Install the *Desktop development with C++* workload. |
| `daemon is not running` | Check `Get-Service ThreadHaulDaemon` and start it, or install it. |
| `access denied: run this command as Administrator` | The control channel is administrator-only. |
| The service stops right after starting | The configuration is invalid. Read Event ID 1900 for the field name and run `validate-config`. |
| Batch processes are never throttled | Check that the names in `processes` match the executable names exactly, and look at `pressure_percent` in `status`. |
| `Cannot open the dashboard` | The dashboard is disabled or its port is in use. Check `dashboard.*` and the `dashboard` entry in the log. |
| Dashboard shows *Access key required* | Run `ThreadHaulDaemon.exe dashboard` again. The key changes when the service restarts. |
| No pop-up on suspension | A user must be signed in at the console, and `alerts.popup` must be `true`. |
| `An Application Control policy has blocked this file` | Windows Smart App Control can block newly compiled, unsigned binaries. Retry, or code-sign the build output. |

## Project layout

```
build.cmd                  one-step build (+ "test")
RUNNING.md                 step-by-step installation and operation guide
INTERFACES.md              internal contract between modules (event struct, log and config schema)
Source/
  main.cpp                 service entry point, console mode, command-line interface
  Actuator/                state machine, priority and thread control, protection list
  Telemetry/               process discovery, CPU/memory/I-O sampling, predictive thresholds
  Config/                  configuration loading and validation, sample config.json
  Logging/                 JSON Lines logger with rotation, Windows Event Log writer
  Daemon/                  service core: poll loop, live reload, control pipe, alerts, controller client,
                           dashboard HTTP server and API
  Dashboard/               web dashboard (index.html, app.js, styles.css; no external dependencies)
  Controller/              ThreadHaulController
  Simulators/              critical_simulator and batch_simulator test workloads
  Scripts/                 install.ps1, uninstall.ps1, log_viewer.py
  Tests/                   unit tests
third_party/nlohmann/      bundled JSON library (MIT licence)
```

## Running the tests

```powershell
.\build.cmd test
```

Three suites run, and each must report `0 failed`:
- **State machine:** throttle/suspend/recover rules, boundaries, manual holds, live configuration changes, the protection list.
- **Telemetry:** CPU calculation in both normalisation modes, threshold streaks, predictive thresholds, real process discovery, and crash detection with exit codes.
- **Configuration and logging:** validation of every field, optional sections, log schema, and rotation.

For an end-to-end check, run the quick start above and confirm the throttle → suspend → recover cycle in the log or with `status`.

## Known limitations

- **Windows only.** The service is built on Win32 process and thread APIs and has no Linux or macOS equivalent.
- **Thread suspension is a snapshot.** A thread created by a batch process in the instant after the snapshot isn't suspended in that pass. Resuming always takes a fresh snapshot, so nothing is left suspended.
- **Priority classes are coarse.** On machines with many idle cores, a throttled batch process may show no visible slowdown, because there is enough CPU for everyone. That is the intended behaviour.
- **Controller traffic is not encrypted.** Use it on a trusted network, or tunnel it.
- **Unsigned binaries.** Builds are not code-signed. Environments with Smart App Control or WDAC may require signing.

## Contributing

Issues and pull requests are welcome. Before submitting:

1. Run `.\build.cmd test` and make sure every suite reports `0 failed`, with no new compiler warnings.
2. Follow the existing code style: C++17, the Win32 API, the `snake_case` JSON field names described in `INTERFACES.md`, and no new third-party dependencies without discussion.
3. If you change a log field, configuration key or cross-module interface, update `INTERFACES.md` in the same pull request.
4. Add or update unit tests for any behaviour you change.
