#include "Secrets.h"

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
