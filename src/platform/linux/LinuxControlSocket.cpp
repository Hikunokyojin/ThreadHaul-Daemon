// Linux control channel: the named pipe's counterpart as a Unix socket.
// One request per connection: the client writes it and shuts down its
// sending side; the server answers and closes.

#include "ControlPipe.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace thd {

namespace {

std::string SocketPath() {
    const char* env = std::getenv("THREADHAUL_CONTROL_SOCKET");
    return env && *env ? env : kControlSocketPath;
}

bool Address(const std::string& path, sockaddr_un& addr) {
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) return false;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

// Reads until the peer shuts down its side, up to `limit` bytes.
bool ReadAll(int fd, std::string& out, size_t limit, int timeoutMs) {
    char buf[4096];
    while (out.size() < limit) {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, timeoutMs) <= 0) return false;
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return true;
        out.append(buf, static_cast<size_t>(n));
    }
    return true;
}

bool WriteAll(int fd, const std::string& data) {
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = send(fd, data.data() + done, data.size() - done, MSG_NOSIGNAL);
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

}

bool ControlPipeServer::Start() {
    stopping_ = false;
    thread_ = std::thread([this] { Loop(); });
    return true;
}

void ControlPipeServer::Stop() {
    if (!thread_.joinable()) return;
    stopping_ = true;  // Loop polls with a short timeout
    thread_.join();
}

void ControlPipeServer::Loop() {
    const std::string path = SocketPath();
    sockaddr_un addr{};
    if (!Address(path, addr)) return;

    // The directory is the access boundary: owner only.
    std::error_code ec;
    const auto dir = std::filesystem::path(path).parent_path();
    std::filesystem::create_directories(dir, ec);
    chmod(dir.c_str(), 0700);

    const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) return;
    unlink(path.c_str());  // a stale socket from a previous run
    const mode_t old = umask(0177);
    const bool bound = bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    umask(old);
    if (!bound || listen(listener, 8) != 0) {
        close(listener);
        return;
    }
    chmod(path.c_str(), 0600);

    while (!stopping_) {
        pollfd p{listener, POLLIN, 0};
        if (poll(&p, 1, 200) <= 0) continue;
        const int client = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) continue;
        // Only root or the daemon's own account may send commands.
        ucred cred{};
        socklen_t len = sizeof(cred);
        const bool allowed = getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 &&
                             (cred.uid == 0 || cred.uid == geteuid());
        std::string request;
        if (!allowed) {
            WriteAll(client, "{\"ok\":false,\"message\":\"access denied\"}");
        } else if (ReadAll(client, request, 4095, 3000) && !request.empty() && !stopping_) {
            WriteAll(client, handler_(request));
        }
        close(client);
    }
    close(listener);
    unlink(path.c_str());
}

bool SendControlRequest(const std::string& request, std::string& response, DWORD timeoutMs) {
    sockaddr_un addr{};
    if (!Address(SocketPath(), addr)) {
        response = "control socket path is too long";
        return false;
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        response = "could not create a socket";
        return false;
    }
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int err = errno;
        close(fd);
        response = err == EACCES || err == EPERM ? "access denied: run this command as root (sudo)"
                   : err == ENOENT || err == ECONNREFUSED ? "daemon is not running"
                                                          : "could not connect to daemon";
        return false;
    }
    bool ok = WriteAll(fd, request) && shutdown(fd, SHUT_WR) == 0;
    response.clear();
    if (ok) ok = ReadAll(fd, response, 16u << 20, static_cast<int>(timeoutMs > 0 ? timeoutMs : 3000));
    if (!ok && response.empty()) response = "daemon did not answer";
    close(fd);
    return ok;
}

}
