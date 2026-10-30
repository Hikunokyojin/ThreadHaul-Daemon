#include "EventLogWriter.h"
#include "JsonLinesLogger.h"

#include <cstring>
#include <cwchar>

namespace logging {

namespace {

constexpr wchar_t kEventSource[] = L"ThreadHaulDaemon";

DWORD EventIdFor(actuator::ProcessState to) {
    switch (to) {
        case actuator::ProcessState::Throttled: return 1000;
        case actuator::ProcessState::Suspended: return 1001;
        case actuator::ProcessState::Normal:    return 1002;
    }
    return 1099;
}

std::wstring Widen(const char* ascii) {
    return std::wstring(ascii, ascii + std::strlen(ascii));
}

}

EventLogWriter::EventLogWriter() {
    source_ = RegisterEventSourceW(nullptr, kEventSource);
}

EventLogWriter::~EventLogWriter() {
    if (source_ != nullptr) {
        DeregisterEventSource(source_);
    }
}

bool EventLogWriter::ReportMessage(WORD type, DWORD eventId, const std::wstring& message) {
    if (source_ == nullptr) {
        return false;
    }
    LPCWSTR strings[] = {message.c_str()};
    return ReportEventW(source_, type, 0, eventId, nullptr, 1, 0, strings, nullptr) != FALSE;
}

bool EventLogWriter::ReportActuatorAction(uint32_t batchProcessId,
                                          const std::wstring& batchProcessName,
                                          uint32_t criticalProcessId,
                                          actuator::ProcessState from,
                                          actuator::ProcessState to, double cpuPercent,
                                          bool actuationSucceeded) {
    if (source_ == nullptr) {
        return false;
    }

    wchar_t details[256];
    std::swprintf(details, sizeof(details) / sizeof(details[0]),
                  L" (PID %u): %ls -> %ls (%ls). Critical process PID %u at %.1f%% CPU.",
                  batchProcessId, Widen(StateName(from)).c_str(), Widen(StateName(to)).c_str(),
                  Widen(ActionName(to)).c_str(), criticalProcessId, cpuPercent);

    std::wstring message = batchProcessName + details;
    if (!actuationSucceeded) {
        message += L" The OS call did not fully take effect.";
    }

    const WORD type = (to == actuator::ProcessState::Suspended || !actuationSucceeded)
                          ? EVENTLOG_WARNING_TYPE
                          : EVENTLOG_INFORMATION_TYPE;
    LPCWSTR strings[] = {message.c_str()};
    return ReportEventW(source_, type, 0, EventIdFor(to), nullptr, 1, 0, strings, nullptr) != FALSE;
}

}
