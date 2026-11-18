// Linux pressure signals (see Signals.h): kernel files instead of PDH.

#include "Signals.h"

#include <glob.h>

#include <chrono>
#include <cstdlib>
#include <map>
#include <sstream>

#include "LinuxProc.h"
#include "Utf8.h"

namespace telemetry {

namespace {

enum class Source { Gpu, DiskInFlight, NetBytes, Pressure, File };

struct Signal {
    SignalSpec spec;
    Source source = Source::File;
    std::string arg;          // pressure resource, or file pattern
    std::string openError;
    // Rate state (network): previous byte totals per interface.
    std::map<std::string, unsigned long long> lastBytes;
    std::chrono::steady_clock::time_point lastAt{};
};

std::vector<std::string> Glob(const std::string& pattern) {
    std::vector<std::string> out;
    glob_t g{};
    if (glob(pattern.c_str(), 0, nullptr, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; ++i) out.emplace_back(g.gl_pathv[i]);
    }
    globfree(&g);
    return out;
}

bool ReadNumber(const std::string& path, double& v) {
    const std::string t = linuxproc::ReadText(path);
    char* end = nullptr;
    v = std::strtod(t.c_str(), &end);
    return end && end != t.c_str();
}

// A whole disk, not a partition or a virtual device.
bool WholeDisk(const std::string& name) {
    if (name.rfind("loop", 0) == 0 || name.rfind("ram", 0) == 0 || name.rfind("zram", 0) == 0) return false;
    return linuxproc::ReadText("/sys/block/" + name + "/dev").size() > 0;
}

std::string Configure(Signal& s) {
    const std::string c = utf8::FromWide(s.spec.counter);
    if (c == "linux:gpu") {
        s.source = Source::Gpu;
    } else if (c == "linux:disk_inflight") {
        s.source = Source::DiskInFlight;
    } else if (c == "linux:net_bytes") {
        s.source = Source::NetBytes;
    } else if (c.rfind("pressure:", 0) == 0) {
        s.source = Source::Pressure;
        s.arg = c.substr(9);
        if (s.arg != "cpu" && s.arg != "memory" && s.arg != "io") return "pressure must be cpu, memory or io";
        if (linuxproc::ReadText("/proc/pressure/" + s.arg).empty()) {
            return "this kernel does not report pressure stall information (/proc/pressure)";
        }
    } else if (c.rfind("file:/", 0) == 0) {
        s.source = Source::File;
        s.arg = c.substr(5);
    } else {
        return "unknown counter source (use pressure:cpu|memory|io or file:/path)";
    }
    return {};
}

bool Read(Signal& s, std::vector<CounterItem>& items, std::string& error) {
    switch (s.source) {
        case Source::Gpu: {
            for (const auto& p : Glob("/sys/class/drm/card*/device/gpu_busy_percent")) {
                double v;
                if (ReadNumber(p, v)) items.push_back({utf8::ToWide(p), v});
            }
            if (items.empty()) error = "no GPU here reports gpu_busy_percent";
            return !items.empty();
        }
        case Source::DiskInFlight: {
            // major minor name reads ... ; field 12 = I/Os currently in progress
            std::istringstream in(linuxproc::ReadText("/proc/diskstats"));
            for (std::string line; std::getline(in, line);) {
                std::istringstream f(line);
                std::string major, minor, name;
                f >> major >> minor >> name;
                unsigned long long x[9] = {};
                for (auto& v : x) f >> v;
                if (f && WholeDisk(name)) items.push_back({utf8::ToWide(name), static_cast<double>(x[8])});
            }
            if (items.empty()) error = "no disks found in /proc/diskstats";
            return !items.empty();
        }
        case Source::NetBytes: {
            const auto now = std::chrono::steady_clock::now();
            const double secs = std::chrono::duration<double>(now - s.lastAt).count();
            const bool first = s.lastBytes.empty();
            std::istringstream in(linuxproc::ReadText("/proc/net/dev"));
            std::string line;
            std::getline(in, line);
            std::getline(in, line);  // two header lines
            while (std::getline(in, line)) {
                const size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string name = line.substr(0, colon);
                name.erase(0, name.find_first_not_of(' '));
                if (name == "lo") continue;
                std::istringstream f(line.substr(colon + 1));
                unsigned long long v[9] = {};  // rx bytes ... (8 fields), tx bytes
                for (auto& x : v) f >> x;
                const unsigned long long total = v[0] + v[8];
                const auto it = s.lastBytes.find(name);
                if (!first && it != s.lastBytes.end() && total >= it->second && secs > 0) {
                    items.push_back({utf8::ToWide(name), static_cast<double>(total - it->second) / secs});
                }
                s.lastBytes[name] = total;
            }
            s.lastAt = now;
            if (first) {
                error = "counter has no data yet";
                return false;
            }
            return true;
        }
        case Source::Pressure: {
            // "some avg10=1.23 avg60=... total=..."
            const std::string t = linuxproc::ReadText("/proc/pressure/" + s.arg);
            const size_t at = t.find("avg10=");
            if (t.rfind("some", 0) != 0 || at == std::string::npos) {
                error = "cannot read /proc/pressure/" + s.arg;
                return false;
            }
            items.push_back({utf8::ToWide(s.arg), std::strtod(t.c_str() + at + 6, nullptr)});
            return true;
        }
        case Source::File: {
            const auto paths = s.arg.find_first_of("*?[") == std::string::npos ? std::vector<std::string>{s.arg}
                                                                                : Glob(s.arg);
            for (const auto& p : paths) {
                double v;
                if (ReadNumber(p, v)) items.push_back({utf8::ToWide(p), v});
            }
            if (items.empty()) error = "no readable number at " + s.arg;
            return !items.empty();
        }
    }
    return false;
}

}

bool BuiltInSignal(const std::string& type, SignalSpec& spec) {
    spec.instanceContains.clear();
    if (type == "gpu") {
        spec.counter = L"linux:gpu";
        spec.aggregate = Aggregate::Max;
        spec.scale = 1.0;
        return true;
    }
    if (type == "disk_queue") {
        spec.counter = L"linux:disk_inflight";
        spec.aggregate = Aggregate::Sum;
        spec.scale = 1.0;
        return true;
    }
    if (type == "network") {
        spec.counter = L"linux:net_bytes";
        spec.aggregate = Aggregate::Sum;
        spec.scale = 1.0 / (1024.0 * 1024.0);
        return true;
    }
    return false;
}

struct SignalSet::Impl {
    std::vector<std::unique_ptr<Signal>> signals;
};

SignalSet::SignalSet() : impl_(std::make_unique<Impl>()) {}
SignalSet::~SignalSet() = default;

bool SignalSet::Empty() const { return impl_->signals.empty(); }

void SignalSet::Configure(const std::vector<SignalSpec>& specs) {
    impl_->signals.clear();
    for (const auto& spec : specs) {
        auto s = std::make_unique<Signal>();
        s->spec = spec;
        s->openError = telemetry::Configure(*s);
        if (s->openError.empty() && s->source == Source::NetBytes) {
            std::vector<CounterItem> primed;
            std::string ignored;
            Read(*s, primed, ignored);  // first sample sets the baseline for the rate
        }
        impl_->signals.push_back(std::move(s));
    }
}

std::vector<SignalReading> SignalSet::Sample() {
    std::vector<SignalReading> out;
    for (auto& s : impl_->signals) {
        SignalReading r;
        r.name = s->spec.name;
        std::vector<CounterItem> items;
        if (!s->openError.empty()) {
            r.error = s->openError;
        } else if (Read(*s, items, r.error)) {
            double v = 0.0;
            CombineInstances(items, s->spec.aggregate, s->spec.instanceContains, v, r.instances);
            r.ok = true;
            r.value = v * s->spec.scale;
            r.percent = s->spec.limit > 0 ? 100.0 * r.value / s->spec.limit : 0.0;
        }
        out.push_back(r);
    }
    return out;
}

}
