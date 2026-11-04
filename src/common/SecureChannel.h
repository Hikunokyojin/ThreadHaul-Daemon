#pragma once

#include <winsock2.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace securechannel {

// Encrypted, mutually authenticated message channel over a connected TCP
// socket, built only on Windows CNG (bcrypt). Used between the service and
// ThreadHaulController.
//
// Handshake (plaintext JSON frames):
//   client -> {"type":"hello","version":2,"host":...,"nonce":<32 random bytes>}
//   server -> {"type":"challenge","nonce":<32 random bytes>}
//   client -> {"type":"proof","mac":HMAC(K,"client-proof")}
//   server -> {"type":"proof","mac":HMAC(K,"server-proof")}
// where K = HMAC-SHA256(SHA256("threadhaul-psk-v2" || secret),
//                        "keys" || client nonce || server nonce || host).
// Both sides prove knowledge of the shared secret without sending it, and the
// fresh nonces make every session's keys unique (no replay of a handshake).
//
// After the handshake every frame is AES-256-GCM with a per-direction key and
// a 64-bit sequence number in the nonce; a frame whose sequence number is not
// exactly the next one expected is rejected, which stops replay, reordering
// and deletion. Frames are length-prefixed (4 bytes, big-endian) and capped
// at 1 MiB.
//
// Limitation: keys derive from the pre-shared secret, so there is no forward
// secrecy -- someone who later learns the secret and recorded the traffic can
// decrypt it. Rotate the secret if it may have leaked.
class Channel {
public:
    explicit Channel(SOCKET socket) : sock_(socket) {}

    bool ClientHandshake(const std::string& secret, const std::string& host, std::string& error);
    bool ServerHandshake(const std::string& secret, std::string& peerHost, std::string& error);

    // Thread-safe. Returns false if the connection failed.
    bool Send(const std::string& plaintext);

    // 1 = message received, 0 = timeout, -1 = closed, broken or tampered.
    int Receive(std::string& plaintext, int timeoutMs);

    static constexpr size_t kMaxFrame = 1024 * 1024;
    static constexpr size_t kMinSecretLength = 16;

private:
    bool SendFrame(const std::string& frame);
    int ReceiveFrame(std::string& frame, int timeoutMs);
    void DeriveKeys(const std::string& secret, const std::string& clientNonce,
                    const std::string& serverNonce, const std::string& host, bool isClient);

    SOCKET sock_;
    std::mutex sendMutex_;
    std::string rxBuffer_;
    std::string sessionKey_;
    std::string sendKey_;
    std::string recvKey_;
    bool isClient_ = false;
    uint64_t sendSeq_ = 0;
    uint64_t recvSeq_ = 0;
    bool ready_ = false;
};

// Exposed for tests.
std::string Sha256(const std::string& data);
std::string HmacSha256(const std::string& key, const std::string& data);
bool AesGcmEncrypt(const std::string& key, const std::string& nonce, const std::string& plain,
                   std::string& cipherAndTag);
bool AesGcmDecrypt(const std::string& key, const std::string& nonce,
                   const std::string& cipherAndTag, std::string& plain);
std::string RandomBytes(size_t n);
std::string ToHex(const std::string& bytes);
std::string FromHex(const std::string& hex);
bool ConstantTimeEqual(const std::string& a, const std::string& b);

}
