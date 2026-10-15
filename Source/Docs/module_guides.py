"""Generates one PDF per team member describing their module.

    python Source\\Docs\\module_guides.py

Writes Source\\Docs\\ModuleGuides\\<Name>_<Module>.pdf. Edit the CONTENT
dictionary below and re-run to regenerate.
"""

import os

from reportlab.lib import colors
from reportlab.lib.enums import TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.platypus import (KeepTogether, PageBreak, Paragraph, Preformatted,
                                SimpleDocTemplate, Spacer, Table, TableStyle)

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ModuleGuides")

styles = getSampleStyleSheet()
H1 = ParagraphStyle("H1", parent=styles["Heading1"], fontSize=18, spaceAfter=6,
                    textColor=colors.HexColor("#1F3A5F"))
H2 = ParagraphStyle("H2", parent=styles["Heading2"], fontSize=13, spaceBefore=10,
                    spaceAfter=4, textColor=colors.HexColor("#1F3A5F"))
H3 = ParagraphStyle("H3", parent=styles["Heading3"], fontSize=11, spaceBefore=6, spaceAfter=2)
BODY = ParagraphStyle("Body", parent=styles["BodyText"], fontSize=9.5, leading=13,
                      alignment=TA_LEFT)
SMALL = ParagraphStyle("Small", parent=BODY, fontSize=8.5, leading=11)
CELL = ParagraphStyle("Cell", parent=BODY, fontSize=8.3, leading=10.5)
CELLB = ParagraphStyle("CellB", parent=CELL, fontName="Helvetica-Bold")
CODE = ParagraphStyle("Code", parent=styles["Code"], fontSize=7.8, leading=9.6,
                      backColor=colors.HexColor("#F3F5F8"), borderPadding=4)
SUB = ParagraphStyle("Sub", parent=BODY, textColor=colors.HexColor("#555555"))


def P(text, style=BODY):
    return Paragraph(text, style)


def bullets(items):
    return [P("&bull; " + i, BODY) for i in items]


def table(header, rows, widths):
    data = [[P(h, CELLB) for h in header]] + [[P(c, CELL) for c in r] for r in rows]
    t = Table(data, colWidths=[w * mm for w in widths], repeatRows=1)
    t.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#DCE4EE")),
        ("GRID", (0, 0), (-1, -1), 0.4, colors.HexColor("#9AA8B8")),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("LEFTPADDING", (0, 0), (-1, -1), 4),
        ("RIGHTPADDING", (0, 0), (-1, -1), 4),
        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, colors.HexColor("#F7F9FB")]),
    ]))
    return t


def footer(canvas, doc):
    canvas.saveState()
    canvas.setFont("Helvetica", 7.5)
    canvas.setFillColor(colors.HexColor("#777777"))
    canvas.drawString(18 * mm, 10 * mm, f"ThreadHaul-Daemon - {doc.title}")
    canvas.drawRightString(A4[0] - 18 * mm, 10 * mm, f"Page {doc.page}")
    canvas.restoreState()


# --------------------------------------------------------------------------
# Shared section: where each module sits in the whole system.
# --------------------------------------------------------------------------
PIPELINE = """\
 every poll (1 s)
 +-----------------+   pressure %   +------------------+   OS calls   +----------------+
 |   TELEMETRY     | -------------> |    ACTUATOR      | -----------> | batch process  |
 | find + sample   |                | state machine    |  priority /  | (invoice etc.) |
 | critical procs  |                | per batch proc   |  suspend     +----------------+
 +-----------------+                +------------------+
          |                                  | transition callback
          v                                  v
 +-------------------------------------------------------------+
 |  INTEGRATION (Daemon)  config hot-reload, JSON Lines log,   |
 |  Event Log, alerts, CLI pipe, controller client             |
 +-------------------------------------------------------------+
"""

CONTENT = [
# ==========================================================================
{
"file": "Divik_Actuator_Module.pdf",
"title": "Actuator / Scheduling Module",
"owner": "Divik",
"summary": (
    "The Actuator is the part of ThreadHaul-Daemon that <b>acts</b>. Every second it "
    "receives one number, the pressure on the critical process (its CPU%, or memory/I-O "
    "pressure when those triggers are on), and for every batch process it decides whether "
    "to leave it alone, <b>demote its priority</b>, <b>suspend all its threads</b> or "
    "<b>restore it</b>. Each batch process has its own state machine, so one batch job can "
    "be suspended while another is only throttled. The module also owns the safety rules "
    "(protection list, PID-reuse guard, partial-failure reporting) and the manual "
    "override used by the CLI."),
"files": [
    ["Source/Actuator/StateMachine.h/.cpp", "ProcessStateMachine: the Normal / Throttled / Suspended state machine for one batch process."],
    ["Source/Actuator/ActuatorManager.h/.cpp", "ActuatorManager: owns one state machine per batch PID, protection list, config and manual-override entry points."],
    ["Source/Actuator/PriorityControl.h/.cpp", "SetProcessPriority(): wraps OpenProcess + SetPriorityClass. IsRunningElevated()."],
    ["Source/Actuator/ThreadControl.h/.cpp", "SuspendAllThreads() / ResumeAllThreads(): thread snapshot + SuspendThread/ResumeThread."],
    ["Source/Actuator/ThresholdEvent.h", "Shared struct ThresholdEvent (contract with Telemetry, INTERFACES.md section 2)."],
    ["Source/main.cpp", "Service entry point (ServiceMain, control handler), console mode, CLI commands, SeDebugPrivilege."],
    ["Source/Tests/StateMachineTests.cpp", "50 unit checks for the state machine and manager."],
],
"states": [
    ["Normal", "pressure >= 85% for 3 consecutive samples", "Throttled", "SetPriorityClass(IDLE_PRIORITY_CLASS)"],
    ["Throttled", "pressure >= 90% for 5 consecutive samples (counted after the throttle)", "Suspended", "SuspendThread on every thread"],
    ["Throttled or Suspended", "pressure < 60% for 5 consecutive samples", "Normal", "ResumeThread on every thread (if suspended) + SetPriorityClass(NORMAL_PRIORITY_CLASS)"],
    ["any", "manual command (CLI / controller)", "requested state, held", "same OS calls; Update() ignores samples while held"],
    ["any", "service stop / shutdown", "Normal", "ForceRecoverForShutdown(): resume + restore"],
],
"win32": [
    ["OpenProcess(PROCESS_SET_INFORMATION, ...)", "PriorityControl.cpp", "Gets a handle that is allowed to change the batch process's priority class."],
    ["SetPriorityClass(h, IDLE_PRIORITY_CLASS / NORMAL_PRIORITY_CLASS)", "PriorityControl.cpp", "Demotes the batch process so the Windows scheduler runs it only when nothing else wants the CPU, and later restores it."],
    ["CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0) + Thread32First / Thread32Next", "ThreadControl.cpp", "Lists every thread in the system; we keep those whose th32OwnerProcessID is the batch PID."],
    ["OpenThread(THREAD_SUSPEND_RESUME, ...)", "ThreadControl.cpp", "Gets a handle to each thread with just the right needed to suspend/resume it."],
    ["SuspendThread / ResumeThread", "ThreadControl.cpp", "Freeze or unfreeze one thread. Return value -1 = failure, counted in ThreadOpResult."],
    ["OpenProcess(SYNCHRONIZE, ...) + WaitForSingleObject(h, 0)", "StateMachine.cpp", "Keeps a handle to each managed process. While we hold it, Windows cannot reuse the PID, and a signalled handle means the process exited (race-condition safety)."],
    ["OpenProcessToken + GetTokenInformation(TokenElevation)", "PriorityControl.cpp", "IsRunningElevated(): console mode refuses to start unelevated unless --allow-unelevated."],
    ["AdjustTokenPrivileges(SE_DEBUG_NAME)", "main.cpp", "Enables SeDebugPrivilege so the service can open processes owned by other accounts."],
    ["StartServiceCtrlDispatcherW, RegisterServiceCtrlHandlerExW, SetServiceStatus", "main.cpp", "Turn the exe into a real Windows Service: report START_PENDING / RUNNING / STOP_PENDING / STOPPED and receive STOP and SHUTDOWN."],
    ["SetConsoleCtrlHandler", "main.cpp", "Ctrl+C in console mode sets the stop event so shutdown still restores every process."],
    ["CompareStringOrdinal(..., TRUE)", "ActuatorManager.cpp", "Case-insensitive process-name comparison for the protection list."],
],
"functions": [
    ["ProcessStateMachine::Update(double cpu)", "Counts consecutive high / critical / low samples and calls TransitionTo when a rule is met. Returns true if the state changed. Does nothing while manually held."],
    ["ProcessStateMachine::TransitionTo(newState, cpu)", "Performs the OS calls for the transition, records whether they fully succeeded (actuation_succeeded), resets the counters, then fires the TransitionCallback (used for logging)."],
    ["ProcessStateMachine::ForceState(state, cpu, hold)", "Manual override: jumps straight to a state (including Normal->Suspended and Suspended->Throttled) and optionally holds it."],
    ["ProcessStateMachine::ForceRecoverForShutdown()", "Resumes threads and restores NORMAL priority; used on service stop."],
    ["ProcessStateMachine::HasExited()", "True once the batch process ended; ActuatorManager then drops it."],
    ["ProcessStateMachine::SetConfig(cfg)", "Applies new thresholds (hot-reload or adaptive change) without resetting counters."],
    ["ActuatorManager::AddBatchProcess(name, pid)", "Starts managing a batch process unless it is protected (own process, core Windows processes, configured list)."],
    ["ActuatorManager::UpdateAll(pressure)", "Feeds one sample to every managed state machine, removing exited ones. Returns true if anything changed."],
    ["ActuatorManager::OnThresholdEvent(evt)", "Entry point from INTERFACES.md section 2; forwards evt.observedCpuPercent to UpdateAll."],
    ["ActuatorManager::SetProtectedProcessNames(list)", "Replaces the configurable protection list; a managed process that becomes protected is restored and released."],
    ["ActuatorManager::ForceState / ReleaseManualHold / Find / ForEach", "Used by the CLI and the status command."],
    ["SetProcessPriority(pid, level)", "Returns Success / AccessDenied / ProcessNotFound / OtherError so callers can report denied access instead of crashing."],
    ["SuspendAllThreads(pid) / ResumeAllThreads(pid)", "Returns ThreadOpResult (attempted / succeeded / failed). A thread that exited between snapshot and OpenThread is not counted as a failure."],
    ["wmain (main.cpp)", "Parses arguments: --console, --config, --allow-unelevated, validate-config, and the CLI verbs throttle / suspend / resume / release / status / stop."],
    ["DirectAction (main.cpp)", "If no daemon is running, the CLI performs throttle / suspend / resume directly, still honouring the protection list."],
],
"safety": [
    "<b>Protection list:</b> the daemon's own PID, System, Registry, smss.exe, csrss.exe, wininit.exe, winlogon.exe, services.exe and lsass.exe are always protected, plus <i>processes.protected</i> from config. Suspending these could hang Windows.",
    "<b>PID reuse / race condition:</b> each state machine holds a SYNCHRONIZE handle, so the PID cannot be recycled while it is managed; if the process exits mid-suspension the resume step sees HasExited() and skips the OS calls instead of touching an unrelated process.",
    "<b>Partial failure:</b> if SetPriorityClass is denied or not every thread could be suspended, the state still advances (so the machine does not retry forever) but <i>actuation_succeeded</i> is logged as false.",
    "<b>Escalation counting:</b> counters reset on every transition, so the 5 critical samples for suspension are counted <i>after</i> throttling, exactly as the spec requires.",
    "<b>Graceful shutdown:</b> service STOP, system SHUTDOWN, Ctrl+C and the CLI stop command all end in ForceRecoverAllForShutdown().",
],
"demo": [
    "Build and test: <font face='Courier'>build.cmd test</font>; StateMachineTests must print 0 failed.",
    "Run the console demo (RUNNING.md section 4) and watch batch_simulator switch to IDLE, stop printing, then print 'resumed after a pause'.",
    "CLI: <font face='Courier'>ThreadHaulDaemon.exe suspend batch_simulator.exe</font>, then <font face='Courier'>status</font> shows manual_hold true; <font face='Courier'>suspend explorer.exe</font> is refused.",
],
"qa": [
    ["Why IDLE priority first instead of suspending straight away?", "A priority change is cheap and reversible and lets the batch job keep using idle CPU; suspension is a stronger action kept for when demotion is not enough (graded escalation)."],
    ["Why suspend threads rather than kill the process?", "Batch work (invoices, sync) must not be lost; suspended threads continue exactly where they stopped."],
    ["What if a thread is created after the snapshot?", "It would not be suspended in that pass; ResumeAllThreads takes a fresh snapshot so nothing is left suspended. This is a known limitation of SuspendThread-based freezing."],
    ["How does the CLI avoid fighting the automatic logic?", "ForceState(..., hold=true) sets manualHold_; Update() keeps counting but never transitions until 'release'."],
],
},
# ==========================================================================
{
"file": "Prakul_Telemetry_Module.pdf",
"title": "Telemetry / Monitoring Module",
"owner": "Prakul",
"summary": (
    "Telemetry is the part of ThreadHaul-Daemon that <b>observes</b>. Once per polling "
    "interval it finds every configured critical process by executable name, measures its "
    "CPU usage from kernel+user time, its memory working set and its I/O rate, and turns "
    "that into the single <b>pressure %</b> that drives the Actuator. It survives a target "
    "that is not running yet, crashes, or restarts under a new PID. It also contains the "
    "<b>predictive (adaptive) thresholds</b>, the threshold-event tracker, the daemon's own "
    "self-monitoring, and the two dummy test applications used for the demo."),
"files": [
    ["Source/Telemetry/CpuMath.h/.cpp", "CpuPercentFromTicks(): converts CPU-time deltas into CPU% with per-core or whole-system normalisation."],
    ["Source/Telemetry/ProcessMonitor.h/.cpp", "SnapshotProcesses, FindByName, TrackedProcess (per-name sampler with lifecycle events), SystemMemoryLoadPercent, SelfMonitor."],
    ["Source/Telemetry/ThresholdTracker.h", "Counts consecutive samples per threshold and reports when a streak completes (for the threshold_event log)."],
    ["Source/Telemetry/AdaptiveThresholds.h/.cpp", "Least-squares trend + forecast that lowers thresholds while load is rising."],
    ["Source/Simulators/critical_simulator.cpp", "Dummy 'dispatch API': burns CPU following a load schedule, can simulate a crash."],
    ["Source/Simulators/batch_simulator.cpp", "Dummy 'invoice job': burns CPU continuously and prints progress and its priority each second."],
    ["Source/Tests/TelemetryTests.cpp", "48 unit checks: CPU math, tracker, adaptive thresholds, real process discovery and crash detection."],
],
"win32": [
    ["CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS) + Process32FirstW / Process32NextW", "ProcessMonitor.cpp", "One snapshot per poll lists every process with its exe name and PID; FindByName matches names case-insensitively."],
    ["OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE)", "ProcessMonitor.cpp", "Least-privilege handle that is enough to read times, memory and I/O and to wait for exit. ERROR_ACCESS_DENIED is reported once as access_denied."],
    ["GetProcessTimes", "ProcessMonitor.cpp", "Returns kernel and user CPU time in 100 ns units; the difference between two polls is the CPU used in that interval."],
    ["GetSystemTimeAsFileTime", "ProcessMonitor.cpp", "Wall-clock time in the same 100 ns units, giving the interval length."],
    ["GetProcessMemoryInfo (psapi)", "ProcessMonitor.cpp", "Working-set size of the critical process (memory trigger, memory_mb in logs)."],
    ["GetProcessIoCounters", "ProcessMonitor.cpp", "Read + write + other bytes transferred; delta / seconds = I/O MB/s (I/O trigger)."],
    ["WaitForSingleObject(handle, 0) + GetExitCodeProcess", "ProcessMonitor.cpp", "A signalled handle means the process ended; exit code 0 = exited, non-zero = crashed."],
    ["GlobalMemoryStatusEx", "ProcessMonitor.cpp", "System-wide memory load % for the memory-pressure trigger."],
    ["GetSystemInfo (dwNumberOfProcessors)", "Daemon.cpp", "Logical core count used for normalisation."],
    ["GetProcessHandleCount, GetCurrentProcess", "ProcessMonitor.cpp", "Self-monitoring of the daemon's own footprint."],
],
"functions": [
    ["CpuPercentFromTicks(dProc, dWall, cores, mode)", "PerCore: 100 x dProc / dWall (one busy core = 100%, clamped to 100 x cores). TotalSystem: that value / cores, clamped to 100. Zero wall time returns 0."],
    ["SnapshotProcesses() / FindByName()", "Builds the per-poll process list and returns all PIDs with a given exe name."],
    ["TrackedProcess::Poll(snapshot, samples, events)", "1) drops instances whose handle is signalled and emits Exited / Crashed; 2) opens new instances and emits Acquired, or Restarted (with previousPid) if the old one just ended; 3) samples every live instance."],
    ["TrackedProcess::SampleInstance()", "Reads times, memory and I/O. The first sample after (re)acquiring is marked valid=false because there is no delta yet, so a 0% reading is never fed to the Actuator."],
    ["ThresholdTracker::Add(percent, cfg)", "Updates three streak counters and returns the thresholds whose streak just reached its length (high 3, critical 5, low 5). Reset() is called whenever the Actuator changes state so events line up with actions."],
    ["AdaptiveThresholds::AddSample / Slope / Forecast", "Keeps the last window_samples values and fits y = a + b x by least squares; Forecast = fitted value horizon_samples ahead."],
    ["AdaptiveThresholds::Offset(base, bias)", "If load is rising and the forecast reaches the throttle threshold, offset = slope x horizon, plus any controller bias, capped at max_adjust_percent and at (high - low - 1) so the hysteresis band survives."],
    ["AdaptiveThresholds::Adjust(base, bias)", "Returns the effective config: high and critical lowered by the offset, low never changed."],
    ["SelfMonitor::Sample()", "The daemon's own CPU%, working set, private bytes and handle count for the self_monitor log line."],
    ["critical_simulator: Worker()", "Busy-loops for <i>load</i>% of every 100 ms slice; main() steps through the --schedule phases and can ExitProcess(3) for --crash-after."],
    ["batch_simulator: Worker()", "Spins flat out; the reporter prints units/s and GetPriorityClass() each second and flags a gap > 2 s as 'resumed after a pause'."],
],
"formula": (
    "CPU% (per core) = 100 x [(kernel<sub>t</sub> + user<sub>t</sub>) - (kernel<sub>t-1</sub> + user<sub>t-1</sub>)] / (wall<sub>t</sub> - wall<sub>t-1</sub>)<br/>"
    "Pressure % = max( CPU%, 100 x memLoad / memLimit, 100 x workingSet / wsLimit, 100 x ioRate / ioLimit )  (memory/I-O terms only when enabled)<br/>"
    "Adaptive offset = min( slope x horizon + controllerBias, max_adjust_percent, high - low - 1 ), applied only while slope > 0 and forecast >= high"),
"safety": [
    "<b>Target not running at startup:</b> TrackedProcess simply finds no PID and keeps looking every poll; no crash, and pressure is treated as 0 so batch jobs are not throttled for nothing.",
    "<b>Crash mid-monitoring:</b> the held handle becomes signalled; the exit code distinguishes a clean exit from a crash and is logged.",
    "<b>Restart under a new PID:</b> the new instance is picked up on the next poll and logged as restarted with previous_process_id.",
    "<b>Several instances of one name:</b> all are sampled; the highest pressure wins and its PID is reported as critical_process_id.",
    "<b>Normalisation:</b> <i>per_core</i> (default) keeps 85% meaning 'most of one core' on any machine; <i>total</i> means 85% of the whole server.",
],
"demo": [
    "<font face='Courier'>build.cmd test</font>: TelemetryTests must print 0 failed (it launches a real cmd.exe that exits with code 3 to test crash detection).",
    "Console demo (RUNNING.md 4.2): the log shows a sample line per second with cpu_percent, memory_mb, io_mb_per_sec and pressure_percent.",
    "Crash / restart: <font face='Courier'>critical_simulator.exe --load 100 --crash-after 10</font>, then start it again; look for crashed and restarted lifecycle entries.",
    "Adaptive: RUNNING.md 4.7, look for adaptive_threshold entries with high_percent below 85.",
],
"qa": [
    ["Why not use Performance Counters / PDH?", "GetProcessTimes on a handle we already hold is cheaper, needs no counter instance names (which change with duplicates), and gives exact kernel+user time."],
    ["Why is the first sample ignored?", "CPU% needs two readings; feeding a fake 0% after a restart could trigger a wrong recovery."],
    ["Why a linear fit and not an LSTM?", "The literature review (Gap 2) shows learned predictors need training data and a lot of compute; a monitor running on the protected server must stay at about 1% CPU. The fit is a few dozen arithmetic operations per second."],
    ["Can adaptation make recovery happen too early?", "No. Only the throttle and escalate thresholds are lowered; the recovery threshold never moves and throttle always stays above it."],
],
},
# ==========================================================================
{
"file": "Pranav_Config_Logging_Integration_Module.pdf",
"title": "Configuration, Logging & Integration Module",
"owner": "Pranav",
"summary": (
    "This module <b>connects and operates</b> the system. It loads and validates "
    "<i>config.json</i> (and re-applies it live when the file changes), writes every "
    "decision to a rotating JSON Lines log and the Windows Event Log, and runs the "
    "integration loop (<i>Daemon</i>) that wires Telemetry to the Actuator once per second. "
    "Around that loop it provides the local control pipe used by the CLI, config tamper "
    "protection, suspension alerts, the client and the central controller for multi-server "
    "coordination, the installer / uninstaller and the log-viewer graphs."),
"files": [
    ["Source/Config/Config.h/.cpp, config.json", "DaemonConfig struct, ParseConfig / LoadConfigFile with field-by-field validation, sample config."],
    ["Source/Logging/JsonLinesLogger.h/.cpp", "Thread-safe JSON Lines writer with size-based rotation; LogSample, LogThresholdEvent, LogActuatorAction, LogEvent."],
    ["Source/Logging/EventLogWriter.h/.cpp", "Writes actuator actions to the Windows Application log (IDs 1000/1001/1002)."],
    ["Source/Daemon/Daemon.h/.cpp", "The integration loop: poll, pressure, adaptive thresholds, actuator, hot-reload, commands, status."],
    ["Source/Daemon/ControlPipe.h/.cpp", "Named-pipe server/client for the CLI (admin-only DACL)."],
    ["Source/Daemon/ConfigSecurity.h/.cpp", "ACL check (is config writable by non-admins?) and SHA-256 of the file."],
    ["Source/Daemon/Alerts.h/.cpp", "Popup (WTSSendMessage) and webhook (WinHTTP POST) on suspension."],
    ["Source/Daemon/Coordination.h/.cpp", "Agent side of multi-server coordination (TCP, JSON lines, reconnect with back-off)."],
    ["Source/Controller/controller.cpp", "ThreadHaulController: accepts agents, logs, computes cluster bias, operator commands."],
    ["Source/Scripts/install.ps1, uninstall.ps1, log_viewer.py; build.cmd", "Service installation, removal, graphs, one-step build."],
    ["Source/Tests/ConfigLoggingTests.cpp", "91 unit checks for validation, optional sections, log schema and rotation."],
],
"section4": "4. How one poll and one config reload work",
"formula": (
    "<b>Every poll (default 1 s):</b> check config.json timestamp/size -> snapshot processes -> "
    "Telemetry samples critical processes -> log samples and lifecycle events -> pressure = worst signal -> "
    "pick up new batch processes -> adaptive thresholds (+ controller bias) -> threshold_event lines -> "
    "ActuatorManager::UpdateAll(pressure) -> each transition is logged to JSON Lines and the Event Log, sent to the "
    "controller, and alerts fire on suspension -> self_monitor line every 30 s.<br/><br/>"
    "<b>Config reload:</b> file changed -> parse + validate the whole file -> ACL check -> if valid, ApplyConfig() and log "
    "config_reload success with the new SHA-256; if invalid, log the error and keep the previous config."),
"win32": [
    ["nlohmann::json (third_party/nlohmann/json.hpp)", "Config.cpp, JsonLinesLogger.cpp", "Parses config.json and builds every log line (ordered_json keeps log_type and timestamp first)."],
    ["MultiByteToWideChar / WideCharToMultiByte (CP_UTF8)", "Config.cpp, logger", "UTF-8 config text to wide process names and back for logs."],
    ["CreateFileW(FILE_APPEND_DATA) / WriteFile / MoveFileExW / DeleteFileW", "JsonLinesLogger.cpp", "Append log lines; rotate threadhaul.jsonl -> .1 -> .2 ... and delete the oldest."],
    ["GetSystemTime", "JsonLinesLogger.cpp", "ISO 8601 UTC timestamps with milliseconds."],
    ["RegisterEventSourceW / ReportEventW / DeregisterEventSource", "EventLogWriter.cpp, main.cpp", "Windows Event Log entries (information or warning; error 1900 on a startup failure)."],
    ["GetFileAttributesExW", "Daemon.cpp", "Last-write time + size of config.json each poll: change detection for hot-reload."],
    ["GetNamedSecurityInfoW + GetAce + CreateWellKnownSid + EqualSid", "ConfigSecurity.cpp", "Reads the config file's ACL and checks whether Everyone / Users / Authenticated Users / Interactive have write rights."],
    ["BCryptOpenAlgorithmProvider(SHA256) / BCryptHashData / BCryptFinishHash", "ConfigSecurity.cpp", "SHA-256 of config.json logged on start and every reload (tamper evidence)."],
    ["CreateNamedPipeW, ConnectNamedPipe, ConvertStringSecurityDescriptorToSecurityDescriptorW", "ControlPipe.cpp", "\\\\.\\pipe\\ThreadHaulDaemon with an SDDL DACL that admits only SYSTEM, Administrators and the daemon's account; PIPE_REJECT_REMOTE_CLIENTS."],
    ["WaitNamedPipeW, CreateFileW, SetNamedPipeHandleState", "ControlPipe.cpp", "CLI client side."],
    ["WTSGetActiveConsoleSessionId + WTSSendMessageW", "Alerts.cpp", "Popup in the logged-in user's session, which works from a Session 0 service."],
    ["WinHttpOpen / WinHttpConnect / WinHttpSendRequest", "Alerts.cpp", "Webhook POST (Teams, Slack, email relay)."],
    ["WSAStartup, GetAddrInfoW, socket/connect/send/recv/select", "Coordination.cpp, controller.cpp", "TCP link between each daemon and the controller."],
    ["sc.exe create / failure / privs / sidtype, icacls, New-EventLog", "install.ps1", "Service registration, restart-on-failure, least privileges, config ACLs, event source."],
],
"functions": [
    ["config::ParseConfig(text) / LoadConfigFile(path)", "Validates every field (type, range, cross-field rules such as low < high <= critical, no name both critical and batch) and throws ConfigError naming the field."],
    ["JsonLinesLogger::LogActuatorAction(...)", "Writes the actuator_action line: batch and critical PIDs, action, from/to state, CPU%, actuation_succeeded, trigger."],
    ["JsonLinesLogger::LogEvent(type, fields)", "Generic writer for all other log kinds (sample, lifecycle, config_reload, self_monitor, alert ...)."],
    ["JsonLinesLogger::Rotate()", "Size-based rotation keeping max_rotated_files old files."],
    ["Daemon::Initialize()", "Loads and verifies config, creates the ActuatorManager with the logging callback, starts the control pipe, logs service start."],
    ["Daemon::Run(stopEvent)", "Every interval: CheckConfigReload() then Poll(); on stop: restore every process, log, then disconnect from the controller."],
    ["Daemon::Poll()", "Telemetry -> lifecycle logs -> pressure -> batch discovery -> adaptive thresholds -> threshold events -> ActuatorManager::UpdateAll -> self-monitor."],
    ["Daemon::ApplyConfig(cfg)", "Applies a new config live: logger, thresholds, tracked names, protection list, alerts, coordination; restores batch processes whose name was removed."],
    ["Daemon::OnTransition(...)", "Callback for every Actuator transition: JSON log, Event Log, controller event, and alert if suspended."],
    ["Daemon::HandleCommand(line)", "Executes CLI / controller commands (throttle, suspend, resume, release, status, stop) and returns JSON."],
    ["IsWritableByNonAdmins(path) / Sha256File(path)", "Config tamper protection; with security.require_protected_config the daemon refuses an unprotected file."],
    ["CoordinationClient::Session()", "hello (with shared token) -> heartbeats -> queued events -> receive bias and commands."],
    ["Controller Rebalance()", "If >= cluster-percent of agents are above --pressure, send every agent bias = --bias; clear it when the cluster calms down."],
],
"safety": [
    "<b>Invalid config at startup</b> stops the service with a message naming the field (console stderr, or Event Log ID 1900 for the service).",
    "<b>Invalid config during hot-reload</b> is rejected and logged; the daemon keeps running on the last good config.",
    "<b>Config tamper protection:</b> installer ACL (SYSTEM + Administrators write, service read, Users nothing), runtime ACL check and SHA-256 logging.",
    "<b>Least privilege:</b> the service keeps only SeDebugPrivilege and SeChangeNotifyPrivilege (sc privs); a LocalService option exists.",
    "<b>Control pipe security:</b> local only, DACL restricted to administrators, SYSTEM and the daemon's own account.",
    "<b>Controller down:</b> the daemon keeps protecting the server locally and reconnects with 1-30 s back-off; the bias is reset to 0 while disconnected.",
],
"demo": [
    "<font face='Courier'>ThreadHaulDaemon.exe validate-config Source\\Config\\config.json</font>, then break a value and run it again to see the field-specific error.",
    "Hot-reload (RUNNING.md 4.5): a valid edit is applied, an invalid one is rejected and logged.",
    "Graphs: <font face='Courier'>python Source\\Scripts\\log_viewer.py demo.jsonl --out demo.png</font>.",
    "Service: <font face='Courier'>install.ps1</font> as Administrator, then Event Viewer -> Application -> source ThreadHaulDaemon.",
    "Multi-server (RUNNING.md 6): ThreadHaulController shows [connect], [event] and [cluster] lines; type <font face='Courier'>list</font>.",
],
"qa": [
    ["Why JSON Lines rather than one JSON document?", "Each line is a complete record, so the file can be appended to, rotated and tailed safely, and a crash never corrupts earlier entries."],
    ["Why both a log file and the Event Log?", "The file is detailed and machine-readable for graphs; the Event Log is where Windows administrators and monitoring tools already look."],
    ["How does hot-reload avoid partial reads?", "The whole file is parsed and validated before anything is applied; a half-written or invalid file is simply rejected and retried on the next change."],
    ["Why does the controller send a bias rather than commands?", "Each server still decides locally from its own measurements; the bias only makes cooler servers start shedding batch load earlier during a fleet-wide peak, so a controller outage never stops local protection."],
],
},
]


def build(spec):
    path = os.path.join(OUT_DIR, spec["file"])
    doc = SimpleDocTemplate(path, pagesize=A4, leftMargin=18 * mm, rightMargin=18 * mm,
                            topMargin=16 * mm, bottomMargin=18 * mm,
                            title=spec["title"], author=spec["owner"],
                            subject="ThreadHaul-Daemon module guide")
    s = []
    s.append(P("ThreadHaul-Daemon", SUB))
    s.append(P(spec["title"], H1))
    s.append(P(f"Module owner: <b>{spec['owner']}</b> &nbsp;&nbsp;|&nbsp;&nbsp; "
               "Logistics server resource monitor &amp; priority throttler (Windows, C++17)", SUB))
    s.append(Spacer(1, 6))

    s.append(P("1. What this module does", H2))
    s.append(P(spec["summary"]))

    s.append(P("2. Where it fits in the system", H2))
    s.append(Preformatted(PIPELINE, CODE))
    s.append(Spacer(1, 2))
    s.append(P("Thresholds (defaults, all configurable): throttle at 85% for 3 samples, "
               "suspend at 90% for 5 samples after throttling, recover below 60% for 5 samples, "
               "1 sample per second.", SMALL))

    s.append(P("3. Files in this module", H2))
    s.append(table(["File", "Purpose"], spec["files"], [62, 112]))

    if "states" in spec:
        s.append(P("4. The state machine", H2))
        s.append(table(["From", "Condition", "To", "OS action"], spec["states"], [24, 58, 26, 66]))
        n = 5
    else:
        s.append(P(spec.get("section4", "4. Key formulas"), H2))
        s.append(P(spec["formula"]))
        n = 5

    s.append(P(f"{n}. Windows APIs and libraries used", H2))
    s.append(table(["API / library", "Where", "What it is used for"], spec["win32"], [58, 32, 84]))

    s.append(P(f"{n + 1}. Main functions written in this module", H2))
    s.append(table(["Function", "What it does"], spec["functions"], [62, 112]))

    s.append(KeepTogether([P(f"{n + 2}. Robustness and safety handled here", H2)] + bullets(spec["safety"])))

    s.append(KeepTogether([P(f"{n + 3}. How to demonstrate it", H2)] + bullets(spec["demo"])))

    s.append(P(f"{n + 4}. Likely review questions", H2))
    for q, a in spec["qa"]:
        s.append(KeepTogether([P(f"<b>Q.</b> {q}", BODY), P(f"<b>A.</b> {a}", BODY), Spacer(1, 4)]))

    s.append(Spacer(1, 6))
    s.append(P("See also: README.md (overview), RUNNING.md (how to run), INTERFACES.md "
               "(cross-module contract), Source/Docs/literature-review.md (design rationale).", SMALL))
    doc.build(s, onFirstPage=footer, onLaterPages=footer)
    return path


if __name__ == "__main__":
    os.makedirs(OUT_DIR, exist_ok=True)
    for spec in CONTENT:
        print("wrote", build(spec))
