#pragma once

// /proc helpers shared by the Linux engine (LinuxTelemetry.cpp and
// LinuxActuator.cpp). Linux only.

#include <dirent.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace linuxproc {

inline std::string ReadText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

inline bool WriteText(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << text;
    f.flush();
    return static_cast<bool>(f);
}

// UTF-8 -> wide. Invalid sequences become U+FFFD.
inline std::wstring Widen(const std::string& s) {
    std::wstring out;
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int extra;
        unsigned long cp;
        if (c < 0x80) { extra = 0; cp = c; }
        else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
        else { out += L'\xFFFD'; ++i; continue; }
        bool ok = i + static_cast<size_t>(extra) < s.size() + (extra == 0 ? 1 : 0) &&
                  i + static_cast<size_t>(extra) <= s.size() - 1;
        for (int k = 1; ok && k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
            if ((cc & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { out += L'\xFFFD'; ++i; continue; }
        out += static_cast<wchar_t>(cp);
        i += static_cast<size_t>(extra) + 1;
    }
    return out;
}

// wide -> UTF-8.
inline std::string Narrow(const std::wstring& w) {
    std::string out;
    for (wchar_t wc : w) {
        const unsigned long c = static_cast<unsigned long>(wc);
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// Fields of /proc/<pid>/stat that the engine uses.
struct Stat {
    std::string comm;
    char state = '?';
    int ppid = 0;
    uint64_t utime = 0;      // clock ticks
    uint64_t stime = 0;      // clock ticks
    uint64_t starttime = 0;  // clock ticks since boot
};

inline bool ReadStat(int pid, Stat& st) {
    const std::string s = ReadText("/proc/" + std::to_string(pid) + "/stat");
    const size_t open = s.find('('), close = s.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) return false;
    st.comm = s.substr(open + 1, close - open - 1);
    std::istringstream rest(s.substr(close + 1));
    std::vector<std::string> f;  // f[0] is field 3 (state)
    for (std::string t; rest >> t;) f.push_back(t);
    if (f.size() < 20) return false;
    st.state = f[0].empty() ? '?' : f[0][0];
    st.ppid = std::atoi(f[1].c_str());
    st.utime = std::strtoull(f[11].c_str(), nullptr, 10);
    st.stime = std::strtoull(f[12].c_str(), nullptr, 10);
    st.starttime = std::strtoull(f[19].c_str(), nullptr, 10);
    return true;
}

// Numeric entries of a /proc directory (processes, or a process's threads).
inline std::vector<int> NumericEntries(const std::string& dir) {
    std::vector<int> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        char* end = nullptr;
        const long v = std::strtol(e->d_name, &end, 10);
        if (end && *end == '\0' && v > 0) out.push_back(static_cast<int>(v));
    }
    closedir(d);
    return out;
}

inline std::vector<int> Threads(int pid) { return NumericEntries("/proc/" + std::to_string(pid) + "/task"); }

inline std::string ExePath(int pid) {
    char buf[4096];
    const ssize_t n = readlink(("/proc/" + std::to_string(pid) + "/exe").c_str(), buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    std::string p(buf, static_cast<size_t>(n));
    const std::string deleted = " (deleted)";
    if (p.size() > deleted.size() && p.compare(p.size() - deleted.size(), deleted.size(), deleted) == 0) {
        p.resize(p.size() - deleted.size());
    }
    return p;
}

// The process name ThreadHaul matches on: the executable's file name, or the
// kernel's 15-character comm when the executable cannot be read.
inline std::string ProcessName(int pid, const std::string& comm) {
    const std::string exe = ExePath(pid);
    if (!exe.empty()) {
        const size_t slash = exe.rfind('/');
        return slash == std::string::npos ? exe : exe.substr(slash + 1);
    }
    return comm;
}

// /proc/<pid>/status "Key:\tvalue" lookup.
inline std::string StatusField(int pid, const std::string& key) {
    std::istringstream s(ReadText("/proc/" + std::to_string(pid) + "/status"));
    for (std::string line; std::getline(s, line);) {
        if (line.compare(0, key.size() + 1, key + ":") == 0) {
            size_t i = key.size() + 1;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            return line.substr(i);
        }
    }
    return {};
}

inline uint64_t Now100ns() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 10000000ull + static_cast<uint64_t>(ts.tv_nsec) / 100;
}

inline uint64_t TicksTo100ns(uint64_t ticks) {
    static const long hz = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
    return ticks * (10000000ull / static_cast<uint64_t>(hz));
}

inline unsigned LogicalCores() {
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<unsigned>(n) : 1;
}

}
