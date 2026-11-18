#pragma once

#include "PlatformTypes.h"

#ifndef _WIN32
using WORD = unsigned short;
constexpr WORD EVENTLOG_ERROR_TYPE = 0x0001;
constexpr WORD EVENTLOG_WARNING_TYPE = 0x0002;
constexpr WORD EVENTLOG_INFORMATION_TYPE = 0x0004;
#endif
#include <cstdint>
#include <string>

#include "StateMachine.h"

namespace logging {

// Linux: the same entries go to syslog (and so to journald) as
// "threadhauld", with the event ID in the text and the matching severity.
//
// Writes actuator state changes to the Windows Application event log under
// the source "ThreadHaulDaemon". Event IDs: 1000 throttled, 1001 suspended,
// 1002 recovered. Suspensions and failed actuations are logged as warnings.
//
// Until the installer registers the event source in the registry, Event
// Viewer still records each entry but prefixes it with a "description could
// not be found" note; the message text is preserved.
class EventLogWriter {
public:
    EventLogWriter();
    ~EventLogWriter();
    EventLogWriter(const EventLogWriter&) = delete;
    EventLogWriter& operator=(const EventLogWriter&) = delete;

    bool ReportActuatorAction(uint32_t batchProcessId, const std::wstring& batchProcessName,
                              uint32_t criticalProcessId, actuator::ProcessState from,
                              actuator::ProcessState to, double cpuPercent,
                              bool actuationSucceeded);

    // Free-text entry (used by auto-protect: 1100 throttled, 1101 lowered to
    // idle, 1102 restored).
    bool ReportMessage(WORD type, DWORD eventId, const std::wstring& message);

private:
    HANDLE source_ = nullptr;
};

}
