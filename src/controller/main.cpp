// ThreadHaulController: central coordinator for several dispatch servers.
//
// Every daemon with "coordination.enabled" connects here, says hello with
// the shared token, and then sends heartbeats (pressure, thresholds, batch
// states) and actuator events. The controller:
//   - keeps a live table of agents and writes everything it receives to
//     controller.jsonl (JSON Lines, same conventions as the daemon log);
//   - computes cluster pressure, and when at least `--cluster-percent` of
//     agents are above `--pressure` it sends every agent a bias that lowers
//     their throttle thresholds by `--bias` points, so servers that are not
//     yet starved start shedding batch load early (and clears it when the
//     cluster calms down);
//   - lets an operator send commands to any agent from its console:
//       list
//       <host> throttle|suspend|resume|release <pid|name>
//       <host> status
//       all suspend <name>
//       quit
//
// Usage: ThreadHaulController.exe [--port 7878] [--token secret]
//            [--pressure 85] [--cluster-percent 50] [--bias 5] [--log controller.jsonl]

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using nlohmann::json;

struct Agent {
    SOCKET sock = INVALID_SOCKET;
    std::string host;
    json lastHeartbeat;
    ULONGLONG lastSeen = 0;
    double biasSent = 0.0;
    std::mutex sendMutex;
};

std::mutex g_mutex;
std::map<std::string, std::shared_ptr<Agent>> g_agents;
std::ofstream g_log;
std::string g_token;
double g_pressureThreshold = 85.0;
double g_clusterPercent = 50.0;
double g_bias = 5.0;
std::atomic<int> g_nextId{1};
std::atomic<bool> g_running{true};

std::string Timestamp() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", st.wYear, st.wMonth,
                  st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

void LogLine(const std::string& logType, json fields) {
    json j;
    j["log_type"] = logType;
    j["timestamp"] = Timestamp();
    for (auto& [k, v] : fields.items()) j[k] = v;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_log << j.dump() << "\n";
    g_log.flush();
}

bool SendLine(Agent& a, const json& msg) {
    std::lock_guard<std::mutex> lock(a.sendMutex);
    const std::string data = msg.dump() + "\n";
    size_t sent = 0;
    while (sent < data.size()) {
        const int n = send(a.sock, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n == SOCKET_ERROR) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Cluster-wide coordination: bias every agent when enough of them are hot.
void Rebalance() {
    std::vector<std::shared_ptr<Agent>> agents;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& [h, a] : g_agents) agents.push_back(a);
    }
    if (agents.empty()) return;
    int hot = 0;
    for (auto& a : agents) {
        if (a->lastHeartbeat.value("pressure_percent", 0.0) >= g_pressureThreshold) ++hot;
    }
    const double hotPercent = 100.0 * hot / agents.size();
    const double bias = hotPercent >= g_clusterPercent ? g_bias : 0.0;
    for (auto& a : agents) {
        if (a->biasSent != bias) {
            a->biasSent = bias;
            SendLine(*a, json{{"type", "bias"}, {"offset_percent", bias}});
            LogLine("cluster_bias", json{{"host", a->host},
                                         {"offset_percent", bias},
                                         {"hot_agents", hot},
                                         {"total_agents", agents.size()}});
            std::printf("[cluster] %d/%zu agents above %.0f%% -> bias %.1f sent to %s\n", hot,
                        agents.size(), g_pressureThreshold, bias, a->host.c_str());
        }
    }
}

void ServeAgent(SOCKET sock, std::string peer) {
    auto agent = std::make_shared<Agent>();
    agent->sock = sock;
    std::string buf;
    bool authed = false;
    char chunk[4096];
    while (g_running) {
        const int n = recv(sock, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        buf.append(chunk, static_cast<size_t>(n));
        if (buf.size() > 4 * 1024 * 1024) break;
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            const json msg = json::parse(buf.substr(0, pos), nullptr, false);
            buf.erase(0, pos + 1);
            if (msg.is_discarded() || !msg.is_object()) continue;
            const std::string type = msg.value("type", "");
            if (!authed) {
                if (type != "hello" || msg.value("token", "") != g_token) {
                    SendLine(*agent, json{{"type", "error"}, {"message", "bad token"}});
                    LogLine("agent_rejected", json{{"peer", peer}});
                    std::printf("[reject] %s: bad hello/token\n", peer.c_str());
                    closesocket(sock);
                    return;
                }
                authed = true;
                agent->host = msg.value("host", peer);
                agent->lastSeen = GetTickCount64();
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_agents[agent->host] = agent;
                }
                LogLine("agent_connected", json{{"host", agent->host}, {"peer", peer}});
                std::printf("[connect] %s (%s)\n", agent->host.c_str(), peer.c_str());
                continue;
            }
            agent->lastSeen = GetTickCount64();
            if (type == "heartbeat") {
                agent->lastHeartbeat = msg;
                LogLine("heartbeat", msg);
                Rebalance();
            } else if (type == "event") {
                LogLine("agent_event", msg);
                const json& e = msg["event"];
                std::printf("[event] %s: %s %s (PID %d) %s -> %s at %.1f%% [%s]\n",
                            agent->host.c_str(), e.value("action", "").c_str(),
                            e.value("batch_process_name", "").c_str(),
                            e.value("batch_process_id", 0), e.value("from_state", "").c_str(),
                            e.value("to_state", "").c_str(), e.value("cpu_percent", 0.0),
                            e.value("trigger", "").c_str());
            } else if (type == "command_result") {
                LogLine("command_result", msg);
                std::printf("[result] %s\n", msg.dump().c_str());
            }
        }
    }
    closesocket(sock);
    if (authed) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_agents.find(agent->host);
            if (it != g_agents.end() && it->second == agent) g_agents.erase(it);
        }
        LogLine("agent_disconnected", json{{"host", agent->host}});
        std::printf("[disconnect] %s\n", agent->host.c_str());
        Rebalance();
    }
}

void PrintAgents() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_agents.empty()) {
        std::printf("no agents connected\n");
        return;
    }
    for (auto& [host, a] : g_agents) {
        const json& hb = a->lastHeartbeat;
        std::printf("%-20s pressure %5.1f%%  bias %4.1f  seen %llus ago\n", host.c_str(),
                    hb.value("pressure_percent", 0.0), a->biasSent,
                    (GetTickCount64() - a->lastSeen) / 1000);
        if (hb.contains("batch")) {
            for (const auto& b : hb["batch"]) {
                std::printf("    batch %-24s pid %-6d %s%s\n", b.value("name", "").c_str(),
                            b.value("pid", 0), b.value("state", "").c_str(),
                            b.value("manual_hold", false) ? " (manual)" : "");
            }
        }
    }
}

void Console() {
    std::string line;
    while (g_running && std::getline(std::cin, line)) {
        if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);  // UTF-8 BOM from piped input
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream in(line);
        std::string host;
        in >> host;
        if (host.empty()) continue;
        if (host == "quit" || host == "exit") {
            g_running = false;
            break;
        }
        if (host == "list") {
            PrintAgents();
            continue;
        }
        std::string rest;
        std::getline(in, rest);
        while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
        if (rest.empty()) {
            std::printf("usage: <host|all> <command> [target]   |  list  |  quit\n");
            continue;
        }
        std::vector<std::shared_ptr<Agent>> targets;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (auto& [h, a] : g_agents) {
                if (host == "all" || _stricmp(h.c_str(), host.c_str()) == 0) targets.push_back(a);
            }
        }
        if (targets.empty()) {
            std::printf("no agent named %s\n", host.c_str());
            continue;
        }
        for (auto& a : targets) {
            const json cmd{{"type", "command"}, {"id", g_nextId++}, {"command", rest}};
            SendLine(*a, cmd);
            LogLine("command_sent", json{{"host", a->host}, {"command", rest}});
        }
    }
}

}

int main(int argc, char* argv[]) {
    if (argc > 1 && (std::string(argv[1]) == "help" || std::string(argv[1]) == "--help")) {
        std::puts("Usage: ThreadHaulController.exe [--port 7878] [--token secret] [--pressure 85]");
        std::puts("           [--cluster-percent 50] [--bias 5] [--log controller.jsonl]");
        return 0;
    }
    int port = 7878;
    std::string logPath = "controller.jsonl";
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        if (k == "--port") port = std::atoi(argv[i + 1]);
        else if (k == "--token") g_token = argv[i + 1];
        else if (k == "--pressure") g_pressureThreshold = std::atof(argv[i + 1]);
        else if (k == "--cluster-percent") g_clusterPercent = std::atof(argv[i + 1]);
        else if (k == "--bias") g_bias = std::atof(argv[i + 1]);
        else if (k == "--log") logPath = argv[i + 1];
    }
    g_log.open(logPath, std::ios::app);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<u_short>(port));
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listener, SOMAXCONN) != 0) {
        std::fprintf(stderr, "cannot listen on port %d (error %d)\n", port, WSAGetLastError());
        return 1;
    }
    std::printf("ThreadHaulController listening on port %d. Log: %s\n"
                "Commands: list | <host|all> <throttle|suspend|resume|release> <pid|name> | "
                "<host> status | quit\n",
                port, logPath.c_str());
    if (g_token.empty()) {
        std::printf("WARNING: no --token set; any agent with an empty token is accepted.\n");
    }
    LogLine("controller_started", json{{"port", port}});

    std::thread([listener] {
        while (g_running) {
            sockaddr_in peer{};
            int len = sizeof(peer);
            SOCKET s = accept(listener, reinterpret_cast<sockaddr*>(&peer), &len);
            if (s == INVALID_SOCKET) continue;
            char ip[64];
            inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
            std::thread(ServeAgent, s, std::string(ip)).detach();
        }
    }).detach();

    Console();
    closesocket(listener);
    LogLine("controller_stopped", json::object());
    WSACleanup();
    return 0;
}
