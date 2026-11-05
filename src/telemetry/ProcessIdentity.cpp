#include "ProcessIdentity.h"

#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <algorithm>
#include <cwctype>

namespace telemetry {

namespace {

bool SameText(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return std::towlower(c); });
    return s;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

unsigned long long StartTime(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) return 0;
    FILETIME created{}, exited{}, kernel{}, user{};
    unsigned long long t = 0;
    if (GetProcessTimes(h, &created, &exited, &kernel, &user)) {
        t = (static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    }
    CloseHandle(h);
    return t;
}

}

std::wstring ProcessImagePath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) return L"";
    wchar_t buf[MAX_PATH * 4];
    DWORD size = ARRAYSIZE(buf);
    std::wstring path;
    if (QueryFullProcessImageNameW(h, 0, buf, &size)) path.assign(buf, size);
    CloseHandle(h);
    return path;
}

std::wstring FilePublisher(const std::wstring& path) {
    if (path.empty()) return L"";
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;  // servers are often offline
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;

    std::wstring publisher;
    if (WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data) == ERROR_SUCCESS) {
        CRYPT_PROVIDER_DATA* prov = WTHelperProvDataFromStateData(data.hWVTStateData);
        CRYPT_PROVIDER_SGNR* signer = prov ? WTHelperGetProvSignerFromChain(prov, 0, FALSE, 0) : nullptr;
        if (signer && signer->csCertChain > 0 && signer->pasCertChain[0].pCert) {
            wchar_t name[512];
            if (CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0,
                                   nullptr, name, ARRAYSIZE(name)) > 1) {
                publisher = name;
            }
        }
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    return publisher;
}

std::string IdentityMismatch(const ExpectedIdentity& want, const ObservedIdentity& got) {
    if (!want.path.empty()) {
        if (got.path.empty()) return "image path could not be read";
        if (!SameText(want.path, got.path)) return "runs from " + Narrow(got.path) + ", expected " + Narrow(want.path);
    }
    if (!want.publisher.empty()) {
        if (got.publisher.empty()) return "not signed by a trusted publisher";
        if (!SameText(want.publisher, got.publisher)) {
            return "signed by " + Narrow(got.publisher) + ", expected " + Narrow(want.publisher);
        }
    }
    return "";
}

void IdentityFilter::SetExpected(std::map<std::wstring, ExpectedIdentity> expected) {
    std::lock_guard<std::mutex> lock(mutex_);
    expected_.clear();
    for (auto& [name, id] : expected) expected_[Lower(name)] = std::move(id);
    cache_.clear();
}

bool IdentityFilter::Empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return expected_.empty();
}

std::vector<ProcessEntry> IdentityFilter::Filter(const std::vector<ProcessEntry>& snapshot,
                                                 std::vector<Rejection>& rejected) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (expected_.empty()) return snapshot;
    std::vector<ProcessEntry> out;
    out.reserve(snapshot.size());
    std::map<DWORD, Cached> seen;
    for (const ProcessEntry& e : snapshot) {
        const auto want = expected_.find(Lower(e.name));
        if (want == expected_.end()) {
            out.push_back(e);
            continue;
        }
        const unsigned long long start = StartTime(e.pid);
        auto it = cache_.find(e.pid);
        if (it == cache_.end() || it->second.startTime != start) {
            ObservedIdentity got;
            got.path = ProcessImagePath(e.pid);
            if (!want->second.publisher.empty()) got.publisher = FilePublisher(got.path);
            const std::string reason = IdentityMismatch(want->second, got);
            Cached c{start, reason.empty()};
            if (!c.ok) rejected.push_back({e, got, reason});
            it = cache_.insert_or_assign(e.pid, c).first;
        }
        seen[e.pid] = it->second;
        if (it->second.ok) out.push_back(e);
    }
    cache_.swap(seen);  // forget processes that have exited
    return out;
}

}
