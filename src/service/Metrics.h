#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace daemon {

// Cumulative counters since the service started, for Prometheus.
struct MetricCounters {
    std::map<std::string, uint64_t> logEvents;                          // log_type -> count
    std::map<std::pair<std::string, std::string>, uint64_t> actions;    // (action, trigger) -> count
};

// Renders the Prometheus text exposition format (version 0.0.4) from the
// dashboard status JSON (Daemon::DashboardStatus) and the counters.
// Missing status fields are skipped, never invented.
std::string PrometheusText(const nlohmann::json& status, const MetricCounters& counters);

// Escapes a label value: backslash, double quote and newline.
std::string PrometheusLabel(const std::string& value);

constexpr const char* kPrometheusContentType = "text/plain; version=0.0.4; charset=utf-8";

}
