// Linux service-layer tests: each platform piece against the real OS, in a
// temporary folder. Nothing here needs root; checks that do say so.

#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "Alerts.h"
#include "Config.h"
#include "ConfigSecurity.h"
#include "ControlPipe.h"
#include "HttpServer.h"
#include "JsonLinesLogger.h"
#include "LinuxProc.h"
#include "LogChain.h"
#include "NetCompat.h"
#include "Secrets.h"
#include "ServiceCompat.h"
#include "Signals.h"
#include "Utf8.h"

namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_checks = 0;
std::string g_dir;

void Check(bool cond, const char* test, const std::string& what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        std::printf("FAIL  %s: %s\n", test, what.c_str());
    }
}

std::string Slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

void Put(const std::string& path, const std::string& text) {
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

void Utf8() {
    const char* t = "Utf8";
    const std::wstring w{L'a', static_cast<wchar_t>(0xE9), static_cast<wchar_t>(0x4E2D), static_cast<wchar_t>(0x1F600)};
    Check(utf8::ToWide(utf8::FromWide(w)) == w, t, "round trip through UTF-8");
    Check(utf8::FromWide(w).size() == 1 + 2 + 3 + 4, t, "encoded lengths");
    Check(utf8::ToWide(std::string(1, static_cast<char>(0xC3))) == std::wstring(1, static_cast<wchar_t>(0xFFFD)), t,
          "truncated sequence replaced");
}

void Events() {
    const char* t = "Events";
    HANDLE e = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(WaitForSingleObject(e, 30) == WAIT_TIMEOUT, t, "unsignaled event times out");
    std::thread([e] { Sleep(20); SetEvent(e); }).detach();
    Check(WaitForSingleObject(e, 2000) == WAIT_OBJECT_0, t, "SetEvent wakes a waiter");
    Check(WaitForSingleObject(e, 0) == WAIT_OBJECT_0, t, "manual-reset event stays signaled");
    CloseHandle(e);
    HANDLE a = CreateEventW(nullptr, FALSE, TRUE, nullptr);
    Check(WaitForSingleObject(a, 0) == WAIT_OBJECT_0 && WaitForSingleObject(a, 0) == WAIT_TIMEOUT, t,
          "auto-reset event resets after one wait");
    CloseHandle(a);
    FILETIME idle, kernel, user;
    Check(GetSystemTimes(&idle, &kernel, &user) && kernel.dwLowDateTime + kernel.dwHighDateTime > 0, t,
          "system CPU times read");
    SYSTEMTIME st;
    GetLocalTime(&st);
    Check(st.wYear >= 2024 && st.wMonth >= 1 && st.wMonth <= 12 && st.wDayOfWeek <= 6, t, "local time");
}

void Secrets() {
    const char* t = "Secrets";
    const std::string key = g_dir + "/secret.key";
    setenv("THREADHAUL_SECRET_KEY", key.c_str(), 1);
    const std::string sealed = config::ProtectSecret("hunter2-token");
    Check(sealed.rfind("sealed:", 0) == 0, t, "sealed value has the prefix (got " + sealed + ")");
    struct stat st{};
    Check(stat(key.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, t, "machine key created 0600");
    std::string plain, err;
    Check(config::UnprotectSecret(sealed, plain, err) && plain == "hunter2-token", t, "round trip: " + err);
    Check(config::UnprotectSecret("plain", plain, err) && plain == "plain", t, "plain value passes through");
    std::string edited = sealed;
    edited[edited.size() - 3] = edited[edited.size() - 3] == 'A' ? 'B' : 'A';
    Check(!config::UnprotectSecret(edited, plain, err), t, "edited value rejected");
    Check(!config::UnprotectSecret("sealed:!!!", plain, err), t, "bad base64 rejected");
    Check(!config::UnprotectSecret("sealed:QUJD", plain, err), t, "too-short value rejected");
    Check(!config::UnprotectSecret(sealed + " ", plain, err), t, "non-canonical base64 rejected");
    Put(key, std::string(32, 'x'));  // another machine's key
    Check(!config::UnprotectSecret(sealed, plain, err), t, "value from another key rejected");
    unsetenv("THREADHAUL_SECRET_KEY");
}

void Logger() {
    const char* t = "Logger";
    const std::string path = g_dir + "/logs/th.jsonl";
    {
        logging::JsonLinesLogger log(utf8::ToWide(path), 600, 2);
        for (int i = 0; i < 12; ++i) {
            Check(log.LogSample(static_cast<uint32_t>(1000 + i), 12.5), t, "line written");
        }
    }
    Check(fs::exists(path) && fs::exists(path + ".1"), t, "rotated to .1");
    Check(!fs::exists(path + ".3"), t, "at most two rotated files kept");
    std::ifstream in(path, std::ios::binary);
    const auto r = logging::VerifyChain(in, logging::LastLineHash(utf8::ToWide(path + ".1")));
    Check(r.Ok() && r.chained > 0, t, "current file links to the rotated one: " + r.message);
    const std::string text = Slurp(path);
    Check(text.find("\"timestamp\":\"20") != std::string::npos && text.find("Z\"") != std::string::npos, t,
          "ISO-8601 UTC timestamps");
    std::string tampered = text;
    const size_t at = tampered.find("12.5");
    tampered.replace(at, 4, "99.9");
    std::istringstream bad(tampered);
    Check(!logging::VerifyChain(bad).Ok(), t, "edited line breaks the chain");
}

void ConfigLoad() {
    const char* t = "Config";
    const std::string example = Slurp("config/config.linux.example.json");
    Check(!example.empty(), t, "Linux example present (run from the repo root)");
    try {
        const auto cfg = config::ParseConfig(example);
        Check(cfg.logPath == L"/var/log/threadhaul/threadhaul.jsonl", t, "log path");
        Check(cfg.requireProtectedConfig, t, "Linux example requires a protected config");
    } catch (const config::ConfigError& e) {
        Check(false, t, std::string("example rejected: ") + e.what());
    }
    auto rejects = [&](const std::string& patchKey, const std::string& patchJson, const char* what) {
        auto j = nlohmann::json::parse(example);
        j[patchKey] = nlohmann::json::parse(patchJson);
        bool threw = false;
        try {
            config::ParseConfig(j.dump());
        } catch (const config::ConfigError&) {
            threw = true;
        }
        Check(threw, t, what);
    };
    auto accepts = [&](const std::string& patchKey, const std::string& patchJson, const char* what) {
        auto j = nlohmann::json::parse(example);
        j[patchKey] = nlohmann::json::parse(patchJson);
        try {
            config::ParseConfig(j.dump());
            Check(true, t, what);
        } catch (const config::ConfigError& e) {
            Check(false, t, std::string(what) + ": " + e.what());
        }
    };
    accepts("identity", R"({"postgres": {"path": "/usr/lib/postgresql/16/bin/postgres"}})", "absolute path pin");
    rejects("identity", R"({"postgres": {"path": "bin/postgres"}})", "relative path pin rejected");
    rejects("identity", R"({"postgres": {"publisher": "PostgreSQL"}})", "publisher pin rejected on Linux");
    accepts("signals", R"([{"type": "counter", "name": "cpu_psi", "counter": "pressure:cpu", "limit": 40}])",
            "pressure counter");
    accepts("signals", R"([{"type": "counter", "name": "temp", "counter": "file:/sys/class/thermal/*/temp", "limit": 90000}])",
            "file counter");
    rejects("signals", R"([{"type": "counter", "name": "x", "counter": "\\Processor(_Total)\\% Processor Time", "limit": 1}])",
            "Windows counter path rejected");
}

void Permissions() {
    const char* t = "Permissions";
    const std::string dir = g_dir + "/cfg";
    fs::create_directories(dir);
    chmod(dir.c_str(), 0700);
    const std::string file = dir + "/config.json";
    Put(file, "{}");
    std::wstring who;
    chmod(file.c_str(), 0666);
    Check(thd::IsWritableByNonAdmins(utf8::ToWide(file), who), t, "world-writable config flagged");
    chmod(file.c_str(), 0600);
    const bool flagged = thd::IsWritableByNonAdmins(utf8::ToWide(file), who);
    if (geteuid() == 0) {
        Check(!flagged, t, "root-owned 0600 config accepted");
    } else {
        Check(flagged && utf8::FromWide(who).find("owner") != std::string::npos, t,
              "config owned by an ordinary user flagged");
    }
    Check(thd::Sha256File(utf8::ToWide(file)) == "44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a", t,
          "SHA-256 of the file");
}

void ControlSocket() {
    const char* t = "ControlSocket";
    const std::string sock = g_dir + "/run/control.sock";
    setenv("THREADHAUL_CONTROL_SOCKET", sock.c_str(), 1);
    std::string response;
    Check(!thd::SendControlRequest("status", response) && response == "daemon is not running", t,
          "no daemon reported (got " + response + ")");
    {
        thd::ControlPipeServer server([](const std::string& req) { return "{\"ok\":true,\"echo\":\"" + req + "\"}"; });
        server.Start();
        for (int i = 0; i < 50 && !fs::exists(sock); ++i) Sleep(20);
        struct stat st{};
        Check(stat(sock.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, t, "socket is 0600");
        Check(stat((g_dir + "/run").c_str(), &st) == 0 && (st.st_mode & 0777) == 0700, t, "socket folder is 0700");
        Check(thd::SendControlRequest("throttle ffmpeg", response) && response == "{\"ok\":true,\"echo\":\"throttle ffmpeg\"}",
              t, "request answered (got " + response + ")");
        server.Stop();
    }
    Check(!fs::exists(sock), t, "socket removed on stop");
    unsetenv("THREADHAUL_CONTROL_SOCKET");
}

// Sends one raw HTTP request to 127.0.0.1:port and returns the reply.
std::string HttpGet(int port, const std::string& request) {
    const SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    std::string out;
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
        send(s, request.data(), request.size(), MSG_NOSIGNAL);
        char buf[4096];
        for (ssize_t n; (n = recv(s, buf, sizeof(buf), 0)) > 0;) out.append(buf, static_cast<size_t>(n));
    }
    closesocket(s);
    return out;
}

void Http() {
    const char* t = "Http";
    std::string lastBody;
    thd::HttpServer server([&lastBody](const thd::HttpRequest& r) {
        lastBody = r.body;
        thd::HttpResponse resp;
        resp.status = r.path == "/hook" ? 204 : 200;
        resp.body = r.path == "/hook" ? "" : "{\"path\":\"" + r.path + "\"}";
        return resp;
    });
    std::string err;
    int port = 0;
    for (int p = 38470; p < 38490 && !port; ++p) {
        if (server.Start(p, err)) port = p;
    }
    Check(port != 0, t, "server listens on loopback: " + err);
    if (!port) return;
    const std::string reply = HttpGet(port, "GET /api/x HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    Check(reply.rfind("HTTP/1.1 200", 0) == 0 && reply.find("{\"path\":\"/api/x\"}") != std::string::npos, t,
          "GET answered");
    Check(reply.find("Content-Security-Policy") != std::string::npos, t, "security headers sent");

    // The webhook client against the same server (plain HTTP).
    std::wstring werr;
    Check(thd::PostJson(L"http://127.0.0.1:" + std::to_wstring(port) + L"/hook", "{\"a\":1}", werr), t,
          "webhook POST accepted: " + utf8::FromWide(werr));
    Check(lastBody == "{\"a\":1}", t, "webhook body delivered");
    Check(!thd::PostJson(L"ftp://example/x", "{}", werr) && werr == L"invalid URL", t, "bad scheme rejected");
    Check(!thd::PostJson(L"https://127.0.0.1:" + std::to_wstring(port) + L"/hook", "{}", werr), t,
          "TLS to a plain server fails closed");
    server.Stop();
}

void Signals() {
    const char* t = "Signals";
    using telemetry::SignalSpec;
    std::vector<SignalSpec> specs;
    SignalSpec disk;
    telemetry::BuiltInSignal("disk_queue", disk);
    disk.name = "disk_queue";
    specs.push_back(disk);
    SignalSpec net;
    telemetry::BuiltInSignal("network", net);
    net.name = "network";
    specs.push_back(net);
    const bool psi = !linuxproc::ReadText("/proc/pressure/cpu").empty();
    SignalSpec cpu;
    cpu.name = "cpu_psi";
    cpu.counter = L"pressure:cpu";
    cpu.limit = 50;
    specs.push_back(cpu);
    fs::create_directories(g_dir + "/sig/a");
    fs::create_directories(g_dir + "/sig/b");
    Put(g_dir + "/sig/a/v", "30\n");
    Put(g_dir + "/sig/b/v", "12\n");
    SignalSpec file;
    file.name = "files";
    file.counter = utf8::ToWide("file:" + g_dir + "/sig/*/v");
    file.aggregate = telemetry::Aggregate::Max;
    file.limit = 60;
    specs.push_back(file);
    SignalSpec bad;
    bad.name = "bad";
    bad.counter = L"pressure:gpu";
    specs.push_back(bad);

    telemetry::SignalSet set;
    set.Configure(specs);
    Sleep(300);
    const auto r = set.Sample();
    Check(r.size() == 5, t, "one reading per signal");
    if (r.size() != 5) return;
    Check(r[0].ok && r[0].instances >= 1, t, "disk queue read: " + r[0].error);
    Check(r[1].ok && r[1].value >= 0, t, "network rate read: " + r[1].error);
    if (psi) Check(r[2].ok && r[2].value >= 0 && r[2].value <= 100, t, "CPU pressure read: " + r[2].error);
    else Check(!r[2].ok && !r[2].error.empty(), t, "missing /proc/pressure reported");
    Check(r[3].ok && r[3].instances == 2 && r[3].value == 30 && r[3].percent == 50, t, "file glob, max of 30 and 12");
    Check(!r[4].ok && r[4].error.find("pressure must be") != std::string::npos, t, "bad source reported");
}

}

int main() {
    char tmpl[] = "/tmp/threadhaul-test-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) {
        std::printf("cannot create a temporary folder\n");
        return 1;
    }
    g_dir = dir;
    Utf8();
    Events();
    Secrets();
    Logger();
    ConfigLoad();
    Permissions();
    ControlSocket();
    Http();
    Signals();
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
