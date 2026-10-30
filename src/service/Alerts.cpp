#include "Alerts.h"

#include <windows.h>
#include <winhttp.h>
#include <wtsapi32.h>

#include <thread>

namespace daemon {

void Alerter::Configure(bool popup, std::wstring webhookUrl) {
    popup_ = popup;
    webhookUrl_ = std::move(webhookUrl);
}

void Alerter::Fire(const std::wstring& title, const std::wstring& message,
                   const std::string& jsonBody) {
    const bool popup = popup_;
    const std::wstring url = webhookUrl_;
    std::thread([popup, url, title, message, jsonBody] {
        if (popup) {
            const DWORD session = WTSGetActiveConsoleSessionId();
            if (session != 0xFFFFFFFF) {
                DWORD response = 0;
                WTSSendMessageW(WTS_CURRENT_SERVER_HANDLE, session,
                                const_cast<LPWSTR>(title.c_str()),
                                static_cast<DWORD>(title.size() * sizeof(wchar_t)),
                                const_cast<LPWSTR>(message.c_str()),
                                static_cast<DWORD>(message.size() * sizeof(wchar_t)),
                                MB_OK | MB_ICONWARNING | MB_SETFOREGROUND, 30, &response, FALSE);
            }
        }
        if (!url.empty()) {
            std::wstring error;
            PostJson(url, jsonBody, error);
        }
    }).detach();
}

bool PostJson(const std::wstring& url, const std::string& body, std::wstring& error) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = ARRAYSIZE(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        error = L"invalid URL";
        return false;
    }

    bool ok = false;
    HINTERNET session = WinHttpOpen(L"ThreadHaulDaemon/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"POST", path, nullptr,
                                                     WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, flags)
                                : nullptr;
    if (request != nullptr) {
        WinHttpSetTimeouts(request, 5000, 5000, 5000, 5000);
        const wchar_t* headers = L"Content-Type: application/json\r\n";
        if (WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L),
                               const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                               static_cast<DWORD>(body.size()), 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0;
            DWORD size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                WINHTTP_NO_HEADER_INDEX);
            ok = status >= 200 && status < 300;
            if (!ok) error = L"HTTP status " + std::to_wstring(status);
        } else {
            error = L"request failed, error " + std::to_wstring(GetLastError());
        }
    } else {
        error = L"could not open connection, error " + std::to_wstring(GetLastError());
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

}
