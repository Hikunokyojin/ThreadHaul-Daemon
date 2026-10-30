// batch_simulator: a CPU-hungry stand-in for invoice generation / inventory
// sync. Spins `--threads` threads flat out and prints a progress line every
// second, so a throttle (progress slows when the critical app is busy) and a
// suspension (progress stops completely) are visible in its console.
//
//   batch_simulator.exe                 one thread per two logical cores
//   batch_simulator.exe --threads 4
//   batch_simulator.exe --name invoice  label shown in its output

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<unsigned long long> g_work{0};

void Worker() {
    unsigned long long local = 0;
    while (true) {
        for (int i = 0; i < 100000; ++i) local += i ^ (local >> 3);
        g_work.fetch_add(1, std::memory_order_relaxed);
    }
}

const char* PriorityName(DWORD p) {
    switch (p) {
        case IDLE_PRIORITY_CLASS:         return "IDLE";
        case BELOW_NORMAL_PRIORITY_CLASS: return "BELOW_NORMAL";
        case NORMAL_PRIORITY_CLASS:       return "NORMAL";
        case ABOVE_NORMAL_PRIORITY_CLASS: return "ABOVE_NORMAL";
        case HIGH_PRIORITY_CLASS:         return "HIGH";
        case REALTIME_PRIORITY_CLASS:     return "REALTIME";
    }
    return "?";
}

}

int main(int argc, char* argv[]) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int threads = std::max(1, static_cast<int>(si.dwNumberOfProcessors) / 2);
    std::string name = "batch";
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--threads") threads = std::atoi(argv[++i]);
        else if (a == "--name") name = argv[++i];
    }
    std::printf("batch_simulator '%s' PID %lu, %d worker thread(s)\n", name.c_str(),
                GetCurrentProcessId(), threads);
    for (int i = 0; i < threads; ++i) std::thread(Worker).detach();

    // The reporter is also suspended when the daemon suspends this process,
    // so a pause in output, followed by a large catch-up gap, is the signal.
    unsigned long long last = 0;
    ULONGLONG lastTick = GetTickCount64();
    while (true) {
        Sleep(1000);
        const unsigned long long now = g_work.load();
        const ULONGLONG tick = GetTickCount64();
        const double secs = (tick - lastTick) / 1000.0;
        std::printf("[%s] %8.0f units/s  priority %-12s%s\n", name.c_str(), (now - last) / secs,
                    PriorityName(GetPriorityClass(GetCurrentProcess())),
                    secs > 2.0 ? "  <- resumed after a pause" : "");
        std::fflush(stdout);
        last = now;
        lastTick = tick;
    }
}
