Dynamic Logistics Server Resource Monitor \& Priority Throttler

1\. PROJECT OVERVIEW



A native Windows background service that prevents resource starvation on a logistics dispatch server during peak load. The service continuously monitors CPU usage of one or more designated critical processes (e.g., real-time driver tracking / route-dispatch APIs) and one or more designated low-priority batch processes (e.g., invoice generation, inventory sync, archiving). When a critical process is starved for CPU, the service dynamically demotes the priority of batch processes, and — if that isn't enough — suspends them outright, restoring normal operation once load drops. Before implementation, the team will do an informal scan of recent (2023–2026) academic literature (IEEE/Springer/Elsevier/ACM/Wiley/MDPI/Nature/Scopus-indexed) on OS-level CPU scheduling and resource management to surface gaps in present-day OS schedulers (e.g., static thresholds, coarse priority granularity, purely reactive throttling) and fold applicable ideas into the design, including a formal literature-review artifact and adaptive/predictive threshold logic.



This spec has expanded beyond an initial proof-of-concept scope: all 25 candidate capabilities identified during scoping were designated Core (must-have) rather than split across Nice-to-have/Maybe-later/Out. This is a substantially larger build than a minimal POC — see Section 4 for the resulting implications.



2\. TARGET AUDIENCE



The three-person student/dev team itself (Divik, Prakul, Pranav) building this system — first to demonstrate the mechanism works end-to-end on real (self-built) test processes, and second as a foundation to divide into individual module ownership across the full feature set below. Not yet intended for a production logistics environment or external users, though several Core items (least-privilege service account, config tamper protection, distributed coordination) point toward production-readiness.



3\. MUST-HAVES



Research \& design



Informal research pass: Scan recent (2023–2026) papers from the listed venues on OS scheduling/resource-contention topics; extract gaps/ideas and reflect them in design choices.

Formal written literature-review artifact: A standalone document (separate from the README's short rationale section) formally summarizing the papers reviewed and the research gaps identified, in enough detail to justify each major design decision below.



Telemetry



Multi-critical-process monitoring: Support monitoring multiple simultaneous critical processes, not just one.

Telemetry module: Uses CreateToolhelp32Snapshot to locate target processes by name, and GetProcessTimes to sample CPU usage every 1 second, converting kernel+user time deltas into live CPU %.

CPU% normalization across multi-core systems: Explicitly define and normalize whether thresholds refer to % of one core or % of total system capacity, and apply consistently.

Memory/I-O-based triggers: In addition to CPU, support triggering throttling/suspension based on memory pressure and/or I-O contention on the critical process(es).

Self-monitoring: The service logs its own CPU/memory footprint to demonstrate it is lightweight.



Throttling \& suspension logic



Throttling logic: When a critical process's CPU usage is ≥85% for 3+ consecutive samples, demote the batch process(es) via SetPriorityClass (to IDLE\_PRIORITY\_CLASS or BELOW\_NORMAL\_PRIORITY\_CLASS) within 2 seconds of the trigger.

Per-batch-process independent suspend/resume: Each batch process is throttled/suspended/resumed independently rather than as an all-or-nothing group.

Escalation logic: If, after throttling, the critical process remains ≥90% CPU for 5 consecutive samples (5s), escalate by calling SuspendThread across all threads of the affected batch process(es) (via thread snapshot).

Predictive/adaptive thresholds: Threshold values adapt based on observed load trends rather than remaining purely static, directly informed by the research-gap findings.

Recovery logic: Resume suspended batch processes (ResumeThread) and restore normal priority once the associated critical process's CPU usage drops below 60% for 5 consecutive samples.

Race-condition safety: Guard against a suspended process's threads or PID becoming invalid before ResumeThread is called (e.g., the process exits while suspended).



Robustness \& lifecycle



Startup handling: Gracefully handle a configured target process that isn't running yet when the service starts (retry/wait rather than crash).

Crash handling: Detect and recover cleanly if a target process crashes mid-monitoring.

Restart/PID-change handling: Re-acquire a target process's handle automatically if it restarts under a new PID.

Graceful service shutdown: Resume any suspended threads and restore normal priorities before the service stops.

Protection list: A configurable list of processes (e.g., explorer.exe, csrss.exe, the monitor's own process) that must never be throttled or suspended, regardless of other config.

Access-denied handling: Handle OpenProcess/OpenThread failures gracefully for protected or elevated processes the service cannot touch.



Configuration \& operations



Dynamic JSON configuration: config.json (parsed via nlohmann/json) controlling target critical/batch process names (arrays), throttle/escalate/resume thresholds, polling interval — tunable without recompiling.

Config hot-reload: Apply changes to config.json without restarting the service.

Config validation: Reject malformed JSON, missing fields, or out-of-range thresholds at load time with a clear startup error.

Installer/uninstaller: A script that cleanly registers/removes the Windows Service (not just building the .exe).

CLI manual-override mode: A command-line interface to force throttle/suspend/resume actions without waiting for thresholds, for debugging and demos.

Least-privilege account \& config tamper protection: The service runs under a least-privilege account where possible, and config.json is protected against unauthorized modification.



Logging, monitoring \& alerting



Structured JSON logging: JSON Lines format, logging every priority-state change and suspend/resume event with timestamp, process name, PID, old state, new state, and CPU% at time of change.

Log file rotation: A size- or age-based rotation policy so logs don't grow unbounded.

Windows Event Log integration: Priority-state changes and suspend/resume events are also written to the Windows Event Log alongside the JSON log file.

Alerting/notification: A notification (e.g., toast or email) fires when a suspension event is triggered.

Log-viewer/graphing script: A companion script that consumes the JSON Lines log output and renders CPU%/state-change graphs.



Testing \& scale



Automated unit tests: Cover the CPU%-from-ticks calculation and the threshold/consecutive-sample state machine.

Multi-server/distributed coordination: Support multiple dispatch servers each running the monitor, reporting to (or coordinating through) a central controller.



Test harness



Two self-built dummy test applications: a "critical" CPU-hungry app and a "batch" CPU-hungry app, used as real target processes for the demo — not mocked.

Windows Service packaging: The monitor installs and runs as an actual Windows Service (auto-start, runs silently in the background).

Administrator privilege handling: The service runs with the rights needed for PROCESS\_SET\_INFORMATION on target processes.

4\. CONSTRAINTS \& OUT-OF-SCOPE

Windows-only. No Linux/macOS support — this system relies on Win32 APIs (SetPriorityClass, SuspendThread, ResumeThread, GetProcessTimes) that have no equivalent on other platforms.

Language: C++17 or higher. Headers: <windows.h>, <tlhelp32.h>, <psapi.h>. JSON library: nlohmann/json (single-header).

No GUI — this is a background service; the log-viewer/graphing script is a script, not a full dashboard application.

Module ownership (Divik/Prakul/Pranav split) is not finalized in this spec.

Scope-size flag (carried over from the expand-and-contract review): every candidate capability considered was placed in Core, and nothing was placed in Out. This means there is no pre-agreed fallback list to cut from if timeline pressure hits — that trade-off was made deliberately and is documented here rather than silently discovered later. In particular, distributed multi-server coordination and predictive/adaptive thresholds are substantially larger efforts than the rest of the list and may warrant being staged (built and validated after the core single-server throttle/suspend/resume loop works), even though they remain in-scope for this spec.

5\. DELIVERABLES

A Source/ folder containing:

The C++ monitor/service source code (telemetry, throttling, suspension, config loading, logging, CLI override, self-monitoring, distributed coordination client)

The two dummy test applications (critical-simulator and batch-simulator)

The installer/uninstaller script

The log-viewer/graphing script

Automated unit tests

A sample config.json with working default values matching the thresholds above

A formal literature-review document summarizing the papers reviewed and research gaps identified

A README.md covering: build instructions, how to run/install the monitor as a Windows Service, how to run the dummy test apps for a demo, how to use the CLI override, and a short "Design Rationale" section noting which design decisions were informed by the research scan

6\. DEFINITION OF DONE

Repo contains a Source/ folder with the monitor/service C++ source, both dummy test app sources, the installer/uninstaller script, the log-viewer script, unit tests, a sample config.json, a formal literature-review document, and a README.md.

The monitor successfully installs and runs as a Windows Service that auto-starts and runs silently in the background, via the installer script.

Running a critical dummy app to ≥85% CPU for 3+ consecutive 1-second samples triggers a priority demotion of the associated batch dummy app within 2 seconds.

If a critical dummy app stays ≥90% CPU for 5 consecutive samples after throttling, the associated batch dummy app's threads are suspended via SuspendThread, independently of any other batch process.

When a critical dummy app's CPU usage drops below 60% for 5 consecutive samples, its associated batch dummy app is resumed (ResumeThread) and restored to normal priority.

All threshold values, target process names, and polling interval are read from config.json, are validated on load (with a clear error on invalid config), and can be changed via hot-reload without restarting the service.

Every priority-state change and suspend/resume event is written both as a JSON Lines log entry (timestamp, process name, PID, old state, new state, CPU%) and as a Windows Event Log entry.

The service handles a target process that is not yet running at startup, that crashes mid-monitoring, or that restarts under a new PID, without the service itself crashing.

The service never throttles or suspends a process on the configured protection list, even if it matches a batch-process name pattern.

Automated unit tests exist and pass for the CPU%-from-ticks calculation and the threshold/consecutive-sample state machine.

The CLI override can force a throttle, suspend, or resume action on demand, independent of the threshold state machine.

The literature-review document lists the papers reviewed (2023–2026, from the specified venues) and at least 2 research gaps that map to specific design decisions in the system (e.g., predictive thresholds, memory/I-O triggers).

The monitor runs with Administrator (or configured least-privilege) rights sufficient to call SetPriorityClass and SuspendThread/ResumeThread on target processes, and config.json is protected against unauthorized edits.

No component of the system requires or references Linux/macOS-specific code paths.

