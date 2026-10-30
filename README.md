<div align="center">

# ThreadHaul

**Keep latency-critical Windows workloads responsive, automatically, under CPU pressure.**

[![CI](https://github.com/Hikunokyojin/ThreadHaul-Daemon/actions/workflows/ci.yml/badge.svg)](https://github.com/Hikunokyojin/ThreadHaul-Daemon/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/Hikunokyojin/ThreadHaul-Daemon?sort=semver)](https://github.com/Hikunokyojin/ThreadHaul-Daemon/releases)
[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
![Platform: Windows](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011%20%7C%20Server-0078D4)
![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C)

</div>

ThreadHaul is a lightweight Windows Service, written in C++17 against the Win32 API. It watches the processes you mark as **critical** and, when they are short of CPU, steps in against the processes you mark as **batch**: first it lowers their priority, then, if that isn't enough, it suspends them. When the pressure passes, it restores them. Independently of any roles, **auto-protect** keeps the whole machine responsive by lowering the heaviest applications whenever total CPU is overloaded.

Everything is visible and controllable from a built-in **web dashboard**, recorded in a structured log and the Windows Event Log, and reconfigurable without a restart.

---

## Contents

- [Why ThreadHaul](#why-threadhaul)
- [Features](#features)
- [How it works](#how-it-works)
- [Quick start](#quick-start)
- [Building from source](#building-from-source)
- [Installation](#installation)
- [Using ThreadHaul](#using-threadhaul)
- [Configuration](#configuration)
- [Logs and monitoring](#logs-and-monitoring)
- [Multi-server deployments](#multi-server-deployments)
- [Security model](#security-model)
- [Project structure](#project-structure)
- [Development](#development)
- [Troubleshooting](#troubleshooting)
- [Known limitations](#known-limitations)
- [Contributing](#contributing)
- [License](#license)

---

## Why ThreadHaul

Servers rarely run one thing. An order-routing or dispatch server, for example, runs a real-time API next to batch work: invoicing, synchronisation, reporting, backups. Under load the batch work competes for the same cores, and because the Windows scheduler treats both kinds of work as equally important, the latency-critical service slows down exactly when it matters most.

| Common workaround | Drawback |
|---|---|
| Schedule batch jobs off-peak | Peaks are unpredictable, and some batch work can't wait |
| Permanently lower batch priority | Batch work is slow even when the machine is idle |
| Move batch work to other machines | Extra hardware and operating cost |
| Kill batch jobs during incidents | Slow, manual, and loses work in progress |

ThreadHaul makes the decision continuously, from what the critical workload is actually experiencing. Batch work runs at full speed whenever there is capacity and steps aside the moment there isn't. Nothing is ever killed, and paused work resumes exactly where it stopped.

## Features

**Protection**
- **Graded response** for each batch process: lowered to Idle priority, then suspended, then automatically recovered. Each batch process has its own state machine.
- **Automatic CPU protection.** When total system CPU stays overloaded, the heaviest ordinary applications are lowered in priority and later restored to their exact original priority. It is on by default and needs no configuration.
- **Predictive thresholds.** A least-squares trend lowers the thresholds while load is rising, so batch work is shed *before* the critical process starves.
- **Memory and I/O triggers** alongside CPU.

**Operations**
- **Native Windows Service:** automatic start, restart on failure, clean shutdown that always restores every managed process.
- **Web dashboard** on `127.0.0.1`:
  - live statistics and a pressure chart;
  - a process picker where you assign roles to real programs;
  - control buttons;
  - a filterable activity log with raw-JSON view and CSV export.
- **Live configuration reload** with full validation. An invalid edit is rejected and the service keeps running on the last good configuration.
- **Command-line override:** throttle, suspend, resume, release or exempt any process, and query live status.
- **Observability:**
  - JSON Lines log with size-based rotation;
  - Windows Event Log entries;
  - pop-up and webhook alerts;
  - self-monitoring (typically about 1% of one core and under 10 MB of memory).
- **Optional central controller** for fleets, with cluster-wide coordination.

**Safety**
- **Every process has a safety level.** Processes Windows depends on (such as `svchost.exe` and `dwm.exe`) are never managed. Other system processes and services require an explicit confirmation, which the service enforces.
- **Robust to process churn:** targets that aren't running yet, that crash, or that restart under a new PID are handled, and PID reuse is guarded against.
- **Hardened by default:** least-privilege service account, tamper-checked configuration, administrator-only control channel, and a loopback-only dashboard protected by a per-session key.

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
 |  Service core: configuration + live reload, auto-protect, JSON Lines log, Event Log, alerts,  |
 |  control pipe (CLI), dashboard HTTP server, optional controller connection                    |
 +-----------------------------------------------------------------------------------------------+
```

**Telemetry.** Processes are found by executable name with a Toolhelp snapshot. CPU usage is derived from `GetProcessTimes` (kernel + user time) between samples. Memory comes from `GetProcessMemoryInfo` and I/O from `GetProcessIoCounters`. CPU is normalised **per core** by default: 100% means one fully used logical core, so a threshold means the same thing on a 4-core and a 64-core machine.

**Batch control: the state machine.**

```
            >= high% for N samples                >= critical% for M samples
  NORMAL  ------------------------->  THROTTLED  ---------------------------->  SUSPENDED
    ^        (SetPriorityClass IDLE)      |          (SuspendThread on all threads)    |
    |                                     |  < low% for K samples                      |
    +-------------------------------------+--------------------------------------------+
                         (ResumeThread + SetPriorityClass NORMAL)
```

The defaults are 85% for 3 samples, 90% for 5 more samples, and below 60% for 5 samples. Escalation counts samples *after* throttling, so suspension only happens when demotion has demonstrably not been enough. Recovery uses a lower threshold than throttling (hysteresis), so processes don't flap between states.

**Automatic CPU protection.** This works on its own, with no roles configured:

| When total system CPU... | Auto-protect... |
|---|---|
| is at or above **90%** for 5 s | lowers the heaviest ordinary applications (up to 3, each using at least 50% of a core) to **Below Normal** priority |
| is still at or above **97%** for 10 s | lowers them to **Idle** priority |
| drops below **75%** for 10 s | restores each one to **its exact original priority** |

Auto-protect only lowers priority; it never suspends anything.

**Process safety levels.**

| Level | Meaning | Behaviour |
|---|---|---|
| **Essential** | Windows cannot run without it: kernel, session, logon and security processes, `svchost.exe`, `dwm.exe`, `fontdrvhost.exe`, `LsaIso.exe`, Memory Compression, Secure System, and ThreadHaul itself | Never managed, by any path |
| **System process** | The program file is under the Windows folder, or Windows denies access to it | Hidden by default in the dashboard; managing it needs confirmation |
| **Background service** | Runs as SYSTEM, LOCAL SERVICE or NETWORK SERVICE, or in the services session | Managing it needs confirmation |
| **Normal** | An ordinary application | Managed freely; eligible for auto-protect |

## Quick start

**From a release (no compiler needed):**

1. Download `ThreadHaul-<version>-win-x64.zip` from the [Releases](https://github.com/Hikunokyojin/ThreadHaul-Daemon/releases) page and extract it.
2. In an **Administrator PowerShell** inside the extracted folder:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\scripts\install.ps1
   & "C:\Program Files\ThreadHaulDaemon\ThreadHaulDaemon.exe" dashboard
   ```

The service is now running, starts with Windows, and auto-protect is active. The dashboard opens in your browser.

**From source:**

```powershell
git clone https://github.com/Hikunokyojin/ThreadHaul-Daemon.git
cd ThreadHaul-Daemon
.\build.cmd test                                                         # build + unit tests
powershell -ExecutionPolicy Bypass -File .\scripts\install.ps1           # Administrator PowerShell
& "C:\Program Files\ThreadHaulDaemon\ThreadHaulDaemon.exe" dashboard
```

**See it in action** with the bundled test workloads. Run these in two normal PowerShell windows, from `build\bin` after `build.cmd` or from `tools\` in a release:

```powershell
.\batch_simulator.exe --threads 2     # a CPU-hungry "batch job"; prints progress and its priority
.\critical_simulator.exe              # a "critical service": 8 s at 30%, 20 s at 100%, 15 s at 20%, repeating
```

The default configuration already lists these two as critical and batch. During each full-load phase, the batch window:
1. shows `priority IDLE` (throttled);
2. stops printing (suspended);
3. reports `resumed after a pause` (recovered).

## Building from source

**Requirements:**
- Windows 10, Windows 11 or Windows Server 2016 or later (x64);
- Visual Studio 2022 or later with the **Desktop development with C++** workload, which includes MSVC and CMake.

There are no other dependencies: [nlohmann/json](https://github.com/nlohmann/json) is vendored in `third_party/`.

**Option A: `build.cmd`** (MSVC directly, works from any prompt):

```powershell
.\build.cmd          # build into build\
.\build.cmd test     # build, then run the unit tests
```

The output goes to `build\bin` (the service, controller, simulators, `dashboard\` and a `config.json` for console runs) and `build\tests`.

**Option B: CMake** (also opens directly in Visual Studio, VS Code and CLion):

```powershell
cmake --preset windows-msvc
cmake --build --preset release
ctest --preset release
cmake --install out\build --config Release --prefix out\package   # release layout
```

## Installation

From an **Administrator PowerShell**, in the repository (after building) or in an extracted release:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install.ps1
```

The installer:
1. Validates the configuration file, and stops if it is invalid.
2. Stops and replaces any previous installation. The old service restores every process it was managing when it stops.
3. Copies the program and dashboard to `C:\Program Files\ThreadHaulDaemon\`.
4. Installs the configuration to `C:\ProgramData\ThreadHaulDaemon\config.json`. An existing configuration is kept unless you pass `-OverwriteConfig`.
5. Restricts the configuration file so only SYSTEM and Administrators can change it.
6. Registers the `ThreadHaulDaemon` Event Log source.
7. Creates the service with automatic start, restart on failure, and only `SeDebugPrivilege` and `SeChangeNotifyPrivilege`.
8. Starts the service.

| Option | Effect |
|---|---|
| `-OverwriteConfig` | Replace an installed `config.json` with the packaged example |
| `-Account LocalService` | Lower-privilege account. It can then only manage processes that also run as LocalService. |
| `-NoStart` | Install without starting |
| `-BinDir <path>` / `-ConfigSource <path>` | Install from a different binary folder or configuration file |

```powershell
Get-Service ThreadHaulDaemon                       # state
Stop-Service ThreadHaulDaemon                      # restores every managed process first
Start-Service ThreadHaulDaemon
powershell -ExecutionPolicy Bypass -File .\scripts\uninstall.ps1              # keep config and logs
powershell -ExecutionPolicy Bypass -File .\scripts\uninstall.ps1 -RemoveData  # remove everything
```

**Running without installing.** The same engine runs in the foreground:

```powershell
.\build\bin\ThreadHaulDaemon.exe --console --config .\build\bin\config.json --allow-unelevated
```

`--allow-unelevated` limits it to processes owned by your own account. A standard user can't write to `C:\ProgramData`, so for an unelevated run, set `logging.log_path` to a writable file such as `"threadhaul.jsonl"`. To stop, press **Ctrl+C** or run `ThreadHaulDaemon.exe stop`; either way every managed process is restored.

## Using ThreadHaul

### Web dashboard

```powershell
& "C:\Program Files\ThreadHaulDaemon\ThreadHaulDaemon.exe" dashboard
```

This command asks the service for a per-session access key over the administrator-only control channel, then opens `http://127.0.0.1:8787` in your browser. The key changes every time the service starts.

| Tab | What it provides |
|---|---|
| **Overview** | Critical pressure against the effective thresholds; system CPU and memory; auto-protect state and the programs it has throttled (with **Restore**); batch processes by state, with Throttle, Suspend, Resume and Release buttons; service uptime and footprint; a five-minute live chart with threshold lines and shaded protection periods |
| **Processes** | Every running process with CPU % (per core), memory, path and safety level, sortable and searchable. Assign a role to any program: **Critical**, **Batch**, **Protected** or **Unassigned**. The role is written to `config.json` (keeping its permissions) and applied immediately. |
| **Activity log** | The JSON Lines log as a readable, colour-coded table, newest first. Filter by event type, search text or time range; click a row for its raw JSON; export the filtered rows as CSV. New entries stream in live. |

**HTTP API.** Every request must carry the `X-ThreadHaul-Token` header.

| Method and path | Purpose |
|---|---|
| `GET /api/status` | Pressure, thresholds, critical and batch processes, auto-protect state, system CPU and memory, service details |
| `GET /api/processes` | Every running process with CPU %, memory, path, role, safety level and managed state |
| `GET /api/log?offset=N` | Log entries after byte offset `N`. `-1` returns the tail. The response includes the next offset and a `reset` flag after rotation. |
| `POST /api/command` | `{"command":"suspend","target":"<pid or exe>","confirm":false}`. Commands are `throttle`, `suspend`, `resume`, `release` and `exempt`. |
| `POST /api/role` | `{"name":"<exe>","role":"critical","confirm":false}`. Roles are `critical`, `batch`, `protected` and `none`. |

Making a system process or service critical or batch, or throttling or suspending one, returns `{"ok":false,"needs_confirmation":true,...}` until the request is repeated with `"confirm":true`. Essential processes are always refused.

### Command line

```powershell
$thd = "C:\Program Files\ThreadHaulDaemon\ThreadHaulDaemon.exe"
& $thd status                         # live state as a JSON object
& $thd throttle invoice_batch.exe     # Idle priority, held
& $thd suspend  invoice_batch.exe     # suspend all threads, held
& $thd resume   invoice_batch.exe     # resume at normal priority, held
& $thd release  invoice_batch.exe     # back to automatic control
& $thd exempt   heavy_app.exe         # undo auto-protect and exclude it until it exits
& $thd dashboard                      # open the dashboard
& $thd validate-config C:\ProgramData\ThreadHaulDaemon\config.json
& $thd help
```

How the commands behave:
- A target can be an executable name (every instance) or a PID.
- Manual actions are **held** until `release`, so the automatic logic never fights an operator.
- Essential processes and configured critical processes are always refused.
- If no service is running, `throttle`, `suspend` and `resume` act on the process directly.

## Configuration

The installed service reads `C:\ProgramData\ThreadHaulDaemon\config.json`; a fully commented-by-example copy is [`config/config.example.json`](config/config.example.json). Edit it from an elevated editor (for example `notepad` in an Administrator PowerShell). Changes apply within one poll interval; there is no need to restart.

| Key | Description | Default (range) |
|---|---|---|
| `polling.interval_ms` | Time between samples | 1000 (100–60000) |
| `thresholds.high` | Throttle when pressure ≥ `percent` for `consecutive_samples` | 85%, 3 |
| `thresholds.critical` | Suspend when pressure ≥ `percent` for `consecutive_samples` after throttling | 90%, 5 |
| `thresholds.low` | Recover when pressure < `percent` for `consecutive_samples` | 60%, 5 |
| `processes.critical` / `batch` / `protected` | Executable names (not paths); matched case-insensitively. May be empty. | |
| `logging.log_path` | JSON Lines log file | |
| `logging.max_file_size_mb` / `max_rotated_files` | Rotation policy | 50 MB (1–1024), 5 files (0–100) |
| `telemetry.cpu_normalization` | `per_core` (100% = one core) or `total` (100% = the whole machine) | `per_core` |
| `telemetry.log_samples` | Log every sample | `true` |
| `telemetry.self_monitor_interval_seconds` | How often the service logs its own footprint (0 = off) | 30 |
| `triggers.memory.enabled` / `system_load_limit_percent` / `critical_working_set_limit_mb` | Memory pressure as a percentage of these limits | `false`, 95, 0 (off) |
| `triggers.io.enabled` / `critical_io_limit_mb_per_sec` | I/O pressure of the critical process | `false`, 200 |
| `adaptive.enabled` / `window_samples` / `horizon_samples` / `max_adjust_percent` | Predictive thresholds | `false`, 10, 3, 10 |
| `auto_protect.enabled` | Automatic CPU protection (on even if the section is absent) | `true` |
| `auto_protect.throttle_at_percent` / `throttle_samples` | System CPU that starts throttling | 90, 5 |
| `auto_protect.use_idle` / `idle_at_percent` / `idle_samples` | Escalate to Idle under extreme load | `true`, 97, 10 |
| `auto_protect.recover_below_percent` / `recover_samples` | Restore below this level | 75, 10 |
| `auto_protect.max_processes` / `min_process_cpu_percent` | At most this many programs; minimum per-core CPU to be chosen | 3, 50 |
| `alerts.popup` / `webhook_url` | Pop-up and HTTP(S) webhook when a batch process is suspended | `true`, empty |
| `coordination.*` | Central controller connection (see [Multi-server deployments](#multi-server-deployments)) | disabled |
| `dashboard.enabled` / `port` | Dashboard on `127.0.0.1` | `true`, 8787 (1024–65535) |
| `security.require_protected_config` | Refuse a configuration file that non-administrators can modify | `false` |

**Validation:**
- Every field is type- and range-checked.
- `low` must be below `high`, and `high` must not exceed `critical`.
- For `auto_protect`, `recover` must be below `throttle`, and `throttle` must not exceed `idle`.
- A name can't be both critical and batch.
- An invalid file stops the service at start-up with a message naming the field (also Event ID 1900). The same file edited while running is rejected, logged, and the previous configuration stays active.

**Memory and I/O triggers** convert each reading into a percentage of its limit and pass it through the same thresholds as CPU; the highest signal wins. For example, with a 95% limit, a system memory load of 81% counts as 85% pressure.

## Logs and monitoring

### JSON Lines log

Every line of `C:\ProgramData\ThreadHaulDaemon\logs\threadhaul.jsonl` is one JSON object: `log_type` first, then `timestamp` (ISO 8601 UTC, milliseconds), then `snake_case` fields. The file rotates by size.

```json
{"log_type":"actuator_action","timestamp":"2026-10-15T07:27:41.749Z","batch_process_id":5190,"batch_process_name":"invoice_batch.exe","critical_process_id":4821,"action":"threads_suspended","from_state":"throttled","to_state":"suspended","cpu_percent":100.0,"actuation_succeeded":true,"trigger":"automatic"}
```

| `log_type` | Written when | Key fields |
|---|---|---|
| `actuator_action` | A batch process changes state | `batch_process_id/name`, `critical_process_id`, `action` (`priority_lowered`, `threads_suspended`, `recovered`), `from_state`, `to_state`, `cpu_percent`, `actuation_succeeded`, `trigger` (`automatic`, `manual`, `shutdown`, `config_reload`) |
| `sample` | Every poll, per critical process | `process_id/name`, `cpu_percent`, `memory_mb`, `io_mb_per_sec`, `pressure_percent`, `trigger_signal` |
| `threshold_event` | A threshold streak completes | `process_id`, `threshold`, `consecutive_samples`, `cpu_percent` |
| `auto_protect` | Auto-protect acts | `event` (`level`, `throttled`, `lowered_to_idle`, `restored`, `exempted`), `process_name/id`, `from_priority`, `to_priority`, `system_cpu_percent` |
| `process_lifecycle` | A target appears, exits, crashes, restarts or is refused | `role`, `event`, `process_name/id`, `exit_code`, `previous_process_id` |
| `config_reload` / `config_security` | The configuration changed, was rejected, or its permissions are too open | `success`, `error`, `config_sha256` |
| `adaptive_threshold` | Predictive thresholds moved | `offset_percent`, `slope_percent_per_sample`, `forecast_percent` |
| `manual_override` / `dashboard_action` | A command or role change from the CLI or dashboard | `command` / `role`, `result`, `confirmed_risk` |
| `alert`, `self_monitor`, `service_lifecycle`, `dashboard`, `coordination` | Alerts, the service's own footprint, start and stop, dashboard and controller events | |

```powershell
Get-Content C:\ProgramData\ThreadHaulDaemon\logs\threadhaul.jsonl -Tail 20 -Wait
python .\tools\log_viewer.py C:\ProgramData\ThreadHaulDaemon\logs\threadhaul.jsonl --out activity.png
```

The log viewer needs `matplotlib`. It prints every state change and renders three charts: critical load against the thresholds, batch states over time, and the service's own footprint.

### Windows Event Log

Source **ThreadHaulDaemon**, in the *Application* log:

| Event ID | Meaning |
|---|---|
| 1000 / 1001 / 1002 | Batch process throttled / suspended / recovered |
| 1100 / 1101 / 1102 | Auto-protect throttled / lowered to Idle / restored a program |
| 1900 | Service failed to start (the message gives the reason) |

## Multi-server deployments

An optional controller collects state from every server and coordinates them. Start it on the controller host:

```powershell
ThreadHaulController.exe --port 7878 --token <shared-secret> --pressure 85 --cluster-percent 50 --bias 5
```

Then set this in each server's `config.json`:

```json
"coordination": {
  "enabled": true,
  "controller_host": "controller.example.internal",
  "controller_port": 7878,
  "shared_token": "<shared-secret>",
  "heartbeat_seconds": 5
}
```

**What each service does:**
- connects with the shared token;
- sends a heartbeat (pressure, thresholds, batch states) every few seconds;
- forwards every state change;
- reconnects with back-off if the controller is unavailable, protecting its own server meanwhile.

**Cluster bias.** When at least `--cluster-percent` of servers are above `--pressure`, the controller sends every server a **bias** that lowers its throttle thresholds by `--bias` points. Servers that aren't yet starved then start shedding batch load early. The bias is cleared when the cluster calms down.

**Operator commands** at the controller console:
- `list`;
- `<host> suspend <exe>`;
- `all release <exe>`;
- `quit`.

## Security model

- **Least privilege.** The installer removes every service privilege except `SeDebugPrivilege` (needed to open other accounts' processes) and `SeChangeNotifyPrivilege`. `-Account LocalService` runs it with even fewer rights.
- **Protected configuration:**
  - `config.json` is restricted to SYSTEM and Administrators (write), with read access for the service account.
  - At every load the service checks that Everyone, Users, Authenticated Users and Interactive cannot write the file, and logs a warning if they can. With `security.require_protected_config` it refuses to load such a file instead.
  - The file's SHA-256 is logged at start-up and on every reload.
- **Control pipe.** `\\.\pipe\ThreadHaulDaemon` admits only SYSTEM, Administrators and the service account, and rejects remote clients.
- **Dashboard:**
  - binds to `127.0.0.1` only;
  - requires a random per-session key on every API call, handed out only over the control pipe;
  - rejects foreign `Host` headers (DNS rebinding), cross-site `Origin`s and non-JSON POST bodies;
  - serves only its own static files;
  - sends a strict Content-Security-Policy;
  - renders all content as text.
- **Process safety.** Essential processes are never managed. Other system processes and services need explicit confirmation, which the service enforces.
- **Controller.** Agents authenticate with a shared token. Traffic is not encrypted, so keep the controller on a trusted network.

To report a vulnerability, see [SECURITY.md](SECURITY.md).

## Project structure

```
├─ src/
│  ├─ actuator/      per-process state machine; priority and thread control; protection list
│  ├─ telemetry/     process discovery, CPU/memory/I-O sampling, safety levels,
│  │                 predictive-threshold and auto-protect policies
│  ├─ config/        config.json parsing and validation; role editing
│  ├─ logging/       JSON Lines logger with rotation; Windows Event Log writer
│  ├─ service/       entry point (service, console, CLI), poll loop, control pipe,
│  │                 dashboard HTTP server and API, auto-protect, alerts, controller client
│  └─ controller/    central controller for multi-server deployments
├─ web/dashboard/    dashboard front end (HTML/CSS/JS, no build step, no external libraries)
├─ tests/            unit tests (state machine, telemetry and policies, configuration and logging)
├─ tools/            test workloads (critical/batch simulators) and log_viewer.py
├─ scripts/          install.ps1, uninstall.ps1
├─ config/           config.example.json
├─ third_party/      nlohmann/json (MIT)
├─ CMakeLists.txt, CMakePresets.json, build.cmd
└─ .github/          CI and release workflows, issue and pull request templates
```

## Development

- **Tests.** Three suites with 231 checks cover:
  - the state machine and its edge cases;
  - the CPU calculation in both normalisation modes;
  - threshold streaks, predictive thresholds and the auto-protect policy;
  - process discovery and crash detection against real processes;
  - safety classification;
  - every configuration rule, role editing, the log schema and rotation.

  Run them with `build.cmd test` or `ctest --preset release`.
- **CI.** Every push and pull request is built with both CMake and `build.cmd` on a Windows runner, all tests are run, and the PowerShell scripts, dashboard JavaScript and Python tool are checked.
- **Releases.** Pushing a tag such as `v1.0.0` builds, tests and packages the binaries, dashboard, scripts and tools into `ThreadHaul-v1.0.0-win-x64.zip` with a SHA-256 checksum. The workflow publishes it as a GitHub Release, with notes taken from [CHANGELOG.md](CHANGELOG.md).

## Troubleshooting

| Symptom | Resolution |
|---|---|
| `Run this script from an elevated (Administrator) PowerShell` | Open PowerShell with **Run as administrator**. |
| `ThreadHaulDaemon.exe not found. Run build.cmd first` | Build first, or run the installer from an extracted release. |
| `daemon is not running` | `Get-Service ThreadHaulDaemon`; start it, or install it. |
| `access denied: run this command as Administrator` | The control channel is administrator-only. |
| The service stops right after starting | The configuration is invalid. Event ID 1900 names the field; check it with `validate-config`. |
| `Cannot open the dashboard` | The service isn't running, the dashboard is disabled, or the port is in use (see the `dashboard` log entry). Set `dashboard.port` to a free port. |
| The dashboard asks for an access key | Run `ThreadHaulDaemon.exe dashboard` again. The key changes when the service restarts. |
| Batch processes are never throttled | The names in `processes` must match the executable names, and a critical process must be running and busy. Check `status`. |
| `An Application Control policy has blocked this file` | Windows Smart App Control can block freshly compiled, unsigned binaries. Rebuild, or use a signed build. |

## Known limitations

- **Windows only.** The project is built on Win32 process, thread and service APIs.
- **Foreground apps aren't detected.** A background service can't reliably tell which window you're using, so a heavy foreground application can be chosen by auto-protect. Mark such programs **Protected**, or raise `min_process_cpu_percent`.
- **Thread suspension is a snapshot.** A thread created in the instant after the snapshot isn't suspended in that pass. Resuming always takes a fresh snapshot, so nothing is left suspended.
- **Priority classes are coarse.** On machines with many idle cores, a throttled program may not visibly slow down, because there is capacity for everyone. That is the intended behaviour.
- **Controller traffic is not encrypted.**
- **Builds are not code-signed.**

## Contributing

Contributions are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for the development workflow, and [CHANGELOG.md](CHANGELOG.md) for release history.

## License

ThreadHaul is licensed under the [Apache License 2.0](LICENSE). It bundles [nlohmann/json](https://github.com/nlohmann/json), which is MIT-licensed (see `third_party/nlohmann/LICENSE.MIT`).
