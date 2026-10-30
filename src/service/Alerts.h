#pragma once

#include <string>

namespace daemon {

// Fires a notification when a batch process is suspended. Two channels:
//  - popup: a message box in the active console session (WTSSendMessage),
//    which works from a Session 0 service where toast notifications cannot;
//  - webhook: an HTTP(S) POST of a JSON body to a configured URL (Teams,
//    Slack, an email relay, ...).
// Both run on a background thread so a slow network or an unattended
// desktop never delays the control loop.
class Alerter {
public:
    void Configure(bool popup, std::wstring webhookUrl);
    void Fire(const std::wstring& title, const std::wstring& message, const std::string& jsonBody);

private:
    bool popup_ = true;
    std::wstring webhookUrl_;
};

bool PostJson(const std::wstring& url, const std::string& body, std::wstring& error);

}
