// critical_simulator: a CPU-hungry stand-in for a real-time dispatch API.
//
// Runs a load schedule of phases "<seconds>:<percent>" with `--threads`
// worker threads, each busy for <percent>% of every 100 ms slice. With the
// daemon's default per-core normalisation, one thread at 100% reads ~100%.
//
//   critical_simulator.exe                            default demo cycle, repeats
//   critical_simulator.exe --schedule 10:30,20:100,15:20 --threads 1
//   critical_simulator.exe --load 95                  constant load until Ctrl+C
//   critical_simulator.exe --once                     run the schedule once and exit
//   critical_simulator.exe --crash-after 20           exit with code 3 after 20 s

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<int> g_load{0};
std::atomic<bool> g_run{true};

void Worker() {
    volatile unsigned long long sink = 0;
    while (g_run) {
        const ULONGLONG start = GetTickCount64();
        const int busy = g_load.load();
        while (GetTickCount64() - start < static_cast<ULONGLONG>(busy)) {
            for (int i = 0; i < 10000; ++i) sink += i;
        }
        const ULONGLONG used = GetTickCount64() - start;
        if (used < 100) Sleep(static_cast<DWORD>(100 - used));
    }
}

struct Phase { int seconds; int percent; };

std::vector<Phase> ParseSchedule(const std::string& s) {
    std::vector<Phase> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t comma = s.find(',', start);
        if (comma == std::string::npos) comma = s.size();
        const std::string item = s.substr(start, comma - start);
        const size_t colon = item.find(':');
        if (colon != std::string::npos) {
            out.push_back({std::atoi(item.substr(0, colon).c_str()),
                           std::atoi(item.substr(colon + 1).c_str())});
        }
        start = comma + 1;
    }
    return out;
}

}

int main(int argc, char* argv[]) {
    // Default cycle: calm, then starved long enough to throttle (3 s) and
    // escalate (5 more s), then calm long enough to recover (5 s).
    std::string schedule = "8:30,20:100,15:20";
    int threads = 1;
    bool once = false;
    int crashAfter = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--schedule" && i + 1 < argc) schedule = argv[++i];
        else if (a == "--load" && i + 1 < argc) schedule = "86400:" + std::string(argv[++i]);
        else if (a == "--threads" && i + 1 < argc) threads = std::atoi(argv[++i]);
        else if (a == "--once") once = true;
        else if (a == "--crash-after" && i + 1 < argc) crashAfter = std::atoi(argv[++i]);
    }
    const std::vector<Phase> phases = ParseSchedule(schedule);
    if (phases.empty() || threads < 1) {
        std::fprintf(stderr, "invalid --schedule or --threads\n");
        return 1;
    }

    std::printf("critical_simulator PID %lu, %d thread(s), schedule %s%s\n", GetCurrentProcessId(),
                threads, schedule.c_str(), once ? " (once)" : " (repeating)");
    std::vector<std::thread> pool;
    for (int i = 0; i < threads; ++i) pool.emplace_back(Worker);

    const ULONGLONG begin = GetTickCount64();
    do {
        for (const Phase& p : phases) {
            g_load = std::max(0, std::min(100, p.percent));
            std::printf("[%6.1fs] load %3d%% for %d s\n", (GetTickCount64() - begin) / 1000.0,
                        p.percent, p.seconds);
            std::fflush(stdout);
            for (int s = 0; s < p.seconds; ++s) {
                Sleep(1000);
                if (crashAfter > 0 && (GetTickCount64() - begin) / 1000 >= static_cast<ULONGLONG>(crashAfter)) {
                    std::printf("simulated crash\n");
                    std::fflush(stdout);
                    ExitProcess(3);
                }
            }
        }
    } while (!once);

    g_run = false;
    for (auto& t : pool) t.join();
    return 0;
}
