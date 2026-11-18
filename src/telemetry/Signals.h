#pragma once

#include <memory>
#include <string>
#include <vector>

namespace telemetry {

// Extra pressure signals read from Windows performance counters (PDH).
// Each signal reads one counter, optionally a wildcard such as
// "\Network Interface(*)\Bytes Total/sec", combines its instances, scales
// the value and expresses it as a percentage of a limit. The service takes
// the highest of all its signals as the critical-process pressure.
//
// Counter paths use the English names (PdhAddEnglishCounter), so a config
// works on any Windows display language. No code is loaded: a signal is
// data, which keeps a privileged service free of third-party plugins.
//
// Linux reads kernel files instead. Built-ins: gpu = gpu_busy_percent of
// each DRM card (summed, or the highest with "aggregate": "max"); disk_queue = I/Os in flight on whole
// disks (/proc/diskstats); network = bytes per second over every interface
// but loopback (/proc/net/dev). A "counter" signal names its source as
// "pressure:cpu", "pressure:memory" or "pressure:io" (the kernel's pressure
// stall figure, some avg10, in %), or "file:/path" (a number; a * in the
// path makes each match one instance).

enum class Aggregate { Sum, Max };

struct SignalSpec {
    std::string name;              // shown in logs, status and metrics
    std::wstring counter;          // e.g. L"\\PhysicalDisk(_Total)\\Current Disk Queue Length"
    Aggregate aggregate = Aggregate::Sum;
    std::wstring instanceContains; // keep only instances whose name contains this (case-insensitive)
    double scale = 1.0;            // value multiplier, e.g. 1/1048576 for bytes -> MB
    double limit = 100.0;          // value at which pressure reaches 100%
};

struct SignalReading {
    std::string name;
    bool ok = false;
    double value = 0.0;     // scaled counter value
    double percent = 0.0;   // 100 * value / limit
    int instances = 0;      // counter instances combined
    std::string error;
};

// Built-in signal types. Returns false if `type` is unknown.
//   gpu         sum of the 3D-engine utilisation of every GPU process, %
//   disk_queue  physical disk queue length (_Total)
//   network     bytes per second over every network interface, in MB/s
bool BuiltInSignal(const std::string& type, SignalSpec& spec);

// Combines counter instances; pure, for tests.
struct CounterItem {
    std::wstring instance;
    double value;
};
bool CombineInstances(const std::vector<CounterItem>& items, Aggregate aggregate,
                      const std::wstring& instanceContains, double& out, int& used);

// Owns one PDH query per signal. Not thread-safe: use from the poll thread.
class SignalSet {
public:
    SignalSet();
    ~SignalSet();
    SignalSet(const SignalSet&) = delete;
    SignalSet& operator=(const SignalSet&) = delete;

    // Replaces all signals. Counters that cannot be opened are kept with an
    // error, reported by Sample() and ignored for pressure.
    void Configure(const std::vector<SignalSpec>& specs);
    bool Empty() const;

    std::vector<SignalReading> Sample();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
