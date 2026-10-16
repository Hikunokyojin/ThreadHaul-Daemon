#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace daemon {

// Agent side of multi-server coordination. Each dispatch server's daemon
// keeps one TCP connection to the central controller (ThreadHaulController)
// and exchanges newline-delimited JSON:
//
//   agent -> controller
//     {"type":"hello","host":"DISPATCH-01","token":"...","version":1}
//     {"type":"heartbeat","host":...,"pressure_percent":..,"offset_percent":..,"processes":[...]}
//     {"type":"event","host":...,"event":{ ...actuator_action log entry... }}
//     {"type":"command_result","id":..,"ok":true,"message":"..."}
//
//   controller -> agent
//     {"type":"command","id":..,"command":"suspend 1234"}   (same syntax as the CLI)
//     {"type":"bias","offset_percent":5.0}                  (cluster-wide pressure)
//
// The connection is re-established with exponential back-off if the
// controller is down; the daemon keeps working locally in the meantime.
class CoordinationClient {
public:
    using StatusFn = std::function<std::string()>;                     // heartbeat JSON body
    using CommandFn = std::function<std::string(const std::string&)>;  // returns result JSON
    using LogFn = std::function<void(const std::string& what, const std::string& detail)>;

    CoordinationClient(StatusFn status, CommandFn command, LogFn log);
    ~CoordinationClient() { Stop(); }

    void Start(std::wstring host, int port, std::string token, int heartbeatSeconds);
    void Stop();
    void SendEvent(const std::string& eventJson);

    double ExternalBias() const { return bias_.load(); }
    bool Connected() const { return connected_.load(); }

private:
    void Loop();
    bool Session(unsigned long long sock);

    StatusFn status_;
    CommandFn command_;
    LogFn log_;

    std::wstring host_;
    int port_ = 0;
    std::string token_;
    int heartbeatSeconds_ = 5;

    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> connected_{false};
    std::atomic<double> bias_{0.0};

    std::mutex queueMutex_;
    std::deque<std::string> outbox_;
};

std::string LocalHostName();

}
