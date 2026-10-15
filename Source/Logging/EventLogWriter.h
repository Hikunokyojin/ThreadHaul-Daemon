#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

#include "StateMachine.h"

namespace logging {

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

private:
    HANDLE source_ = nullptr;
};

}
