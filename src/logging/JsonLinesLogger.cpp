#include "JsonLinesLogger.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>

namespace logging {

namespace {

using ordered_json = nlohmann::ordered_json;

std::string Timestamp() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  st.wMilliseconds);
    return buf;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n,
                        nullptr, nullptr);
    return s;
}

const char* ThresholdName(actuator::ThresholdType type) {
    switch (type) {
        case actuator::ThresholdType::High:     return "high";
        case actuator::ThresholdType::Critical: return "critical";
        case actuator::ThresholdType::Low:      return "low";
    }
    return "unknown";
}

ordered_json Entry(const char* logType) {
    ordered_json j;
    j["log_type"] = logType;
    j["timestamp"] = Timestamp();
    return j;
}

std::string Serialize(const ordered_json& j) {
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::wstring RotatedName(const std::wstring& path, int index) {
    return path + L"." + std::to_wstring(index);
}

}

const char* StateName(actuator::ProcessState state) {
    switch (state) {
        case actuator::ProcessState::Normal:    return "normal";
        case actuator::ProcessState::Throttled: return "throttled";
        case actuator::ProcessState::Suspended: return "suspended";
    }
    return "unknown";
}

const char* ActionName(actuator::ProcessState to) {
    switch (to) {
        case actuator::ProcessState::Throttled: return "priority_lowered";
        case actuator::ProcessState::Suspended: return "threads_suspended";
        case actuator::ProcessState::Normal:    return "recovered";
    }
    return "unknown";
}

JsonLinesLogger::JsonLinesLogger(std::wstring path, uint64_t maxFileBytes, int maxRotatedFiles)
    : path_(std::move(path)), maxFileBytes_(maxFileBytes), maxRotatedFiles_(maxRotatedFiles) {}

JsonLinesLogger::~JsonLinesLogger() {
    if (file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
    }
}

bool JsonLinesLogger::LogSample(uint32_t processId, double cpuPercent) {
    ordered_json j = Entry("sample");
    j["process_id"] = processId;
    j["cpu_percent"] = cpuPercent;
    return WriteLine(Serialize(j));
}

bool JsonLinesLogger::LogThresholdEvent(uint32_t processId, actuator::ThresholdType type,
                                        int consecutiveSamples, double cpuPercent) {
    ordered_json j = Entry("threshold_event");
    j["process_id"] = processId;
    j["threshold"] = ThresholdName(type);
    j["consecutive_samples"] = consecutiveSamples;
    j["cpu_percent"] = cpuPercent;
    return WriteLine(Serialize(j));
}

bool JsonLinesLogger::LogActuatorAction(uint32_t batchProcessId,
                                        const std::wstring& batchProcessName,
                                        uint32_t criticalProcessId,
                                        actuator::ProcessState from,
                                        actuator::ProcessState to, double cpuPercent,
                                        bool actuationSucceeded, const char* trigger) {
    ordered_json j = Entry("actuator_action");
    j["batch_process_id"] = batchProcessId;
    j["batch_process_name"] = WideToUtf8(batchProcessName);
    j["critical_process_id"] = criticalProcessId;
    j["action"] = ActionName(to);
    j["from_state"] = StateName(from);
    j["to_state"] = StateName(to);
    j["cpu_percent"] = cpuPercent;
    j["actuation_succeeded"] = actuationSucceeded;
    j["trigger"] = trigger;
    return WriteLine(Serialize(j));
}

bool JsonLinesLogger::LogEvent(const char* logType, const ordered_json& fields) {
    ordered_json j = Entry(logType);
    for (const auto& [key, value] : fields.items()) {
        if (key != "log_type" && key != "timestamp") {
            j[key] = value;
        }
    }
    return WriteLine(Serialize(j));
}

bool JsonLinesLogger::WriteLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string data = line + "\n";

    if (!EnsureOpen()) {
        return false;
    }
    if (currentSize_ > 0 && currentSize_ + data.size() > maxFileBytes_) {
        Rotate();
        if (!EnsureOpen()) {
            return false;
        }
    }

    DWORD written = 0;
    if (!WriteFile(file_, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) ||
        written != data.size()) {
        return false;
    }
    currentSize_ += written;
    return true;
}

bool JsonLinesLogger::EnsureOpen() {
    if (file_ != INVALID_HANDLE_VALUE) {
        return true;
    }
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(path_).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    file_ = CreateFileW(path_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    currentSize_ = GetFileSizeEx(file_, &size) ? static_cast<uint64_t>(size.QuadPart) : 0;
    return true;
}

void JsonLinesLogger::Rotate() {
    CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE;
    currentSize_ = 0;

    if (maxRotatedFiles_ <= 0) {
        DeleteFileW(path_.c_str());
        return;
    }
    DeleteFileW(RotatedName(path_, maxRotatedFiles_).c_str());
    for (int i = maxRotatedFiles_ - 1; i >= 1; --i) {
        MoveFileExW(RotatedName(path_, i).c_str(), RotatedName(path_, i + 1).c_str(),
                    MOVEFILE_REPLACE_EXISTING);
    }
    MoveFileExW(path_.c_str(), RotatedName(path_, 1).c_str(), MOVEFILE_REPLACE_EXISTING);
}

}
