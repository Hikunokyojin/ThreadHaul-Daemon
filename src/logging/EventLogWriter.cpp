#include "EventLogWriter.h"
#include "JsonLinesLogger.h"

#include <cstring>
#include <cwchar>

#ifndef _WIN32
#include <syslog.h>

#include "Utf8.h"
#endif

namespace logging {

namespace {

#ifdef _WIN32
constexpr wchar_t kEventSource[] = L"ThreadHaulDaemon";
#endif

DWORD EventIdFor(actuator::ProcessState to) {
    switch (to) {
        case actuator::ProcessState::Throttled: return 1000;
        case actuator::ProcessState::Suspended: return 1001;
        case actuator::ProcessState::Normal:    return 1002;
    }
    return 1099;
}

bool Emit(HANDLE source, WORD type, DWORD eventId, const std::wstring& message) {
#ifdef _WIN32
    LPCWSTR strings[] = {message.c_str()};
    return ReportEventW(source, type, 0, eventId, nullptr, 1, 0, strings, nullptr) != FALSE;
#else
    (void)source;
    const int priority = type == EVENTLOG_ERROR_TYPE     ? LOG_ERR
                         : type == EVENTLOG_WARNING_TYPE ? LOG_WARNING
                                                         : LOG_INFO;
    syslog(priority, "[event %u] %s", static_cast<unsigned>(eventId), utf8::FromWide(message).c_str());
    return true;
#endif
}

std::wstring Widen(const char* ascii) {
    return std::wstring(ascii, ascii + std::strlen(ascii));
}

}

EventLogWriter::EventLogWriter() {
#ifdef _WIN32
    source_ = RegisterEventSourceW(nullptr, kEventSource);
#else
    openlog("threadhauld", LOG_PID | LOG_NDELAY, LOG_DAEMON);
    source_ = reinterpret_cast<HANDLE>(1);  // any non-null value: logging is available
#endif
}

EventLogWriter::~EventLogWriter() {
#ifdef _WIN32
    if (source_ != nullptr) {
        DeregisterEventSource(source_);
    }
#else
    closelog();
#endif
}

bool EventLogWriter::ReportMessage(WORD type, DWORD eventId, const std::wstring& message) {
    if (source_ == nullptr) {
        return false;
    }
    return Emit(source_, type, eventId, message);
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
    return Emit(source_, type, EventIdFor(to), message);
}

}
