#pragma once

#include <windows.h>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "AutoProtectPolicy.h"
#include "ProcessTable.h"

namespace daemon {

// Applies AutoProtectPolicy to real processes. Only ordinary applications
// are ever selected: anything classified essential, system or service, and
// anything the caller excludes (configured critical/batch/protected names,
// processes already managed by the Actuator), is left alone. Processes are
// only ever lowered in priority, never suspended, and each one's original
// priority class is restored exactly. A handle is held on every throttled
// process so its PID cannot be reused before it is restored.
//
// Not thread-safe: the Daemon calls it under its own mutex.
class AutoProtector {
public:
    struct Entry {
        DWORD        pid = 0;
        std::wstring name;
        HANDLE       handle = nullptr;
        DWORD        originalClass = NORMAL_PRIORITY_CLASS;
        DWORD        currentClass = NORMAL_PRIORITY_CLASS;
        double       cpuAtAction = 0.0;
        ULONGLONG    sinceTick = 0;
    };

    // true = never select this process
    using ExcludeFn = std::function<bool(const std::wstring& name, DWORD pid)>;
    // event: "throttled", "lowered_to_idle", "restored", "exempted", "level"
    using LogFn = std::function<void(const char* event, const Entry* entry, DWORD fromClass,
                                     DWORD toClass, const std::string& detail)>;

    explicit AutoProtector(LogFn log);
    ~AutoProtector();

    void Configure(const telemetry::AutoProtectSettings& settings, std::vector<std::wstring> essential);
    void Tick(const ExcludeFn& exclude);
    void RestoreAll(const std::string& reason);

    // Restores the process (if throttled) and exempts it from auto-protect
    // until it exits. Returns false if the PID is not running.
    bool Exempt(DWORD pid, const std::wstring& name);
    bool IsAutoManaged(DWORD pid) const { return throttled_.count(pid) != 0; }

    std::string StatusJson() const;  // for /api/status
    std::map<DWORD, std::string> States() const;  // pid -> "auto: below normal" / "auto: idle"
    double SystemCpu() const { return systemCpu_; }
    telemetry::AutoLevel Level() const { return state_.Level(); }

private:
    double SampleSystemCpu();
    void Restore(std::map<DWORD, Entry>::iterator it, const std::string& reason);
    bool Lower(const telemetry::ProcessTable::Row& row, DWORD targetClass);

    LogFn log_;
    telemetry::AutoProtectSettings settings_;
    telemetry::AutoProtectState state_;
    telemetry::ProcessTable table_;
    std::map<DWORD, Entry> throttled_;
    std::set<DWORD> exempt_;
    uint64_t lastIdle_ = 0;
    uint64_t lastTotal_ = 0;
    double systemCpu_ = 0.0;
};

const char* PriorityClassName(DWORD cls);  // "idle", "below normal", "normal", ...

}
