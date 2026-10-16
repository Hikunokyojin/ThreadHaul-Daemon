#include "ConfigSecurity.h"

#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace daemon {

bool IsWritableByNonAdmins(const std::wstring& path, std::wstring& who) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                              nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS) {
        return false;
    }
    if (dacl == nullptr) {
        LocalFree(sd);
        who = L"anyone (NULL DACL)";
        return true;
    }

    struct Group { WELL_KNOWN_SID_TYPE type; const wchar_t* name; };
    const Group groups[] = {
        {WinWorldSid, L"Everyone"},
        {WinBuiltinUsersSid, L"Users"},
        {WinAuthenticatedUserSid, L"Authenticated Users"},
        {WinInteractiveSid, L"Interactive"},
    };
    constexpr ACCESS_MASK kWrite = FILE_WRITE_DATA | FILE_APPEND_DATA | WRITE_DAC | WRITE_OWNER |
                                   DELETE | GENERIC_WRITE | GENERIC_ALL;

    bool writable = false;
    for (DWORD i = 0; i < dacl->AceCount && !writable; ++i) {
        ACE_HEADER* header = nullptr;
        if (!GetAce(dacl, i, reinterpret_cast<void**>(&header)) ||
            header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & INHERIT_ONLY_ACE)) {
            continue;
        }
        auto* ace = reinterpret_cast<ACCESS_ALLOWED_ACE*>(header);
        if ((ace->Mask & kWrite) == 0) {
            continue;
        }
        PSID aceSid = &ace->SidStart;
        for (const Group& g : groups) {
            BYTE buf[SECURITY_MAX_SID_SIZE];
            DWORD size = sizeof(buf);
            if (CreateWellKnownSid(g.type, nullptr, buf, &size) && EqualSid(aceSid, buf)) {
                who = g.name;
                writable = true;
                break;
            }
        }
    }
    LocalFree(sd);
    return writable;
}

std::string Sha256File(const std::wstring& path) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) {
        return {};
    }
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    std::string hex;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<char> buf(64 * 1024);
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const auto n = in.gcount();
            if (n > 0) {
                BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(n), 0);
            }
        }
        UCHAR digest[32];
        if (BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            static const char* digits = "0123456789abcdef";
            for (UCHAR b : digest) {
                hex += digits[b >> 4];
                hex += digits[b & 15];
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

}
