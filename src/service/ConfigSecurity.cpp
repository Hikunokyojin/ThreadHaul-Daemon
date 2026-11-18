#include "ConfigSecurity.h"

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace thd {

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
        bool ok = true;
        while (in && ok) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const auto n = in.gcount();
            if (n > 0) {
                ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(n), 0) == 0;
            }
        }
        UCHAR digest[32];
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
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

#else  // Linux

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "SecureChannel.h"
#include "Utf8.h"

namespace thd {

namespace {

std::string UserName(uid_t uid) {
    const passwd* p = getpwuid(uid);
    return p ? p->pw_name : "uid " + std::to_string(uid);
}

std::string GroupName(gid_t gid) {
    const group* g = getgrgid(gid);
    return g ? g->gr_name : "gid " + std::to_string(gid);
}

// Who other than root can change `st`: its owner, its group, or everyone.
bool NonRootWriter(const struct stat& st, std::string& who) {
    if (st.st_uid != 0) {
        who = "its owner " + UserName(st.st_uid);
        return true;
    }
    if ((st.st_mode & S_IWGRP) && st.st_gid != 0) {
        who = "group " + GroupName(st.st_gid);
        return true;
    }
    if (st.st_mode & S_IWOTH) {
        who = "every user";
        return true;
    }
    return false;
}

}

// The Linux counterpart of the ACL check: the file, and the folder holding
// it (which decides who can replace it), must be changeable only by root.
bool IsWritableByNonAdmins(const std::wstring& path, std::wstring& who) {
    const std::string file = utf8::FromWide(path);
    struct stat st{};
    if (stat(file.c_str(), &st) != 0) return false;
    std::string w;
    if (NonRootWriter(st, w)) {
        who = utf8::ToWide(w);
        return true;
    }
    const std::string dir = std::filesystem::path(file).parent_path().string();
    struct stat ds{};
    if (!dir.empty() && stat(dir.c_str(), &ds) == 0 && NonRootWriter(ds, w) &&
        !((ds.st_mode & S_ISVTX) && st.st_uid == 0 && ds.st_uid == 0)) {  // sticky root dir: cannot replace
        who = utf8::ToWide(w + " (through the folder " + dir + ")");
        return true;
    }
    return false;
}

std::string Sha256File(const std::wstring& path) {
    std::ifstream in(std::filesystem::path(utf8::FromWide(path)), std::ios::binary);
    if (!in) return {};
    std::ostringstream data;
    data << in.rdbuf();
    return securechannel::ToHex(securechannel::Sha256(data.str()));
}

}

#endif
