#include "JsonLinesLogger.h"
#include "LogChain.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <ctime>
#endif

#include "Utf8.h"

namespace logging {

namespace {

using ordered_json = nlohmann::ordered_json;

std::string Timestamp() {
#ifndef _WIN32
    timeval tv{};
    gettimeofday(&tv, nullptr);
    tm t{};
    gmtime_r(&tv.tv_sec, &t);
    char out[64];
    std::snprintf(out, sizeof(out), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", t.tm_year + 1900, t.tm_mon + 1,
                  t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, static_cast<int>(tv.tv_usec / 1000));
    return out;
#else
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  st.wMilliseconds);
    return buf;
#endif
}

std::string WideToUtf8(const std::wstring& w) { return utf8::FromWide(w); }

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

#ifdef _WIN32
JsonLinesLogger::~JsonLinesLogger() {
    if (file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
    }
}
#else
JsonLinesLogger::~JsonLinesLogger() {
    if (file_ >= 0) close(file_);
}
#endif

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
    if (!EnsureOpen()) {
        return false;
    }
    const std::string chained = ChainLine(line, lastHash_);
    const std::string data = chained + "\n";
    if (currentSize_ > 0 && currentSize_ + data.size() > maxFileBytes_) {
        Rotate();
        if (!EnsureOpen()) {
            return false;
        }
    }

#ifndef _WIN32
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t w = write(file_, data.data() + done, data.size() - done);
        if (w <= 0) return false;
        done += static_cast<size_t>(w);
    }
    const size_t written = done;
#else
    DWORD written = 0;
    if (!WriteFile(file_, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) ||
        written != data.size()) {
        return false;
    }
#endif
    currentSize_ += written;
    lastHash_ = LineHash(chained);
    return true;
}

bool JsonLinesLogger::EnsureOpen() {
#ifdef _WIN32
    if (file_ != INVALID_HANDLE_VALUE) {
#else
    if (file_ >= 0) {
#endif
        return true;
    }
    if (lastHash_.empty()) {
        // Continue the chain from whatever is already in the file.
        lastHash_ = LastLineHash(path_);
        if (lastHash_.empty()) lastHash_ = kGenesisHash;
    }
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(path_).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
#ifndef _WIN32
    file_ = open(utf8::FromWide(path_).c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0640);
    if (file_ < 0) return false;
    struct stat sb{};
    currentSize_ = fstat(file_, &sb) == 0 ? static_cast<uint64_t>(sb.st_size) : 0;
    return true;
#else
    file_ = CreateFileW(path_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    currentSize_ = GetFileSizeEx(file_, &size) ? static_cast<uint64_t>(size.QuadPart) : 0;
    return true;
#endif
}

void JsonLinesLogger::Rotate() {
#ifdef _WIN32
    CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE;
#else
    close(file_);
    file_ = -1;
#endif
    currentSize_ = 0;

    // std::filesystem::rename replaces an existing target on both platforms.
    namespace fs = std::filesystem;
    std::error_code ec;
    if (maxRotatedFiles_ <= 0) {
        fs::remove(fs::path(path_), ec);
        return;
    }
    fs::remove(fs::path(RotatedName(path_, maxRotatedFiles_)), ec);
    for (int i = maxRotatedFiles_ - 1; i >= 1; --i) {
        fs::rename(fs::path(RotatedName(path_, i)), fs::path(RotatedName(path_, i + 1)), ec);
    }
    fs::rename(fs::path(path_), fs::path(RotatedName(path_, 1)), ec);
}

}
