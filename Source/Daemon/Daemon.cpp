#include "Daemon.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <sstream>

#include "ConfigSecurity.h"

namespace daemon {

namespace {

using nlohmann::ordered_json;

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool SameName(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool Contains(const std::vector<std::wstring>& names, const std::wstring& name) {
    return std::any_of(names.begin(), names.end(),
                       [&](const std::wstring& n) { return SameName(n, name); });
}

actuator::StateMachineConfig ToStateMachineConfig(const config::DaemonConfig& c) {
    actuator::StateMachineConfig s;
    s.throttleThresholdPercent = c.high.percent;
    s.throttleConsecutiveSamples = c.high.consecutiveSamples;
    s.escalateThresholdPercent = c.critical.percent;
    s.escalateConsecutiveSamples = c.critical.consecutiveSamples;
    s.recoverThresholdPercent = c.low.percent;
    s.recoverConsecutiveSamples = c.low.consecutiveSamples;
    return s;
}

telemetry::AdaptiveSettings ToAdaptive(const config::DaemonConfig& c) {
    telemetry::AdaptiveSettings a;
    a.enabled = c.adaptiveEnabled;
    a.windowSamples = c.adaptiveWindowSamples;
    a.horizonSamples = c.adaptiveHorizonSamples;
    a.maxAdjustPercent = c.adaptiveMaxAdjustPercent;
    return a;
}

const char* LifecycleName(telemetry::LifecycleKind k) {
    switch (k) {
        case telemetry::LifecycleKind::Acquired:     return "acquired";
        case telemetry::LifecycleKind::Exited:       return "exited";
        case telemetry::LifecycleKind::Crashed:      return "crashed";
        case telemetry::LifecycleKind::Restarted:    return "restarted";
        case telemetry::LifecycleKind::AccessDenied: return "access_denied";
    }
    return "unknown";
}

double Round1(double x) { return static_cast<double>(static_cast<long long>(x * 10.0 + (x >= 0 ? 0.5 : -0.5))) / 10.0; }

ordered_json Reply(bool ok, const std::string& message) {
    ordered_json j;
    j["ok"] = ok;
    j["message"] = message;
    return j;
}

}

std::wstring DefaultConfigPath(bool preferExeDirectory) {
    if (preferExeDirectory) {
        wchar_t exe[MAX_PATH];
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0) {
            const auto local = std::filesystem::path(exe).parent_path() / L"config.json";
            if (std::filesystem::exists(local)) {
                return local.wstring();
            }
        }
    }
    wchar_t programData[MAX_PATH] = L"C:\\ProgramData";
    GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
    return std::wstring(programData) + L"\\ThreadHaulDaemon\\config.json";
}

Daemon::Daemon(std::wstring configPath) : configPath_(std::move(configPath)) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    cores_ = si.dwNumberOfProcessors;
}

Daemon::~Daemon() {
    if (pipe_) pipe_->Stop();
    if (coordination_) coordination_->Stop();
}

bool Daemon::LoadAndVerify(config::DaemonConfig& out, std::wstring& error) {
    try {
        out = config::LoadConfigFile(configPath_);
    } catch (const config::ConfigError& e) {
        error = Widen(e.what());
        return false;
    }
    std::wstring who;
    if (IsWritableByNonAdmins(configPath_, who)) {
        if (out.requireProtectedConfig) {
            error = L"config file can be modified by " + who +
                    L"; security.require_protected_config is on. Re-run the installer or "
                    L"restrict the file to SYSTEM and Administrators.";
            return false;
        }
        Log("config_security", ordered_json{{"warning", "config file is writable by " + Narrow(who)},
                                            {"config_path", Narrow(configPath_)}});
    }
    return true;
}

bool Daemon::Initialize(std::wstring& error) {
    config::DaemonConfig cfg;
    if (!LoadAndVerify(cfg, error)) {
        return false;
    }
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (GetFileAttributesExW(configPath_.c_str(), GetFileExInfoStandard, &attr)) {
        configWriteTime_ = attr.ftLastWriteTime;
        configSize_ = (static_cast<ULONGLONG>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    actuator_ = std::make_unique<actuator::ActuatorManager>(
        actuator::StateMachineConfig{},
        [this](const std::wstring& name, DWORD pid, actuator::ProcessState from,
               actuator::ProcessState to, double cpu, bool ok) {
            OnTransition(name, pid, from, to, cpu, ok);
        });
    ApplyConfig(cfg, true);

    pipe_ = std::make_unique<ControlPipeServer>(
        [this](const std::string& req) { return HandleCommand(req); });
    pipe_->Start();
    lastSelfLog_ = GetTickCount64();  // first self-sample needs a full interval

    Log("service_lifecycle",
        ordered_json{{"event", "started"},
                     {"config_path", Narrow(configPath_)},
                     {"config_sha256", Sha256File(configPath_)},
                     {"logical_cores", cores_},
                     {"cpu_normalization", cfg_.cpuPerCore ? "per_core" : "total"},
                     {"pid", GetCurrentProcessId()}});
    return true;
}

void Daemon::ApplyConfig(const config::DaemonConfig& cfg, bool initial) {
    const bool logChanged = initial || cfg.logPath != cfg_.logPath ||
                            cfg.maxFileSizeMb != cfg_.maxFileSizeMb ||
                            cfg.maxRotatedFiles != cfg_.maxRotatedFiles;
    const bool coordChanged =
        initial || cfg.coordinationEnabled != cfg_.coordinationEnabled ||
        cfg.controllerHost != cfg_.controllerHost || cfg.controllerPort != cfg_.controllerPort ||
        cfg.sharedToken != cfg_.sharedToken || cfg.heartbeatSeconds != cfg_.heartbeatSeconds;
    cfg_ = cfg;

    if (logChanged) {
        logger_ = std::make_unique<logging::JsonLinesLogger>(
            cfg.logPath, static_cast<uint64_t>(cfg.maxFileSizeMb) * 1024 * 1024,
            cfg.maxRotatedFiles);
    }

    base_ = ToStateMachineConfig(cfg);
    adaptive_.SetSettings(ToAdaptive(cfg));
    effective_ = adaptive_.Adjust(base_, coordination_ ? coordination_->ExternalBias() : 0.0);
    actuator_->SetConfig(effective_);
    actuator_->SetProtectedProcessNames(cfg.protectedProcesses);

    const auto norm = cfg.cpuPerCore ? telemetry::CpuNormalization::PerCore
                                     : telemetry::CpuNormalization::TotalSystem;
    for (auto it = critical_.begin(); it != critical_.end();) {
        if (!Contains(cfg.criticalProcesses, it->first)) {
            it = critical_.erase(it);
        } else {
            it->second->SetNormalization(norm);
            ++it;
        }
    }
    for (const std::wstring& name : cfg.criticalProcesses) {
        if (critical_.find(name) == critical_.end()) {
            critical_[name] = std::make_unique<telemetry::TrackedProcess>(name, norm, cores_);
        }
    }

    // Batch processes whose name was removed from config go back to normal.
    std::vector<DWORD> dropped;
    actuator_->ForEach([&](const actuator::ProcessStateMachine& m) {
        if (!Contains(cfg.batchProcesses, m.BatchProcessName()) &&
            adhoc_.count(m.BatchProcessId()) == 0) {
            dropped.push_back(m.BatchProcessId());
        }
    });
    for (DWORD pid : dropped) {
        trigger_ = "config_reload";
        actuator_->Find(pid)->ForceRecoverForShutdown();
        actuator_->RemoveBatchProcess(pid);
        trigger_ = "automatic";
    }

    alerter_.Configure(cfg.alertPopup, cfg.alertWebhookUrl);

    if (coordChanged) {
        if (coordination_) {
            coordination_->Stop();
            coordination_.reset();
        }
        if (cfg.coordinationEnabled) {
            coordination_ = std::make_unique<CoordinationClient>(
                [this] { return StatusJson(); },
                [this](const std::string& cmd) { return HandleCommand(cmd); },
                [this](const std::string& what, const std::string& detail) {
                    std::lock_guard<std::recursive_mutex> lock(mutex_);
                    Log("coordination", ordered_json{{"event", what}, {"detail", detail}});
                });
            coordination_->Start(cfg.controllerHost, cfg.controllerPort, cfg.sharedToken,
                                 cfg.heartbeatSeconds);
        }
    }
}

void Daemon::CheckConfigReload() {
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(configPath_.c_str(), GetFileExInfoStandard, &attr)) {
        return;
    }
    const ULONGLONG size = (static_cast<ULONGLONG>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
    if (CompareFileTime(&attr.ftLastWriteTime, &configWriteTime_) == 0 && size == configSize_) {
        return;
    }
    configWriteTime_ = attr.ftLastWriteTime;
    configSize_ = size;

    config::DaemonConfig cfg;
    std::wstring error;
    if (!LoadAndVerify(cfg, error)) {
        // Keep running on the last good config.
        Log("config_reload", ordered_json{{"success", false},
                                          {"error", Narrow(error)},
                                          {"config_sha256", Sha256File(configPath_)}});
        std::fwprintf(stderr, L"config reload rejected: %ls\n", error.c_str());
        return;
    }
    ApplyConfig(cfg, false);
    Log("config_reload",
        ordered_json{{"success", true},
                     {"config_sha256", Sha256File(configPath_)},
                     {"poll_interval_ms", cfg.pollIntervalMs},
                     {"high_percent", cfg.high.percent},
                     {"critical_percent", cfg.critical.percent},
                     {"low_percent", cfg.low.percent},
                     {"adaptive_enabled", cfg.adaptiveEnabled}});
}

void Daemon::Run(HANDLE stopEvent) {
    while (true) {
        const ULONGLONG started = GetTickCount64();
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            CheckConfigReload();
            Poll();
        }
        const ULONGLONG elapsed = GetTickCount64() - started;
        const DWORD interval = static_cast<DWORD>(cfg_.pollIntervalMs);
        const DWORD wait = elapsed >= interval ? 0 : static_cast<DWORD>(interval - elapsed);
        if (WaitForSingleObject(stopEvent, wait) != WAIT_TIMEOUT) {
            break;
        }
    }

    if (pipe_) pipe_->Stop();
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        trigger_ = "shutdown";
        actuator_->ForceRecoverAllForShutdown();
        trigger_ = "automatic";
        Log("service_lifecycle", ordered_json{{"event", "stopped"}});
    }
    if (coordination_) {
        if (coordination_->Connected()) Sleep(500);  // let the recovery events reach the controller
        coordination_->Stop();
    }
}

void Daemon::Poll() {
    const std::vector<telemetry::ProcessEntry> snapshot = telemetry::SnapshotProcesses();

    std::vector<telemetry::ProcessSample> samples;
    std::vector<telemetry::LifecycleEvent> events;
    for (auto& [name, tracked] : critical_) {
        tracked->Poll(snapshot, samples, events);
    }
    for (const telemetry::LifecycleEvent& e : events) {
        ordered_json f{{"role", "critical"},
                       {"event", LifecycleName(e.kind)},
                       {"process_name", Narrow(e.name)},
                       {"process_id", e.pid}};
        if (e.previousPid != 0) f["previous_process_id"] = e.previousPid;
        if (e.kind == telemetry::LifecycleKind::Crashed || e.kind == telemetry::LifecycleKind::Exited) {
            f["exit_code"] = e.exitCode;
        }
        Log("process_lifecycle", f);
    }

    // Combine every critical instance's signals into one pressure value.
    const double memLoad =
        cfg_.memoryTriggerEnabled ? telemetry::SystemMemoryLoadPercent() : 0.0;
    double pressure = 0.0;
    std::string signal = "cpu";
    DWORD pid = 0;
    bool anyValid = false;
    for (const telemetry::ProcessSample& s : samples) {
        if (!s.valid) continue;
        anyValid = true;
        double p = s.cpuPercent;
        std::string sig = "cpu";
        if (cfg_.memoryTriggerEnabled) {
            const double sys = 100.0 * memLoad / cfg_.systemMemoryLimitPercent;
            if (sys > p) { p = sys; sig = "system_memory"; }
            if (cfg_.criticalWorkingSetLimitMb > 0) {
                const double ws = 100.0 * s.workingSetMb / cfg_.criticalWorkingSetLimitMb;
                if (ws > p) { p = ws; sig = "working_set"; }
            }
        }
        if (cfg_.ioTriggerEnabled) {
            const double io = 100.0 * s.ioMbPerSec / cfg_.criticalIoLimitMbPerSec;
            if (io > p) { p = io; sig = "io"; }
        }
        if (cfg_.logSamples) {
            Log("sample", ordered_json{{"process_id", s.pid},
                                       {"cpu_percent", Round1(s.cpuPercent)},
                                       {"process_name", Narrow(s.name)},
                                       {"memory_mb", Round1(s.workingSetMb)},
                                       {"io_mb_per_sec", Round1(s.ioMbPerSec)},
                                       {"pressure_percent", Round1(p)},
                                       {"trigger_signal", sig}});
        }
        if (p >= pressure) {
            pressure = p;
            signal = sig;
            pid = s.pid;
        }
    }
    lastSamples_ = samples;

    DiscoverBatchProcesses(snapshot);

    // A critical instance that was only just (re)acquired has no CPU delta
    // yet; skip the state machines for that one poll rather than feed 0%.
    if (!samples.empty() && !anyValid) {
        return;
    }
    pressure_ = pressure;
    pressureSignal_ = signal;
    criticalPid_ = pid;

    adaptive_.AddSample(pressure);
    const double bias = coordination_ ? coordination_->ExternalBias() : 0.0;
    const actuator::StateMachineConfig eff = adaptive_.Adjust(base_, bias);
    if (std::abs(eff.throttleThresholdPercent - effective_.throttleThresholdPercent) >= 0.5) {
        effective_ = eff;
        actuator_->SetConfig(effective_);
        Log("adaptive_threshold",
            ordered_json{{"offset_percent", Round1(base_.throttleThresholdPercent - eff.throttleThresholdPercent)},
                         {"slope_percent_per_sample", Round1(adaptive_.Slope())},
                         {"forecast_percent", Round1(adaptive_.Forecast())},
                         {"controller_bias_percent", Round1(bias)},
                         {"high_percent", Round1(eff.throttleThresholdPercent)},
                         {"critical_percent", Round1(eff.escalateThresholdPercent)},
                         {"low_percent", Round1(eff.recoverThresholdPercent)}});
    }

    for (const auto& fired : tracker_.Add(pressure, effective_)) {
        if (logger_) {
            logger_->LogThresholdEvent(pid, fired.type, fired.consecutiveSamples, Round1(pressure));
        }
    }

    // Batch processes that exited since the last poll (UpdateAll drops them).
    actuator_->ForEach([&](const actuator::ProcessStateMachine& m) {
        if (m.HasExited()) {
            Log("process_lifecycle", ordered_json{{"role", "batch"},
                                                  {"event", "exited"},
                                                  {"process_name", Narrow(m.BatchProcessName())},
                                                  {"process_id", m.BatchProcessId()},
                                                  {"state", logging::StateName(m.CurrentState())}});
        }
    });
    if (actuator_->UpdateAll(pressure)) {
        tracker_.Reset();
    }
    for (auto it = adhoc_.begin(); it != adhoc_.end();) {
        it = actuator_->Find(*it) == nullptr ? adhoc_.erase(it) : std::next(it);
    }

    if (cfg_.selfMonitorIntervalSec > 0 &&
        GetTickCount64() - lastSelfLog_ >= static_cast<ULONGLONG>(cfg_.selfMonitorIntervalSec) * 1000) {
        LogSelf();
    }
}

void Daemon::DiscoverBatchProcesses(const std::vector<telemetry::ProcessEntry>& snapshot) {
    for (const std::wstring& name : cfg_.batchProcesses) {
        for (DWORD pid : telemetry::FindByName(snapshot, name)) {
            if (actuator_->Find(pid) != nullptr) continue;
            if (actuator_->AddBatchProcess(name, pid)) {
                Log("process_lifecycle", ordered_json{{"role", "batch"},
                                                      {"event", "acquired"},
                                                      {"process_name", Narrow(name)},
                                                      {"process_id", pid}});
            } else if (protectedLogged_.insert(pid).second) {
                Log("process_lifecycle", ordered_json{{"role", "batch"},
                                                      {"event", "protected_skipped"},
                                                      {"process_name", Narrow(name)},
                                                      {"process_id", pid}});
            }
        }
    }
}

void Daemon::LogSelf() {
    lastSelfLog_ = GetTickCount64();
    double cpu, ws, priv;
    DWORD handles;
    self_.Sample(cpu, ws, priv, handles);
    Log("self_monitor", ordered_json{{"cpu_percent", Round1(cpu)},
                                     {"working_set_mb", Round1(ws)},
                                     {"private_mb", Round1(priv)},
                                     {"handle_count", handles},
                                     {"managed_batch_processes", actuator_->ManagedCount()},
                                     {"tracked_critical_names", critical_.size()}});
}

void Daemon::Log(const char* type, const ordered_json& fields) {
    if (logger_) {
        logger_->LogEvent(type, fields);
    }
}

void Daemon::OnTransition(const std::wstring& name, DWORD pid, actuator::ProcessState from,
                          actuator::ProcessState to, double cpu, bool ok) {
    if (logger_) {
        logger_->LogActuatorAction(pid, name, criticalPid_, from, to, Round1(cpu), ok, trigger_);
    }
    eventLog_.ReportActuatorAction(pid, name, criticalPid_, from, to, cpu, ok);

    ordered_json evt{{"log_type", "actuator_action"},
                     {"batch_process_id", pid},
                     {"batch_process_name", Narrow(name)},
                     {"critical_process_id", criticalPid_},
                     {"action", logging::ActionName(to)},
                     {"from_state", logging::StateName(from)},
                     {"to_state", logging::StateName(to)},
                     {"cpu_percent", Round1(cpu)},
                     {"actuation_succeeded", ok},
                     {"trigger", trigger_}};
    if (coordination_) {
        coordination_->SendEvent(evt.dump());
    }
    if (to == actuator::ProcessState::Suspended) {
        wchar_t msg[512];
        std::swprintf(msg, ARRAYSIZE(msg),
                      L"%ls (PID %u) was suspended to protect critical process PID %u "
                      L"(%.1f%% %hs). Trigger: %hs.",
                      name.c_str(), pid, criticalPid_, cpu, pressureSignal_.c_str(), trigger_);
        evt["alert"] = "batch_suspended";
        alerter_.Fire(L"ThreadHaul: batch process suspended", msg, evt.dump());
        Log("alert", ordered_json{{"alert", "batch_suspended"},
                                  {"batch_process_id", pid},
                                  {"batch_process_name", Narrow(name)},
                                  {"popup", cfg_.alertPopup},
                                  {"webhook", !cfg_.alertWebhookUrl.empty()}});
    }
}

std::string Daemon::StatusJson() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ordered_json j;
    j["ok"] = true;
    j["pressure_percent"] = Round1(pressure_);
    j["pressure_signal"] = pressureSignal_;
    j["critical_process_id"] = criticalPid_;
    j["offset_percent"] = Round1(base_.throttleThresholdPercent - effective_.throttleThresholdPercent);
    j["thresholds"] = {{"high", Round1(effective_.throttleThresholdPercent)},
                       {"critical", Round1(effective_.escalateThresholdPercent)},
                       {"low", Round1(effective_.recoverThresholdPercent)}};
    j["coordination_connected"] = coordination_ && coordination_->Connected();
    ordered_json crit = ordered_json::array();
    for (const telemetry::ProcessSample& s : lastSamples_) {
        crit.push_back({{"name", Narrow(s.name)}, {"pid", s.pid}, {"cpu_percent", Round1(s.cpuPercent)}});
    }
    j["critical"] = crit;
    ordered_json batch = ordered_json::array();
    actuator_->ForEach([&](const actuator::ProcessStateMachine& m) {
        batch.push_back({{"name", Narrow(m.BatchProcessName())},
                         {"pid", m.BatchProcessId()},
                         {"state", logging::StateName(m.CurrentState())},
                         {"manual_hold", m.IsManuallyHeld()}});
    });
    j["batch"] = batch;
    return j.dump();
}

std::string Daemon::HandleCommand(const std::string& line) {
    std::istringstream in(line);
    std::string verb, target;
    in >> verb >> target;
    std::transform(verb.begin(), verb.end(), verb.begin(), ::tolower);

    if (verb == "status") {
        return StatusJson();
    }
    if (verb == "stop") {
        if (remoteStopEvent_ == nullptr) {
            return Reply(false, "running as a service: use 'sc stop ThreadHaulDaemon'").dump();
        }
        SetEvent(remoteStopEvent_);
        return Reply(true, "stopping; managed processes will be restored").dump();
    }
    if (verb != "throttle" && verb != "suspend" && verb != "resume" && verb != "release") {
        return Reply(false, "unknown command \"" + verb +
                                "\"; use throttle|suspend|resume|release <pid|name> or status")
            .dump();
    }
    if (target.empty()) {
        return Reply(false, verb + " needs a PID or process name").dump();
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto snapshot = telemetry::SnapshotProcesses();
    std::vector<telemetry::ProcessEntry> targets;
    const bool numeric = std::all_of(target.begin(), target.end(), ::isdigit);
    for (const telemetry::ProcessEntry& e : snapshot) {
        if (numeric ? e.pid == std::stoul(target) : SameName(e.name, Widen(target))) {
            targets.push_back(e);
        }
    }
    if (targets.empty()) {
        return Reply(false, "no running process matches \"" + target + "\"").dump();
    }

    ordered_json results = ordered_json::array();
    bool allOk = true;
    trigger_ = "manual";
    for (const telemetry::ProcessEntry& e : targets) {
        std::string outcome;
        bool ok = true;
        actuator::ProcessStateMachine* m = actuator_->Find(e.pid);
        if (Contains(cfg_.criticalProcesses, e.name)) {
            ok = false;
            outcome = "refused: configured as a critical process";
        } else if (verb == "throttle" || verb == "suspend") {
            if (m == nullptr) {
                if (!actuator_->AddBatchProcess(e.name, e.pid)) {
                    ok = false;
                    outcome = "refused: process is on the protection list";
                } else {
                    adhoc_.insert(e.pid);
                    m = actuator_->Find(e.pid);
                }
            }
            if (m != nullptr) {
                m->ForceState(verb == "throttle" ? actuator::ProcessState::Throttled
                                                 : actuator::ProcessState::Suspended,
                              pressure_, true);
                outcome = std::string(verb == "throttle" ? "throttled" : "suspended") +
                          "; held until 'release'";
            }
        } else if (m == nullptr) {
            ok = false;
            outcome = "not managed by the daemon";
        } else if (verb == "resume") {
            m->ForceState(actuator::ProcessState::Normal, pressure_, true);
            outcome = "resumed at normal priority; held until 'release'";
        } else {  // release
            m->ReleaseManualHold();
            outcome = "returned to automatic control";
            if (adhoc_.count(e.pid) && m->CurrentState() == actuator::ProcessState::Normal) {
                actuator_->RemoveBatchProcess(e.pid);
                adhoc_.erase(e.pid);
                outcome = "released (not a configured batch process, no longer managed)";
            }
        }
        allOk = allOk && ok;
        results.push_back({{"pid", e.pid}, {"name", Narrow(e.name)}, {"ok", ok}, {"result", outcome}});
        Log("manual_override", ordered_json{{"command", verb},
                                            {"process_id", e.pid},
                                            {"process_name", Narrow(e.name)},
                                            {"ok", ok},
                                            {"result", outcome}});
    }
    trigger_ = "automatic";

    ordered_json reply = Reply(allOk, allOk ? "done" : "one or more targets failed");
    reply["results"] = results;
    return reply.dump();
}

}
