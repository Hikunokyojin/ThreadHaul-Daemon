# Running ThreadHaul-Daemon: Step-by-Step Guide

This guide covers how to build, test, run, demo, install and remove the daemon. For what the project is and why it was designed this way, see [README.md](README.md).

All commands are run from the repository folder (`ThreadHaul-Daemon\`) unless a step says otherwise.

---

## 0. One-time setup

| Need | How to get it | Check |
|---|---|---|
| Windows 10/11 or Windows Server 2016+ | | |
| Visual Studio 2019+ with the **Desktop development with C++** workload | Visual Studio Installer | `build.cmd` finds it automatically |
| Python 3 + matplotlib (only for graphs) | `pip install matplotlib` | `python -c "import matplotlib"` |
| Administrator rights (only to install the service) | Right-click PowerShell → *Run as administrator* | |

---

## 1. Build

```bat
build.cmd
```

Expected ending: `Build OK -> ...\build`. Everything lands in `build\`:

```
ThreadHaulDaemon.exe        the monitor (service + console mode + CLI)
ThreadHaulController.exe    central controller for several servers
critical_simulator.exe      fake "dispatch API" that burns CPU on a schedule
batch_simulator.exe         fake "invoice job" that burns CPU all the time
*Tests.exe                  unit tests
config.json                 copy of Source\Config\config.json
```

If the build fails, the compiler output is in `build\build.log`.

## 2. Run the unit tests

```bat
build.cmd test
```

Expected: three suites, each ending `... checks, 0 failed`. The exit code is 0 only if all pass.

## 3. Check a config file

```bat
build\ThreadHaulDaemon.exe validate-config Source\Config\config.json
```

The command prints either `OK: ... is valid.` or `INVALID: <field> <reason>`. The daemon uses the same check at startup and on every hot-reload.

---

## 4. Demo in console mode (no install needed)

Console mode runs the daemon in a terminal window. It behaves exactly like the service, so it's the easiest way to show the system working.

### 4.1 Prepare a demo config

`build\config.json` writes its log to `C:\ProgramData\ThreadHaulDaemon\logs\`, which needs admin rights. For a non-admin demo, edit `build\config.json` and set:

```json
"log_path": "demo.jsonl"
```

A relative path is resolved from the folder you start the daemon in.

### 4.2 Start the three programs

Open **three terminals** in `build\`.

**Terminal 1: the daemon**
```bat
ThreadHaulDaemon.exe --console --config config.json --allow-unelevated
```
- `--allow-unelevated` lets it run without admin, managing only processes you own. Leave it out when running as Administrator.

**Terminal 2: the batch job**
```bat
batch_simulator.exe --threads 2
```
It prints `units/s` and its current priority every second.

**Terminal 3: the critical app**
```bat
critical_simulator.exe
```
The default schedule repeats: 8 s at 30% → 20 s at 100% → 15 s at 20%.

### 4.3 What you should see

| Time in the 100% phase | Daemon decision | Visible in batch_simulator |
|---|---|---|
| 3rd second ≥ 85% | **throttle**: priority → IDLE | `priority IDLE` |
| 5 more seconds ≥ 90% | **suspend** all threads | output **stops** |
| 5 seconds < 60% (after the phase ends) | **recover**: resume + NORMAL | `<- resumed after a pause`, `priority NORMAL` |

Check the live state at any time from a fourth terminal:
```bat
ThreadHaulDaemon.exe status
```

### 4.4 Try the manual override (CLI)

```bat
ThreadHaulDaemon.exe suspend batch_simulator.exe
ThreadHaulDaemon.exe status
ThreadHaulDaemon.exe resume batch_simulator.exe
ThreadHaulDaemon.exe release batch_simulator.exe
ThreadHaulDaemon.exe suspend explorer.exe
```
- `suspend` freezes the batch job immediately. The process is held: the state machine won't touch it.
- `status` shows `"state":"suspended","manual_hold":true`.
- `resume` restores it to normal priority and keeps holding it.
- `release` hands it back to automatic control.
- `suspend explorer.exe` is **refused**, because explorer.exe is on the protection list.

### 4.5 Try hot-reload

While everything is running, edit `config.json`:
- Change `"high": { "percent": 85` to `70` and save. The log gets a `config_reload` entry with `"success": true`, and the new threshold applies on the next second.
- Set `"low"` to `95` (invalid, because low must be below high) and save. The terminal prints `config reload rejected: thresholds.low.percent must be below thresholds.high.percent`, and the daemon keeps the last good config.

### 4.6 Try crash and restart handling

Close the critical simulator and run:
```bat
critical_simulator.exe --load 100 --crash-after 10
```
- After 10 s it exits with code 3, and the log records `"event":"crashed","exit_code":3`.
- Start it again, and the log records `"event":"restarted"` with `previous_process_id`.

### 4.7 Try adaptive (predictive) thresholds

In `config.json` set `"adaptive": { "enabled": true, "window_samples": 5, "horizon_samples": 3, "max_adjust_percent": 10 }`, then run:
```bat
critical_simulator.exe --schedule 3:20,1:40,1:55,1:68,1:78,12:84 --once
```
While the load ramps up, the log shows `adaptive_threshold` with `high_percent` falling below 85. The batch job is throttled before the static rule would have acted.

### 4.8 Stop cleanly

```bat
ThreadHaulDaemon.exe stop
```
Pressing Ctrl+C in Terminal 1 does the same. Any suspended or throttled process is resumed and restored to NORMAL before the daemon exits.

### 4.9 Graph the run

```bat
python ..\Source\Scripts\log_viewer.py demo.jsonl --out demo.png
```

The script prints a table of every state change and the daemon's own CPU/memory, then saves `demo.png` with three panels:
- critical CPU against the thresholds;
- each batch process's state over time (triangles mark manual overrides);
- the daemon's own resource use.

---

## 5. Install as a real Windows Service

Open PowerShell **as Administrator** in the repository folder:

```powershell
powershell -ExecutionPolicy Bypass -File Source\Scripts\install.ps1
```

What it does, in order:
1. Validates `Source\Config\config.json`. It stops if the config is invalid.
2. Removes any older installation, which stops cleanly and restores processes.
3. Copies the exe to `C:\Program Files\ThreadHaulDaemon\`.
4. Copies the config to `C:\ProgramData\ThreadHaulDaemon\config.json`. An existing config is kept unless you pass `-OverwriteConfig`.
5. Locks the config so only SYSTEM and Administrators can change it.
6. Registers the `ThreadHaulDaemon` event-log source.
7. Creates the service: automatic start, restart on failure, minimal privileges.
8. Starts it.

Options:
- `-Account LocalService`: lower-privilege account. It can only manage processes that also run as LocalService.
- `-OverwriteConfig`: replace an installed config.json.
- `-NoStart`: install but don't start.

### 5.1 Check the service

```powershell
Get-Service ThreadHaulDaemon
& "C:\Program Files\ThreadHaulDaemon\ThreadHaulDaemon.exe" status
Get-Content C:\ProgramData\ThreadHaulDaemon\logs\threadhaul.jsonl -Tail 5
```
- `Get-Service` should show Status Running, StartType Automatic.
- `status` prints a JSON line with the current pressure and thresholds.
- The log tail shows the most recent entries.

Then run the two simulators from `build\` (section 4.2). Watch for:
- **Event Viewer** → Windows Logs → Application, source **ThreadHaulDaemon**. Event IDs: 1000 throttled, 1001 suspended, 1002 recovered.
- A **popup** on your desktop when the batch job is suspended.

### 5.2 Change settings on an installed service

Edit `C:\ProgramData\ThreadHaulDaemon\config.json` in an editor started **as Administrator**. Saving applies the changes, with no restart needed. To be able to run the simulators from `build\`, the names in `processes` must match: `critical_simulator.exe` and `batch_simulator.exe` by default.

### 5.3 Start, stop and restart

```powershell
Stop-Service ThreadHaulDaemon      # resumes and restores everything first
Start-Service ThreadHaulDaemon
Restart-Service ThreadHaulDaemon
```

### 5.4 Uninstall

```powershell
powershell -ExecutionPolicy Bypass -File Source\Scripts\uninstall.ps1
powershell -ExecutionPolicy Bypass -File Source\Scripts\uninstall.ps1 -RemoveData
```
- The first command keeps the config and logs.
- The second also deletes `C:\ProgramData\ThreadHaulDaemon`.

---

## 6. Multi-server mode (optional)

On the controller machine:
```bat
build\ThreadHaulController.exe --port 7878 --token mysecret --pressure 85 --cluster-percent 50 --bias 5
```

On every dispatch server, in config.json:
```json
"coordination": { "enabled": true, "controller_host": "<controller-ip>", "controller_port": 7878, "shared_token": "mysecret", "heartbeat_seconds": 5 }
```
Allow TCP port 7878 through the controller's firewall.

Commands you can type into the controller window:
```
list                                      show every connected server and its batch processes
DISPATCH-01 suspend batch_simulator.exe   act on one server
all release batch_simulator.exe           act on every server
quit
```
When at least 50% of servers are above 85%, the controller lowers every server's throttle threshold by 5 points until load drops. This shows up as `[cluster] ... bias 5.0` in the controller window.

---

## 7. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `must run as Administrator` | Run elevated, or add `--allow-unelevated` for a console demo. |
| `Invalid configuration (...)` at start | The message names the bad field. Fix it and re-run `validate-config`. |
| `daemon is not running` from CLI | Start the daemon or service first. Without one, throttle/suspend/resume act directly on the process. |
| `access denied: run this command as Administrator` | The control pipe only admits Administrators, SYSTEM and the daemon's own account. |
| Batch never throttled | The process names in `processes` must match the exe names exactly (any case). Check `status` and the `pressure_percent` field. |
| `An Application Control policy has blocked this file` | Windows Smart App Control can block newly built unsigned exes. Run again, or sign the binaries. |
| No popup from the service | The popup needs someone logged in at the console. Check `alerts.popup`. |
| Event Viewer says "description not found" | The event source isn't registered. Run `install.ps1`. |
