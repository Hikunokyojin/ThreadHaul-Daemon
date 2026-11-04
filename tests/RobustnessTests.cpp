// Robustness ("fuzz") tests: feed every parser that takes outside input a
// large number of random and mutated inputs. Each must either accept the
// input or reject it with its documented error -- never crash, hang, or
// throw anything unexpected. The random seed is fixed so a failure can be
// reproduced; set THREADHAUL_FUZZ_SEED / THREADHAUL_FUZZ_ITERATIONS to vary it.

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstdlib>
#include <random>
#include <sstream>
#include <string>
#include <thread>

#include "Config.h"
#include "LogChain.h"
#include "SecureChannel.h"
#include "Secrets.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* test, const std::string& what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        std::printf("FAIL  %s: %s\n", test, what.c_str());
    }
}

unsigned EnvOr(const char* name, unsigned fallback) {
    char buf[32];
    size_t n = 0;
    if (getenv_s(&n, buf, sizeof(buf), name) == 0 && n > 1) return static_cast<unsigned>(std::strtoul(buf, nullptr, 10));
    return fallback;
}

std::mt19937 g_rng;

std::string RandomString(size_t maxLen) {
    std::uniform_int_distribution<size_t> len(0, maxLen);
    std::uniform_int_distribution<int> byte(0, 255);
    std::string s(len(g_rng), '\0');
    for (char& c : s) c = static_cast<char>(byte(g_rng));
    return s;
}

// Small random edits to a valid input: flip, insert, delete, duplicate.
std::string Mutate(std::string s) {
    std::uniform_int_distribution<int> edits(1, 8), kind(0, 3), byte(0, 255);
    static const char* tokens[] = {"{", "}", "[", "]", "\"", ":", ",", "null", "-1", "1e309",
                                   "true", "\"\\u0000\"", "99999999999999999999", "[]", "{}"};
    const int n = edits(g_rng);
    for (int i = 0; i < n && !s.empty(); ++i) {
        std::uniform_int_distribution<size_t> pos(0, s.size() - 1);
        const size_t p = pos(g_rng);
        switch (kind(g_rng)) {
        case 0: s[p] = static_cast<char>(byte(g_rng)); break;
        case 1: s.insert(p, tokens[std::uniform_int_distribution<size_t>(0, std::size(tokens) - 1)(g_rng)]); break;
        case 2: s.erase(p, std::uniform_int_distribution<size_t>(1, 16)(g_rng)); break;
        default: s.insert(p, s.substr(p, std::uniform_int_distribution<size_t>(1, 32)(g_rng))); break;
        }
    }
    return s;
}

const char* kValidConfig = R"({
  "polling": {"interval_ms": 1000},
  "thresholds": {"high": {"percent": 85, "consecutive_samples": 3},
                 "critical": {"percent": 90, "consecutive_samples": 5},
                 "low": {"percent": 60, "consecutive_samples": 5}},
  "processes": {"critical": ["dispatch_api.exe"], "batch": ["invoice_batch.exe"], "protected": []},
  "logging": {"log_path":"C:\\ProgramData\\ThreadHaulDaemon\\logs\\threadhaul.jsonl",
              "max_file_size_mb": 50, "max_rotated_files": 5},
  "coordination": {"enabled": true, "controller_host": "ctl", "shared_token": "0123456789abcdef-x"},
  "identity": {"dispatch_api.exe": {"path": "C:\\Apps\\dispatch_api.exe", "publisher": "Contoso"}},
  "alerts": {"webhook_url": "https://example.invalid/hook"},
  "dashboard": {"enabled": true, "port": 8787}
})";

void ConfigParserSurvives(unsigned iterations) {
    const char* t = "ConfigParserSurvives";
    bool baseline = true;
    std::string why;
    try { config::ParseConfig(kValidConfig); } catch (const std::exception& e) { baseline = false; why = e.what(); }
    Check(baseline, t, "baseline config parses: " + why);
    int unexpected = 0;
    for (unsigned i = 0; i < iterations; ++i) {
        const std::string input = (i % 4 == 0) ? RandomString(512) : Mutate(kValidConfig);
        try {
            config::ParseConfig(input);
        } catch (const config::ConfigError&) {
            // the documented way to reject input
        } catch (const std::exception& e) {
            if (++unexpected <= 3) Check(false, t, std::string("unexpected exception: ") + e.what());
        }
    }
    Check(unexpected == 0, t, std::to_string(unexpected) + " inputs threw something other than ConfigError");
}

void LogVerifierSurvives(unsigned iterations) {
    const char* t = "LogVerifierSurvives";
    const std::string good = logging::ChainLine("{\"log_type\":\"a\"}", logging::kGenesisHash);
    const std::string chainText = good + "\n" +
        logging::ChainLine("{\"log_type\":\"b\"}", logging::LineHash(good)) + "\n";
    {
        std::istringstream in(chainText);
        Check(logging::VerifyChain(in).Ok(), t, "baseline chain verifies");
    }
    int badReports = 0;
    for (unsigned i = 0; i < iterations; ++i) {
        const std::string text = (i % 2 == 0) ? RandomString(1024) : Mutate(chainText);
        std::istringstream in(text);
        try {
            const auto r = logging::VerifyChain(in);
            if (!r.Ok() && (r.brokenLine == 0 || r.brokenLine > r.lines)) ++badReports;
        } catch (const std::exception& e) {
            Check(false, t, std::string("threw: ") + e.what());
            return;
        }
    }
    Check(badReports == 0, t, "every failure names a line that exists");
}

void SecretDecoderSurvives(unsigned iterations) {
    const char* t = "SecretDecoderSurvives";
    const std::string real = config::ProtectSecret("robustness-secret-value");
    int accepted = 0;
    for (unsigned i = 0; i < iterations; ++i) {
        const std::string input = (i % 2 == 0) ? "dpapi:" + RandomString(200) : Mutate(real);
        std::string plain, err;
        if (config::UnprotectSecret(input, plain, err)) {
            // DPAPI does not authenticate a few fixed header bytes (the
            // provider ID), so an edit there still yields the original
            // secret. What must never happen is a *different* secret.
            if (config::IsProtectedSecret(input) && plain != "robustness-secret-value") ++accepted;
        } else {
            Check(!err.empty(), t, "rejection carries a reason");
        }
    }
    Check(accepted == 0, t, std::to_string(accepted) + " edited dpapi values produced a different secret");
}

bool SocketPair(SOCKET& a, SOCKET& b) {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener, 1) != 0) {
        closesocket(listener);
        return false;
    }
    int len = sizeof(addr);
    getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);
    a = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connect(a, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(listener);
        return false;
    }
    b = accept(listener, nullptr, nullptr);
    closesocket(listener);
    return b != INVALID_SOCKET;
}

void SetLengthPrefix(std::string& junk) {
    if (junk.size() < 4) return;
    const uint32_t n = static_cast<uint32_t>(junk.size() - 4);
    junk[0] = static_cast<char>(n >> 24); junk[1] = static_cast<char>(n >> 16);
    junk[2] = static_cast<char>(n >> 8);  junk[3] = static_cast<char>(n);
}

// Random bytes in place of a handshake: the server must refuse, not crash or hang.
void HandshakeSurvivesGarbage(unsigned iterations) {
    const char* t = "HandshakeSurvivesGarbage";
    const std::string secret = "correct-horse-battery-staple";
    int accepted = 0;
    for (unsigned i = 0; i < iterations; ++i) {
        SOCKET a, b;
        if (!SocketPair(a, b)) { Check(false, t, "socket pair"); return; }
        std::string junk = RandomString(300);
        if (i % 3 == 0) SetLengthPrefix(junk);  // plausible length, junk body
        send(a, junk.data(), static_cast<int>(junk.size()), 0);
        shutdown(a, SD_SEND);  // EOF after the junk, so a short read cannot block forever
        securechannel::Channel server(b);
        std::string host, err;
        if (server.ServerHandshake(secret, host, err)) ++accepted;
        closesocket(a);
        closesocket(b);
    }
    Check(accepted == 0, t, std::to_string(accepted) + " garbage handshakes accepted");
}

// After a good handshake, random frames must end the session, never deliver a message.
void ReceiveSurvivesGarbage(unsigned iterations) {
    const char* t = "ReceiveSurvivesGarbage";
    const std::string secret = "correct-horse-battery-staple";
    int delivered = 0;
    for (unsigned i = 0; i < iterations; ++i) {
        SOCKET a, b;
        if (!SocketPair(a, b)) { Check(false, t, "socket pair"); return; }
        securechannel::Channel client(a), server(b);
        std::string host, e1, e2;
        bool ok = false;
        std::thread th([&] { ok = server.ServerHandshake(secret, host, e1); });
        client.ClientHandshake(secret, "fuzz", e2);
        th.join();
        if (!ok) { Check(false, t, "handshake"); closesocket(a); closesocket(b); return; }
        std::string junk = RandomString(200);
        SetLengthPrefix(junk);
        send(a, junk.data(), static_cast<int>(junk.size()), 0);
        shutdown(a, SD_SEND);
        std::string got;
        if (server.Receive(got, 2000) == 1) ++delivered;
        closesocket(a);
        closesocket(b);
    }
    Check(delivered == 0, t, std::to_string(delivered) + " forged frames delivered");
}

}

int main() {
    const unsigned seed = EnvOr("THREADHAUL_FUZZ_SEED", 20261104u);
    const unsigned n = EnvOr("THREADHAUL_FUZZ_ITERATIONS", 2000u);
    g_rng.seed(seed);
    std::printf("seed %u, %u iterations\n", seed, n);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    ConfigParserSurvives(n * 2);
    LogVerifierSurvives(n);
    SecretDecoderSurvives(n / 4);
    HandshakeSurvivesGarbage(n / 10);
    ReceiveSurvivesGarbage(n / 20);
    WSACleanup();

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
