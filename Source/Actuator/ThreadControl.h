#pragma once

#include <windows.h>
#include <vector>

namespace actuator {

struct ThreadOpResult {
    int threadsAttempted = 0;
    int threadsSucceeded = 0;
    int threadsFailed = 0;

    bool AllSucceeded() const {
        return threadsAttempted > 0 && threadsFailed == 0;
    }
};
ThreadOpResult SuspendAllThreads(DWORD processId);
ThreadOpResult ResumeAllThreads(DWORD processId);

}
