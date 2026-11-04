// Unit tests for the security building blocks: the encrypted controller
// channel (handshake, tampering, replay, wrong secret) and its primitives.

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <string>
#include <thread>

#include "SecureChannel.h"

using namespace securechannel;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* test, const std::string& what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        std::printf("FAIL  %s: %s\n", test, what.c_str());
    }
}

// A connected pair of loopback TCP sockets.
bool SocketPair(SOCKET& a, SOCKET& b) {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    addr.sin_port = 0;
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener, 1) != 0) {
        return false;
    }
    int len = sizeof(addr);
    getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);
    a = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connect(a, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
    b = accept(listener, nullptr, nullptr);
    closesocket(listener);
    return b != INVALID_SOCKET;
}

struct HandshakeResult {
    bool client = false;
    bool server = false;
    std::string host;
};

HandshakeResult Handshake(Channel& c, Channel& s, const std::string& clientSecret,
                          const std::string& serverSecret) {
    HandshakeResult r;
    std::string err;
    std::thread server([&] { r.server = s.ServerHandshake(serverSecret, r.host, err); });
    std::string cerr;
    r.client = c.ClientHandshake(clientSecret, "agent-01", cerr);
    server.join();
    return r;
}

const std::string kSecret = "correct-horse-battery-staple";

void PrimitivesWork() {
    Check(ToHex(Sha256("abc")) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          __func__, "SHA-256 test vector");
    Check(ToHex(HmacSha256("key", "The quick brown fox jumps over the lazy dog")) ==
              "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8",
          __func__, "HMAC-SHA256 test vector");
    const std::string key = RandomBytes(32), nonce = RandomBytes(12);
    std::string sealed, opened;
    Check(AesGcmEncrypt(key, nonce, "hello", sealed) && sealed.size() == 5 + 16, __func__, "seal");
    Check(AesGcmDecrypt(key, nonce, sealed, opened) && opened == "hello", __func__, "open");
    sealed[0] ^= 1;
    Check(!AesGcmDecrypt(key, nonce, sealed, opened), __func__, "tampered ciphertext rejected");
    Check(FromHex(ToHex("\x01\xff")) == "\x01\xff" && FromHex("zz").empty(), __func__, "hex");
    Check(ConstantTimeEqual("abc", "abc") && !ConstantTimeEqual("abc", "abd"), __func__, "compare");
}

void HandshakeAndMessages() {
    SOCKET a, b;
    if (!SocketPair(a, b)) { Check(false, __func__, "socket pair"); return; }
    Channel client(a), server(b);
    const auto r = Handshake(client, server, kSecret, kSecret);
    Check(r.client && r.server, __func__, "same secret: both sides authenticate");
    Check(r.host == "agent-01", __func__, "server learns the agent's host name");
    std::string got;
    Check(client.Send("{\"type\":\"heartbeat\"}") && server.Receive(got, 2000) == 1 &&
              got == "{\"type\":\"heartbeat\"}",
          __func__, "client to server message");
    Check(server.Send("bias") && client.Receive(got, 2000) == 1 && got == "bias", __func__,
          "server to client message");
    for (int i = 0; i < 50; ++i) client.Send("m" + std::to_string(i));
    bool ordered = true;
    for (int i = 0; i < 50; ++i) ordered = ordered && server.Receive(got, 2000) == 1 && got == "m" + std::to_string(i);
    Check(ordered, __func__, "50 messages arrive in order");
    Check(server.Receive(got, 100) == 0, __func__, "timeout when idle");
    closesocket(a);
    Check(server.Receive(got, 1000) == -1, __func__, "closed connection reported");
    closesocket(b);
}

void WrongSecretFails() {
    SOCKET a, b;
    if (!SocketPair(a, b)) { Check(false, __func__, "socket pair"); return; }
    Channel client(a), server(b);
    const auto r = Handshake(client, server, kSecret, "a-completely-different-secret");
    Check(!r.server, __func__, "server rejects a client with the wrong secret");
    Check(!r.client, __func__, "client does not accept an unauthenticated server");
    closesocket(a);
    closesocket(b);
}

void ShortSecretRefused() {
    SOCKET a, b;
    if (!SocketPair(a, b)) { Check(false, __func__, "socket pair"); return; }
    Channel client(a);
    std::string err;
    Check(!client.ClientHandshake("short", "h", err) && !err.empty(), __func__, "secret under 16 chars refused");
    closesocket(a);
    closesocket(b);
}

void TamperedAndReplayedFramesRejected() {
    SOCKET a, b;
    if (!SocketPair(a, b)) { Check(false, __func__, "socket pair"); return; }
    Channel client(a), server(b);
    Handshake(client, server, kSecret, kSecret);
    // Forge a frame: valid length prefix, random bytes inside.
    std::string forged(4, '\0');
    forged[3] = 40;
    forged += RandomBytes(40);
    send(a, forged.data(), static_cast<int>(forged.size()), 0);
    std::string got;
    Check(server.Receive(got, 2000) == -1, __func__, "forged frame breaks the session");
    closesocket(a);
    closesocket(b);

    // Replay: capture a genuine frame by reading the raw socket, then send it twice.
    SOCKET c, d;
    if (!SocketPair(c, d)) return;
    Channel cl(c), sv(d);
    Handshake(cl, sv, kSecret, kSecret);
    SOCKET e, f;
    SocketPair(e, f);
    cl.Send("pay once");
    std::string raw;
    char buf[256];
    const int n = recv(d, buf, sizeof(buf), 0);
    raw.assign(buf, n > 0 ? n : 0);
    // Deliver the same bytes twice to the server through the client socket.
    send(c, raw.data(), static_cast<int>(raw.size()), 0);
    send(c, raw.data(), static_cast<int>(raw.size()), 0);
    Check(sv.Receive(got, 2000) == 1 && got == "pay once", __func__, "first copy accepted");
    Check(sv.Receive(got, 2000) == -1, __func__, "replayed copy rejected");
    closesocket(c);
    closesocket(d);
    closesocket(e);
    closesocket(f);
}

void OversizedFrameRejected() {
    SOCKET a, b;
    if (!SocketPair(a, b)) { Check(false, __func__, "socket pair"); return; }
    Channel server(b);
    const char huge[4] = {0x7f, 0x00, 0x00, 0x00};  // claims ~2 GB
    send(a, huge, 4, 0);
    std::string host, err;
    Check(!server.ServerHandshake(kSecret, host, err), __func__, "oversized frame refused");
    closesocket(a);
    closesocket(b);
}

}

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    PrimitivesWork();
    HandshakeAndMessages();
    WrongSecretFails();
    ShortSecretRefused();
    TamperedAndReplayedFramesRejected();
    OversizedFrameRejected();
    WSACleanup();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
