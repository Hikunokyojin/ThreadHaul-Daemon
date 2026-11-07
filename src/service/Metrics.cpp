#include "Metrics.h"

#include <cstdio>
#include <set>

namespace daemon {

namespace {

using json = nlohmann::json;

class Writer {
public:
    // Declares a metric family once; samples follow with Sample().
    void Family(const char* name, const char* type, const char* help) {
        out_ += "# HELP ";
        out_ += name;
        out_ += ' ';
        out_ += help;
        out_ += "\n# TYPE ";
        out_ += name;
        out_ += ' ';
        out_ += type;
        out_ += '\n';
    }

    void Sample(const char* name, const std::string& labels, double value) {
        char num[64];
        std::snprintf(num, sizeof(num), "%.15g", value);  // 71.3, not 71.299999999999997
        out_ += name;
        if (!labels.empty()) {
            out_ += '{';
            out_ += labels;
            out_ += '}';
        }
        out_ += ' ';
        out_ += num;
        out_ += '\n';
    }

    std::string Take() { return std::move(out_); }

private:
    std::string out_;
};

std::string Label(const char* key, const std::string& value) {
    return std::string(key) + "=\"" + PrometheusLabel(value) + "\"";
}

bool Num(const json& j, const char* key, double& out) {
    if (!j.is_object()) return false;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return false;
    out = it->get<double>();
    return true;
}

// A gauge from one numeric field, if present.
void Gauge(Writer& w, const json& obj, const char* field, const char* name, const char* help,
           double scale = 1.0) {
    double v;
    if (!Num(obj, field, v)) return;
    w.Family(name, "gauge", help);
    w.Sample(name, "", v * scale);
}

}

std::string PrometheusLabel(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

std::string PrometheusText(const json& status, const MetricCounters& counters) {
    Writer w;
    const double GiB = 1024.0 * 1024.0 * 1024.0;

    w.Family("threadhaul_up", "gauge", "1 while the service is running and answering.");
    w.Sample("threadhaul_up", "", 1);

    const json svc = status.value("service", json::object());
    Gauge(w, svc, "uptime_seconds", "threadhaul_uptime_seconds", "Seconds since the service started.");
    Gauge(w, svc, "working_set_mb", "threadhaul_service_working_set_bytes",
          "Memory used by the ThreadHaul service itself.", 1024.0 * 1024.0);

    Gauge(w, status, "pressure_percent", "threadhaul_pressure_percent",
          "Current pressure of the critical processes (highest signal), as a percentage.");
    Gauge(w, status, "offset_percent", "threadhaul_threshold_offset_percent",
          "How far adaptive thresholds or the controller bias have lowered the thresholds.");

    const json th = status.value("thresholds", json::object());
    if (th.is_object() && !th.empty()) {
        w.Family("threadhaul_threshold_percent", "gauge",
                 "Effective thresholds: high throttles, critical suspends, low recovers.");
        for (const char* level : {"high", "critical", "low"}) {
            double v;
            if (Num(th, level, v)) w.Sample("threadhaul_threshold_percent", Label("level", level), v);
        }
    }

    const json crit = status.value("critical", json::array());
    if (crit.is_array() && !crit.empty()) {
        w.Family("threadhaul_critical_process_cpu_percent", "gauge",
                 "CPU of each running critical process instance (per-core %).");
        for (const json& c : crit) {
            double v;
            if (!Num(c, "cpu_percent", v)) continue;
            w.Sample("threadhaul_critical_process_cpu_percent",
                     Label("name", c.value("name", "")) + "," +
                         Label("pid", std::to_string(c.value("pid", 0))),
                     v);
        }
    }

    // Batch processes: a count per state (always all three, so graphs don't gap)
    // and a per-process state-set.
    const json batch = status.value("batch", json::array());
    std::map<std::string, int> perState{{"normal", 0}, {"throttled", 0}, {"suspended", 0}};
    if (batch.is_array()) {
        for (const json& b : batch) perState[b.value("state", "normal")]++;
    }
    w.Family("threadhaul_batch_processes", "gauge", "Managed batch processes by state.");
    for (const auto& [state, n] : perState) {
        w.Sample("threadhaul_batch_processes", Label("state", state), n);
    }
    if (batch.is_array() && !batch.empty()) {
        w.Family("threadhaul_batch_process_state", "gauge",
                 "1 for the current state of each managed batch process.");
        for (const json& b : batch) {
            w.Sample("threadhaul_batch_process_state",
                     Label("name", b.value("name", "")) + "," +
                         Label("pid", std::to_string(b.value("pid", 0))) + "," +
                         Label("state", b.value("state", "")) + "," +
                         Label("manual_hold", b.value("manual_hold", false) ? "true" : "false"),
                     1);
        }
    }

    const json groups = status.value("groups", json::array());
    if (groups.is_array() && !groups.empty()) {
        w.Family("threadhaul_group_pressure_percent", "gauge", "Pressure of each policy group's critical processes.");
        for (const json& g : groups) {
            double v;
            if (Num(g, "pressure_percent", v)) {
                w.Sample("threadhaul_group_pressure_percent", Label("group", g.value("name", "")), v);
            }
        }
        w.Family("threadhaul_group_threshold_percent", "gauge", "Effective thresholds of each policy group.");
        for (const json& g : groups) {
            const json th = g.value("thresholds", json::object());
            for (const char* level : {"high", "critical", "low"}) {
                double v;
                if (Num(th, level, v)) {
                    w.Sample("threadhaul_group_threshold_percent",
                             Label("group", g.value("name", "")) + "," + Label("level", level), v);
                }
            }
        }
        w.Family("threadhaul_group_active", "gauge", "1 when the group's critical processes had a valid sample on the last poll.");
        for (const json& g : groups) {
            w.Sample("threadhaul_group_active", Label("group", g.value("name", "")), g.value("active", false) ? 1 : 0);
        }
    }

    const json signals = status.value("signals", json::array());
    if (signals.is_array() && !signals.empty()) {
        w.Family("threadhaul_signal_value", "gauge", "Current value of each configured performance-counter signal.");
        for (const json& s : signals) {
            double v;
            if (Num(s, "value", v)) w.Sample("threadhaul_signal_value", Label("signal", s.value("name", "")), v);
        }
        w.Family("threadhaul_signal_pressure_percent", "gauge", "Each signal's value as a percentage of its limit.");
        for (const json& s : signals) {
            double v;
            if (Num(s, "pressure_percent", v)) {
                w.Sample("threadhaul_signal_pressure_percent", Label("signal", s.value("name", "")), v);
            }
        }
        w.Family("threadhaul_signal_up", "gauge", "1 when the signal's counter could be read on the last poll.");
        for (const json& s : signals) {
            w.Sample("threadhaul_signal_up", Label("signal", s.value("name", "")), s.value("ok", false) ? 1 : 0);
        }
    }

    if (status.contains("coordination_connected") && status["coordination_connected"].is_boolean()) {
        w.Family("threadhaul_coordination_connected", "gauge",
                 "1 when connected to the central controller.");
        w.Sample("threadhaul_coordination_connected", "", status["coordination_connected"].get<bool>() ? 1 : 0);
    }

    const json sys = status.value("system", json::object());
    Gauge(w, sys, "cpu_percent", "threadhaul_system_cpu_percent", "Whole-machine CPU busy, 0-100.");
    Gauge(w, sys, "memory_load_percent", "threadhaul_system_memory_load_percent",
          "Physical memory in use, 0-100.");
    Gauge(w, sys, "memory_used_gb", "threadhaul_system_memory_used_bytes", "Physical memory in use.", GiB);
    Gauge(w, sys, "memory_total_gb", "threadhaul_system_memory_total_bytes", "Installed physical memory.", GiB);
    Gauge(w, sys, "logical_cores", "threadhaul_system_logical_cores", "Logical processors.");

    const json ap = status.value("auto_protect", json::object());
    if (ap.is_object() && !ap.empty()) {
        w.Family("threadhaul_auto_protect_enabled", "gauge", "1 when automatic CPU protection is on.");
        w.Sample("threadhaul_auto_protect_enabled", "", ap.value("enabled", false) ? 1 : 0);
        w.Family("threadhaul_auto_protect_level", "gauge", "1 for the current auto-protect level.");
        const std::string level = ap.value("level", "");
        for (const char* l : {"normal", "throttling", "idle"}) {
            w.Sample("threadhaul_auto_protect_level", Label("level", l), level == l ? 1 : 0);
        }
        const json procs = ap.value("processes", json::array());
        w.Family("threadhaul_auto_protect_processes", "gauge",
                 "Programs auto-protect has currently lowered.");
        w.Sample("threadhaul_auto_protect_processes", "", procs.is_array() ? static_cast<double>(procs.size()) : 0);
    }

    w.Family("threadhaul_actions_total", "counter",
             "Batch process state changes since the service started, by action and trigger.");
    for (const auto& [key, n] : counters.actions) {
        w.Sample("threadhaul_actions_total", Label("action", key.first) + "," + Label("trigger", key.second),
                 static_cast<double>(n));
    }

    w.Family("threadhaul_log_events_total", "counter",
             "Log entries written since the service started, by log_type.");
    for (const auto& [type, n] : counters.logEvents) {
        w.Sample("threadhaul_log_events_total", Label("log_type", type), static_cast<double>(n));
    }

    return w.Take();
}

}
