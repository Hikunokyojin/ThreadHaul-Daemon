#include "SecureChannel.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#endif

#include <chrono>

#include <cstdio>
#include <cstdlib>

#include <nlohmann/json.hpp>

namespace securechannel {

namespace {

constexpr size_t kTagSize = 16;
constexpr size_t kNonceSize = 12;

#ifdef _WIN32
std::string Hash(const std::string& key, const std::string& data, bool hmac) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    hmac ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0) != 0) {
        return {};
    }
    BCRYPT_HASH_HANDLE h = nullptr;
    std::string out(32, '\0');
    if (BCryptCreateHash(alg, &h, nullptr, 0,
                         hmac ? reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())) : nullptr,
                         hmac ? static_cast<ULONG>(key.size()) : 0, 0) == 0) {
        const bool ok =
            BCryptHashData(h, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                           static_cast<ULONG>(data.size()), 0) == 0 &&
            BCryptFinishHash(h, reinterpret_cast<PUCHAR>(out.data()), 32, 0) == 0;
        BCryptDestroyHash(h);
        if (!ok) out.clear();
    } else {
        out.clear();
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

bool Gcm(bool encrypt, const std::string& key, const std::string& nonce, const std::string& in,
         std::string& out) {
    if (key.size() != 32 || nonce.size() != kNonceSize) return false;
    if (!encrypt && in.size() < kTagSize) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return false;
    bool ok = false;
    BCRYPT_KEY_HANDLE k = nullptr;
    if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0) == 0 &&
        BCryptGenerateSymmetricKey(alg, &k, nullptr, 0,
                                   reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())), 32, 0) == 0) {
        const size_t bodyLen = encrypt ? in.size() : in.size() - kTagSize;
        std::string tag = encrypt ? std::string(kTagSize, '\0') : in.substr(bodyLen);
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = reinterpret_cast<PUCHAR>(const_cast<char*>(nonce.data()));
        info.cbNonce = kNonceSize;
        info.pbTag = reinterpret_cast<PUCHAR>(tag.data());
        info.cbTag = kTagSize;
        std::string body(bodyLen, '\0');
        ULONG written = 0;
        PUCHAR src = reinterpret_cast<PUCHAR>(const_cast<char*>(in.data()));
        PUCHAR dst = bodyLen ? reinterpret_cast<PUCHAR>(body.data()) : nullptr;
        const NTSTATUS st =
            encrypt ? BCryptEncrypt(k, bodyLen ? src : nullptr, static_cast<ULONG>(bodyLen), &info,
                                    nullptr, 0, dst, static_cast<ULONG>(bodyLen), &written, 0)
                    : BCryptDecrypt(k, bodyLen ? src : nullptr, static_cast<ULONG>(bodyLen), &info,
                                    nullptr, 0, dst, static_cast<ULONG>(bodyLen), &written, 0);
        if (st == 0) {
            out = encrypt ? body + tag : body;
            ok = true;
        }
        BCryptDestroyKey(k);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

#else
std::string Hash(const std::string& key, const std::string& data, bool hmac) {
    std::string out(32, '\0');
    unsigned int len = 0;
    const unsigned char* d = reinterpret_cast<const unsigned char*>(data.data());
    const bool ok = hmac
        ? HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), d, data.size(),
               reinterpret_cast<unsigned char*>(out.data()), &len) != nullptr
        : EVP_Digest(d, data.size(), reinterpret_cast<unsigned char*>(out.data()), &len,
                     EVP_sha256(), nullptr) == 1;
    if (!ok || len != 32) out.clear();
    return out;
}

bool Gcm(bool encrypt, const std::string& key, const std::string& nonce, const std::string& in,
         std::string& out) {
    if (key.size() != 32 || nonce.size() != kNonceSize) return false;
    if (!encrypt && in.size() < kTagSize) return false;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    const size_t bodyLen = encrypt ? in.size() : in.size() - kTagSize;
    std::string body(bodyLen + 16, '\0');
    const auto* k = reinterpret_cast<const unsigned char*>(key.data());
    const auto* iv = reinterpret_cast<const unsigned char*>(nonce.data());
    const auto* src = reinterpret_cast<const unsigned char*>(in.data());
    auto* dst = reinterpret_cast<unsigned char*>(body.data());
    int n = 0, fin = 0;
    bool ok = (encrypt ? EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, k, iv)
                       : EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, k, iv)) == 1;
    if (ok && bodyLen) {
        ok = (encrypt ? EVP_EncryptUpdate(ctx, dst, &n, src, static_cast<int>(bodyLen))
                      : EVP_DecryptUpdate(ctx, dst, &n, src, static_cast<int>(bodyLen))) == 1;
    }
    std::string tag = encrypt ? std::string(kTagSize, '\0') : in.substr(bodyLen);
    if (ok && encrypt) {
        ok = EVP_EncryptFinal_ex(ctx, dst + n, &fin) == 1 &&
             EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagSize, tag.data()) == 1;
    } else if (ok) {
        // The tag is checked in DecryptFinal: a mismatch means tampering.
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagSize, tag.data()) == 1 &&
             EVP_DecryptFinal_ex(ctx, dst + n, &fin) == 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return false;
    body.resize(bodyLen);
    out = encrypt ? body + tag : body;
    return true;
}

#endif

std::string Nonce(uint64_t seq, bool clientToServer) {
    std::string n(kNonceSize, '\0');
    n[0] = clientToServer ? 'C' : 'S';
    for (int i = 0; i < 8; ++i) n[4 + i] = static_cast<char>((seq >> (56 - 8 * i)) & 0xFF);
    return n;
}

}

std::string Sha256(const std::string& data) { return Hash({}, data, false); }
std::string HmacSha256(const std::string& key, const std::string& data) { return Hash(key, data, true); }

bool AesGcmEncrypt(const std::string& key, const std::string& nonce, const std::string& plain,
                   std::string& cipherAndTag) {
    return Gcm(true, key, nonce, plain, cipherAndTag);
}

bool AesGcmDecrypt(const std::string& key, const std::string& nonce,
                   const std::string& cipherAndTag, std::string& plain) {
    return Gcm(false, key, nonce, cipherAndTag, plain);
}

std::string RandomBytes(size_t n) {
    std::string out(n, '\0');
#ifdef _WIN32
    const bool failed = BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out.data()),
                                        static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0;
#else
    const bool failed = n && RAND_bytes(reinterpret_cast<unsigned char*>(out.data()),
                                        static_cast<int>(n)) != 1;
#endif
    if (failed) {
        // Predictable keys or nonces would silently break every guarantee;
        // stopping is the only safe answer.
        std::fprintf(stderr, "fatal: the system random number generator failed\n");
        std::abort();
    }
    return out;
}

std::string ToHex(const std::string& bytes) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (unsigned char c : bytes) {
        out += d[c >> 4];
        out += d[c & 15];
    }
    return out;
}

std::string FromHex(const std::string& hex) {
    if (hex.size() % 2) return {};
    std::string out;
    for (size_t i = 0; i < hex.size(); i += 2) {
        auto v = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = v(hex[i]), lo = v(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out += static_cast<char>(hi * 16 + lo);
    }
    return out;
}

bool ConstantTimeEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

bool Channel::SendFrame(const std::string& frame) {
    if (frame.size() > kMaxFrame) return false;
    std::string data(4, '\0');
    const uint32_t n = static_cast<uint32_t>(frame.size());
    data[0] = static_cast<char>(n >> 24);
    data[1] = static_cast<char>(n >> 16);
    data[2] = static_cast<char>(n >> 8);
    data[3] = static_cast<char>(n);
    data += frame;
    size_t sent = 0;
    while (sent < data.size()) {
        const int r = send(sock_, data.data() + sent, static_cast<int>(data.size() - sent), TH_SEND_FLAGS);
        if (r <= 0) return false;
        sent += static_cast<size_t>(r);
    }
    return true;
}

int Channel::ReceiveFrame(std::string& frame, int timeoutMs) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        if (rxBuffer_.size() >= 4) {
            const uint32_t n = (static_cast<uint32_t>(static_cast<unsigned char>(rxBuffer_[0])) << 24) |
                               (static_cast<uint32_t>(static_cast<unsigned char>(rxBuffer_[1])) << 16) |
                               (static_cast<uint32_t>(static_cast<unsigned char>(rxBuffer_[2])) << 8) |
                               static_cast<uint32_t>(static_cast<unsigned char>(rxBuffer_[3]));
            if (n > kMaxFrame) return -1;
            if (rxBuffer_.size() >= 4 + static_cast<size_t>(n)) {
                frame = rxBuffer_.substr(4, n);
                rxBuffer_.erase(0, 4 + static_cast<size_t>(n));
                return 1;
            }
        }
        const auto now = Clock::now();
        if (now >= deadline) return 0;
        fd_set set;
        FD_ZERO(&set);
        FD_SET(sock_, &set);
        const long long left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count() + 1;
        timeval tv{static_cast<long>(left / 1000), static_cast<long>((left % 1000) * 1000)};
        // The first argument is ignored by Winsock and required by POSIX.
        const int ready = select(static_cast<int>(sock_ + 1), &set, nullptr, nullptr, &tv);
        if (ready == SOCKET_ERROR) return -1;
        if (ready == 0) return 0;
        char buf[8192];
        const int r = recv(sock_, buf, sizeof(buf), 0);
        if (r <= 0) return -1;
        rxBuffer_.append(buf, static_cast<size_t>(r));
        if (rxBuffer_.size() > kMaxFrame + 4) return -1;
    }
}

void Channel::DeriveKeys(const std::string& secret, const std::string& clientNonce,
                         const std::string& serverNonce, const std::string& host, bool isClient) {
    const std::string psk = Sha256("threadhaul-psk-v2" + secret);
    sessionKey_ = HmacSha256(psk, "keys" + clientNonce + serverNonce + host);
    const std::string c2s = HmacSha256(sessionKey_, "c2s");
    const std::string s2c = HmacSha256(sessionKey_, "s2c");
    isClient_ = isClient;
    sendKey_ = isClient ? c2s : s2c;
    recvKey_ = isClient ? s2c : c2s;
}

bool Channel::ClientHandshake(const std::string& secret, const std::string& host, std::string& error) {
    using nlohmann::json;
    if (secret.size() < kMinSecretLength) {
        error = "shared secret must be at least 16 characters";
        return false;
    }
    const std::string cn = RandomBytes(32);
    if (!SendFrame(json{{"type", "hello"}, {"version", 2}, {"host", host}, {"nonce", ToHex(cn)}}.dump())) {
        error = "send failed";
        return false;
    }
    std::string frame;
    if (ReceiveFrame(frame, 5000) != 1) {
        error = "no challenge from controller";
        return false;
    }
    json ch = json::parse(frame, nullptr, false);
    if (ch.is_discarded() || !ch.is_object() || ch.value("type", "") != "challenge") {
        error = ch.is_object() ? ch.value("message", "handshake rejected") : "bad challenge";
        return false;
    }
    const std::string sn = FromHex(ch.value("nonce", ""));
    if (sn.size() != 32) {
        error = "bad challenge nonce";
        return false;
    }
    DeriveKeys(secret, cn, sn, host, true);
    SendFrame(json{{"type", "proof"}, {"mac", ToHex(HmacSha256(sessionKey_, "client-proof"))}}.dump());
    if (ReceiveFrame(frame, 5000) != 1) {
        error = "controller closed the connection (wrong shared secret?)";
        return false;
    }
    json pr = json::parse(frame, nullptr, false);
    if (pr.is_discarded() || !pr.is_object() || pr.value("type", "") != "proof" ||
        !ConstantTimeEqual(FromHex(pr.value("mac", "")), HmacSha256(sessionKey_, "server-proof"))) {
        error = "controller failed authentication";
        return false;
    }
    ready_ = true;
    return true;
}

bool Channel::ServerHandshake(const std::string& secret, std::string& peerHost, std::string& error) {
    using nlohmann::json;
    std::string frame;
    if (ReceiveFrame(frame, 5000) != 1) {
        error = "no hello";
        return false;
    }
    json hello = json::parse(frame, nullptr, false);
    if (hello.is_discarded() || !hello.is_object() || hello.value("type", "") != "hello" ||
        hello.value("version", 0) != 2) {
        SendFrame(json{{"type", "error"}, {"message", "protocol version 2 required"}}.dump());
        error = "bad hello or old protocol";
        return false;
    }
    const std::string cn = FromHex(hello.value("nonce", ""));
    peerHost = hello.value("host", "");
    if (cn.size() != 32 || peerHost.empty() || peerHost.size() > 255) {
        error = "malformed hello";
        return false;
    }
    const std::string sn = RandomBytes(32);
    SendFrame(json{{"type", "challenge"}, {"nonce", ToHex(sn)}}.dump());
    DeriveKeys(secret, cn, sn, peerHost, false);
    if (ReceiveFrame(frame, 5000) != 1) {
        error = "no proof";
        return false;
    }
    json pr = json::parse(frame, nullptr, false);
    if (pr.is_discarded() || !pr.is_object() || pr.value("type", "") != "proof" ||
        !ConstantTimeEqual(FromHex(pr.value("mac", "")), HmacSha256(sessionKey_, "client-proof"))) {
        error = "authentication failed";
        return false;  // no reply: don't help an attacker probe
    }
    SendFrame(json{{"type", "proof"}, {"mac", ToHex(HmacSha256(sessionKey_, "server-proof"))}}.dump());
    ready_ = true;
    return true;
}

bool Channel::Send(const std::string& plaintext) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (!ready_) return false;
    std::string sealed;
    if (!AesGcmEncrypt(sendKey_, Nonce(sendSeq_, isClient_), plaintext, sealed)) return false;
    ++sendSeq_;
    return SendFrame(sealed);
}

int Channel::Receive(std::string& plaintext, int timeoutMs) {
    if (!ready_) return -1;
    std::string frame;
    const int r = ReceiveFrame(frame, timeoutMs);
    if (r != 1) return r;
    if (!AesGcmDecrypt(recvKey_, Nonce(recvSeq_, !isClient_), frame, plaintext)) {
        return -1;  // tampered, replayed, reordered or wrong key
    }
    ++recvSeq_;
    return 1;
}

}
