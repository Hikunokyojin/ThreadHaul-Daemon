#pragma once

// The basic Win32 calls the service layer uses everywhere (ticks, process
// id, sleep, local time, stop events). Windows gets <windows.h>; Linux gets
// small equivalents with the same meaning, so the service logic is shared.
// Anything platform-specific beyond these is #ifdef'd at the call site.

#include "PlatformTypes.h"

#ifndef _WIN32
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

using ULONGLONG = unsigned long long;

constexpr BOOL TRUE = 1;
constexpr BOOL FALSE = 0;

constexpr DWORD MAX_PATH = 4096;
constexpr DWORD INFINITE = 0xFFFFFFFF;
constexpr DWORD WAIT_OBJECT_0 = 0;
constexpr DWORD WAIT_TIMEOUT = 258;
constexpr DWORD WAIT_FAILED = 0xFFFFFFFF;

inline ULONGLONG GetTickCount64() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<ULONGLONG>(ts.tv_sec) * 1000ull + static_cast<ULONGLONG>(ts.tv_nsec) / 1000000ull;
}

inline DWORD GetCurrentProcessId() { return static_cast<DWORD>(getpid()); }

inline void Sleep(DWORD ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

inline DWORD GetLastError() { return static_cast<DWORD>(errno); }

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

// Machine-wide CPU times from /proc/stat in 100 ns units, with Windows'
// meaning: kernel time includes idle time.
inline BOOL GetSystemTimes(FILETIME* idle, FILETIME* kernel, FILETIME* user) {
    FILE* f = std::fopen("/proc/stat", "r");
    if (!f) return 0;
    unsigned long long v[8] = {};
    const int n = std::fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3],
                              &v[4], &v[5], &v[6], &v[7]);
    std::fclose(f);
    if (n < 4) return 0;
    const long hz = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
    const unsigned long long scale = 10000000ull / static_cast<unsigned long long>(hz);
    auto put = [scale](FILETIME* ft, unsigned long long ticks) {
        const unsigned long long t = ticks * scale;
        ft->dwLowDateTime = static_cast<DWORD>(t);
        ft->dwHighDateTime = static_cast<DWORD>(t >> 32);
    };
    const unsigned long long idleTicks = v[3] + v[4];               // idle + iowait
    const unsigned long long sysTicks = v[2] + v[5] + v[6] + v[7];  // system + irq + softirq + steal
    put(idle, idleTicks);
    put(kernel, sysTicks + idleTicks);
    put(user, v[0] + v[1]);
    return 1;
}

struct SYSTEMTIME {
    uint16_t wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
};

namespace servicecompat {
inline void Fill(SYSTEMTIME* st, bool local) {
    timeval tv{};
    gettimeofday(&tv, nullptr);
    tm t{};
    if (local) localtime_r(&tv.tv_sec, &t);
    else gmtime_r(&tv.tv_sec, &t);
    st->wYear = static_cast<uint16_t>(t.tm_year + 1900);
    st->wMonth = static_cast<uint16_t>(t.tm_mon + 1);
    st->wDayOfWeek = static_cast<uint16_t>(t.tm_wday);  // 0 = Sunday, as on Windows
    st->wDay = static_cast<uint16_t>(t.tm_mday);
    st->wHour = static_cast<uint16_t>(t.tm_hour);
    st->wMinute = static_cast<uint16_t>(t.tm_min);
    st->wSecond = static_cast<uint16_t>(t.tm_sec);
    st->wMilliseconds = static_cast<uint16_t>(tv.tv_usec / 1000);
}

// A Win32-style event: manual or auto reset, waitable with a timeout.
struct Event {
    std::mutex m;
    std::condition_variable cv;
    bool manual = true;
    bool signaled = false;
};
}

inline void GetLocalTime(SYSTEMTIME* st) { servicecompat::Fill(st, true); }
inline void GetSystemTime(SYSTEMTIME* st) { servicecompat::Fill(st, false); }

inline HANDLE CreateEventW(void*, BOOL manualReset, BOOL initialState, const wchar_t*) {
    auto* e = new servicecompat::Event;
    e->manual = manualReset != 0;
    e->signaled = initialState != 0;
    return e;
}

inline BOOL SetEvent(HANDLE h) {
    auto* e = static_cast<servicecompat::Event*>(h);
    if (!e) return 0;
    {
        std::lock_guard<std::mutex> lock(e->m);
        e->signaled = true;
    }
    e->cv.notify_all();
    return 1;
}

inline BOOL ResetEvent(HANDLE h) {
    auto* e = static_cast<servicecompat::Event*>(h);
    if (!e) return 0;
    std::lock_guard<std::mutex> lock(e->m);
    e->signaled = false;
    return 1;
}

inline DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    auto* e = static_cast<servicecompat::Event*>(h);
    if (!e) return WAIT_FAILED;
    std::unique_lock<std::mutex> lock(e->m);
    auto ready = [e] { return e->signaled; };
    if (ms == INFINITE) {
        e->cv.wait(lock, ready);
    } else if (!e->cv.wait_for(lock, std::chrono::milliseconds(ms), ready)) {
        return WAIT_TIMEOUT;
    }
    if (!e->manual) e->signaled = false;
    return WAIT_OBJECT_0;
}

// Only events are created through this header, so CloseHandle frees one.
inline BOOL CloseHandle(HANDLE h) {
    delete static_cast<servicecompat::Event*>(h);
    return 1;
}
#endif
