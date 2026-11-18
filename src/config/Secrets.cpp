#include "Secrets.h"

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#include <dpapi.h>

#include <vector>

namespace config {

namespace {

const char kEntropyText[] = "ThreadHaul config secret v1";

DATA_BLOB Entropy() {
    DATA_BLOB e;
    e.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kEntropyText));
    e.cbData = sizeof(kEntropyText) - 1;
    return e;
}

}

bool IsProtectedSecret(const std::string& value) {
    return value.rfind(kSecretPrefix, 0) == 0;
}

std::string ProtectSecret(const std::string& plaintext) {
    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    in.cbData = static_cast<DWORD>(plaintext.size());
    DATA_BLOB entropy = Entropy();
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"ThreadHaul", &entropy, nullptr, nullptr,
                          CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return "";
    }
    DWORD chars = 0;
    std::string result;
    if (CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                             nullptr, &chars)) {
        std::string b64(chars, '\0');
        if (CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                                 b64.data(), &chars)) {
            b64.resize(chars);
            result = kSecretPrefix + b64;
        }
    }
    LocalFree(out.pbData);
    return result;
}

bool UnprotectSecret(const std::string& value, std::string& plaintext, std::string& error) {
    if (!IsProtectedSecret(value)) {
        plaintext = value;
        return true;
    }
    const std::string b64 = value.substr(std::char_traits<char>::length(kSecretPrefix));
    DWORD bytes = 0;
    if (b64.empty() || !CryptStringToBinaryA(b64.c_str(), static_cast<DWORD>(b64.size()),
                                             CRYPT_STRING_BASE64, nullptr, &bytes, nullptr, nullptr)) {
        error = "not valid base64";
        return false;
    }
    std::vector<BYTE> blob(bytes);
    CryptStringToBinaryA(b64.c_str(), static_cast<DWORD>(b64.size()), CRYPT_STRING_BASE64,
                         blob.data(), &bytes, nullptr, nullptr);
    // The Windows decoder skips whitespace and stray characters; accept only
    // the exact text ProtectSecret produces, so an edited value is rejected.
    DWORD chars = 0;
    std::string canonical;
    if (CryptBinaryToStringA(blob.data(), bytes, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &chars)) {
        canonical.resize(chars);
        if (CryptBinaryToStringA(blob.data(), bytes, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                                 canonical.data(), &chars)) {
            canonical.resize(chars);
        }
    }
    if (canonical != b64) {
        error = "not in the exact form protect-secret prints (was it edited?)";
        return false;
    }
    DATA_BLOB in{bytes, blob.data()};
    DATA_BLOB entropy = Entropy();
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &out)) {
        error = "cannot decrypt (made on another computer, or corrupted); "
                "re-create it with 'ThreadHaulDaemon.exe protect-secret'";
        return false;
    }
    plaintext.assign(reinterpret_cast<char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

}

#else  // Linux

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "LinuxProc.h"
#include "SecureChannel.h"

namespace config {

namespace {

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64(const std::string& in) {
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                           (static_cast<unsigned char>(in[i + 1]) << 8) | static_cast<unsigned char>(in[i + 2]);
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
    }
    if (i < in.size()) {
        unsigned v = static_cast<unsigned char>(in[i]) << 16;
        if (i + 1 < in.size()) v |= static_cast<unsigned char>(in[i + 1]) << 8;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += i + 1 < in.size() ? kB64[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

// Strict: only canonical padded base64 (what Base64 prints) is accepted.
bool Unbase64(const std::string& in, std::string& out) {
    if (in.empty() || in.size() % 4 != 0) return false;
    out.clear();
    for (size_t i = 0; i < in.size(); i += 4) {
        unsigned v = 0;
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = in[i + static_cast<size_t>(k)];
            const char* pos = c ? std::strchr(kB64, c) : nullptr;
            if (c == '=' && i + 4 == in.size() && k >= 2) {
                ++pad;
                v <<= 6;
            } else if (pos && pad == 0) {
                v = (v << 6) | static_cast<unsigned>(pos - kB64);
            } else {
                return false;
            }
        }
        out += static_cast<char>((v >> 16) & 0xFF);
        if (pad < 2) out += static_cast<char>((v >> 8) & 0xFF);
        if (pad < 1) out += static_cast<char>(v & 0xFF);
    }
    return Base64(out) == in;
}

std::string KeyPath() {
    const char* env = std::getenv("THREADHAUL_SECRET_KEY");
    return env && *env ? env : kSecretKeyPath;
}

// Reads the machine key; creates it (0600) when create is set and it is missing.
bool MachineKey(bool create, std::string& key, std::string& error) {
    const std::string path = KeyPath();
    key = linuxproc::ReadText(path);
    if (key.size() == 32) return true;
    if (!key.empty()) {
        error = "machine key " + path + " is damaged";
        return false;
    }
    if (!create) {
        error = "cannot read the machine key " + path + " (made on another computer, or not running as root)";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    key = securechannel::RandomBytes(32);
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0 || write(fd, key.data(), key.size()) != static_cast<ssize_t>(key.size())) {
        if (fd >= 0) close(fd);
        error = "cannot create the machine key " + path;
        return false;
    }
    close(fd);
    return true;
}

}

bool IsProtectedSecret(const std::string& value) {
    return value.rfind(kSecretPrefix, 0) == 0;
}

std::string ProtectSecret(const std::string& plaintext) {
    std::string key, error;
    if (!MachineKey(true, key, error)) return "";
    const std::string nonce = securechannel::RandomBytes(12);
    std::string sealed;
    if (!securechannel::AesGcmEncrypt(key, nonce, plaintext, sealed)) return "";
    return kSecretPrefix + Base64(nonce + sealed);
}

bool UnprotectSecret(const std::string& value, std::string& plaintext, std::string& error) {
    if (!IsProtectedSecret(value)) {
        plaintext = value;
        return true;
    }
    std::string blob;
    if (!Unbase64(value.substr(std::char_traits<char>::length(kSecretPrefix)), blob)) {
        error = "not valid base64, or not in the exact form protect-secret prints (was it edited?)";
        return false;
    }
    if (blob.size() < 12 + 16) {
        error = "sealed value is too short";
        return false;
    }
    std::string key;
    if (!MachineKey(false, key, error)) return false;
    if (!securechannel::AesGcmDecrypt(key, blob.substr(0, 12), blob.substr(12), plaintext)) {
        error = "cannot decrypt (made on another computer, or corrupted); "
                "re-create it with 'threadhauld protect-secret'";
        return false;
    }
    return true;
}

}

#endif
