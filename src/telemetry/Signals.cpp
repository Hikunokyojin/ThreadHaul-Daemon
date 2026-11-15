#include "Signals.h"

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>

namespace telemetry {

namespace {

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return std::towlower(c); });
    return s;
}

std::string PdhError(PDH_STATUS s) {
    switch (static_cast<DWORD>(s)) {
        case PDH_CSTATUS_NO_OBJECT:  return "counter object not found on this computer";
        case PDH_CSTATUS_NO_COUNTER: return "counter not found on this computer";
        case PDH_CSTATUS_BAD_COUNTERNAME: return "counter path is not valid";
        case PDH_NO_DATA:            return "counter has no data yet";
        case PDH_INVALID_DATA:       return "counter data is not valid yet";
        default: {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "PDH error 0x%08lX", static_cast<unsigned long>(s));
            return buf;
        }
    }
}

struct Signal {
    SignalSpec spec;
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    std::string openError;

    ~Signal() {
        if (query) PdhCloseQuery(query);
    }
};

}

bool BuiltInSignal(const std::string& type, SignalSpec& spec) {
    if (type == "gpu") {
        spec.counter = L"\\GPU Engine(*)\\Utilization Percentage";
        spec.aggregate = Aggregate::Sum;
        spec.instanceContains = L"engtype_3D";
        spec.scale = 1.0;
        return true;
    }
    if (type == "disk_queue") {
        spec.counter = L"\\PhysicalDisk(_Total)\\Current Disk Queue Length";
        spec.aggregate = Aggregate::Sum;
        spec.instanceContains.clear();
        spec.scale = 1.0;
        return true;
    }
    if (type == "network") {
        spec.counter = L"\\Network Interface(*)\\Bytes Total/sec";
        spec.aggregate = Aggregate::Sum;
        spec.instanceContains.clear();
        spec.scale = 1.0 / (1024.0 * 1024.0);
        return true;
    }
    return false;
}

bool CombineInstances(const std::vector<CounterItem>& items, Aggregate aggregate,
                      const std::wstring& instanceContains, double& out, int& used) {
    const std::wstring filter = Lower(instanceContains);
    double acc = 0.0;
    used = 0;
    for (const CounterItem& it : items) {
        if (!filter.empty() && Lower(it.instance).find(filter) == std::wstring::npos) continue;
        if (aggregate == Aggregate::Sum) {
            acc += it.value;
        } else {
            acc = used == 0 ? it.value : std::max(acc, it.value);
        }
        ++used;
    }
    out = acc;
    // A filter that matches nothing is still a valid reading (e.g. no GPU work: 0%).
    return true;
}

struct SignalSet::Impl {
    std::vector<std::unique_ptr<Signal>> signals;
};

SignalSet::SignalSet() : impl_(std::make_unique<Impl>()) {}
SignalSet::~SignalSet() = default;

bool SignalSet::Empty() const { return impl_->signals.empty(); }

void SignalSet::Configure(const std::vector<SignalSpec>& specs) {
    impl_->signals.clear();
    for (const SignalSpec& spec : specs) {
        auto s = std::make_unique<Signal>();
        s->spec = spec;
        PDH_STATUS st = PdhOpenQueryW(nullptr, 0, &s->query);
        if (st == ERROR_SUCCESS) {
            st = PdhAddEnglishCounterW(s->query, spec.counter.c_str(), 0, &s->counter);
        }
        if (st == ERROR_SUCCESS) {
            // Rate counters (per second, %) need two collections; prime the first.
            PdhCollectQueryData(s->query);
        } else {
            s->openError = PdhError(st);
        }
        impl_->signals.push_back(std::move(s));
    }
}

std::vector<SignalReading> SignalSet::Sample() {
    std::vector<SignalReading> out;
    for (auto& s : impl_->signals) {
        SignalReading r;
        r.name = s->spec.name;
        if (!s->openError.empty()) {
            r.error = s->openError;
            out.push_back(r);
            continue;
        }
        PDH_STATUS st = PdhCollectQueryData(s->query);
        DWORD bytes = 0, count = 0;
        if (st == ERROR_SUCCESS) {
            st = PdhGetFormattedCounterArrayW(s->counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes,
                                              &count, nullptr);
        }
        if (st != static_cast<PDH_STATUS>(PDH_MORE_DATA)) {
            r.error = st == ERROR_SUCCESS ? "counter returned no instances" : PdhError(st);
            out.push_back(r);
            continue;
        }
        std::vector<BYTE> buf(bytes);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        st = PdhGetFormattedCounterArrayW(s->counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count,
                                          items);
        if (st != ERROR_SUCCESS) {
            r.error = PdhError(st);
            out.push_back(r);
            continue;
        }
        std::vector<CounterItem> values;
        values.reserve(count);
        for (DWORD i = 0; i < count; ++i) {
            const DWORD cs = items[i].FmtValue.CStatus;
            if (cs != PDH_CSTATUS_VALID_DATA && cs != PDH_CSTATUS_NEW_DATA) continue;
            values.push_back({items[i].szName ? items[i].szName : L"", items[i].FmtValue.doubleValue});
        }
        double v = 0.0;
        CombineInstances(values, s->spec.aggregate, s->spec.instanceContains, v, r.instances);
        r.ok = true;
        r.value = v * s->spec.scale;
        r.percent = s->spec.limit > 0 ? 100.0 * r.value / s->spec.limit : 0.0;
        out.push_back(r);
    }
    return out;
}

}
