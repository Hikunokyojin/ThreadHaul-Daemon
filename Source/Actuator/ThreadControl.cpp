#include "ThreadControl.h"
#include <tlhelp32.h>

namespace actuator {

namespace {
std::vector<DWORD> EnumerateThreadIds(DWORD processId) {
    std::vector<DWORD> threadIds;

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return threadIds;
    }

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(THREADENTRY32);

    if (Thread32First(hSnapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == processId) {
                threadIds.push_back(entry.th32ThreadID);
            }
        } while (Thread32Next(hSnapshot, &entry));
    }

    CloseHandle(hSnapshot);
    return threadIds;
}

}

ThreadOpResult SuspendAllThreads(DWORD processId) {
    ThreadOpResult result;
    const std::vector<DWORD> threadIds = EnumerateThreadIds(processId);

    for (DWORD tid : threadIds) {
        result.threadsAttempted++;

        HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, tid);
        if (hThread == nullptr) {
            result.threadsFailed++;
            continue;
        }

        const DWORD suspendCount = SuspendThread(hThread);
        CloseHandle(hThread);

        if (suspendCount == static_cast<DWORD>(-1)) {
            result.threadsFailed++;
        } else {
            result.threadsSucceeded++;
        }
    }

    return result;
}

ThreadOpResult ResumeAllThreads(DWORD processId) {
    ThreadOpResult result;
    const std::vector<DWORD> threadIds = EnumerateThreadIds(processId);

    for (DWORD tid : threadIds) {
        result.threadsAttempted++;

        HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, tid);
        if (hThread == nullptr) {
            result.threadsFailed++;
            continue;
        }

        const DWORD suspendCount = ResumeThread(hThread);
        CloseHandle(hThread);

        if (suspendCount == static_cast<DWORD>(-1)) {
            result.threadsFailed++;
        } else {
            result.threadsSucceeded++;
        }
    }

    return result;
}

}
