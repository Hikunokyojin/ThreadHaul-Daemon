# ThreadHaul-Daemon: Installation and Operation Guide

This guide takes you from a fresh copy of the project to a running Windows Service, and shows you how to confirm it works, operate it and remove it. You don't need to know how the project works internally; follow the steps in order.

For background on the design, see [README.md](README.md).

---

## What this software does

ThreadHaul-Daemon runs in the background as a Windows Service. It watches two kinds of programs:

- **Critical programs:** work that must stay fast, such as a real-time dispatch system.
- **Batch programs:** work that can wait, such as invoice generation.

When a critical program is short of CPU, the service slows the batch programs down. If that isn't enough, it pauses them. When the pressure is gone, it restores them. Every decision is recorded in a log file and in the Windows Event Log.

The project includes two small test programs that play these roles, so you can see it work without any real system:

| Test program | Plays the role of | Behaviour |
|---|---|---|
| `critical_simulator.exe` | the critical program | Repeats a cycle of light load, then full load, then light load |
| `batch_simulator.exe` | the batch program | Uses CPU continuously and prints its progress every second |

---

## Before you begin

### Requirements

| Requirement | Notes |
|---|---|
| Windows 10, Windows 11 or Windows Server 2016 or later | 64-bit |
| Administrator rights on the computer | Needed to install a Windows Service |
| Visual Studio 2019 or later, with the **Desktop development with C++** workload | Needed once, to compile the program. The free Community edition is fine. In the Visual Studio Installer, choose *Modify* and tick *Desktop development with C++*. |
| Python 3 (optional) | Only needed for the graph in Step 8 |

### Open an Administrator PowerShell window

1. Open the **Start** menu and type `PowerShell`.
2. Right-click **Windows PowerShell** and choose **Run as administrator**.
3. Click **Yes** when Windows asks for permission.

The window title should begin with **Administrator:**. Use this window for every step unless a step tells you otherwise.

### Go to the project folder

Every command in this guide is written to run from the **project folder**: the folder that contains `build.cmd`, `README.md` and the `Source` folder.

Run the following, replacing the path with wherever the project is on your computer. Keep the quotation marks, because the path contains spaces.

```powershell
Set-Location "C:\Claude Cowork\Projects\ThreadHaul-Daemon\ThreadHaul-Daemon"
```

Confirm you're in the right place:

```powershell
Test-Path .\build.cmd
```

This must print `True`. If it prints `False`, you're in the wrong folder; repeat the `Set-Location` command with the correct path.

> **Important:** if you open a new PowerShell window at any point, run the `Set-Location` command in it first. Most "file not found" errors come from running a command in the wrong folder.

---

## Step 1: Build the program

```powershell
.\build.cmd
```

This takes about a minute. It finishes with:

```
Build OK -> ...\build
```

The compiled programs are now in the `build` folder. If you see `BUILD FAILED`, the full compiler output is in `build\build.log`. If you see `Visual Studio not found`, install the C++ workload described in *Requirements*.

## Step 2: Run the self-tests (recommended)

```powershell
.\build.cmd test
```

Three test groups run. Each must end with `0 failed`, for example:

```
[StateMachineTests]
50 checks, 0 failed
```

If any test fails, stop here and report the output to the project team.

## Step 3: Install the service

```powershell
powershell -ExecutionPolicy Bypass -File .\Source\Scripts\install.ps1
```

The installer prints each action as it completes:

```
OK: ...\Source\Config\config.json is valid.
Installing binaries to C:\Program Files\ThreadHaulDaemon
Installed config: C:\ProgramData\ThreadHaulDaemon\config.json
Config locked to SYSTEM + Administrators (write), service account (read).
Registered event-log source ThreadHaulDaemon (Application log).
Service 'ThreadHaulDaemon' created (auto start, account LocalSystem).

Name             Status  StartType
ThreadHaulDaemon Running Automatic
```

What the installer has set up:

| Item | Location |
|---|---|
| Program | `C:\Program Files\ThreadHaulDaemon\ThreadHaulDaemon.exe` |
| Settings file | `C:\ProgramData\ThreadHaulDaemon\config.json` |
| Log file | `C:\ProgramData\ThreadHaulDaemon\logs\threadhaul.jsonl` |
| Windows Service | **ThreadHaul Resource Monitor**: starts automatically with Windows |

If the service was installed before, running the installer again replaces the program and keeps your existing settings file.

## Step 4: Confirm the service is running

```powershell
Get-Service ThreadHaulDaemon
```

**Status** must be `Running`.

Next, ask the service for its current state:

```powershell
& "C:\Program Files\ThreadHaulDaemon\ThreadHaulDaemon.exe" status
```

It replies with one line of data that starts with `{"ok":true,...`. At this point the `critical` and `batch` lists are empty (`[]`), because neither test program is running yet. That's expected.

## Step 5: Demonstrate the service with the test programs

You'll need **two additional PowerShell windows**. These don't need administrator rights: open them from the Start menu normally.

**Window 2: start the batch program**

```powershell
Set-Location "C:\Claude Cowork\Projects\ThreadHaul-Daemon\ThreadHaul-Daemon"
.\build\batch_simulator.exe --threads 2
```

It prints a line every second, for example:

```
[batch] 168154553 units/s  priority NORMAL
```

**Window 3: start the critical program**

```powershell
Set-Location "C:\Claude Cowork\Projects\ThreadHaul-Daemon\ThreadHaul-Daemon"
.\build\critical_simulator.exe
```

It runs a repeating 43-second cycle: 8 seconds of light load, 20 seconds of full load, then 15 seconds of light load. Each phase change is announced, for example `load 100% for 20 s`.

**What to watch in Window 2** once the critical program reaches 100%:

| When | What the service does | What you see in Window 2 |
|---|---|---|
| After about 3 seconds of full load | **Throttles** the batch program: lowers its priority | The line changes to `priority IDLE` |
| After about 5 more seconds | **Suspends** the batch program: pauses it completely | The output **stops**. A warning pop-up appears on screen. |
| About 5 seconds after the load drops | **Restores** the batch program | Output resumes with `<- resumed after a pause` and `priority NORMAL` |

The cycle repeats, so you can watch it as many times as you like.

> On a computer with many CPU cores the batch program may not visibly slow down while throttled, because there is enough spare CPU for both programs. The change to `priority IDLE` and the complete pause during suspension are the reliable signs.

## Step 6: Observe what the service is doing

Run these in the **Administrator window (Window 1)** while the test programs are running.

**Current state**

```powershell
& "C:\Program Files\ThreadHaulDaemon\ThreadHaulDaemon.exe" status
```

The reply now lists both programs. Look for `"state":"throttled"`, `"suspended"` or `"normal"` for `batch_simulator.exe`, and `pressure_percent` for the critical program's current load.

**Live log**

```powershell
Get-Content C:\ProgramData\ThreadHaulDaemon\logs\threadhaul.jsonl -Tail 10 -Wait
```

New entries appear as they're written:
- a `"log_type":"sample"` line every second;
- an `"log_type":"actuator_action"` line for every throttle, suspend or restore.

Press **Ctrl+C** to stop watching. This doesn't affect the service.

**Windows Event Log**

```powershell
Get-EventLog -LogName Application -Source ThreadHaulDaemon -Newest 5
```

| Event ID | Meaning |
|---|---|
| 1000 | Batch program throttled |
| 1001 | Batch program suspended |
| 1002 | Batch program restored |
| 1900 | Service could not start (the message explains why) |

The same entries are visible in **Event Viewer** → *Windows Logs* → *Application*, with source **ThreadHaulDaemon**.

## Step 7: Control a program manually (optional)

An administrator can override the automatic behaviour at any time. In the Administrator window:

```powershell
& "C:\Program Files\ThreadHaulDaemon\ThreadHaulDaemon.exe" suspend batch_simulator.exe
```

The batch program pauses immediately and **stays paused**: the service won't change it automatically while it is under manual control. To hand it back to automatic control:

```powershell
& "C:\Program Files\ThreadHaulDaemon\ThreadHaulDaemon.exe" release batch_simulator.exe
```

All manual commands:

| Command | Effect |
|---|---|
| `throttle <name or PID>` | Lower the program's priority and hold it there |
| `suspend <name or PID>` | Pause the program and hold it there |
| `resume <name or PID>` | Un-pause the program at normal priority and hold it there |
| `release <name or PID>` | Return the program to automatic control |
| `status` | Show the current state of everything the service manages |

Programs on the protected list, such as `explorer.exe`, are always refused, so the service can't destabilise Windows.

## Step 8: Produce a graph of the activity (optional, requires Python)

Install the graphing library once:

```powershell
python -m pip install matplotlib
```

Then, from the project folder:

```powershell
python .\Source\Scripts\log_viewer.py C:\ProgramData\ThreadHaulDaemon\logs\threadhaul.jsonl --out service_run.png
start .\service_run.png
```

The script prints a table of every action the service took and the service's own CPU and memory use, then opens an image with three charts:
- the critical program's load against the thresholds;
- the batch program's state over time;
- the service's own resource use.

## Step 9: Change the settings (optional)

The settings file is protected, so only an administrator can edit it. From the Administrator window:

```powershell
notepad C:\ProgramData\ThreadHaulDaemon\config.json
```

Changes take effect within one second of saving; there's no need to restart the service. Common settings:

| Setting | Meaning | Default |
|---|---|---|
| `thresholds.high.percent` / `consecutive_samples` | Load that triggers throttling, and for how many seconds | 85%, 3 |
| `thresholds.critical.percent` / `consecutive_samples` | Load that triggers suspension after throttling | 90%, 5 |
| `thresholds.low.percent` / `consecutive_samples` | Load below which programs are restored | 60%, 5 |
| `processes.critical` | Names of the programs to protect | `critical_simulator.exe` |
| `processes.batch` | Names of the programs that may be slowed or paused | `batch_simulator.exe` |
| `processes.protected` | Programs that must never be touched | `explorer.exe`, ... |
| `adaptive.enabled` | Lower the thresholds early when load is rising quickly | `false` |
| `alerts.popup` | Show a pop-up when a program is suspended | `true` |

If you save an invalid setting (for example a recovery level higher than the throttle level), the service rejects it, records the reason in the log, and keeps running with the previous settings. To check a settings file before saving it in place:

```powershell
& "C:\Program Files\ThreadHaulDaemon\ThreadHaulDaemon.exe" validate-config C:\ProgramData\ThreadHaulDaemon\config.json
```

## Step 10: Finish the demonstration

Close Windows 2 and 3 (or press **Ctrl+C** in each). The service keeps running in the background and starts automatically when Windows starts.

To stop or start the service manually (Administrator window):

```powershell
Stop-Service ThreadHaulDaemon
Start-Service ThreadHaulDaemon
```

Stopping the service always restores any program it has slowed or paused before it exits.

## Step 11: Uninstall

From the Administrator window, in the project folder:

```powershell
powershell -ExecutionPolicy Bypass -File .\Source\Scripts\uninstall.ps1
```

This stops the service (restoring all programs), removes it, and deletes the program from `C:\Program Files`. The settings and logs in `C:\ProgramData\ThreadHaulDaemon` are kept.

To remove the settings and logs as well:

```powershell
powershell -ExecutionPolicy Bypass -File .\Source\Scripts\uninstall.ps1 -RemoveData
```

To confirm removal:

```powershell
Get-Service ThreadHaulDaemon
```

This should now report that it cannot find the service.

---

## Appendix A: Run without installing (console mode)

To try the software without installing a service, for example on a computer where you lack administrator rights, run it in a normal PowerShell window from the project folder:

```powershell
.\build\ThreadHaulDaemon.exe --console --config .\Source\Config\config.json --allow-unelevated
```

It behaves like the service but runs in that window. A non-administrator can't write to `C:\ProgramData`, so first make a copy of the settings with a local log location:

```powershell
(Get-Content .\Source\Config\config.json -Raw) -replace '"log_path": "[^"]*"', '"log_path": "demo.jsonl"' | Set-Content .\build\demo-config.json
.\build\ThreadHaulDaemon.exe --console --config .\build\demo-config.json --allow-unelevated
```

Then start the two test programs as in Step 5. To stop, press **Ctrl+C** or run `.\build\ThreadHaulDaemon.exe stop` from another window. `--allow-unelevated` limits the software to programs started by your own user account.

## Appendix B: Troubleshooting

| Message or symptom | Cause | What to do |
|---|---|---|
| `... is not recognized` or `No such file or directory` | The command was run from the wrong folder | Run the `Set-Location` command from *Before you begin*, confirm `Test-Path .\build.cmd` prints `True`, and try again |
| `A positional parameter cannot be found that accepts argument` | A path containing spaces was typed without quotation marks | Put the whole path inside `"..."` |
| `Run this script from an elevated (Administrator) PowerShell` | The window isn't running as administrator | Open PowerShell with **Run as administrator** |
| `Not found: ...ThreadHaulDaemon.exe. Run build.cmd first.` | The program hasn't been compiled | Complete Step 1 |
| `Visual Studio not found` | The C++ compiler isn't installed | Install the *Desktop development with C++* workload |
| `daemon is not running` | The service is stopped or not installed | Run `Get-Service ThreadHaulDaemon`; start it, or complete Step 3 |
| `access denied: run this command as Administrator` | Manual commands require administrator rights | Use the Administrator window |
| The batch program never shows `priority IDLE` | The program names in the settings don't match the running programs, or the critical program isn't running | Check the `status` output; the names in `processes.critical` and `processes.batch` must match exactly |
| `An Application Control policy has blocked this file` | Windows Smart App Control sometimes blocks newly compiled programs | Run the command again. If it persists, the programs need to be code-signed. |
| No pop-up appears | A user must be signed in at the console, and `alerts.popup` must be `true` | Check the setting; the log and Event Log still record the event |
| The service stops shortly after starting | The settings file is invalid | Read event 1900 in the Event Log for the reason, correct the file, then `Start-Service ThreadHaulDaemon` |
