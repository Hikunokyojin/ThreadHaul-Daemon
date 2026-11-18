#pragma once

#include "ServiceCompat.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace thd {

#ifdef _WIN32
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\ThreadHaulDaemon";
#else
// Linux: a Unix socket in a root-only directory. The socket is mode 0600 and
// the server also checks the caller's user id (root or the daemon's own).
// THREADHAUL_CONTROL_SOCKET overrides the path (tests).
constexpr char kControlSocketPath[] = "/run/threadhaul/control.sock";
#endif

// Local control channel used by the CLI override. One request line in, one
// response (JSON) out, per connection. The pipe's DACL admits only SYSTEM,
// Administrators and the account the daemon runs as, so an ordinary user
// cannot throttle or suspend processes through it.
class ControlPipeServer {
public:
    using Handler = std::function<std::string(const std::string& request)>;

    explicit ControlPipeServer(Handler handler) : handler_(std::move(handler)) {}
    ~ControlPipeServer() { Stop(); }

    bool Start();
    void Stop();

private:
    void Loop();

    Handler handler_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
};

// Sends one request to a running daemon. Returns false if no daemon is
// listening (or access was denied); `response` then holds the reason.
bool SendControlRequest(const std::string& request, std::string& response, DWORD timeoutMs = 3000);

}
