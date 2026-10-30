#include "ControlPipe.h"

#include <sddl.h>

#include <vector>

namespace daemon {

namespace {

std::wstring CurrentUserSidString() {
    HANDLE token = nullptr;
    std::wstring result;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return result;
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buf(size);
    if (GetTokenInformation(token, TokenUser, buf.data(), size, &size)) {
        LPWSTR sid = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &sid)) {
            result = sid;
            LocalFree(sid);
        }
    }
    CloseHandle(token);
    return result;
}

}

bool ControlPipeServer::Start() {
    stopping_ = false;
    thread_ = std::thread([this] { Loop(); });
    return true;
}

void ControlPipeServer::Stop() {
    if (!thread_.joinable()) {
        return;
    }
    stopping_ = true;
    // Unblock ConnectNamedPipe with a throwaway client connection.
    HANDLE h = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
    }
    thread_.join();
}

void ControlPipeServer::Loop() {
    std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
    const std::wstring self = CurrentUserSidString();
    if (!self.empty()) {
        sddl += L"(A;;GA;;;" + self + L")";
    }
    PSECURITY_DESCRIPTOR sd = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd,
                                                         nullptr);
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};

    while (!stopping_) {
        HANDLE pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
                                           PIPE_REJECT_REMOTE_CLIENTS,
                                       1, 64 * 1024, 64 * 1024, 0, sd ? &sa : nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(1000);
            continue;
        }
        const BOOL connected =
            ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (connected && !stopping_) {
            char buf[4096];
            DWORD read = 0;
            if (ReadFile(pipe, buf, sizeof(buf) - 1, &read, nullptr) && read > 0) {
                std::string response = handler_(std::string(buf, read));
                DWORD written = 0;
                WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), &written,
                          nullptr);
                FlushFileBuffers(pipe);
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    if (sd) {
        LocalFree(sd);
    }
}

bool SendControlRequest(const std::string& request, std::string& response, DWORD timeoutMs) {
    if (!WaitNamedPipeW(kPipeName, timeoutMs)) {
        response = GetLastError() == ERROR_FILE_NOT_FOUND ? "daemon is not running"
                                                          : "daemon did not answer";
        return false;
    }
    HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        response = GetLastError() == ERROR_ACCESS_DENIED
                       ? "access denied: run this command as Administrator"
                       : "could not connect to daemon";
        return false;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
    DWORD written = 0;
    bool ok = WriteFile(pipe, request.data(), static_cast<DWORD>(request.size()), &written,
                        nullptr) != FALSE;
    response.clear();
    if (ok) {
        char buf[64 * 1024];
        DWORD read = 0;
        while (true) {
            const BOOL r = ReadFile(pipe, buf, sizeof(buf), &read, nullptr);
            response.append(buf, read);
            if (r || GetLastError() != ERROR_MORE_DATA) {
                ok = r != FALSE;
                break;
            }
        }
    }
    CloseHandle(pipe);
    return ok;
}

}
