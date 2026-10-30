#pragma once

#include <windows.h>
#include <cstdint>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "StateMachine.h"
#include "ThresholdEvent.h"

namespace logging {

// "priority_lowered", "threads_suspended" or "recovered".
const char* ActionName(actuator::ProcessState to);
const char* StateName(actuator::ProcessState state);

// Thread-safe JSON Lines writer (log schema: README.md, Logs and monitoring).
// Each public method builds one complete line, so callers cannot
// produce an entry with a missing log_type/timestamp or a misnamed field.
// Rotation is size-based: when the next line would push the file past
// maxFileBytes, the file is renamed to <path>.1 (older ones shift to .2, ...)
// and anything beyond maxRotatedFiles is deleted.
class JsonLinesLogger {
public:
    JsonLinesLogger(std::wstring path, uint64_t maxFileBytes, int maxRotatedFiles);
    ~JsonLinesLogger();
    JsonLinesLogger(const JsonLinesLogger&) = delete;
    JsonLinesLogger& operator=(const JsonLinesLogger&) = delete;

    bool LogSample(uint32_t processId, double cpuPercent);

    bool LogThresholdEvent(uint32_t processId, actuator::ThresholdType type,
                           int consecutiveSamples, double cpuPercent);

    bool LogActuatorAction(uint32_t batchProcessId, const std::wstring& batchProcessName,
                           uint32_t criticalProcessId, actuator::ProcessState from,
                           actuator::ProcessState to, double cpuPercent,
                           bool actuationSucceeded, const char* trigger = "automatic");

    // Any other log kind (self_monitor, config_reload, lifecycle, alert, ...).
    // log_type and timestamp are added first; fields follow in insertion order.
    bool LogEvent(const char* logType, const nlohmann::ordered_json& fields);

private:
    bool WriteLine(const std::string& line);
    bool EnsureOpen();
    void Rotate();

    std::wstring path_;
    uint64_t maxFileBytes_;
    int maxRotatedFiles_;

    std::mutex mutex_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    uint64_t currentSize_ = 0;
};

}
