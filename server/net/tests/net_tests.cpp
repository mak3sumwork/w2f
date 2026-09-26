// Tests for the network layer: `make test-net`. Dependency-free runner, same style as the engine tests.
// Layers, bottom up: encoders, WebSocket handshake and frames, command validation, the GameServer (lobby / routing / events /
// privacy, over a fake transport with no sockets), and finally the real TCP server over loopback sockets.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <chrono>

#include "SampleData.h"
#include "w2f/ChampionLoader.h"
#include "w2f/CombatSimulator.h"
#include "w2f/Json.h"
#include "w2f/Rng.h"
#include <fstream>
#include <sstream>
#include "w2f/net/Encoding.h"
#include "w2f/net/Messages.h"
#include "w2f/net/ResumeFile.h"
#include "w2f/net/GameServer.h"
#include "w2f/net/QueueServer.h"
#include "w2f/net/JsonWriter.h"
#include "w2f/net/Protocol.h"
#include "w2f/net/TcpServer.h"
#include "w2f/net/WebSocket.h"

using namespace w2f;
using namespace w2f::net;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(...)                                                                    \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(__VA_ARGS__)) {                                                         \
            ++g_failures;                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #__VA_ARGS__);      \
        }                                                                             \
    } while (0)

// ==== helpers ==================================================================================

static std::string Hex(const std::array<std::uint8_t, 20>& d) { return HexEncode(d.data(), d.size()); }

static json::Value ParseJson(const std::string& text) {
    json::Value v;
    std::string err;
    if (!json::Parse(text, v, &err)) std::printf("  server sent invalid JSON (%s): %.120s\n", err.c_str(), text.c_str());
    return v;
}
static std::string Str(const json::Value& v, const char* key) {
    const json::Value* f = v.Find(key);
    return f && f->IsString() ? f->AsString() : std::string("<none>");
}
static long long Num(const json::Value& v, const char* key, long long fallback = -999999) {
    const json::Value* f = v.Find(key);
    long long n = fallback;
    if (f && f->IsNumber()) f->ToInt(n);
    return n;
}

static std::string Frame(WsOpcode op, std::string_view payload, bool fin = true) {
    static const std::uint8_t kMask[4] = {0x12, 0x34, 0x56, 0x78};
    return EncodeMaskedFrame(op, payload, kMask, fin);
}
static std::vector<WsEvent> FeedAll(WebSocketParser& p, const std::string& bytes) {
    std::vector<WsEvent> out;
    p.Feed(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), out);
    return out;
}

static const char* kValidRequest =
    "GET /?token=abcdef HTTP/1.1\r\nHost: localhost:7777\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

// ==== encoders =================================================================================

static void TestEncoders() {
    CHECK(Hex(Sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(Hex(Sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(Hex(Sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");   // two blocks
    CHECK(Hex(Sha1(std::string(1000, 'a'))) == "291e9a6c66994949b57ba5e650361e98fc36b1ba");
    // Padding edge cases around the one-block / two-block boundaries (55/56 and 119/120 bytes), against Python's hashlib.
    CHECK(Hex(Sha1(std::string(55, 'x'))) == "cef734ba81a024479e09eb5a75b6ddae62e6abf1");
    CHECK(Hex(Sha1(std::string(56, 'x'))) == "901305367c259952f4e7af8323f480d59f81335b");
    CHECK(Hex(Sha1(std::string(63, 'x'))) == "0ddc4e0cccd9a12850deb5abb0853a4425559fec");
    CHECK(Hex(Sha1(std::string(64, 'x'))) == "bb2fa3ee7afb9f54c6dfb5d021f14b1ffe40c163");
    CHECK(Hex(Sha1(std::string(65, 'x'))) == "78c741ddc482e4cdf8c474a0876347a0905b6233");
    CHECK(Hex(Sha1(std::string(119, 'x'))) == "4300320394f7ee239bcdce7d3b8bcee173a0cd5c");
    CHECK(Hex(Sha1(std::string(120, 'x'))) == "ceb2821639c4b6dcb10bce0e522ca2e608ce056d");

    // RFC 6455 section 1.3: the worked handshake example.
    CHECK(AcceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    // RFC 4648 test vectors.
    const std::pair<const char*, const char*> vectors[] = {{"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}};
    for (const auto& [plain, encoded] : vectors) {
        CHECK(Base64Encode(std::string_view(plain)) == encoded);
        std::string back;
        CHECK(Base64Decode(encoded, back) && back == plain);
    }
    std::string sink;
    for (const char* bad : {"Zg=", "Zg", "Z===", "====", "Zm9v!", "Zm=v", "Zg==Zg==", "Zm9v\n", " Zm9v", "Zm9"}) CHECK(!Base64Decode(bad, sink));
    std::string binary;
    for (int i = 0; i < 256; ++i) binary.push_back(static_cast<char>(i));
    CHECK(Base64Decode(Base64Encode(binary), sink) && sink == binary);

    CHECK(HexEncode(reinterpret_cast<const std::uint8_t*>("\x00\xff\x10"), 3) == "00ff10");
    CHECK(IsLowerHex("0123456789abcdef") && !IsLowerHex("ABCDEF") && !IsLowerHex("xyz") && IsLowerHex(""));

    // UTF-8: what is valid and the classic invalid forms.
    CHECK(IsValidUtf8("") && IsValidUtf8("plain ascii") && IsValidUtf8("caf\xC3\xA9") && IsValidUtf8("\xE2\x82\xAC") && IsValidUtf8("\xF0\x9F\x98\x80"));
    CHECK(IsValidUtf8("\xEF\xBF\xBD") && IsValidUtf8("\xF4\x8F\xBF\xBF") && IsValidUtf8("\xED\x9F\xBF"));   // U+FFFD, U+10FFFF, U+D7FF
    for (const char* bad : {"\x80", "\xBF", "\xC0\xAF", "\xC1\xBF", "\xE0\x80\x80", "\xF0\x80\x80\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80",
                            "\xC3", "\xE2\x82", "\xF0\x9F\x98", "\xC3\x28", "\xFF", "\xFE", "a\xC3"}) {
        CHECK(!IsValidUtf8(std::string_view(bad)));
    }
}

// ==== handshake ================================================================================

static void TestHandshake() {
    const HandshakeResult ok = ParseHandshake(kValidRequest);
    CHECK(ok.status == HandshakeStatus::Ok && ok.path == "/" && ok.query == "token=abcdef" && ok.key == "dGhlIHNhbXBsZSBub25jZQ==" && ok.consumed == std::strlen(kValidRequest));
    CHECK(QueryParam(ok.query, "token") == "abcdef" && QueryParam("a=1&token=x&b=2", "token") == "x" && QueryParam("a=1", "token").empty() && QueryParam("", "token").empty());
    const std::string response = BuildHandshakeResponse(ok.key);
    CHECK(response.find("HTTP/1.1 101 Switching Protocols\r\n") == 0 && response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos);
    CHECK(response.size() >= 4 && response.substr(response.size() - 4) == "\r\n\r\n");

    // Delivered one byte at a time: NeedMore until the blank line, then Ok.
    const std::string req = kValidRequest;
    bool needMoreUntilEnd = true;
    for (std::size_t n = 1; n < req.size(); ++n) needMoreUntilEnd = needMoreUntilEnd && ParseHandshake(std::string_view(req).substr(0, n)).status == HandshakeStatus::NeedMore;
    CHECK(needMoreUntilEnd);

    // Bytes after the request (the client's first frame, coalesced into the same segment) are left for the frame parser.
    const std::string withFrame = req + "\x81\x80XXXX";
    const HandshakeResult coalesced = ParseHandshake(withFrame);
    CHECK(coalesced.status == HandshakeStatus::Ok && coalesced.consumed == req.size());

    // Real clients vary: header case, extra Connection tokens, lowercase upgrade, no Host, no query, a path.
    CHECK(ParseHandshake("GET /ws HTTP/1.1\r\nupgrade: WebSocket\r\nCONNECTION: keep-alive, Upgrade\r\nsec-websocket-key: dGhlIHNhbXBsZSBub25jZQ==\r\nSEC-WEBSOCKET-VERSION: 13\r\n\r\n").status == HandshakeStatus::Ok);

    struct Case { std::string request; int status; };
    const std::string base = "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n";
    const Case bad[] = {
        {"POST / HTTP/1.1\r\n" + base + "\r\n", 405},
        {"GET / HTTP/1.0\r\n" + base + "\r\n", 400},
        {"GET / HTTP/1.1 extra\r\n" + base + "\r\n", 400},
        {"GET  / HTTP/1.1\r\n" + base + "\r\n", 400},
        {"GET noslash HTTP/1.1\r\n" + base + "\r\n", 400},
        {"GET / HTTP/1.1\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", 400},   // no Upgrade
        {"GET / HTTP/1.1\r\nUpgrade: websocket\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", 400},      // no Connection
        {"GET / HTTP/1.1\r\nUpgrade: h2c\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n\r\n", 400},                                   // no key
        {"GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: c2hvcnQ=\r\nSec-WebSocket-Version: 13\r\n\r\n", 400},  // key decodes to 5 bytes
        {"GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: !!!!!!!!!!!!!!!!!!!!!!==\r\nSec-WebSocket-Version: 13\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n", 400},               // no version
        {"GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 8\r\n\r\n", 426},
        {"GET / HTTP/1.1\r\n" + base + "Sec-WebSocket-Version: 13\r\n\r\n", 400},                        // duplicated version
        {"GET / HTTP/1.1\r\n" + base + "Content-Length: 5\r\n\r\nhello", 400},                             // a body
        {"GET / HTTP/1.1\r\n" + base + "Transfer-Encoding: chunked\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\n" + base + "no colon here\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\n" + base + "Bad Name: x\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\n" + base + "X: a\nb\r\n\r\n", 400},                                            // bare LF inside a line
        {std::string("GET / HTTP/1.1\r\n") + base + "X: a\x01" "b\r\n\r\n", 400},                          // control character
        {std::string("GET / HTTP/1.1\r\n") + base + "X: a" + std::string(1, '\0') + "b\r\n\r\n", 400},
        {"\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\n" + base + std::string(9000, 'x') + "\r\n\r\n", 431},                          // too large (blank line found late)
        {"GET / HTTP/1.1\r\n" + base + "X: " + std::string(9000, 'y'), 431},                                // too large and no blank line yet
    };
    int index = 0;
    for (const Case& c : bad) {
        const HandshakeResult r = ParseHandshake(c.request);
        const bool matches = r.status == HandshakeStatus::Bad && r.httpStatus == c.status;
        if (!matches) std::printf("  handshake case %d: status %d / http %d, wanted Bad / %d\n", index, static_cast<int>(r.status), r.httpStatus, c.status);
        CHECK(matches);
        ++index;
    }
    // 64 headers are fine, 65 are not.
    std::string many = "GET / HTTP/1.1\r\n" + base;
    for (int i = 0; i < 59; ++i) many += "X-" + std::to_string(i) + ": v\r\n";
    CHECK(ParseHandshake(many + "\r\n").status == HandshakeStatus::Ok);
    for (int i = 0; i < 10; ++i) many += "Y-" + std::to_string(i) + ": v\r\n";
    CHECK(ParseHandshake(many + "\r\n").status == HandshakeStatus::Bad);

    const std::string err = BuildHttpError(426, "unsupported WebSocket version");
    CHECK(err.find("HTTP/1.1 426 Upgrade Required\r\n") == 0 && err.find("Sec-WebSocket-Version: 13") != std::string::npos && err.find("Connection: close") != std::string::npos);
    CHECK(BuildHttpError(400, "line\r\nbreak").find("X-Reason: line??break") != std::string::npos);   // a reason can never inject headers
}

// ==== frames ===================================================================================

static void TestFrames() {
    {   // RFC 6455 section 5.7: a single-frame masked text message "Hello".
        WebSocketParser p(1024);
        const std::string bytes("\x81\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58", 11);
        const auto ev = FeedAll(p, bytes);
        CHECK(ev.size() == 1 && ev[0].kind == WsEvent::Kind::Text && ev[0].payload == "Hello" && !p.dead());
    }
    {   // ... and the fragmented example ("Hel" + "lo"), and a masked Ping with "Hello".
        WebSocketParser p(1024);
        const auto ev = FeedAll(p, Frame(WsOpcode::Text, "Hel", false) + Frame(WsOpcode::Continuation, "lo"));
        CHECK(ev.size() == 1 && ev[0].kind == WsEvent::Kind::Text && ev[0].payload == "Hello");
        const auto ping = FeedAll(p, std::string("\x89\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58", 11));
        CHECK(ping.size() == 1 && ping[0].kind == WsEvent::Kind::Ping && ping[0].payload == "Hello");
    }
    {   // Server frames are unmasked, and the three length encodings round-trip through a client-side reading.
        CHECK(EncodeFrame(WsOpcode::Text, "Hello") == std::string("\x81\x05Hello", 7));
        const std::string mid = EncodeFrame(WsOpcode::Text, std::string(300, 'a'));
        CHECK(static_cast<std::uint8_t>(mid[1]) == 126 && static_cast<std::uint8_t>(mid[2]) == 1 && static_cast<std::uint8_t>(mid[3]) == 44 && mid.size() == 304);
        const std::string big = EncodeFrame(WsOpcode::Text, std::string(70000, 'a'));
        CHECK(static_cast<std::uint8_t>(big[1]) == 127 && big.size() == 70010);
        CHECK(EncodeClose(1000, "bye") == std::string("\x88\x05\x03\xe8" "bye", 7));
        // Masked client frames of every length class parse back to the payload.
        for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{125}, std::size_t{126}, std::size_t{127}, std::size_t{65535}, std::size_t{65536}, std::size_t{100000}}) {
            WebSocketParser p(200000);
            const std::string payload(n, 'q');
            const auto ev = FeedAll(p, Frame(WsOpcode::Text, payload));
            CHECK(ev.size() == 1 && ev[0].kind == WsEvent::Kind::Text && ev[0].payload == payload);
        }
    }
    {   // One byte at a time, and many frames in a single feed.
        WebSocketParser p(1024);
        const std::string a = Frame(WsOpcode::Text, "one"), b = Frame(WsOpcode::Ping, "p"), c = Frame(WsOpcode::Text, "two");
        std::vector<WsEvent> ev;
        for (char ch : a + b + c) {
            std::vector<WsEvent> part = FeedAll(p, std::string(1, ch));
            for (WsEvent& e : part) ev.push_back(std::move(e));
        }
        CHECK(ev.size() == 3 && ev[0].payload == "one" && ev[1].kind == WsEvent::Kind::Ping && ev[2].payload == "two");
        WebSocketParser q(1024);
        const auto all = FeedAll(q, a + b + c);
        CHECK(all.size() == 3);
    }
    {   // A control frame may arrive in the middle of a fragmented message.
        WebSocketParser p(1024);
        const auto ev = FeedAll(p, Frame(WsOpcode::Text, "he", false) + Frame(WsOpcode::Ping, "!") + Frame(WsOpcode::Continuation, "llo", true));
        CHECK(ev.size() == 2 && ev[0].kind == WsEvent::Kind::Ping && ev[1].kind == WsEvent::Kind::Text && ev[1].payload == "hello");
    }
    {   // Close: with and without a code; the parser stops afterwards.
        WebSocketParser p(1024);
        std::string closePayload = "\x03\xe8" "done";
        const auto ev = FeedAll(p, Frame(WsOpcode::Close, closePayload));
        CHECK(ev.size() == 1 && ev[0].kind == WsEvent::Kind::Close && ev[0].code == 1000 && ev[0].reason == "done" && p.dead());
        WebSocketParser q(1024);
        const auto none = FeedAll(q, Frame(WsOpcode::Close, ""));
        CHECK(none.size() == 1 && none[0].kind == WsEvent::Kind::Close && none[0].code == 1005);
        CHECK(FeedAll(q, Frame(WsOpcode::Text, "late")).empty());   // nothing after a close
    }

    // Everything a peer can do wrong, with the close code the RFC requires.
    struct Bad { const char* name; std::string bytes; std::uint16_t code; };
    const std::string unmasked = std::string("\x81\x05Hello", 7);
    std::string reserved = Frame(WsOpcode::Text, "x");
    reserved[0] = static_cast<char>(reserved[0] | 0x40);
    std::string unknownOp = Frame(WsOpcode::Text, "x");
    unknownOp[0] = static_cast<char>((unknownOp[0] & 0xF0) | 0x03);
    std::string unknownControl = Frame(WsOpcode::Text, "x");
    unknownControl[0] = static_cast<char>((unknownControl[0] & 0xF0) | 0x0B);
    std::string pingTooBig = Frame(WsOpcode::Ping, std::string(126, 'p'));
    std::string nonMinimal16 = std::string("\x81\xFE\x00\x05", 4) + std::string("XXXX", 4) + "hello";
    std::string topBit64 = std::string("\x81\xFF\x80\x00\x00\x00\x00\x00\x00\x01", 10) + "XXXX";
    std::string nonMinimal64 = std::string("\x81\xFF\x00\x00\x00\x00\x00\x00\x00\x05", 10) + "XXXX" + "hello";
    const Bad bad[] = {
        {"unmasked frame", unmasked, 1002},
        {"reserved bit", reserved, 1002},
        {"unknown data opcode", unknownOp, 1002},
        {"unknown control opcode", unknownControl, 1002},
        {"oversized control frame", pingTooBig, 1002},
        {"fragmented control frame", Frame(WsOpcode::Ping, "x", false), 1002},
        {"continuation with nothing to continue", Frame(WsOpcode::Continuation, "x"), 1002},
        {"new message inside a fragmented one", Frame(WsOpcode::Text, "a", false) + Frame(WsOpcode::Text, "b"), 1002},
        {"non-minimal 16-bit length", nonMinimal16, 1002},
        {"64-bit length with the top bit set", topBit64, 1002},
        {"non-minimal 64-bit length", nonMinimal64, 1002},
        {"binary message", Frame(WsOpcode::Binary, "x"), 1003},
        {"message over the limit", Frame(WsOpcode::Text, std::string(2000, 'a')), 1009},
        {"fragments adding up over the limit", Frame(WsOpcode::Text, std::string(600, 'a'), false) + Frame(WsOpcode::Continuation, std::string(600, 'a')), 1009},
        {"invalid UTF-8", Frame(WsOpcode::Text, "\xC3\x28"), 1007},
        {"UTF-8 split across fragments and never completed", Frame(WsOpcode::Text, "\xE2\x82", false) + Frame(WsOpcode::Continuation, "x"), 1007},
        {"close with a one-byte payload", Frame(WsOpcode::Close, "x"), 1002},
        {"close with an invalid code", Frame(WsOpcode::Close, std::string("\x03\xed", 2)), 1002},   // 1005 must not be sent
        {"close with code 0", Frame(WsOpcode::Close, std::string("\x00\x00", 2)), 1002},
        {"close with a non-UTF-8 reason", Frame(WsOpcode::Close, std::string("\x03\xe8\xff", 3)), 1007},
    };
    for (const Bad& b : bad) {
        WebSocketParser p(1024);
        const auto ev = FeedAll(p, b.bytes);
        const bool ok = !ev.empty() && ev.back().kind == WsEvent::Kind::Error && ev.back().code == b.code && p.dead();
        if (!ok) std::printf("  frame case '%s': got %zu events, last kind %d code %d, wanted error %d\n", b.name, ev.size(), ev.empty() ? -1 : static_cast<int>(ev.back().kind), ev.empty() ? 0 : ev.back().code, b.code);
        CHECK(ok);
        CHECK(FeedAll(p, Frame(WsOpcode::Text, "x")).empty());   // a failed stream stays failed
    }
    {   // The size limit is enforced from the HEADER: a 1 GiB frame is refused as soon as its 10 header bytes arrive, without
        // waiting for (or buffering) the payload.
        WebSocketParser p(1024);
        std::string header = std::string("\x81\xFF", 2);
        const std::uint64_t huge = 1ull << 30;
        for (int i = 7; i >= 0; --i) header.push_back(static_cast<char>((huge >> (8 * i)) & 0xFF));
        header += "XXXX";
        const auto ev = FeedAll(p, header);
        CHECK(ev.size() == 1 && ev[0].kind == WsEvent::Kind::Error && ev[0].code == 1009);
    }
    {   // A partial frame followed by nothing just waits (no events, no error).
        WebSocketParser p(1024);
        const std::string f = Frame(WsOpcode::Text, "hello world");
        CHECK(FeedAll(p, f.substr(0, f.size() - 3)).empty() && !p.dead());
        const auto rest = FeedAll(p, f.substr(f.size() - 3));
        CHECK(rest.size() == 1 && rest[0].payload == "hello world");
    }
    {   // Fuzz: random bytes, and random corruptions of valid traffic, never crash and always end in a defined state.
        Rng rng(1234);
        int errors = 0, texts = 0;
        for (int i = 0; i < 4000; ++i) {
            WebSocketParser p(512);
            std::string bytes;
            if (i % 2 == 0) {
                const std::size_t n = rng.NextBelow(64);
                for (std::size_t k = 0; k < n; ++k) bytes.push_back(static_cast<char>(rng.NextBelow(256)));
            } else {
                bytes = Frame(WsOpcode::Text, "{\"action\":\"ping\"}") + Frame(WsOpcode::Ping, "x") + Frame(WsOpcode::Text, "again");
                for (int m = 0; m < 3; ++m) bytes[rng.NextBelow(static_cast<std::uint32_t>(bytes.size()))] = static_cast<char>(rng.NextBelow(256));
            }
            std::vector<WsEvent> ev;
            for (std::size_t k = 0; k < bytes.size(); k += 1 + rng.NextBelow(9)) {
                const std::size_t n = std::min<std::size_t>(1 + rng.NextBelow(9), bytes.size() - k);
                p.Feed(reinterpret_cast<const std::uint8_t*>(bytes.data() + k), n, ev);
            }
            for (const WsEvent& e : ev) {
                errors += e.kind == WsEvent::Kind::Error ? 1 : 0;
                texts += e.kind == WsEvent::Kind::Text ? 1 : 0;
                if (e.kind == WsEvent::Kind::Text) CHECK(IsValidUtf8(e.payload));
            }
        }
        CHECK(errors > 500 && texts > 100);
    }
}

// ==== command validation ========================================================================

static void TestCommandParsing() {
    struct Good { const char* json; CommandType type; };
    const Good good[] = {
        {R"({"action": "buy_unit", "shop_index": 2})", CommandType::BuyUnit},
        {R"({"action":"buy_unit","shop_index":0,"id":17})", CommandType::BuyUnit},
        {R"({"id": 0, "action": "reroll_shop"})", CommandType::RerollShop},
        {R"({"action": "buy_xp"})", CommandType::BuyXp},
        {R"({"action": "pick_gift", "gift_index": 0})", CommandType::PickGift},
        {R"({"id": 4, "action": "pick_gift", "gift_index": 3})", CommandType::PickGift},
        {R"({"action": "sell_unit", "unit_id": 16777217})", CommandType::SellUnit},
        {R"({"action": "move_unit", "unit_id": 5, "location": "bench", "x": 8})", CommandType::MoveUnit},
        {R"({"action": "move_unit", "unit_id": 5, "location": "bench", "x": 0, "y": 0})", CommandType::MoveUnit},
        {R"({"action": "move_unit", "unit_id": 5, "location": "board", "x": 6, "y": 3})", CommandType::MoveUnit},
        {R"({"action": "equip_item", "unit_id": 5, "item_id": 3})", CommandType::EquipItem},
        {R"({"action": "unequip_item", "unit_id": 5, "slot": 2})", CommandType::UnequipItem},
        {R"({"action": "get_state"})", CommandType::GetState},
        {R"({"action": "get_fight", "fight_index": 7})", CommandType::GetFight},
        {R"({"action": "ping", "id": 9007199254740991})", CommandType::Ping},
        {"  \n{\"action\":\"ping\"}\t ", CommandType::Ping},
        {R"({"action": "buy_unit", "shop_index": 2.0})", CommandType::BuyUnit},   // a whole number written with a decimal point
        {R"({"action": "buy_unit", "shop_index": 2e0})", CommandType::BuyUnit},
        {R"({"action": "sell_unit", "unit_id": 4294967295})", CommandType::SellUnit},
    };
    for (const Good& g : good) {
        const ParseResult r = ParseCommand(g.json);
        if (!r.ok) std::printf("  rejected a valid command %s: %s / %s\n", g.json, r.error.code.c_str(), r.error.detail.c_str());
        CHECK(r.ok && r.command.type == g.type);
    }
    {
        const ParseResult r = ParseCommand(R"({"action": "pick_gift", "gift_index": 2, "id": 8})");
        CHECK(r.ok && r.command.type == CommandType::PickGift && r.command.giftIndex == 2 && r.command.hasId && r.command.id == 8);
    }
    {   // The fields land where they should.
        const ParseResult r = ParseCommand(R"({"action": "move_unit", "unit_id": 33554433, "location": "board", "x": 4, "y": 2, "id": 41})");
        CHECK(r.ok && r.command.unit == 33554433u && r.command.location == LocationType::Board && r.command.x == 4 && r.command.y == 2 && r.command.hasId && r.command.id == 41);
        const ParseResult b = ParseCommand(R"({"action": "move_unit", "unit_id": 3, "location": "bench", "x": 5})");
        CHECK(b.ok && b.command.location == LocationType::Bench && b.command.x == 5 && b.command.y == 0 && !b.command.hasId);
        const ParseResult e = ParseCommand(R"({"action": "equip_item", "unit_id": 3, "item_id": 3000000000})");
        CHECK(e.ok && e.command.item == 3000000000u);
    }

    struct Bad { const char* json; const char* code; };
    const std::string tooLong = std::string(R"({"action": "ping", "id": 1, "pad": ")") + std::string(5000, 'x') + "\"}";
    const std::string deep = std::string(200, '[') + std::string(200, ']');
    const Bad bad[] = {
        {"", "invalid_json"}, {"   ", "invalid_json"}, {"not json", "invalid_json"}, {"{", "invalid_json"}, {R"({"action": "ping",})", "invalid_json"},
        {R"({"action": "ping"} trailing)", "invalid_json"}, {R"({"action": "ping", "action": "ping"})", "invalid_json"},   // duplicate keys
        {"[1,2]", "not_an_object"}, {"7", "not_an_object"}, {"\"buy_unit\"", "not_an_object"}, {"null", "not_an_object"}, {"true", "not_an_object"},
        {"{}", "missing_action"}, {R"({"id": 1})", "missing_action"},
        {R"({"action": 5})", "wrong_type"}, {R"({"action": null})", "wrong_type"}, {R"({"action": ["ping"]})", "wrong_type"},
        {R"({"action": "teleport"})", "unknown_action"}, {R"({"action": ""})", "unknown_action"}, {R"({"action": "BUY_UNIT", "shop_index": 1})", "unknown_action"},
        {R"({"action": "buy_unit"})", "missing_field"}, {R"({"action": "sell_unit"})", "missing_field"}, {R"({"action": "pick_gift"})", "missing_field"},
        {R"({"action": "pick_gift", "gift_index": -1})", "out_of_range"}, {R"({"action": "pick_gift", "gift_index": 4})", "out_of_range"},
        {R"({"action": "pick_gift", "gift_index": "0"})", "wrong_type"}, {R"({"action": "pick_gift", "gift_index": 0.5})", "wrong_type"},
        {R"({"action": "pick_gift", "gift_index": 0, "shop_index": 0})", "unknown_field"}, {R"({"action": "pick_gift", "index": 0})", "unknown_field"},
        {R"({"action": "move_unit", "unit_id": 5, "x": 1, "y": 1})", "missing_field"},
        {R"({"action": "move_unit", "unit_id": 5, "location": "board", "x": 1})", "missing_field"},
        {R"({"action": "equip_item", "unit_id": 5})", "missing_field"},
        {R"({"action": "buy_unit", "shop_index": "2"})", "wrong_type"}, {R"({"action": "buy_unit", "shop_index": null})", "wrong_type"},
        {R"({"action": "buy_unit", "shop_index": true})", "wrong_type"}, {R"({"action": "buy_unit", "shop_index": [2]})", "wrong_type"},
        {R"({"action": "buy_unit", "shop_index": 1.5})", "wrong_type"}, {R"({"action": "buy_unit", "shop_index": 1e300})", "invalid_json"},
        {R"({"action": "buy_unit", "shop_index": -1})", "out_of_range"}, {R"({"action": "buy_unit", "shop_index": 64})", "out_of_range"},
        {R"({"action": "sell_unit", "unit_id": 0})", "out_of_range"}, {R"({"action": "sell_unit", "unit_id": 4294967296})", "out_of_range"},
        {R"({"action": "sell_unit", "unit_id": -5})", "out_of_range"}, {R"({"action": "sell_unit", "unit_id": 99999999999999999999})", "invalid_json"},
        {R"({"action": "move_unit", "unit_id": 5, "location": "sky", "x": 1, "y": 1})", "out_of_range"},
        {R"({"action": "move_unit", "unit_id": 5, "location": 3, "x": 1, "y": 1})", "wrong_type"},
        {R"({"action": "move_unit", "unit_id": 5, "location": "bench", "x": 9})", "out_of_range"},
        {R"({"action": "move_unit", "unit_id": 5, "location": "bench", "x": 1, "y": 1})", "out_of_range"},
        {R"({"action": "move_unit", "unit_id": 5, "location": "board", "x": 7, "y": 0})", "out_of_range"},
        {R"({"action": "move_unit", "unit_id": 5, "location": "board", "x": 0, "y": 4})", "out_of_range"},
        {R"({"action": "move_unit", "unit_id": 5, "location": "board", "x": -1, "y": 0})", "out_of_range"},
        {R"({"action": "equip_item", "unit_id": 5, "item_id": 0})", "out_of_range"},
        {R"({"action": "unequip_item", "unit_id": 5, "slot": 3})", "out_of_range"}, {R"({"action": "unequip_item", "unit_id": 5, "slot": -1})", "out_of_range"},
        {R"({"action": "get_fight", "fight_index": 8})", "out_of_range"},
        {R"({"action": "ping", "id": -1})", "wrong_type"}, {R"({"action": "ping", "id": 9007199254740992})", "wrong_type"},
        {R"({"action": "ping", "id": "7"})", "wrong_type"}, {R"({"action": "ping", "id": 1.5})", "wrong_type"},
        {R"({"action": "ping", "extra": 1})", "unknown_field"}, {R"({"action": "buy_unit", "shop_index": 1, "shop_idx": 2})", "unknown_field"},
        {R"({"action": "buy_unit", "shop_index": 1, "unit_id": 2})", "unknown_field"}, {R"({"action": "reroll_shop", "shop_index": 1})", "unknown_field"},
        {R"({"Action": "ping"})", "missing_action"},
    };
    for (const Bad& b : bad) {
        const ParseResult r = ParseCommand(b.json);
        const bool ok = !r.ok && r.error.code == b.code && !r.error.detail.empty();
        if (!ok) std::printf("  '%s': ok=%d code='%s' (wanted '%s')\n", b.json, r.ok, r.error.code.c_str(), b.code);
        CHECK(ok);
    }
    CHECK(ParseCommand(tooLong).error.code == "too_large");
    CHECK(ParseCommand(deep).error.code == "invalid_json");     // nesting depth is capped by the parser
    CHECK(ParseCommand(std::string("{\"action\":\"ping\"}\0garbage", 24)).ok == false);
    // The id survives on a failing command so the client can match the error to its request.
    const ParseResult withId = ParseCommand(R"({"action": "buy_unit", "shop_index": 99, "id": 12})");
    CHECK(!withId.ok && withId.hasId && withId.id == 12);
    // A hostile action name is truncated in the error text, never echoed whole.
    const ParseResult longName = ParseCommand(std::string(R"({"action": ")") + std::string(1000, 'z') + "\"}");
    CHECK(!longName.ok && longName.error.detail.size() < 100);

    {   // Fuzz: mutate valid commands byte by byte. Whatever comes back, ParseCommand returns a definite verdict and never crashes.
        Rng rng(99);
        int accepted = 0, rejected = 0;
        for (int i = 0; i < 20000; ++i) {
            std::string s = good[rng.NextBelow(static_cast<std::uint32_t>(sizeof(good) / sizeof(good[0])))].json;
            const int edits = 1 + static_cast<int>(rng.NextBelow(4));
            for (int e = 0; e < edits && !s.empty(); ++e) {
                const std::size_t at = rng.NextBelow(static_cast<std::uint32_t>(s.size()));
                switch (rng.NextBelow(4)) {
                    case 0: s[at] = static_cast<char>(rng.NextBelow(256)); break;
                    case 1: s.erase(at, 1); break;
                    case 2: s.insert(at, 1, "{}[]\":,-.0123456789 "[rng.NextBelow(21)]); break;
                    default: s.resize(at); break;
                }
            }
            const ParseResult r = ParseCommand(s);
            (r.ok ? accepted : rejected)++;
            if (r.ok) CHECK(r.command.shopIndex >= 0 && r.command.shopIndex <= 63 && r.command.x >= 0 && r.command.x <= 8 && r.command.y >= 0 && r.command.y <= 3 &&
                            r.command.slot >= 0 && r.command.slot <= 2 && r.command.fightIndex >= 0 && r.command.fightIndex <= 7);
        }
        CHECK(accepted > 100 && rejected > 1000);
    }
}

static void TestJsonWriter() {
    JsonWriter w;
    w.BeginObject().Field("a", 1).Field("b", "x\"y\\z\n\t\x01").Key("c").BeginArray().Int(1).Null().Bool(true).BeginObject().Field("k", 2u).EndObject().EndArray().Field("d", false).EndObject();
    CHECK(w.str() == R"({"a":1,"b":"x\"y\\z\n\t\u0001","c":[1,null,true,{"k":2}],"d":false})");
    json::Value v = ParseJson(w.str());
    CHECK(v.IsObject() && Num(v, "a") == 1 && Str(v, "b") == "x\"y\\z\n\t\x01");
    JsonWriter empty;
    empty.BeginObject().Key("a").BeginArray().EndArray().Key("o").BeginObject().EndObject().EndObject();
    CHECK(empty.str() == R"({"a":[],"o":{}})");
    JsonWriter utf;
    utf.BeginArray().String("caf\xC3\xA9").EndArray();
    CHECK(utf.str() == "[\"caf\xC3\xA9\"]");
}

// ==== GameServer over a fake transport (no sockets) =============================================

namespace {

struct FakeTransport : IServerTransport {
    struct Closed { std::uint16_t code = 0; std::string reason; };
    std::map<ConnectionId, std::vector<std::string>> sent;
    std::map<ConnectionId, Closed> closed;
    void Send(ConnectionId id, std::string_view text) override {
        if (closed.count(id)) return;   // like the real transport: nothing is sent to a connection that is closing
        sent[id].push_back(std::string(text));
    }
    void Close(ConnectionId id, std::uint16_t code, std::string_view reason) override {
        if (!closed.count(id)) closed[id] = Closed{code, std::string(reason)};
    }
};

struct Rig {
    std::unique_ptr<ChampionDatabase> champions = sample::MakeCombatDatabase();
    std::unique_ptr<ItemDatabase> items;
    std::unique_ptr<TraitDatabase> traits = sample::LoadProductionTraits();
    std::unique_ptr<EncounterDatabase> encounters;
    std::unique_ptr<TextTable> display = TextTable::FromFile(sample::ProductionTextPath());
    GameData data;
    GameServerConfig cfg;
    FakeTransport net;
    std::unique_ptr<GameServer> server;
    std::uint64_t now = 1000;
    ConnectionId nextConn = 0;
    std::uint64_t entropyCounter = 0;

    explicit Rig(int seats, const std::function<void(GameData&, GameServerConfig&)>& tweak = {}, const std::string& itemsPath = sample::ProductionItemsPath()) {
        std::string err;
        items = w2f::LoadItemDatabaseFromFile(itemsPath, &err);
        encounters = w2f::LoadEncounterDatabaseFromFile(sample::ProductionPvePath(), champions.get(), items.get(), &err);
        if (!champions || !items || !traits || !encounters) std::printf("  Rig data failed to load: %s\n", err.c_str());
        data.champions = champions.get();
        data.items = items.get();
        data.traits = traits.get();
        data.encounters = encounters.get();
        data.text = display.get();
        data.config.match.motherNatureTicks = 30;
        data.config.match.planningTicks = 90;
        data.config.match.combatTicks = Seconds(125);   // the real maximum: a fight that a shorter phase cut off would be decided by the tie-break, not fought to the end
        data.config.match.resolutionTicks = 30;
        data.config.player.startingGold = 20;
        data.config.match.shopClosedOpeningRounds = 0;   // the protocol tests want a shop from round 1 and a free-form board;
        data.config.match.openingUnitCosts.clear();      // TestOpeningAndBoardLimit runs the real defaults
        data.config.player.limitBoardToLevel = false;
        cfg.seats = seats;
        cfg.seed = 12345;
        cfg.rateBurst = 100;   // roomy by default: the tests that are about rate limiting set their own
        cfg.entropy = [this] {   // deterministic, well-mixed, never repeating
            std::uint64_t z = (entropyCounter += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        };
        if (tweak) tweak(data, cfg);
        server = std::make_unique<GameServer>(cfg, data, net);
    }
    ConnectionId Connect(const std::string& token = "") {
        const ConnectionId id = ++nextConn;
        server->OnConnect(id, token, now);
        return id;
    }
    void Say(ConnectionId id, const std::string& text) { server->OnMessage(id, text, now); }
    void Tick(int n = 1) {
        for (int i = 0; i < n; ++i) {
            now += 33;
            server->Tick(now);
        }
    }
    std::vector<json::Value> Inbox(ConnectionId id) {
        std::vector<json::Value> out;
        for (const std::string& s : net.sent[id]) out.push_back(ParseJson(s));
        return out;
    }
    std::vector<std::string> Types(ConnectionId id) {
        std::vector<std::string> out;
        for (const json::Value& v : Inbox(id)) out.push_back(Str(v, "type"));
        return out;
    }
    // The last message of `type` this connection received (a null Value if none).
    json::Value Last(ConnectionId id, const std::string& type) {
        json::Value found;
        for (const json::Value& v : Inbox(id)) {
            if (Str(v, "type") == type) found = v;
        }
        return found;
    }
    int CountType(ConnectionId id, const std::string& type) {
        int n = 0;
        for (const json::Value& v : Inbox(id)) n += Str(v, "type") == type ? 1 : 0;
        return n;
    }
    const MatchManager& Match() { return *server->match(); }
    bool RunUntil(const std::function<bool()>& done, int maxTicks) {
        for (int i = 0; i < maxTicks; ++i) {
            if (done()) return true;
            Tick();
        }
        return done();
    }
};

bool IsPlanning(Rig& r) { return r.server->match() && r.Match().Phase() == MatchPhase::Planning; }

}  // namespace

static void TestLobby() {
    Rig r(3);
    CHECK(r.server->state() == GameServer::State::Lobby && r.server->match() == nullptr && r.server->seats() == 3);

    const ConnectionId a = r.Connect();
    CHECK(r.Types(a) == std::vector<std::string>({"welcome", "lobby"}));
    const json::Value wa = r.Last(a, "welcome");
    CHECK(Num(wa, "player_id") == 0 && Num(wa, "protocol") == kProtocolVersion && Num(wa, "protocol_revision") == kProtocolRevision && Num(wa, "seats") == 3 && Num(wa, "connected") == 1 &&
          wa.Find("reconnected")->AsBool() == false && wa.Find("match_running")->AsBool() == false);
    const std::string tokenA = Str(wa, "token");
    CHECK(tokenA.size() == 32 && IsLowerHex(tokenA));
    const json::Value lobby = r.Last(a, "lobby");
    long long firstPlayer = -1;
    CHECK(Num(lobby, "connected") == 1 && lobby.Find("players")->Items().size() == 1 && lobby.Find("players")->Items()[0].ToInt(firstPlayer) && firstPlayer == 0);

    const ConnectionId b = r.Connect();
    CHECK(Num(r.Last(b, "welcome"), "player_id") == 1 && Str(r.Last(b, "welcome"), "token") != tokenA);
    CHECK(Num(r.Last(a, "lobby"), "connected") == 2);   // the first player is told somebody joined

    // Before the match, only ping is available.
    r.Say(a, R"({"action": "ping", "id": 5})");
    CHECK(Str(r.Last(a, "pong"), "type") == "pong" && Num(r.Last(a, "pong"), "id") == 5);
    r.Say(a, R"({"action": "buy_unit", "shop_index": 0, "id": 6})");
    const json::Value early = r.Last(a, "error");
    CHECK(Str(early, "code") == "not_in_match" && Num(early, "id") == 6);
    r.Tick(50);   // ticking a lobby does nothing
    CHECK(r.server->state() == GameServer::State::Lobby && r.server->tickCount() == 0);

    // A player leaving frees the seat; the next one gets the lowest free id, and the leaver's token is dead.
    const std::string tokenB = Str(r.Last(b, "welcome"), "token");
    r.server->OnDisconnect(b);
    CHECK(r.server->connectedPlayers() == 1 && Num(r.Last(a, "lobby"), "connected") == 1);
    const ConnectionId c = r.Connect();
    CHECK(Num(r.Last(c, "welcome"), "player_id") == 1);
    r.server->OnDisconnect(c);
    const ConnectionId ghostReturn = r.Connect(tokenB);
    const json::Value wg = r.Last(ghostReturn, "welcome");
    CHECK(Num(wg, "player_id") == 1 && wg.Find("reconnected")->AsBool() == false && Str(wg, "token") != tokenB);   // a fresh seat, not the old one

    // The last seat fills: the match starts by itself, and every player is told who they are.
    CHECK(r.server->state() == GameServer::State::Lobby);
    const ConnectionId d = r.Connect();
    CHECK(Num(r.Last(d, "welcome"), "player_id") == 2);
    CHECK(r.server->state() == GameServer::State::Running && r.server->match() != nullptr && r.server->connectedPlayers() == 3);
    for (ConnectionId id : {a, ghostReturn, d}) {
        const auto types = r.Types(id);
        CHECK(std::count(types.begin(), types.end(), "match_started") == 1 && std::count(types.begin(), types.end(), "phase") >= 1);
        CHECK(std::count(types.begin(), types.end(), "state") >= 1 && std::count(types.begin(), types.end(), "public_state") >= 1);
    }
    const json::Value started = r.Last(ghostReturn, "match_started");
    CHECK(Num(started, "player_id") == 1 && Num(started, "seats") == 3 && Num(started, "tick_rate") == kTicksPerSecond &&
          Num(*started.Find("phase_ticks"), "planning") == 90 && Num(*started.Find("board"), "columns") == kBoardColumns &&
          started.Find("combat_event_types")->Items().size() == 16);

    // The match is full of its own players: a 4th connection is turned away, and the seat holders are unaffected.
    const ConnectionId late = r.Connect();
    CHECK(Str(r.Last(late, "error"), "code") == "match_in_progress" && r.net.closed.count(late) && r.net.closed[late].code == 1013);
    CHECK(r.server->connectedPlayers() == 3);
    r.server->OnDisconnect(late);
    CHECK(r.server->connectedPlayers() == 3);
    // Ticking a running match runs the engine.
    r.Tick(5);
    CHECK(r.server->tickCount() == 5 && r.Match().TicksInPhase() == 5);
}

static void TestReconnect() {
    Rig r(2);
    const ConnectionId a = r.Connect();
    const ConnectionId b = r.Connect();
    const std::string tokenA = Str(r.Last(a, "welcome"), "token");
    const std::string tokenB = Str(r.Last(b, "welcome"), "token");
    CHECK(r.server->state() == GameServer::State::Running);
    r.Tick(40);   // well inside round 1's Planning

    // A drops. The seat stays reserved and the match carries on without them.
    r.server->OnDisconnect(a);
    CHECK(r.server->connectedPlayers() == 1);
    r.Tick(3);
    const ConnectionId stranger = r.Connect();
    CHECK(Str(r.Last(stranger, "error"), "code") == "match_in_progress" && r.net.closed[stranger].code == 1013);
    r.server->OnDisconnect(stranger);
    // Tokens that are not the real one never grant a seat: wrong length, wrong alphabet, unknown but well-formed.
    for (const std::string& fake : {std::string("abc"), std::string(32, 'A'), std::string(32, 'z'), std::string(31, 'a'), std::string(33, 'a'), std::string(32, '0'), tokenA + tokenB}) {
        const ConnectionId f = r.Connect(fake);
        CHECK(Str(r.Last(f, "error"), "code") == "match_in_progress");
        r.server->OnDisconnect(f);
    }
    CHECK(r.server->connectedPlayers() == 1);

    // With the token the player gets their seat back and everything needed to redraw the game as it is now.
    const ConnectionId a2 = r.Connect(tokenA);
    const json::Value w = r.Last(a2, "welcome");
    CHECK(Num(w, "player_id") == 0 && w.Find("reconnected")->AsBool() && Str(w, "token") == tokenA && w.Find("match_running")->AsBool());
    CHECK(r.Types(a2) == std::vector<std::string>({"welcome", "match_started", "phase", "state", "public_state"}));
    const json::Value phase = r.Last(a2, "phase");
    CHECK(Str(phase, "phase") == "Planning" && Num(phase, "round") == 1 && Num(phase, "ticks_remaining") > 0 && Num(phase, "ticks_remaining") <= 90);
    const json::Value state = r.Last(a2, "state");
    CHECK(Num(state, "gold") == r.Match().Players().Get(0)->Gold() && Num(state, "player_id") == 0);
    r.Say(a2, R"({"action": "buy_xp", "id": 1})");
    CHECK(Str(r.Last(a2, "result"), "result") == "Ok");   // and it can act again

    // The same player connecting a second time (say, a phone and a PC): the newer connection wins, the older one is told why.
    const ConnectionId b2 = r.Connect(tokenB);
    CHECK(Str(r.Last(b, "error"), "code") == "replaced" && r.net.closed[b].code == 4001);
    CHECK(r.server->connectedPlayers() == 2);
    r.server->OnDisconnect(b);   // the old socket finally goes away: it must not free the seat the new one holds
    CHECK(r.server->connectedPlayers() == 2);
    r.Say(b2, R"({"action": "buy_xp"})");
    CHECK(Str(r.Last(b2, "result"), "result") == "Ok");
    r.Say(b, R"({"action": "buy_xp"})");   // late traffic from the dead connection is ignored
    CHECK(r.CountType(b, "result") == 0);

    // Reconnecting in the middle of a fight brings the fight with it.
    CHECK(r.RunUntil([&] { return r.Match().Phase() == MatchPhase::Combat; }, 500));
    r.server->OnDisconnect(a2);
    const ConnectionId a3 = r.Connect(tokenA);
    const auto types = r.Types(a3);
    CHECK(std::count(types.begin(), types.end(), "combat_summary") == 1 && std::count(types.begin(), types.end(), "combat") == 1);
    const json::Value combat = r.Last(a3, "combat");
    CHECK(Num(combat, "round") == 1 && (Num(combat, "home") == 0 || Num(combat, "away") == 0));   // round 1 is PvE: their own fight vs the monsters
    CHECK(combat.Find("away_is_monsters")->AsBool() && Num(combat, "encounter") == 1);
}

static void TestCommandRouting() {
    Rig r(2, [](GameData& d, GameServerConfig&) { d.config.player.startingGold = 40; });
    struct Seen { std::uint64_t tick; PlayerId player; CommandType type; ActionResult result; };
    std::vector<Seen> observed;
    r.server->SetCommandObserver([&](std::uint64_t tick, PlayerId p, const Command& c, ActionResult res) { observed.push_back({tick, p, c.type, res}); });
    const ConnectionId a = r.Connect();
    const ConnectionId b = r.Connect();

    // Round 1 opens straight into Planning (Mother Nature comes every 3rd round).
    CHECK(IsPlanning(r));
    r.net.sent.clear();

    // The shop is private state: read it from the state message the server pushed when the phase began.
    // (Sent on the tick that entered Planning; fetch it again with get_state.)
    r.Say(a, R"({"action": "get_state"})");
    const json::Value s0 = r.Last(a, "state");
    const auto& shop = s0.Find("shop")->Items();
    CHECK(shop.size() == static_cast<std::size_t>(r.data.config.shop.slotCount));
    int filled = 0;
    for (const json::Value& slot : shop) {
        long long id = 0;
        slot.ToInt(id);
        filled += id != 0 ? 1 : 0;
    }
    CHECK(filled == r.data.config.shop.slotCount && Num(s0, "gold") == r.Match().Players().Get(0)->Gold());

    // A purchase: the answer comes first, then what the engine did, then the new state.
    r.net.sent.clear();
    const int goldBefore = r.Match().Players().Get(0)->Gold();
    r.Say(a, R"({"action": "buy_unit", "shop_index": 0, "id": 7})");
    {
        const auto types = r.Types(a);
        CHECK(!types.empty() && types[0] == "result");
        CHECK(std::count(types.begin(), types.end(), "unit_event") >= 1 && std::count(types.begin(), types.end(), "state") == 1);
        CHECK(Str(r.Last(a, "result"), "result") == "Ok" && Num(r.Last(a, "result"), "id") == 7);
        const json::Value bought = r.Last(a, "unit_event");
        CHECK(Str(bought, "event") == "bought" && Num(*bought.Find("unit"), "star") == 1 && Num(bought, "gold_spent") >= 1);
        const json::Value st = r.Last(a, "state");
        const PlayerState& truth = *r.Match().Players().Get(0);
        CHECK(Num(st, "gold") == truth.Gold() && truth.Gold() < goldBefore && truth.Roster().Count() == 1);
        long long first = 0;
        st.Find("bench")->Items()[0].Find("id")->ToInt(first);
        CHECK(static_cast<UnitId>(first) == truth.Roster().Units()[0].id);
        // The other player saw only public changes (their own state did not move; a purchase does not change public state).
        CHECK(r.CountType(b, "state") == 0 && r.CountType(b, "unit_event") == 0 && r.CountType(b, "result") == 0);
    }
    const UnitId mine = r.Match().Players().Get(0)->Roster().Units()[0].id;

    // Every engine refusal is passed on verbatim.
    struct Expect { std::string command; const char* result; };
    const std::vector<Expect> refusals = {
        {R"({"action": "buy_unit", "shop_index": 0})", "EmptySlot"},                               // that slot was just bought
        {R"({"action": "buy_unit", "shop_index": 40})", "InvalidSlot"},                            // no such slot
        {R"({"action": "sell_unit", "unit_id": 99999})", "InvalidUnit"},
        {R"({"action": "equip_item", "unit_id": )" + std::to_string(mine) + R"(, "item_id": 3})", "InvalidItem"},   // not in the bag
        {R"({"action": "equip_item", "unit_id": 99999, "item_id": 3})", "InvalidItem"},
        {R"({"action": "unequip_item", "unit_id": )" + std::to_string(mine) + R"(, "slot": 0})", "InvalidSlot"},    // nothing there
        {R"({"action": "unequip_item", "unit_id": 99999, "slot": 0})", "InvalidUnit"},
        {R"({"action": "move_unit", "unit_id": 99999, "location": "bench", "x": 3})", "InvalidUnit"},
    };
    for (const Expect& e : refusals) {
        r.net.sent.clear();
        r.Say(a, e.command);
        const json::Value res = r.Last(a, "result");
        if (Str(res, "result") != e.result) std::printf("  %s -> %s (wanted %s)\n", e.command.c_str(), Str(res, "result").c_str(), e.result);
        CHECK(Str(res, "result") == e.result && res.Find("ok")->AsBool() == false);
    }

    // A player can only touch their OWN units: another player's unit id is simply "invalid" for them.
    r.net.sent.clear();
    r.Say(b, R"({"action": "sell_unit", "unit_id": )" + std::to_string(mine) + "}");
    CHECK(Str(r.Last(b, "result"), "result") == "InvalidUnit");
    r.Say(b, R"({"action": "move_unit", "unit_id": )" + std::to_string(mine) + R"(, "location": "bench", "x": 8})");
    CHECK(Str(r.Last(b, "result"), "result") == "InvalidUnit");
    CHECK(r.Match().Players().Get(0)->Roster().Count() == 1);

    // Moves and sells work, and the results match what the engine did.
    r.Say(a, R"({"action": "move_unit", "unit_id": )" + std::to_string(mine) + R"(, "location": "board", "x": 3, "y": 2})");
    CHECK(Str(r.Last(a, "result"), "result") == "Ok");
    CHECK(r.Match().Players().Get(0)->Roster().BoardAt(3, 2) != nullptr);
    CHECK(Str(r.Last(a, "unit_event"), "event") == "moved");
    // The board is public: the other player is told (through public state) where the unit stands, bench and gold stay private.
    {
        const json::Value pub = r.Last(b, "public_state");
        bool onBoard = false;
        for (const json::Value& p : pub.Find("players")->Items()) {
            if (Num(p, "player_id") == 0) {
                for (const json::Value& u : p.Find("board")->Items()) onBoard = onBoard || (Num(u, "x") == 3 && Num(u, "y") == 2);
            }
        }
        CHECK(onBoard);
    }
    const int goldBeforeSale = r.Match().Players().Get(0)->Gold();
    r.Say(a, R"({"action": "sell_unit", "unit_id": )" + std::to_string(mine) + "}");
    CHECK(Str(r.Last(a, "result"), "result") == "Ok" && r.Match().Players().Get(0)->Gold() > goldBeforeSale && r.Match().Players().Get(0)->Roster().Count() == 0);
    CHECK(r.Match().VerifyPoolIntegrity());

    // Run out of money: NotEnoughGold, and the gold never goes negative.
    std::string last;
    for (int i = 0; i < 40 && last != "NotEnoughGold"; ++i) {
        r.now += 100;
        r.Say(a, R"({"action": "buy_xp"})");
        last = Str(r.Last(a, "result"), "result");
    }
    CHECK(last == "NotEnoughGold" && r.Match().Players().Get(0)->Gold() >= 0 && r.Match().Players().Get(0)->Gold() < r.data.config.player.buyXpCost);

    // Every command that reached the engine was reported to the observer with the engine's own result, in order.
    CHECK(observed.size() > 20);
    bool consistent = true;
    for (const Seen& s : observed) consistent = consistent && s.tick <= r.server->tickCount();
    CHECK(consistent);

    // Past the end of the Planning phase the shop stays open (buy, reroll, XP), but the engine itself refuses what is only for Planning or Mother
    // Nature's phase, and the client learns why.
    CHECK(r.RunUntil([&] { return r.Match().Phase() == MatchPhase::Combat; }, 300));
    r.Say(a, R"({"action": "pick_gift", "gift_index": 0})");
    CHECK(Str(r.Last(a, "result"), "result") == "WrongPhase");
    r.Say(a, R"({"action": "reroll_shop"})");
    const std::string inCombat = Str(r.Last(a, "result"), "result");
    CHECK(inCombat == "Ok" || inCombat == "NotEnoughGold");
    // The board is locked while units fight: a board unit cannot be sold (the bench stays open).
    const PlayerState* seat0 = r.Match().Players().Get(0);
    for (const UnitInstance& u : seat0->Roster().Units()) {
        if (u.location != LocationType::Board) continue;
        r.Say(a, R"({"action": "sell_unit", "unit_id": )" + std::to_string(u.id) + "}");
        CHECK(Str(r.Last(a, "result"), "result") == "UnitInCombat");
        break;
    }
}

static void TestMotherNatureOverTheProtocol() {
    std::string err;
    auto items = w2f::LoadItemDatabaseFromFile(sample::ProductionItemsPath(), &err);
    auto nature = w2f::LoadMotherNatureDatabaseFromFile(sample::ProductionMotherNaturePath(), items.get(), &err);
    CHECK(items != nullptr && nature != nullptr);
    if (!items || !nature) return;
    // Mother Nature every round, so the very first phase of the match is hers.
    Rig r(2, [&](GameData& d, GameServerConfig&) { d.motherNature = nature.get(); d.config.match.motherNatureEveryRounds = 1; });
    const ConnectionId a = r.Connect();
    const ConnectionId b = r.Connect();
    CHECK(r.server->state() == GameServer::State::Running && r.Match().Phase() == MatchPhase::MotherNature && r.Match().IsMotherNatureRound());

    // What the client is told when the match opens: how long she stays, how often she comes, and that this round has no shop.
    const json::Value started = r.Last(a, "match_started");
    CHECK(Num(*started.Find("phase_ticks"), "mother_nature") == 30 && Num(started, "mother_nature_every") == 1 && started.Find("phase_ticks")->Find("draft") == nullptr);
    const json::Value phase = r.Last(a, "phase");
    CHECK(Str(phase, "phase") == "MotherNature" && phase.Find("mother_nature")->AsBool() && Num(phase, "duration_ticks") == 30);

    // Each player is privately offered 2 concrete gifts.
    const json::Value offeredA = r.Last(a, "gift_event");
    CHECK(Str(offeredA, "event") == "offered" && offeredA.Find("gifts")->Items().size() == 2);
    const auto& giftsA = offeredA.Find("gifts")->Items();
    CHECK(Num(giftsA[0], "index") == 0 && Num(giftsA[1], "index") == 1 && Num(giftsA[0], "gift") != Num(giftsA[1], "gift"));
    for (const json::Value& g : giftsA) {
        const std::string kind = Str(g, "kind");
        CHECK(!Str(g, "name").empty());
        if (kind == "gold" || kind == "xp" || kind == "heal") CHECK(Num(g, "amount") > 0);
        else if (kind == "item") CHECK(Num(g, "item") >= 1 && Num(g, "item") <= 8);
        else if (kind == "unit") CHECK(Num(g, "champion") > 0 && Num(g, "cost") >= 2 && Num(g, "cost") <= 3);
        else CHECK(false);
    }
    const json::Value stateA = r.Last(a, "state");
    CHECK(stateA.Find("gifts")->Items().size() == 2 && !stateA.Find("gift_settled")->AsBool());
    // Nobody else sees them: b's messages carry b's own offers, and a's are not in there.
    CHECK(r.CountType(b, "gift_event") == 1 && r.Last(b, "gift_event").Find("gifts")->Items().size() == 2);
    r.net.sent.clear();

    // The pick: the answer, the private event, and the new state (the offers are gone, the choice is settled).
    r.Say(a, R"({"action": "pick_gift", "gift_index": 1, "id": 5})");
    {
        const json::Value res = r.Last(a, "result");
        CHECK(Str(res, "result") == "Ok" && Num(res, "id") == 5 && Str(res, "action") == "pick_gift" && res.Find("ok")->AsBool());
        const json::Value picked = r.Last(a, "gift_event");
        CHECK(Str(picked, "event") == "picked" && Num(*picked.Find("gift"), "index") == 1 && Num(*picked.Find("gift"), "gift") == Num(giftsA[1], "gift") &&
              !picked.Find("automatic")->AsBool() && Num(picked, "gold_converted") == 0);
        const json::Value st = r.Last(a, "state");
        CHECK(st.Find("gifts")->Items().empty() && st.Find("gift_settled")->AsBool());
        CHECK(r.CountType(b, "gift_event") == 0 && r.CountType(b, "result") == 0);   // b learns nothing about a's pick
    }
    // Refusals come back as engine results; malformed commands are protocol errors.
    r.net.sent.clear();
    r.Say(a, R"({"action": "pick_gift", "gift_index": 0, "id": 6})");
    CHECK(Str(r.Last(a, "result"), "result") == "AlreadyPicked");
    r.Say(b, R"({"action": "pick_gift", "gift_index": 3})");
    CHECK(Str(r.Last(b, "result"), "result") == "InvalidSlot");
    r.Say(b, R"({"action": "buy_unit", "shop_index": 0})");
    CHECK(Str(r.Last(b, "result"), "result") == "WrongPhase");   // not even the shop's command works during her phase
    r.Say(b, R"({"action": "pick_gift", "gift_index": 9})");
    CHECK(Str(r.Last(b, "error"), "code") == "out_of_range");

    // b picks; the phase ends at once and (demo 1.1) the round's Planning has its shop like any other.
    r.Say(b, R"({"action": "pick_gift", "gift_index": 0})");
    CHECK(Str(r.Last(b, "result"), "result") == "Ok");
    r.Tick(2);
    CHECK(r.Match().Phase() == MatchPhase::Planning && r.Match().Round() == 1);
    const json::Value planning = r.Last(a, "phase");
    CHECK(Str(planning, "phase") == "Planning" && planning.Find("mother_nature")->AsBool());
    r.Say(a, R"({"action": "get_state"})");
    const json::Value shopState = r.Last(a, "state");
    CHECK(shopState.Find("shop")->Items().size() == static_cast<std::size_t>(r.data.config.shop.slotCount));
    for (const json::Value& slot : shopState.Find("shop")->Items()) {   // every slot filled
        long long id = -1;
        CHECK(slot.ToInt(id) && id != 0);
    }
    CHECK(!planning.Find("shop_closed")->AsBool());
    r.net.sent.clear();
    r.Say(a, R"({"action": "reroll_shop", "id": 7})");
    CHECK(Num(r.Last(a, "result"), "id") == 7 && Str(r.Last(a, "result"), "result") != "ShopClosed");
    r.Say(a, R"({"action": "pick_gift", "gift_index": 0})");
    CHECK(Str(r.Last(a, "result"), "result") == "WrongPhase");
    CHECK(r.Match().VerifyPoolIntegrity());

    // A player who reconnects during the phase gets the offers again (they are part of the private state).
    Rig q(2, [&](GameData& d, GameServerConfig&) { d.motherNature = nature.get(); d.config.match.motherNatureEveryRounds = 1; });
    const ConnectionId c1 = q.Connect();
    q.Connect();
    const std::string token = Str(q.Last(c1, "welcome"), "token");
    q.server->OnDisconnect(c1);
    const ConnectionId c2 = q.Connect(token);
    const json::Value again = q.Last(c2, "state");
    CHECK(again.Find("gifts")->Items().size() == 2 && !again.Find("gift_settled")->AsBool() && Str(q.Last(c2, "phase"), "phase") == "MotherNature");
}

// Without Mother Nature data the server plays exactly as before: no gift phase, no closed shop, and the messages say so.
static void TestNoMotherNatureData() {
    Rig r(2);
    const ConnectionId a = r.Connect();
    r.Connect();
    CHECK(Num(r.Last(a, "match_started"), "mother_nature_every") == 0);
    CHECK(r.Match().Phase() == MatchPhase::Planning && !r.Match().IsMotherNatureRound(3));
    CHECK(!r.Last(a, "phase").Find("mother_nature")->AsBool());
    r.Say(a, R"({"action": "get_state"})");
    CHECK(r.Last(a, "state").Find("gifts")->Items().empty() && r.Last(a, "state").Find("gift_settled")->AsBool());
    r.Say(a, R"({"action": "pick_gift", "gift_index": 0})");
    CHECK(Str(r.Last(a, "result"), "result") == "WrongPhase");
}

static void TestItemCombinationOverTheProtocol() {
    // equip_item is all a client needs: give a unit a component it can combine with and the server reports the combination.
    Rig r(2, {}, sample::Phase10ItemsPath());
    const ConnectionId a = r.Connect();
    const ConnectionId b = r.Connect();
    CHECK(r.RunUntil([&] { return IsPlanning(r); }, 200));
    r.Say(a, R"({"action": "buy_unit", "shop_index": 0})");
    r.net.sent.clear();
    PlayerState* player = const_cast<MatchManager*>(r.server->match())->PlayersMutable().Get(0);   // admin access: items normally arrive as PvE drops
    CHECK(player->Roster().Count() == 1 && player->AddItemToBag(1) && player->AddItemToBag(2));
    const UnitId unit = player->Roster().Units()[0].id;
    r.Say(a, R"({"action": "equip_item", "unit_id": )" + std::to_string(unit) + R"(, "item_id": 1, "id": 1})");
    CHECK(Str(r.Last(a, "result"), "result") == "Ok" && r.CountType(a, "unit_event") == 1);
    r.net.sent.clear();
    r.Say(a, R"({"action": "equip_item", "unit_id": )" + std::to_string(unit) + R"(, "item_id": 2, "id": 2})");
    CHECK(Str(r.Last(a, "result"), "result") == "Ok");
    bool sawCombination = false;
    for (const json::Value& v : r.Inbox(a)) {
        if (Str(v, "type") == "unit_event" && Str(v, "event") == "items_combined") {
            sawCombination = Num(v, "first") == 1 && Num(v, "second") == 2 && Num(v, "result") == 10;
            const json::Value* unitItems = v.Find("unit")->Find("items");
            long long only = 0;
            CHECK(unitItems && unitItems->Items().size() == 1 && unitItems->Items()[0].ToInt(only) && only == 10);   // one item left: the Casket
        }
    }
    CHECK(sawCombination);
    // The private state shows ONE item on the unit and an empty bag.
    const json::Value state = r.Last(a, "state");
    CHECK(state.Find("item_bag")->Items().empty());
    long long carried = 0;
    state.Find("bench")->Items()[0].Find("items")->Items()[0].ToInt(carried);
    CHECK(carried == 10);
    // Private: the other player never hears about any of it.
    CHECK(r.CountType(b, "unit_event") == 0 && r.CountType(b, "state") == 0);
}

static void TestItemRemoverOverTheProtocol() {
    Rig r(2, {}, sample::ProductionItemsPath());
    const ConnectionId a = r.Connect();
    const ConnectionId b = r.Connect();
    CHECK(r.RunUntil([&] { return IsPlanning(r); }, 200));
    r.Say(a, R"({"action": "buy_unit", "shop_index": 0})");
    PlayerState* player = const_cast<MatchManager*>(r.server->match())->PlayersMutable().Get(0);   // admin access: items normally arrive as PvE drops
    CHECK(player->Roster().Count() == 1);
    for (ItemId item : {7u, 4u, 50u, 50u}) CHECK(player->AddItemToBag(item));
    const std::string unit = std::to_string(player->Roster().Units()[0].id);
    r.Say(a, R"({"action": "equip_item", "unit_id": )" + unit + R"(, "item_id": 7})");
    r.Say(a, R"({"action": "equip_item", "unit_id": )" + unit + R"(, "item_id": 4})");
    r.net.sent.clear();

    r.Say(a, R"({"action": "equip_item", "unit_id": )" + unit + R"(, "item_id": 50, "id": 9})");
    CHECK(Str(r.Last(a, "result"), "result") == "Ok" && Num(r.Last(a, "result"), "id") == 9);
    std::vector<long long> unequipped;
    json::Value used;
    for (const json::Value& v : r.Inbox(a)) {
        if (Str(v, "type") != "unit_event") continue;
        long long item = 0;
        if (Str(v, "event") == "item_unequipped" && v.Find("item")->ToInt(item)) unequipped.push_back(item);
        if (Str(v, "event") == "consumable_used") used = v;
    }
    CHECK((unequipped == std::vector<long long>{7, 4}));
    CHECK(Str(used, "event") == "consumable_used" && Num(used, "item") == 50 && used.Find("returned")->Items().size() == 2 && used.Find("unit")->Find("items")->Items().empty());
    const json::Value state = r.Last(a, "state");
    std::vector<long long> bag;
    for (const json::Value& item : state.Find("item_bag")->Items()) {
        long long id = 0;
        item.ToInt(id);
        bag.push_back(id);
    }
    CHECK((bag == std::vector<long long>{50, 7, 4}));   // one remover used up, the other still there, the unit's items back in the bag
    // Nothing left to remove: refused, the remover stays.
    r.net.sent.clear();
    r.Say(a, R"({"action": "equip_item", "unit_id": )" + unit + R"(, "item_id": 50, "id": 10})");
    CHECK(Str(r.Last(a, "result"), "result") == "NoItemsToRemove" && !r.Last(a, "result").Find("ok")->AsBool() && r.CountType(a, "unit_event") == 0);
    CHECK(player->ItemBag().size() == 3);
    CHECK(r.CountType(b, "unit_event") == 0 && r.CountType(b, "state") == 0);   // private to the owner

    // The catalog tells a client which items are consumables.
    r.Say(a, R"({"action": "get_catalog"})");
    bool removerFlagged = false, swordPlain = false;
    const json::Value catalog = r.Last(a, "catalog");
    for (const json::Value& item : catalog.Find("items")->Items()) {
        if (Num(item, "id") == 50) removerFlagged = Str(item, "name") == "Item Remover" && item.Find("consumable")->AsBool() && item.Find("components")->Items().empty();
        if (Num(item, "id") == 3) swordPlain = !item.Find("consumable")->AsBool();
    }
    CHECK(removerFlagged && swordPlain);
}

static void TestCombatPhaseLengthOnTheWire() {
    // The Combat phase lasts as long as the round's fights (plus a short linger, at most 35 s): the phase message says how long, from its first tick.
    Rig r(2);
    const ConnectionId a = r.Connect();
    r.Connect();
    const json::Value started = r.Last(a, "match_started");
    CHECK(Num(*started.Find("phase_ticks"), "combat") == r.data.config.match.combatTicks && Num(*started.Find("phase_ticks"), "resolution") == r.data.config.match.resolutionTicks);
    int combatPhases = 0;
    bool lengthsRight = true;
    for (int round = 0; round < 6; ++round) {
        CHECK(r.RunUntil([&] { return r.Match().Phase() == MatchPhase::Combat && r.Match().TicksInPhase() > 0; }, 5000));
        const json::Value phase = r.Last(a, "phase");
        const MatchConfig& mc = r.data.config.match;
        int lastEnd = 0;
        for (const CombatOutcome& o : r.Match().CurrentCombatOutcomes()) lastEnd = std::max(lastEnd, o.log.endTick);
        lengthsRight = lengthsRight && Str(phase, "phase") == "Combat" && Num(phase, "duration_ticks") == r.Match().PhaseTicks() &&
                       Num(phase, "duration_ticks") == std::min(mc.combatTicks, std::max(mc.combatMinTicks, lastEnd + mc.combatLingerTicks)) &&
                       Num(phase, "ticks_remaining") <= Num(phase, "duration_ticks") && Num(phase, "duration_ticks") <= r.data.config.match.combatTicks;
        ++combatPhases;
        CHECK(r.RunUntil([&] { return r.Match().Phase() != MatchPhase::Combat; }, 5000));
    }
    CHECK(combatPhases == 6 && lengthsRight);
}

static void TestMalformedTrafficNeverReachesTheEngine() {
    Rig r(2, [](GameData&, GameServerConfig& c) { c.maxViolations = 100000; });   // this test is about validation, not about disconnecting
    int reached = 0;
    r.server->SetCommandObserver([&](std::uint64_t, PlayerId, const Command&, ActionResult) { ++reached; });
    const ConnectionId a = r.Connect();
    const ConnectionId b = r.Connect();
    CHECK(r.RunUntil([&] { return IsPlanning(r); }, 100));
    (void)b;
    const std::uint64_t before = r.Match().StateHash();
    const std::size_t sentBefore = r.net.sent[a].size();

    Rng rng(7);
    std::vector<std::string> garbage = {"", "{", "}", "[]", "null", "\"x\"", "12345", std::string(10000, 'A'), "{\"action\":\"buy_unit\"}", "{\"action\":\"buy_unit\",\"shop_index\":-1}",
                                        "{\"action\":\"sell_unit\",\"unit_id\":\"1\"}", "{\"action\":\"move_unit\",\"unit_id\":1,\"location\":\"board\",\"x\":99,\"y\":0}",
                                        std::string("{\"action\":\"ping\"}\0x", 19), "\xff\xfe\xfd", "{\"action\":\"buy_xp\",\"extra\":true}", "{\"action\":\"BUY_XP\"}",
                                        "{'action':'buy_xp'}", "action=buy_xp", "<xml/>", std::string(300, '[') + std::string(300, ']')};
    for (int i = 0; i < 300; ++i) {
        std::string s;
        const std::size_t n = rng.NextBelow(80);
        for (std::size_t k = 0; k < n; ++k) s.push_back(static_cast<char>(rng.NextBelow(256)));
        garbage.push_back(s);
    }
    // Spread over time so the rate limiter does not swallow them before validation (it has its own test).
    for (const std::string& g : garbage) {
        r.now += 200;
        r.Say(a, g);
    }
    CHECK(reached == 0);
    CHECK(r.Match().StateHash() == before);                    // not one bit of game state moved
    CHECK(r.net.sent[a].size() > sentBefore);                  // every bad message got an error back...
    int errors = 0;
    for (const json::Value& v : r.Inbox(a)) errors += Str(v, "type") == "error" ? 1 : 0;
    CHECK(errors >= static_cast<int>(garbage.size()));
    // The other player and the match were never affected.
    CHECK(!r.net.closed.count(b) && !r.net.closed.count(a) && r.server->state() == GameServer::State::Running);
}

static void TestRateLimiting() {
    Rig r(2, [](GameData&, GameServerConfig& c) { c.rateBurst = 10; c.rateRefillPerSecond = 5; c.maxViolations = 1000; });
    const ConnectionId a = r.Connect();
    r.Connect();
    r.net.sent.clear();
    // 30 pings in the same millisecond: the burst (10) is answered, the rest are refused, nothing else breaks.
    for (int i = 0; i < 30; ++i) r.Say(a, R"({"action": "ping"})");
    CHECK(r.CountType(a, "pong") == 10 && r.CountType(a, "error") == 20);
    CHECK(Str(r.Last(a, "error"), "code") == "rate_limited");
    // Time refills the bucket: 5 per second.
    r.now += 1000;
    r.net.sent.clear();
    for (int i = 0; i < 10; ++i) r.Say(a, R"({"action": "ping"})");
    CHECK(r.CountType(a, "pong") == 5);
    // The bucket never holds more than the burst, however long the client was quiet.
    r.now += 3'600'000;
    r.net.sent.clear();
    for (int i = 0; i < 30; ++i) r.Say(a, R"({"action": "ping"})");
    CHECK(r.CountType(a, "pong") == 10);
    // Heavy commands cost more (get_state 3, get_fight 5).
    r.now += 3'600'000;
    r.net.sent.clear();
    for (int i = 0; i < 10; ++i) r.Say(a, R"({"action": "get_state"})");
    CHECK(r.CountType(a, "state") == 3);   // 10 tokens / 3 = 3 whole requests
    // Limits are per connection: someone else is not affected.
    const ConnectionId other = r.Connect();
    (void)other;
    CHECK(r.server->connectedPlayers() == 2);
    // Repeated abuse ends in a disconnect.
    Rig strict(2, [](GameData&, GameServerConfig& c) { c.rateBurst = 2; c.maxViolations = 5; });
    const ConnectionId s = strict.Connect();
    strict.Connect();
    for (int i = 0; i < 20; ++i) strict.Say(s, R"({"action": "ping"})");
    CHECK(strict.net.closed.count(s) && strict.net.closed[s].code == 1008 && strict.CountType(s, "error") >= 6);
    const std::size_t afterClose = strict.net.sent[s].size();
    strict.Say(s, R"({"action": "ping"})");
    CHECK(strict.net.sent[s].size() == afterClose);   // a connection being closed gets no more answers
}

// ---- a client that plays through the protocol only -------------------------------------------

namespace {

// Reads server messages and plays: at the start of Planning it buys everything in the shop, then puts its bench on the board.
// It sees the game only through the JSON the server sends, like a real client would.
struct Bot {
    Rig& rig;
    ConnectionId conn;
    int seat = -1;
    std::size_t consumed = 0;
    json::Value state;
    bool haveState = false;
    std::string phase;
    int round = 0;
    std::uint64_t planningStart = 0;
    int plannedRound = 0;

    Bot(Rig& r, ConnectionId c) : rig(r), conn(c) {}

    void Poll() {
        const std::vector<std::string>& inbox = rig.net.sent[conn];
        for (; consumed < inbox.size(); ++consumed) {
            const json::Value v = ParseJson(inbox[consumed]);
            const std::string type = Str(v, "type");
            if (type == "welcome") seat = static_cast<int>(Num(v, "player_id"));
            if (type == "phase") {
                phase = Str(v, "phase");
                round = static_cast<int>(Num(v, "round"));
                if (phase == "Planning") planningStart = rig.server->tickCount();
            }
            if (type == "state") { state = v; haveState = true; }
        }
        if (phase != "Planning" || round == plannedRound || !haveState) return;
        const std::uint64_t into = rig.server->tickCount() - planningStart;
        if (into == 3) {
            const auto& shop = state.Find("shop")->Items();
            for (std::size_t i = 0; i < shop.size(); ++i) {
                long long id = 0;
                shop[i].ToInt(id);
                if (id != 0) rig.Say(conn, R"({"action": "buy_unit", "shop_index": )" + std::to_string(i) + "}");
            }
        } else if (into == 10) {
            plannedRound = round;
            int column = static_cast<int>(state.Find("board")->Items().size());
            for (const json::Value& unit : state.Find("bench")->Items()) {
                if (!unit.IsObject() || column >= kBoardColumns) continue;
                rig.Say(conn, R"({"action": "move_unit", "unit_id": )" + std::to_string(Num(unit, "id")) + R"(, "location": "board", "x": )" +
                                  std::to_string(column++) + R"(, "y": 2})");
            }
        }
    }
};

}  // namespace

static void TestPrivacyAndDelivery() {
    // A whole 3-player match played through the protocol. Every message every client receives is audited as it arrives.
    Rig r(3, [](GameData& d, GameServerConfig&) {
        d.config.player.startingHealth = 40;
        d.config.damage.baseDamageByStage = {6, 10};
    });
    std::vector<ConnectionId> conns;
    for (int i = 0; i < 3; ++i) conns.push_back(r.Connect());
    std::vector<Bot> bots;
    for (ConnectionId c : conns) bots.emplace_back(r, c);
    std::vector<int> seatOf(bots.size());

    std::map<std::string, int> typeCounts;
    int privateViolations = 0, publicLeaks = 0, misroutedCombat = 0, badUnitOwner = 0, malformedRows = 0;
    std::vector<int> stateMessages(3, 0), drops(3, 0), dropsExpected(3, 0), combatSeen(3, 0);
    std::vector<std::size_t> audited(conns.size(), 0);
    std::set<int> phasesSeen;
    std::vector<std::string> phaseOrder;
    bool combatChecksumsOk = true, summaryOk = true, spectateOk = true;
    int combatRounds = 0;

    const auto audit = [&] {
        for (std::size_t i = 0; i < conns.size(); ++i) {
            const int me = bots[i].seat;
            const std::vector<std::string>& inbox = r.net.sent[conns[i]];
            for (; audited[i] < inbox.size(); ++audited[i]) {
                const std::string& text = inbox[audited[i]];
                const json::Value v = ParseJson(text);
                const std::string type = Str(v, "type");
                ++typeCounts[type];
                if (type == "state") {
                    ++stateMessages[i];
                    if (Num(v, "player_id") != me) ++privateViolations;
                }
                if (type == "income" || type == "pve_drop") {
                    if (type == "income" && Num(v, "player_id") != me) ++privateViolations;
                    if (type == "pve_drop") ++drops[i];
                }
                if (type == "unit_event") {
                    const json::Value* unit = v.Find("unit");
                    if (unit == nullptr || (static_cast<std::uint32_t>(Num(*unit, "id")) >> 24) != static_cast<std::uint32_t>(me + 1)) ++badUnitOwner;
                }
                if (type == "public_state") {
                    for (const char* forbidden : {"\"gold\"", "\"shop\"", "\"item_bag\"", "\"xp\"", "\"xp_to_next\""}) {   // (the bench is public since revision 2)
                        if (text.find(forbidden) != std::string::npos) ++publicLeaks;
                    }
                    CHECK(v.Find("players")->Items().size() == 3);
                }
                if (type == "combat") {
                    ++combatSeen[i];
                    const bool mine = Num(v, "home") == me || (Num(v, "away") == me && !v.Find("away_is_ghost")->AsBool() && !v.Find("away_is_monsters")->AsBool());
                    if (!mine) ++misroutedCombat;
                    if (v.Find("columns")->Items().size() != 27) ++malformedRows;
                    for (const json::Value& row : v.Find("events")->Items()) {
                        if (row.Items().size() != 27) { ++malformedRows; break; }
                    }
                }
            }
        }
    };

    std::vector<int> pveWinsPerSeat(3, 0);
    bool finished = false;
    for (int tick = 0; tick < 20000 && !finished; ++tick) {
        for (std::size_t i = 0; i < bots.size(); ++i) bots[i].Poll();
        r.Tick();
        for (std::size_t i = 0; i < bots.size(); ++i) seatOf[i] = bots[i].seat;
        const MatchManager& m = r.Match();
        if (m.TicksInPhase() == 0 && phaseOrder.empty()) phaseOrder.push_back(ToString(m.Phase()));
        if (m.Phase() == MatchPhase::Combat && m.TicksInPhase() == 0) {
            ++combatRounds;
            audit();
            // The fights of this round: summary to everybody, each fight's log only to the players in it.
            const auto& outcomes = m.CurrentCombatOutcomes();
            for (std::size_t i = 0; i < conns.size(); ++i) {
                const json::Value summary = r.Last(conns[i], "combat_summary");
                summaryOk = summaryOk && summary.Find("fights") && summary.Find("fights")->Items().size() == outcomes.size() && Num(summary, "round") == m.Round();
                for (std::size_t f = 0; f < outcomes.size(); ++f) {
                    const CombatOutcome& o = outcomes[f];
                    const bool participant = o.matchup.home == bots[i].seat || (!o.matchup.awayIsGhost && !o.matchup.awayIsMonsters && o.matchup.away == bots[i].seat);
                    int received = 0;
                    for (const json::Value& msg : r.Inbox(conns[i])) {
                        if (Str(msg, "type") != "combat" || Num(msg, "round") != m.Round() || Num(msg, "fight_index") != static_cast<long long>(f)) continue;
                        ++received;
                        char hex[17];
                        std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(o.log.checksum));
                        combatChecksumsOk = combatChecksumsOk && Str(msg, "checksum") == hex && msg.Find("events")->Items().size() == o.log.events.size() &&
                                            Num(msg, "end_tick") == o.log.endTick && Str(msg, "winner") == (o.winner == CombatWinner::Home ? "home" : o.winner == CombatWinner::Away ? "away" : "draw");
                    }
                    if (received != (participant ? 1 : 0)) summaryOk = false;
                }
            }
            // Anyone can fetch any fight (they are public): the answer is byte-identical to what the participants received.
            if (!outcomes.empty()) {
                audit();
                r.now += 1000;   // let the rate limiter refill
                r.Say(conns[0], R"({"action": "get_fight", "fight_index": 0, "id": 3})");
                const json::Value fetched = r.Last(conns[0], "combat");
                spectateOk = spectateOk && Num(fetched, "fight_index") == 0 && fetched.Find("events")->Items().size() == outcomes[0].log.events.size();
                r.Say(conns[0], R"({"action": "get_fight", "fight_index": 7, "id": 4})");
                spectateOk = spectateOk && Str(r.Last(conns[0], "error"), "code") == "no_such_fight" && Num(r.Last(conns[0], "error"), "id") == 4;
                audited[0] = r.net.sent[conns[0]].size();   // these replies were asked for: not part of the pushed-message audit
            }
            audit();
        }
        if (m.Phase() == MatchPhase::Resolution && m.TicksInPhase() == 0) {
            for (const CombatOutcome& o : m.CurrentCombatOutcomes()) {
                if (o.matchup.awayIsMonsters && o.drop.type != PveDropType::None) ++dropsExpected[o.matchup.home];
                if (o.matchup.awayIsMonsters) dropsExpected[o.matchup.home] += static_cast<int>(o.loot.size());   // the guaranteed loot (demo 1.1)
            }
        }
        audit();
        finished = r.server->state() == GameServer::State::Finished;
    }

    CHECK(finished);
    CHECK(privateViolations == 0 && publicLeaks == 0 && misroutedCombat == 0 && badUnitOwner == 0 && malformedRows == 0);
    CHECK(combatChecksumsOk && summaryOk && spectateOk);
    CHECK(combatRounds >= 5);
    for (int i = 0; i < 3; ++i) {
        CHECK(stateMessages[static_cast<std::size_t>(i)] > 5);
        CHECK(drops[static_cast<std::size_t>(i)] == dropsExpected[static_cast<std::size_t>(i)]);   // every drop reached exactly its owner
    }
    CHECK(dropsExpected[0] + dropsExpected[1] + dropsExpected[2] > 0);
    CHECK(typeCounts["phase"] >= 20 && typeCounts["income"] >= 9 && typeCounts["unit_event"] > 10 && typeCounts["combat"] >= 5 && typeCounts["player_damaged"] >= 1);
    CHECK(typeCounts["player_eliminated"] >= 1 && typeCounts["match_over"] == 3 && typeCounts["public_state"] >= 20);

    // Damage and elimination are public (everyone sees them), and the final result names the engine's winner.
    const json::Value over = r.Last(conns[0], "match_over");
    CHECK(Num(over, "winner") == r.Match().Winner() && over.Find("placements")->Items().size() == 3);
    // The phase messages walk the state machine in order, with the right stage-round labelling.
    long long lastRound = 0;
    bool ordered = true;
    for (const json::Value& v : r.Inbox(conns[0])) {
        if (Str(v, "type") != "phase") continue;
        const long long round = Num(v, "round");
        ordered = ordered && round >= lastRound;
        lastRound = round;
        ordered = ordered && Num(v, "stage") >= 1 && Num(v, "round_in_stage") >= 1;
        if (round <= 3) ordered = ordered && v.Find("pve")->AsBool();
        if (round == 4) ordered = ordered && !v.Find("pve")->AsBool() && Num(v, "stage") == 2 && Num(v, "round_in_stage") == 1;
    }
    CHECK(ordered && lastRound > 3);
    // Each client's last private state equals the engine's truth.
    for (std::size_t i = 0; i < conns.size(); ++i) {
        const json::Value& st = bots[i].state;
        const PlayerState& truth = *r.Match().Players().Get(static_cast<PlayerId>(bots[i].seat));
        CHECK(Num(st, "gold") == truth.Gold() && Num(st, "health") == truth.Health() && Num(st, "level") == truth.Level());
    }
    std::printf("  audited a whole 3-player match: %d phase / %d state / %d combat / %d unit_event messages, %d PvE drops delivered\n",
                typeCounts["phase"], typeCounts["state"], typeCounts["combat"], typeCounts["unit_event"], dropsExpected[0] + dropsExpected[1] + dropsExpected[2]);
}

static void TestNetworkedMatchEqualsBareEngine() {
    // Play a match through the network layer with protocol bots, record every command that reached the engine, and replay
    // exactly those on a plain MatchManager with no network code anywhere. The two must end in the identical state, with the
    // identical result for every command: the network layer adds nothing and changes nothing.
    struct Logged { std::uint64_t tick; PlayerId player; Command command; ActionResult result; };
    std::vector<Logged> log;
    Rig r(2, [](GameData& d, GameServerConfig&) {
        d.config.player.startingHealth = 30;
        d.config.damage.baseDamageByStage = {5, 8};
    });
    r.server->SetCommandObserver([&](std::uint64_t t, PlayerId p, const Command& c, ActionResult res) { log.push_back({t, p, c, res}); });
    const ConnectionId a = r.Connect();
    const ConnectionId b = r.Connect();
    std::vector<Bot> bots;
    bots.emplace_back(r, a);
    bots.emplace_back(r, b);
    for (int tick = 0; tick < 12000 && r.server->state() == GameServer::State::Running; ++tick) {
        for (Bot& bot : bots) bot.Poll();
        // Some noise a real client would produce: refused commands, malformed ones, queries.
        if (tick % 97 == 0) r.Say(a, R"({"action": "sell_unit", "unit_id": 5})");
        if (tick % 89 == 0) r.Say(b, "garbage");
        if (tick % 131 == 0) r.Say(b, R"({"action": "get_state"})");
        if (tick % 173 == 0) r.Say(a, R"({"action": "reroll_shop"})");
        r.Tick();
    }
    CHECK(r.server->state() == GameServer::State::Finished);
    const std::uint64_t networkedHash = r.Match().StateHash();
    const std::uint64_t ticks = r.server->tickCount();
    CHECK(log.size() > 40);

    GameConfig gc = r.data.config;
    gc.match.playerCount = 2;
    std::string err;
    auto bare = MatchManager::Create(gc, *r.champions, r.cfg.seed, std::make_unique<CombatSimulator>(gc.combat, r.traits.get(), r.items.get()), &err, r.items.get(), r.encounters.get(),
                                     nullptr, r.traits.get());
    CHECK(bare != nullptr);
    if (!bare) return;
    bare->Start();
    std::size_t next = 0;
    bool sameResults = true;
    for (std::uint64_t t = 0; t <= ticks; ++t) {
        while (next < log.size() && log[next].tick == t) {
            const Logged& l = log[next++];
            ActionResult res = ActionResult::Ok;
            const Command& c = l.command;
            switch (c.type) {
                case CommandType::BuyUnit: res = bare->TryBuyShopUnit(l.player, static_cast<std::size_t>(c.shopIndex)); break;
                case CommandType::RerollShop: res = bare->TryRerollShop(l.player); break;
                case CommandType::BuyXp: res = bare->TryBuyXp(l.player); break;
                case CommandType::SellUnit: res = bare->TrySellUnit(l.player, c.unit); break;
                case CommandType::MoveUnit: res = bare->TryMoveUnit(l.player, c.unit, c.location, c.x, c.y); break;
                case CommandType::EquipItem: res = bare->TryEquipItem(l.player, c.unit, c.item); break;
                case CommandType::UnequipItem: res = bare->TryUnequipItem(l.player, c.unit, c.slot); break;
                case CommandType::PickTraitChoice: res = bare->TryPickTraitChoice(l.player, c.choiceIndex); break;
                default: break;
            }
            sameResults = sameResults && res == l.result;
        }
        if (t < ticks) bare->Tick();
    }
    CHECK(next == log.size() && sameResults);
    CHECK(bare->StateHash() == networkedHash);
    CHECK(bare->IsFinished() && bare->Winner() == r.Match().Winner() && bare->Round() == r.Match().Round());
    std::printf("  %zu commands replayed on a bare engine over %llu ticks: identical final state\n", log.size(), static_cast<unsigned long long>(ticks));
}

static void TestMatchEndAndLobbyReset() {
    Rig r(2, [](GameData& d, GameServerConfig& c) {
        d.config.player.startingHealth = 20;
        d.config.damage.baseDamageByStage = {8, 12};
        c.postMatchTicks = 60;
    });
    const ConnectionId a = r.Connect();
    const ConnectionId b = r.Connect();
    std::vector<Bot> bots;
    bots.emplace_back(r, a);
    bots.emplace_back(r, b);
    for (int tick = 0; tick < 12000 && r.server->state() == GameServer::State::Running; ++tick) {
        for (Bot& bot : bots) bot.Poll();
        r.Tick();
    }
    CHECK(r.server->state() == GameServer::State::Finished);
    const json::Value over = r.Last(a, "match_over");
    CHECK(Str(over, "type") == "match_over" && Num(over, "winner") == r.Match().Winner() && r.CountType(b, "match_over") == 1);
    // The finished match still answers (with the engine's own refusals) while players read the result...
    r.Say(a, R"({"action": "buy_xp", "id": 1})");
    CHECK(Str(r.Last(a, "result"), "result") == "WrongPhase" || Str(r.Last(a, "result"), "result") == "PlayerEliminated");
    // ...a newcomer is told the match is over...
    const ConnectionId late = r.Connect();
    CHECK(Str(r.Last(late, "error"), "code") == "match_finished");
    r.server->OnDisconnect(late);
    // ...and after the grace period everyone is sent home and a fresh lobby opens.
    r.Tick(70);
    CHECK(r.server->state() == GameServer::State::Lobby && r.net.closed.count(a) && r.net.closed[a].code == 1000 && r.net.closed.count(b));
    r.server->OnDisconnect(a);
    r.server->OnDisconnect(b);
    CHECK(r.server->connectedPlayers() == 0 && r.server->match() == nullptr);
    const ConnectionId n1 = r.Connect();
    const ConnectionId n2 = r.Connect();
    CHECK(Num(r.Last(n1, "welcome"), "player_id") == 0 && Num(r.Last(n2, "welcome"), "player_id") == 1 && r.server->state() == GameServer::State::Running);
    CHECK(r.CountType(n1, "match_started") == 1);
}


// A queue server's rig: the same data as Rig, a QueueServer in front of it.
struct QueueRig {
    Rig data;
    FakeTransport net;
    std::unique_ptr<QueueServer> hub;
    std::uint64_t now = 1000;
    ConnectionId nextConn = 100;
    QueueRig(int seats, long long fillMs, int abandonTicks = kTicksPerSecond * 120)
        : data(seats, [](GameData& d, GameServerConfig& c) {
              d.config.player.startingHealth = 20;
              d.config.damage.baseDamageByStage = {8, 12};
              c.postMatchTicks = 60;
          }) {
        QueueServerConfig qc;
        qc.match = data.cfg;
        qc.fillMs = fillMs;
        qc.abandonTicks = abandonTicks;
        hub = std::make_unique<QueueServer>(qc, data.data, net);
    }
    ConnectionId Connect(const std::string& token = "") { const ConnectionId id = ++nextConn; hub->OnConnect(id, token, now); return id; }
    void Say(ConnectionId id, const std::string& text) { hub->OnMessage(id, text, now); }
    void Tick(int n = 1) { for (int i = 0; i < n; ++i) { now += 33; hub->Tick(now); } }
    json::Value Last(ConnectionId id, const std::string& type) {
        json::Value found;
        for (const std::string& s : net.sent[id]) { json::Value v = ParseJson(s); if (Str(v, "type") == type) found = v; }
        return found;
    }
    int CountType(ConnectionId id, const std::string& type) {
        int n = 0;
        for (const std::string& s : net.sent[id]) n += Str(ParseJson(s), "type") == type ? 1 : 0;
        return n;
    }
};

static void TestQueueServer() {
    // 1. Bots queue: a match at once, you + AI players; when it is over you are back on the home screen with the socket still open.
    {
        QueueRig q(2, 5000);
        const ConnectionId a = q.Connect();
        CHECK(Str(q.Last(a, "queue_status"), "state") == "idle" && Num(q.Last(a, "queue_status"), "seats") == 2);
        q.Say(a, R"({"action": "buy_xp"})");
        CHECK(Str(q.Last(a, "error"), "code") == "not_in_match");
        q.Say(a, R"({"action": "get_catalog"})");
        CHECK(q.CountType(a, "catalog") == 1);
        q.Say(a, R"({"action": "queue", "mode": "bots"})");
        const json::Value found = q.Last(a, "match_found");
        CHECK(Str(found, "mode") == "bots" && Num(found, "humans") == 1 && Num(found, "bots") == 1);
        CHECK(q.CountType(a, "welcome") == 1 && q.CountType(a, "match_started") == 1 && q.hub->matches() == 1);
        const GameServer* game = q.hub->MatchOf(a);
        CHECK(game != nullptr && game->state() == GameServer::State::Running && game->bots() == 1);
        q.Say(a, R"({"action": "queue", "mode": "normal", "id": 3})");
        CHECK(Str(q.Last(a, "error"), "code") == "in_match" && Num(q.Last(a, "error"), "id") == 3);
        q.Say(a, R"({"action": "get_state"})");   // match commands reach the match
        CHECK(q.CountType(a, "state") >= 2);
        for (int tick = 0; tick < 20000 && q.CountType(a, "match_over") == 0; ++tick) q.Tick();
        CHECK(q.CountType(a, "match_over") == 1);
        q.Tick(80);
        CHECK(Str(q.Last(a, "queue_status"), "reason") == "match_over" && Str(q.Last(a, "queue_status"), "state") == "idle");
        CHECK(!q.net.closed.count(a) && q.hub->matches() == 0 && q.hub->MatchOf(a) == nullptr);
        q.Say(a, R"({"action": "queue", "mode": "bots"})");   // and play again on the same connection
        CHECK(q.CountType(a, "match_found") == 2 && q.hub->matches() == 1);
    }
    // 1b. Eliminated before the match is over: still in the match (watching) until it ends or you leave.
    {
        QueueRig q(4, 5000);
        const ConnectionId a = q.Connect();
        q.Say(a, R"({"action": "queue", "mode": "bots"})");
        int tick = 0;
        for (; tick < 40000 && q.CountType(a, "match_over") == 0; ++tick) {
            q.Tick();
            if (q.CountType(a, "player_eliminated") > 0 && q.hub->MatchOf(a) == nullptr) break;
        }
        const bool eliminatedEarly = q.CountType(a, "match_over") == 0;
        CHECK(q.CountType(a, "match_over") == 1);
        CHECK(!eliminatedEarly && q.hub->MatchOf(a) != nullptr);   // after match_over: still in the match (reading the result) until the post-match wait ends
        q.Tick(80);
        CHECK(Str(q.Last(a, "queue_status"), "reason") == "match_over" && Num(q.Last(a, "queue_status"), "matches") == 0);
    }
    // 2. Normal queue: waits for players; a full queue starts at once, otherwise AI players fill the seats after fillMs.
    {
        QueueRig q(4, 3000);
        const ConnectionId a = q.Connect(), b = q.Connect(), c = q.Connect();
        for (ConnectionId id : {a, b, c}) q.Say(id, R"({"action": "queue", "mode": "normal"})");
        CHECK(q.hub->searching(QueueMode::Normal) == 3 && q.hub->matches() == 0);
        CHECK(Str(q.Last(c, "queue_status"), "state") == "searching" && Num(q.Last(c, "queue_status"), "in_queue") == 3 && Num(q.Last(c, "queue_status"), "fill_ms") == 3000);
        q.Say(b, R"({"action": "leave_queue"})");
        CHECK(Str(q.Last(b, "queue_status"), "reason") == "cancelled" && q.hub->searching(QueueMode::Normal) == 2);
        q.Say(b, R"({"action": "leave_queue"})");
        CHECK(Str(q.Last(b, "error"), "code") == "not_searching");
        q.Tick(60);   // 2 s: still waiting, with status updates
        CHECK(q.hub->matches() == 0 && q.CountType(a, "queue_status") >= 2 && Num(q.Last(a, "queue_status"), "waited_ms") >= 1000);
        q.Tick(40);   // past 3 s: a and c play together, with 2 AI players
        CHECK(q.hub->matches() == 1 && q.hub->MatchOf(a) != nullptr && q.hub->MatchOf(a) == q.hub->MatchOf(c) && q.hub->MatchOf(b) == nullptr);
        CHECK(Num(q.Last(a, "match_found"), "humans") == 2 && Num(q.Last(a, "match_found"), "bots") == 2 && q.hub->MatchOf(a)->bots() == 2);
        CHECK(Num(q.Last(a, "welcome"), "player_id") != Num(q.Last(c, "welcome"), "player_id"));
        // Four players at once: no waiting.
        const ConnectionId d = q.Connect(), e = q.Connect(), f = q.Connect();
        for (ConnectionId id : {b, d, e, f}) q.Say(id, R"({"action": "queue", "mode": "normal"})");
        CHECK(q.hub->matches() == 2 && q.hub->MatchOf(b) == q.hub->MatchOf(f) && q.hub->MatchOf(b)->bots() == 0 && q.hub->MatchOf(b)->state() == GameServer::State::Running);

        // 3. Reconnect: a dropped player's token brings them straight back into their match.
        const std::string token = Str(q.Last(a, "welcome"), "token");
        const GameServer* match = q.hub->MatchOf(a);
        q.hub->OnDisconnect(a);
        const ConnectionId a2 = q.Connect(token);
        CHECK(q.hub->MatchOf(a2) == match && q.CountType(a2, "welcome") == 1 && q.CountType(a2, "queue_status") == 0);
        const ConnectionId stranger = q.Connect(std::string(32, 'f'));   // an unknown token: just the home screen
        CHECK(q.hub->MatchOf(stranger) == nullptr && Str(q.Last(stranger, "queue_status"), "state") == "idle");

        // 4. leave_match: back home, the match goes on; with nobody left it is closed after abandonTicks.
        q.Say(a2, R"({"action": "leave_match"})");
        q.Say(c, R"({"action": "leave_match"})");
        CHECK(Str(q.Last(a2, "queue_status"), "reason") == "left_match" && q.hub->MatchOf(a2) == nullptr && q.hub->MatchOf(c) == nullptr);
        q.Say(c, R"({"action": "leave_match"})");
        CHECK(Str(q.Last(c, "error"), "code") == "not_in_match");
    }
    {
        QueueRig q(2, 1000, 90);
        const ConnectionId a = q.Connect();
        q.Say(a, R"({"action": "queue", "mode": "bots"})");
        q.Say(a, R"({"action": "leave_match"})");
        CHECK(q.hub->matches() == 1);
        q.Tick(95);
        CHECK(q.hub->matches() == 0 && q.hub->online() == 1);
    }
    // 5. Bad messages from idle connections are protocol errors; a single-lobby GameServer refuses queue commands.
    {
        QueueRig q(2, 1000);
        const ConnectionId a = q.Connect();
        q.Say(a, R"({"action": "queue", "mode": "ranked"})");
        CHECK(Str(q.Last(a, "error"), "code") == "out_of_range" && q.hub->matches() == 0);
        q.Say(a, R"({"action": "queue"})");
        CHECK(Str(q.Last(a, "error"), "code") == "missing_field");
        Rig r(2);
        const ConnectionId s = r.Connect();
        r.Say(s, R"({"action": "queue", "mode": "bots"})");
        CHECK(Str(r.Last(s, "error"), "code") == "no_queue");
    }
}

static void TestGameServerFuzz() {
    // Connections come, go, shout garbage, send plausible commands, reconnect with tokens; time passes. Nothing may crash, the
    // engine's invariants must hold at the end, and the server must still be answering.
    Rig r(4, [](GameData&, GameServerConfig& c) { c.maxViolations = 1000000; c.rateBurst = 1000000; });
    Rng rng(4242);
    std::vector<ConnectionId> live;
    std::vector<std::string> tokens;
    const auto randomCommand = [&]() -> std::string {
        switch (rng.NextBelow(14)) {
            case 0: return R"({"action": "buy_unit", "shop_index": )" + std::to_string(rng.NextBelow(7)) + "}";
            case 1: return R"({"action": "reroll_shop"})";
            case 2: return R"({"action": "buy_xp"})";
            case 3: return R"({"action": "sell_unit", "unit_id": )" + std::to_string(16777217 + rng.NextBelow(5) + (rng.NextBelow(2) ? 16777216u : 0u)) + "}";
            case 4: return R"({"action": "move_unit", "unit_id": )" + std::to_string(16777217 + rng.NextBelow(9)) + R"(, "location": ")" + (rng.NextBelow(2) ? "board" : "bench") +
                           R"(", "x": )" + std::to_string(rng.NextBelow(9)) + R"(, "y": )" + std::to_string(rng.NextBelow(4)) + "}";
            case 5: return R"({"action": "equip_item", "unit_id": )" + std::to_string(16777217 + rng.NextBelow(9)) + R"(, "item_id": )" + std::to_string(1 + rng.NextBelow(4)) + "}";
            case 6: return R"({"action": "unequip_item", "unit_id": )" + std::to_string(16777217 + rng.NextBelow(9)) + R"(, "slot": )" + std::to_string(rng.NextBelow(4)) + "}";
            case 7: return R"({"action": "get_state"})";
            case 8: return R"({"action": "get_fight", "fight_index": )" + std::to_string(rng.NextBelow(9)) + "}";
            case 9: return R"({"action": "ping", "id": )" + std::to_string(rng.NextBelow(1000)) + "}";
            default: {
                std::string s;
                const std::size_t n = rng.NextBelow(40);
                for (std::size_t k = 0; k < n; ++k) s.push_back(static_cast<char>(rng.NextBelow(256)));
                return s;
            }
        }
    };
    for (int step = 0; step < 30000; ++step) {
        switch (rng.NextBelow(10)) {
            case 0: {
                const std::string token = (!tokens.empty() && rng.NextBelow(2)) ? tokens[rng.NextBelow(static_cast<std::uint32_t>(tokens.size()))] : "";
                const ConnectionId c = r.Connect(token);
                live.push_back(c);
                const json::Value w = r.Last(c, "welcome");
                if (w.IsObject()) tokens.push_back(Str(w, "token"));
                break;
            }
            case 1:
                if (!live.empty()) {
                    const std::size_t i = rng.NextBelow(static_cast<std::uint32_t>(live.size()));
                    r.server->OnDisconnect(live[i]);
                    live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
                }
                break;
            case 2: r.Tick(1 + static_cast<int>(rng.NextBelow(6))); break;
            default:
                if (!live.empty()) r.Say(live[rng.NextBelow(static_cast<std::uint32_t>(live.size()))], randomCommand());
                break;
        }
        r.net.sent.clear();   // keep memory flat
        if (r.server->connectedPlayers() > r.server->seats()) { CHECK(false); break; }
    }
    CHECK(r.server->connectedPlayers() <= r.server->seats());
    if (r.server->match()) CHECK(r.Match().VerifyPoolIntegrity() && r.Match().VerifyRosterLayouts());
    // Still alive and answering: a player of the running match can come back with their token, or the lobby takes a newcomer.
    ConnectionId probe = 0;
    for (auto it = tokens.rbegin(); it != tokens.rend() && r.server->state() != GameServer::State::Lobby; ++it) {
        const ConnectionId c = r.Connect(*it);
        if (r.net.closed.count(c) == 0) { probe = c; break; }
    }
    if (probe == 0) probe = r.Connect();
    r.Say(probe, R"({"action": "ping", "id": 1})");
    CHECK(r.CountType(probe, "pong") == 1);
}

// ==== the real TCP server over loopback sockets ===================================================

namespace {

// Loopback client sockets: POSIX and Winsock behind a few tiny shims, so the same tests run on every platform.
#ifdef _WIN32
using SockFd = SOCKET;
constexpr SockFd kBadSock = INVALID_SOCKET;
void SockClose(SockFd fd) { closesocket(fd); }
void SockNonBlocking(SockFd fd) { u_long mode = 1; ioctlsocket(fd, FIONBIO, &mode); }
bool SockWouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
struct WsaGuard {   // (the server starts Winsock too; it is reference-counted)
    WsaGuard() { WSADATA data; WSAStartup(MAKEWORD(2, 2), &data); }
    ~WsaGuard() { WSACleanup(); }
} g_wsaGuard;
#else
using SockFd = int;
constexpr SockFd kBadSock = -1;
void SockClose(SockFd fd) { close(fd); }
void SockNonBlocking(SockFd fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }
bool SockWouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
#endif
long long SockSend(SockFd fd, const char* data, std::size_t n) {
#ifdef _WIN32
    return send(fd, data, static_cast<int>(std::min<std::size_t>(n, 1u << 20)), 0);
#else
    return send(fd, data, n, 0);
#endif
}
long long SockRecv(SockFd fd, char* buf, std::size_t n) {
#ifdef _WIN32
    return recv(fd, buf, static_cast<int>(std::min<std::size_t>(n, 1u << 20)), 0);
#else
    return recv(fd, buf, n, 0);
#endif
}
void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// A blocking-free test client: a plain TCP socket plus just enough WebSocket to talk to the server. Its frame READER is written
// separately from the server's parser (it only has to understand unmasked server frames).
struct RawClient;
std::set<RawClient*>& LiveClients() {
    static std::set<RawClient*> live;
    return live;
}

struct RawClient {
    SockFd fd = kBadSock;
    std::string in;
    bool closed = false;

    RawClient() { LiveClients().insert(this); }
    RawClient(const RawClient&) = delete;
    RawClient& operator=(const RawClient&) = delete;
    ~RawClient() {
        LiveClients().erase(this);
        Drop();
    }
    bool Connect(std::uint16_t port) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == kBadSock) return false;
        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        SockNonBlocking(fd);
        return true;
    }
    void Drop() {
        if (fd != kBadSock) SockClose(fd);
        fd = kBadSock;
        closed = true;
    }
    void SendRaw(const std::string& bytes) {
        std::size_t off = 0;
        while (off < bytes.size() && fd != kBadSock) {
            const long long n = SockSend(fd, bytes.data() + off, bytes.size() - off);
            if (n > 0) off += static_cast<std::size_t>(n);
            else if (n < 0 && SockWouldBlock()) continue;
            else { closed = true; return; }
        }
    }
    void Read() {
        if (fd == kBadSock) return;
        char buf[65536];
        for (;;) {
            const long long n = SockRecv(fd, buf, sizeof(buf));
            if (n > 0) in.append(buf, static_cast<std::size_t>(n));
            else if (n == 0) { closed = true; return; }
            else if (SockWouldBlock()) return;
            else { closed = true; return; }
        }
    }
    static std::string HandshakeRequest(const std::string& path = "/", const std::string& key = "dGhlIHNhbXBsZSBub25jZQ==") {
        return "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    }
    // Removes and returns the HTTP response head if it is complete.
    bool TakeHttpHead(std::string& head) {
        const std::size_t end = in.find("\r\n\r\n");
        if (end == std::string::npos) return false;
        head = in.substr(0, end + 4);
        in.erase(0, end + 4);
        return true;
    }
    // One server frame (never masked), if a whole one has arrived.
    bool TakeFrame(int& opcode, std::string& payload) {
        if (in.size() < 2) return false;
        const auto b0 = static_cast<std::uint8_t>(in[0]);
        const auto b1 = static_cast<std::uint8_t>(in[1]);
        std::uint64_t len = b1 & 0x7F;
        std::size_t hdr = 2;
        if (len == 126) {
            if (in.size() < 4) return false;
            len = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(in[2])) << 8) | static_cast<std::uint8_t>(in[3]);
            hdr = 4;
        } else if (len == 127) {
            if (in.size() < 10) return false;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<std::uint8_t>(in[2 + static_cast<std::size_t>(i)]);
            hdr = 10;
        }
        if (in.size() < hdr + len) return false;
        opcode = b0 & 0x0F;
        payload = in.substr(hdr, static_cast<std::size_t>(len));
        in.erase(0, hdr + static_cast<std::size_t>(len));
        return true;
    }
};

struct NetRig {
    Rig rig;   // the game data and configuration (its own fake transport is unused here)
    std::unique_ptr<TcpServer> tcp;
    std::unique_ptr<GameServer> game;
    std::uint64_t now = 1;

    NetRig(int seats, const std::function<void(TcpServerConfig&)>& tweakTcp = {}, const std::function<void(GameData&, GameServerConfig&)>& tweakGame = {})
        : rig(seats, tweakGame) {
        TcpServerConfig cfg;
        cfg.bindAddress = "127.0.0.1";
        cfg.port = 0;
        if (tweakTcp) tweakTcp(cfg);
        tcp = std::make_unique<TcpServer>(cfg);
        game = std::make_unique<GameServer>(rig.cfg, rig.data, *tcp);
        tcp->SetHandler(game.get());
        std::string err;
        if (!tcp->Listen(&err)) std::printf("  Listen failed: %s\n", err.c_str());
    }
    void Step(int pollMs = 1) {
        tcp->RunOnce(pollMs, now);
        for (RawClient* c : LiveClients()) c->Read();
    }
    bool PumpUntil(const std::function<bool()>& done, int maxSteps = 600) {
        for (int i = 0; i < maxSteps; ++i) {
            Step();
            if (done()) return true;
        }
        return done();
    }
    // Opens a connection, completes the WebSocket handshake, returns once the 101 response is in.
    bool Open(RawClient& c, const std::string& path = "/") {
        if (!c.Connect(tcp->port())) return false;
        c.SendRaw(RawClient::HandshakeRequest(path));
        std::string head;
        if (!PumpUntil([&] { return c.TakeHttpHead(head); })) return false;
        return head.find("101 Switching Protocols") != std::string::npos && head.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos;
    }
    // Waits for the next text frame and parses it as JSON.
    bool NextJson(RawClient& c, json::Value& out) {
        int op = 0;
        std::string payload;
        if (!PumpUntil([&] { return c.TakeFrame(op, payload) && (op == 1 || op == 8); })) return false;
        if (op == 8) return false;
        out = ParseJson(payload);
        return true;
    }
    // Reads until a frame of `type` arrives (skipping others).
    bool WaitForType(RawClient& c, const std::string& type, json::Value& out) {
        for (int i = 0; i < 40; ++i) {
            if (!NextJson(c, out)) return false;
            if (Str(out, "type") == type) return true;
        }
        return false;
    }
    // Waits for a close frame; returns its code (0 if none arrived).
    int WaitForClose(RawClient& c) {
        int op = 0;
        std::string payload;
        int code = 0;
        PumpUntil([&] {
            while (c.TakeFrame(op, payload)) {
                if (op == 8) { code = payload.size() >= 2 ? (static_cast<std::uint8_t>(payload[0]) << 8) | static_cast<std::uint8_t>(payload[1]) : 1005; return true; }
            }
            return false;
        });
        return code;
    }
    ~NetRig() { tcp->Shutdown(); }
};

std::string Masked(const std::string& text) { return Frame(WsOpcode::Text, text); }

}  // namespace

static void TestSocketHandshakeAndMessages() {
    NetRig n(2);
    RawClient a;
    CHECK(n.Open(a));
    json::Value welcome;
    CHECK(n.NextJson(a, welcome) && Str(welcome, "type") == "welcome" && Num(welcome, "player_id") == 0);
    a.SendRaw(Masked(R"({"action": "ping", "id": 5})"));
    json::Value pong;
    CHECK(n.WaitForType(a, "pong", pong) && Num(pong, "id") == 5);

    // A WebSocket ping is answered with a pong carrying the same payload.
    a.SendRaw(Frame(WsOpcode::Ping, "are you there"));
    int op = 0;
    std::string payload;
    CHECK(n.PumpUntil([&] { return a.TakeFrame(op, payload) && op == 10; }) && payload == "are you there");

    // A polite close is answered with a close, and the socket then ends.
    a.SendRaw(Frame(WsOpcode::Close, std::string("\x03\xe8" "bye", 5)));
    CHECK(n.WaitForClose(a) == 1000);
    CHECK(n.PumpUntil([&] { return a.closed; }));   // the server half-closed after its close frame...
    a.Drop();                                       // ...and a well-behaved client closes its side too
    CHECK(n.PumpUntil([&] { return n.tcp->connectionCount() == 0; }));
    CHECK(n.game->connectedPlayers() == 0);   // the lobby seat came back

    // The token in the URL is passed through: a second connection with the first player's token in a lobby is a new player.
    RawClient b;
    CHECK(n.Open(b, "/?token=0123456789abcdef0123456789abcdef"));
    json::Value w2;
    CHECK(n.NextJson(b, w2) && Num(w2, "player_id") == 0 && w2.Find("reconnected")->AsBool() == false);
}

static void TestSocketBadHandshakes() {
    NetRig n(2);
    struct Case { std::string request; const char* status; };
    const std::string good = "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n";
    const Case cases[] = {
        {"GET / HTTP/1.1\r\nHost: x\r\n\r\n", "400"},                                                                    // a plain browser request
        {"POST / HTTP/1.1\r\n" + good + "\r\n", "405"},
        {"GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 7\r\n\r\n", "426"},
        {"GET / HTTP/1.1\r\n" + good + "X-Big: " + std::string(9000, 'a') + "\r\n\r\n", "431"},
        {"GET / HTTP/1.1\r\n" + good + "X-Big: " + std::string(9000, 'a'), "431"},                                       // never even ends
        {"GARBAGE\r\n\r\n", "400"},
        {std::string("\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03", 11) + "\r\n\r\n", "400"},                            // a TLS ClientHello sent to a plain port
    };
    for (const Case& c : cases) {
        RawClient client;
        CHECK(client.Connect(n.tcp->port()));
        client.SendRaw(c.request);
        std::string head;
        const bool got = n.PumpUntil([&] { return client.TakeHttpHead(head); });
        if (!got || head.find(std::string("HTTP/1.1 ") + c.status) != 0) std::printf("  wanted %s, got '%.60s'\n", c.status, head.c_str());
        CHECK(got && head.find(std::string("HTTP/1.1 ") + c.status) == 0);
        CHECK(n.PumpUntil([&] { return client.closed; }));   // and the connection is ended
    }
    CHECK(n.PumpUntil([&] { return n.tcp->connectionCount() == 0; }));
    CHECK(n.game->connectedPlayers() == 0);   // none of them ever became a player
}

static void TestSocketProtocolViolations() {
    NetRig n(2, [](TcpServerConfig& c) { c.maxMessageBytes = 8192; });
    struct Case { const char* name; std::string bytes; int code; };
    static const std::uint8_t mask[4] = {1, 2, 3, 4};
    const Case cases[] = {
        {"unmasked frame", std::string("\x81\x02hi", 4), 1002},
        {"binary frame", Frame(WsOpcode::Binary, "x"), 1003},
        {"oversized message", Frame(WsOpcode::Text, std::string(9000, 'a')), 1009},
        {"invalid UTF-8", Frame(WsOpcode::Text, "\xC3\x28"), 1007},
        {"reserved bit", std::string(1, static_cast<char>(0xC1)) + std::string("\x80\x01\x02\x03\x04", 5), 1002},
        {"control frame too large", EncodeMaskedFrame(WsOpcode::Ping, std::string(200, 'p'), mask), 1002},
        {"continuation from nowhere", Frame(WsOpcode::Continuation, "x"), 1002},
    };
    for (const Case& c : cases) {
        RawClient client;
        CHECK(n.Open(client));
        client.SendRaw(c.bytes);
        const int code = n.WaitForClose(client);
        if (code != c.code) std::printf("  %s: close code %d, wanted %d\n", c.name, code, c.code);
        CHECK(code == c.code);
        CHECK(n.PumpUntil([&] { return client.closed; }));
    }
    // The server is unharmed: a fresh client works.
    RawClient ok;
    CHECK(n.Open(ok));
    ok.SendRaw(Masked(R"({"action": "ping", "id": 1})"));
    json::Value pong;
    CHECK(n.WaitForType(ok, "pong", pong));
}

static void TestSocketFragmentedDelivery() {
    NetRig n(2);
    RawClient a;
    CHECK(a.Connect(n.tcp->port()));
    // The handshake and the first two commands, delivered one byte at a time.
    const std::string all = RawClient::HandshakeRequest() + Masked(R"({"action": "ping", "id": 1})") + Masked(R"({"action": "ping", "id": 2})");
    for (char ch : all) {
        a.SendRaw(std::string(1, ch));
        n.Step(0);
    }
    json::Value first, p1, p2;
    std::string head;
    CHECK(n.PumpUntil([&] { return a.TakeHttpHead(head); }));
    CHECK(n.WaitForType(a, "pong", p1) && Num(p1, "id") == 1);
    CHECK(n.WaitForType(a, "pong", p2) && Num(p2, "id") == 2);

    // A handshake and a frame coalesced into one TCP segment, and several frames in one segment.
    RawClient b;
    CHECK(b.Connect(n.tcp->port()));
    b.SendRaw(RawClient::HandshakeRequest() + Masked(R"({"action": "ping", "id": 10})") + Masked(R"({"action": "ping", "id": 11})") + Masked(R"({"action": "ping", "id": 12})"));
    CHECK(n.PumpUntil([&] { return b.TakeHttpHead(head); }));
    std::vector<long long> ids;
    json::Value msg;
    while (ids.size() < 3 && n.WaitForType(b, "pong", msg)) ids.push_back(Num(msg, "id"));
    CHECK((ids == std::vector<long long>{10, 11, 12}));

    // One command split across two segments with a pause between them.
    const std::string cmd = Masked(R"({"action": "ping", "id": 20})");
    b.SendRaw(cmd.substr(0, 5));
    for (int i = 0; i < 20; ++i) n.Step();
    b.SendRaw(cmd.substr(5));
    CHECK(n.WaitForType(b, "pong", msg) && Num(msg, "id") == 20);
}

static void TestSocketAbruptDisconnects() {
    NetRig n(2);
    // Every way a peer can vanish, many times over.
    for (int round = 0; round < 10; ++round) {
        {   RawClient c; c.Connect(n.tcp->port()); c.Drop(); }                                                 // connected, said nothing, gone
        {   RawClient c; c.Connect(n.tcp->port()); c.SendRaw("GET / HTT"); c.Drop(); }                        // half a request
        {   RawClient c; c.Connect(n.tcp->port()); c.SendRaw(RawClient::HandshakeRequest().substr(0, 60)); n.Step(); c.Drop(); }
        {   RawClient c; CHECK(n.Open(c)); c.Drop(); }                                                                     // handshaken, then gone
        {   RawClient c; CHECK(n.Open(c)); const std::string f = Masked(R"({"action": "ping"})"); c.SendRaw(f.substr(0, f.size() / 2)); c.Drop(); }   // half a frame
        {   RawClient c; CHECK(n.Open(c)); c.SendRaw(Masked(R"({"action": "ping"})")); c.Drop(); }                          // spoke and left before the answer
        for (int i = 0; i < 30; ++i) n.Step();
    }
    CHECK(n.PumpUntil([&] { return n.tcp->connectionCount() == 0; }));
    CHECK(n.game->connectedPlayers() == 0 && n.game->state() == GameServer::State::Lobby);   // no ghost seats left behind
    RawClient ok;
    CHECK(n.Open(ok));
    json::Value w;
    CHECK(n.NextJson(ok, w) && Num(w, "player_id") == 0);   // the first seat is free again
    // Sending to a connection that no longer exists is harmless.
    n.tcp->Send(999999, "nobody home");
    n.tcp->Close(999999, 1000, "nobody home");
}

static void TestSocketTimeouts() {
    NetRig n(2, [](TcpServerConfig& c) { c.handshakeTimeoutMs = 2000; c.idleTimeoutMs = 5000; c.pingIntervalMs = 1000; c.closeGraceMs = 500; });
    // Slowloris: a request that never finishes is dropped after the handshake timeout.
    RawClient slow;
    CHECK(slow.Connect(n.tcp->port()));
    slow.SendRaw("GET / HTTP/1.1\r\nHost: x\r\nUpgrade: web");
    n.Step();
    CHECK(n.tcp->connectionCount() == 1);
    n.now += 1900;
    n.Step();
    CHECK(n.tcp->connectionCount() == 1 && !slow.closed);
    n.now += 200;
    CHECK(n.PumpUntil([&] { return slow.closed; }));
    CHECK(n.tcp->connectionCount() == 0);

    // A silent client is pinged, and dropped when it never answers; one that answers stays.
    RawClient quiet, lively;
    CHECK(n.Open(quiet));
    CHECK(n.Open(lively));
    json::Value ignore;
    int pingsSeen = 0;
    bool quietClosed = false;
    for (int second = 0; second < 8; ++second) {
        n.now += 1000;
        for (int i = 0; i < 5; ++i) n.Step();
        int op = 0;
        std::string payload;
        while (lively.TakeFrame(op, payload)) {
            if (op == 9) { ++pingsSeen; lively.SendRaw(Frame(WsOpcode::Pong, payload)); }
        }
        while (quiet.TakeFrame(op, payload)) {
            if (op == 8) quietClosed = true;   // (no pong from this one: it "does not answer")
        }
    }
    for (int i = 0; i < 20; ++i) { n.now += 100; n.Step(); }
    CHECK(pingsSeen >= 5);
    CHECK(quiet.closed || quietClosed);
    CHECK(!lively.closed);
    n.now += 100;
    lively.SendRaw(Masked(R"({"action": "ping", "id": 3})"));
    json::Value pong;
    CHECK(n.WaitForType(lively, "pong", pong));
}

static void TestSocketConnectionCap() {
    NetRig n(8, [](TcpServerConfig& c) { c.maxConnections = 3; });
    std::vector<std::unique_ptr<RawClient>> clients;
    for (int i = 0; i < 5; ++i) {
        clients.push_back(std::make_unique<RawClient>());
        CHECK(clients.back()->Connect(n.tcp->port()));
        clients.back()->SendRaw(RawClient::HandshakeRequest());
    }
    for (int i = 0; i < 100; ++i) n.Step();
    CHECK(n.tcp->connectionCount() == 3);   // the other two wait in the kernel's backlog: no resources spent on them
    int answered = 0;
    for (auto& c : clients) {
        std::string head;
        answered += c->TakeHttpHead(head) ? 1 : 0;
    }
    CHECK(answered == 3);
    // A slot frees up and the next in line is served; the last one keeps waiting until another slot frees.
    clients[0]->Drop();
    std::string head;
    CHECK(n.PumpUntil([&] { return clients[3]->TakeHttpHead(head); }, 300));
    for (int i = 0; i < 50; ++i) n.Step();
    CHECK(!clients[4]->TakeHttpHead(head) && n.tcp->connectionCount() == 3);
    clients[1]->Drop();
    CHECK(n.PumpUntil([&] { return clients[4]->TakeHttpHead(head); }, 300));
}

namespace {
struct RecordingHandler : IServerHandler {
    std::vector<ConnectionId> connected;
    std::vector<ConnectionId> disconnected;
    void OnConnect(ConnectionId id, std::string_view, std::uint64_t) override { connected.push_back(id); }
    void OnMessage(ConnectionId, std::string_view, std::uint64_t) override {}
    void OnDisconnect(ConnectionId id) override { disconnected.push_back(id); }
};
}  // namespace

static void TestSocketSlowReaderIsDropped() {
    TcpServerConfig cfg;
    cfg.bindAddress = "127.0.0.1";
    cfg.maxOutboundBytes = 256 * 1024;
    cfg.closeGraceMs = 300;
    TcpServer tcp(cfg);
    RecordingHandler handler;
    tcp.SetHandler(&handler);
    std::string err;
    CHECK(tcp.Listen(&err));
    RawClient c;
    CHECK(c.Connect(tcp.port()));
    c.SendRaw(RawClient::HandshakeRequest());
    std::uint64_t now = 1;
    for (int i = 0; i < 50 && handler.connected.empty(); ++i) tcp.RunOnce(1, now);
    CHECK(handler.connected.size() == 1);
    if (handler.connected.empty()) return;
    // The client never reads. The server keeps queueing until the cap, then gives up on this client instead of growing forever.
    const std::string big(64 * 1024, 'x');
    for (int i = 0; i < 100; ++i) {
        tcp.Send(handler.connected[0], big);
        tcp.RunOnce(0, now);
    }
    now += 1000;
    for (int i = 0; i < 50; ++i) tcp.RunOnce(1, now);
    CHECK(tcp.connectionCount() == 0 && handler.disconnected.size() == 1);
}

static void TestSocketFullMatchFlow() {
    NetRig n(2, {}, [](GameData& d, GameServerConfig&) { d.config.match.motherNatureTicks = 10; d.config.match.planningTicks = 200; d.config.player.startingGold = 30; });
    RawClient a, b;
    CHECK(n.Open(a));
    json::Value wa, wb;
    CHECK(n.NextJson(a, wa) && Str(wa, "type") == "welcome");
    const std::string tokenA = Str(wa, "token");
    CHECK(n.Open(b));
    CHECK(n.NextJson(b, wb) && Num(wb, "player_id") == 1);
    CHECK(n.PumpUntil([&] { return n.game->state() == GameServer::State::Running; }));
    json::Value started;
    CHECK(n.WaitForType(a, "match_started", started) && Num(started, "player_id") == 0);
    // A third player is turned away with a proper WebSocket close.
    RawClient c;
    CHECK(n.Open(c));
    json::Value err;
    CHECK(n.WaitForType(c, "error", err) && Str(err, "code") == "match_in_progress");
    CHECK(n.WaitForClose(c) == 1013);

    // Advance the game by hand (the real loop is tested below) into Planning, then shop over the wire.
    for (int i = 0; i < 12; ++i) { n.now += 33; n.game->Tick(n.now); }
    n.Step(2);
    a.SendRaw(Masked(R"({"action": "buy_unit", "shop_index": 0, "id": 1})"));
    json::Value result;
    CHECK(n.WaitForType(a, "result", result) && Str(result, "result") == "Ok" && Num(result, "id") == 1);
    json::Value state;
    CHECK(n.WaitForType(a, "state", state));
    CHECK(Num(state, "gold") == n.game->match()->Players().Get(0)->Gold() && n.game->match()->Players().Get(0)->Roster().Count() == 1);

    // Player A's connection is cut without warning; the match goes on, and the token brings them back on a new socket.
    a.Drop();
    CHECK(n.PumpUntil([&] { return n.game->connectedPlayers() == 1; }));
    for (int i = 0; i < 5; ++i) { n.now += 33; n.game->Tick(n.now); }
    RawClient a2;
    CHECK(n.Open(a2, "/?token=" + tokenA));
    json::Value back;
    CHECK(n.NextJson(a2, back) && Str(back, "type") == "welcome" && Num(back, "player_id") == 0 && back.Find("reconnected")->AsBool());
    json::Value restored;
    CHECK(n.WaitForType(a2, "state", restored));
    CHECK(Num(restored, "gold") == n.game->match()->Players().Get(0)->Gold());
    json::Value bench;
    CHECK(restored.Find("bench")->Items()[0].IsObject());   // the unit they bought before the drop is still theirs
}

static void TestSocketServerLoopRunsAndStops() {
    // The production loop, on a real thread: it paces itself with the wall clock, serves clients, and stops cleanly on request.
    TcpServerConfig cfg;
    cfg.bindAddress = "127.0.0.1";
    TcpServer tcp(cfg);
    Rig rig(2);
    GameServer game(rig.cfg, rig.data, tcp);
    tcp.SetHandler(&game);
    std::string err;
    CHECK(tcp.Listen(&err));
    std::atomic<bool> stop{false};
    std::thread loop([&] { RunServerLoop(tcp, game, stop); });

    RawClient a, b;
    const auto readAll = [](RawClient& c, int ms) {
        for (int i = 0; i < ms; ++i) {
            c.Read();
            SleepMs(1);
        }
    };
    CHECK(a.Connect(tcp.port()));
    a.SendRaw(RawClient::HandshakeRequest());
    readAll(a, 200);
    std::string head;
    CHECK(a.TakeHttpHead(head) && head.find("101") != std::string::npos);
    a.SendRaw(Masked(R"({"action": "ping", "id": 77})"));
    readAll(a, 200);
    bool gotPong = false;
    int op = 0;
    std::string payload;
    while (a.TakeFrame(op, payload)) {
        if (op == 1 && Str(ParseJson(payload), "type") == "pong") gotPong = true;
    }
    CHECK(gotPong);
    stop.store(true);
    loop.join();   // returns: the loop noticed the flag
    readAll(a, 100);
    bool sawGoingAway = false;
    while (a.TakeFrame(op, payload)) {
        if (op == 8 && payload.size() >= 2 && static_cast<std::uint8_t>(payload[0]) == 0x03 && static_cast<std::uint8_t>(payload[1]) == 0xE9) sawGoingAway = true;   // 1001
    }
    CHECK(sawGoingAway || a.closed);
    CHECK(tcp.connectionCount() == 0);
}

// ==== main =====================================================================================

// ==== Bot seats ====================================================================================

static void TestBotSeats() {
    {   // One human, three bots: the lobby waits for the human only, and the match starts the moment they connect.
        Rig r(4, [](GameData&, GameServerConfig& c) { c.bots = 3; });
        CHECK(r.server->seats() == 4 && r.server->bots() == 3);
        const ConnectionId a = r.Connect();
        const json::Value welcome = r.Last(a, "welcome");
        CHECK(Num(welcome, "player_id") == 0 && Num(welcome, "seats") == 4 && Num(welcome, "bots") == 3 && Num(welcome, "connected") == 1);
        CHECK(r.server->state() == GameServer::State::Running && r.server->connectedPlayers() == 1);
        const json::Value started = r.Last(a, "match_started");
        CHECK(Num(started, "player_id") == 0 && Num(started, "seats") == 4);
        const json::Value* botSeats = started.Find("bot_seats");
        long long b1 = 0, b2 = 0, b3 = 0;
        CHECK(botSeats && botSeats->Items().size() == 3 && botSeats->Items()[0].ToInt(b1) && botSeats->Items()[1].ToInt(b2) && botSeats->Items()[2].ToInt(b3) &&
              b1 == 1 && b2 == 2 && b3 == 3);
        CHECK(r.CountType(a, "lobby") == 1);   // (the one sent when the human took their seat)
        // Nobody else can take a bot's seat.
        const ConnectionId late = r.Connect();
        CHECK(Str(r.Last(late, "error"), "code") == "match_in_progress");

        // The bots play: they act in the planning phase like any other player (shop, roster), untouched by the human.
        r.RunUntil([&] { return IsPlanning(r); }, 200);
        r.Tick(10);
        for (PlayerId bot = 1; bot <= 3; ++bot) {
            const PlayerState* p = r.Match().Players().Get(bot);
            CHECK(p->IsAlive() && p->Roster().Count() > 0);
        }
        CHECK(r.Match().Players().Get(0)->Roster().Count() == 0);   // the human has not done anything
        // ...and nothing about them is ever sent to the human beyond what is public: every private message is about seat 0.
        for (const json::Value& m : r.Inbox(a)) {
            const std::string type = Str(m, "type");
            if (type == "state" || type == "income") CHECK(Num(m, "player_id") == 0);
            CHECK(type != "unit_event" || Num(*m.Find("unit"), "id") >> 24 == 1);   // unit ids carry (seat + 1) << 24: only seat 0's
        }
    }
    {   // Two humans and one bot: the match waits for both humans.
        Rig r(3, [](GameData&, GameServerConfig& c) { c.bots = 1; });
        const ConnectionId a = r.Connect();
        CHECK(r.server->state() == GameServer::State::Lobby && Num(r.Last(a, "lobby"), "bots") == 1 && Num(r.Last(a, "lobby"), "connected") == 1);
        const ConnectionId b = r.Connect();
        CHECK(Num(r.Last(a, "welcome"), "player_id") == 0 && Num(r.Last(b, "welcome"), "player_id") == 1 && r.server->state() == GameServer::State::Running);
        long long bot = 0;
        CHECK(r.Last(b, "match_started").Find("bot_seats")->Items()[0].ToInt(bot) && bot == 2);
    }
    {   // A human leaving the lobby frees a human seat: the next one to arrive gets it, and the bot seat stays a bot.
        Rig r(3, [](GameData&, GameServerConfig& c) { c.bots = 1; });
        const ConnectionId a = r.Connect();
        r.server->OnDisconnect(a);
        const ConnectionId a2 = r.Connect();
        const ConnectionId b = r.Connect();
        CHECK(Num(r.Last(a2, "welcome"), "player_id") == 0 && Num(r.Last(b, "welcome"), "player_id") == 1);
    }
    {   // The bot count is bounded: at least one human seat stays.
        Rig r(3, [](GameData&, GameServerConfig& c) { c.bots = 9; });
        CHECK(r.server->bots() == 2);
        Rig none(3, [](GameData&, GameServerConfig& c) { c.bots = -4; });
        CHECK(none.server->bots() == 0);
    }
    {   // A whole match: one human who does nothing against 7 bots. It ends, the engine's books balance the whole way through, and after the
        // grace period the lobby reopens with the same bot seats.
        Rig r(8, [](GameData& d, GameServerConfig& c) {
            c.bots = 7;
            c.postMatchTicks = 60;
            d.config.player.startingHealth = 30;
            d.config.damage.baseDamageByStage = {8, 12};
        });
        const ConnectionId a = r.Connect();
        CHECK(r.server->state() == GameServer::State::Running);
        bool balanced = true;
        for (int tick = 0; tick < 60000 && r.server->state() == GameServer::State::Running; ++tick) {
            r.Tick();
            if (tick % 50 == 0) balanced = balanced && r.Match().VerifyPoolIntegrity() && r.Match().VerifyRosterLayouts();
        }
        CHECK(balanced);
        CHECK(r.server->state() == GameServer::State::Finished && r.CountType(a, "match_over") == 1);
        r.Tick(70);
        CHECK(r.server->state() == GameServer::State::Lobby && r.server->bots() == 7);
        r.server->OnDisconnect(a);
        const ConnectionId again = r.Connect();
        CHECK(Num(r.Last(again, "welcome"), "player_id") == 0 && r.server->state() == GameServer::State::Running);
        r.Tick(600);   // the second match's bots are alive and playing
        CHECK(r.Match().Players().Get(7)->Roster().Count() > 0);
    }
    {   // Bots are deterministic: the same seed and the same inputs give the same match, tick for tick.
        std::uint64_t hashes[2] = {0, 0};
        for (int run = 0; run < 2; ++run) {
            Rig r(8, [](GameData&, GameServerConfig& c) { c.bots = 7; });
            r.Connect();
            r.Tick(4000);
            hashes[run] = r.Match().StateHash();
        }
        CHECK(hashes[0] != 0 && hashes[0] == hashes[1]);
    }
}

// ==== JSON Schemas (docs/schemas): the files a UE5 client is built from must describe exactly what the server sends ============================

#ifndef W2F_SCHEMA_DIR
#define W2F_SCHEMA_DIR "docs/schemas"
#endif

namespace {

std::string ReadWholeFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// A small JSON Schema checker: the subset the W2F schemas use (draft 2020-12 spelling). $ref (local #/$defs/...), type, enum, const, properties, required,
// additionalProperties, items, minItems / maxItems, minimum / maximum, minLength / maxLength, oneOf, anyOf. Everything else (title, description...) is ignored.
class SchemaChecker {
public:
    explicit SchemaChecker(const json::Value& root) : root_(root) {}

    // Empty string = valid; otherwise where and why not.
    std::string Check(const json::Value& schema, const json::Value& value) const { return Walk(schema, value, "$"); }

private:
    static bool TypeMatches(const std::string& type, const json::Value& v) {
        long long ignored = 0;
        if (type == "object") return v.IsObject();
        if (type == "array") return v.IsArray();
        if (type == "string") return v.IsString();
        if (type == "boolean") return v.IsBool();
        if (type == "null") return v.IsNull();
        if (type == "integer") return v.IsNumber() && v.ToInt(ignored);
        if (type == "number") return v.IsNumber();
        return false;
    }

    std::string Walk(const json::Value& schema, const json::Value& v, const std::string& at) const {
        if (const json::Value* ref = schema.Find("$ref")) {
            const std::string name = ref->AsString();
            const json::Value* defs = root_.Find("$defs");
            const json::Value* target = defs != nullptr && name.rfind("#/$defs/", 0) == 0 ? defs->Find(name.substr(8)) : nullptr;
            return target != nullptr ? Walk(*target, v, at) : at + ": unresolved $ref " + name;
        }
        if (const json::Value* type = schema.Find("type")) {
            bool ok = false;
            if (type->IsString()) ok = TypeMatches(type->AsString(), v);
            else for (const json::Value& t : type->Items()) ok = ok || TypeMatches(t.AsString(), v);
            if (!ok) return at + ": wrong type (expected " + (type->IsString() ? type->AsString() : std::string("one of several")) + ")";
        }
        if (const json::Value* c = schema.Find("const")) {
            long long a = 0, b = 0;
            const bool same = c->IsString() ? v.IsString() && v.AsString() == c->AsString() : c->IsNumber() ? v.IsNumber() && c->ToInt(a) && v.ToInt(b) && a == b : false;
            if (!same) return at + ": not the constant";
        }
        if (const json::Value* e = schema.Find("enum")) {
            bool found = false;
            for (const json::Value& option : e->Items()) found = found || (option.IsString() && v.IsString() && option.AsString() == v.AsString());
            if (!found) return at + ": not one of the allowed values" + (v.IsString() ? " (\"" + v.AsString() + "\")" : "");
        }
        long long n = 0;
        if (v.IsNumber() && v.ToInt(n)) {
            long long bound = 0;
            if (const json::Value* m = schema.Find("minimum")) { if (m->ToInt(bound) && n < bound) return at + ": below the minimum"; }
            if (const json::Value* m = schema.Find("maximum")) { if (m->ToInt(bound) && n > bound) return at + ": above the maximum"; }
        }
        if (v.IsString()) {
            long long bound = 0;
            if (const json::Value* m = schema.Find("minLength")) { if (m->ToInt(bound) && static_cast<long long>(v.AsString().size()) < bound) return at + ": too short"; }
            if (const json::Value* m = schema.Find("maxLength")) { if (m->ToInt(bound) && static_cast<long long>(v.AsString().size()) > bound) return at + ": too long"; }
        }
        if (v.IsArray()) {
            long long bound = 0;
            if (const json::Value* m = schema.Find("minItems")) { if (m->ToInt(bound) && static_cast<long long>(v.Items().size()) < bound) return at + ": too few items"; }
            if (const json::Value* m = schema.Find("maxItems")) { if (m->ToInt(bound) && static_cast<long long>(v.Items().size()) > bound) return at + ": too many items"; }
            if (const json::Value* items = schema.Find("items")) {
                for (std::size_t i = 0; i < v.Items().size(); ++i) {
                    const std::string why = Walk(*items, v.Items()[i], at + "[" + std::to_string(i) + "]");
                    if (!why.empty()) return why;
                }
            }
        }
        if (v.IsObject()) {
            if (const json::Value* required = schema.Find("required")) {
                for (const json::Value& key : required->Items()) {
                    if (v.Find(key.AsString()) == nullptr) return at + ": missing required field \"" + key.AsString() + "\"";
                }
            }
            const json::Value* properties = schema.Find("properties");
            const json::Value* additional = schema.Find("additionalProperties");
            for (std::size_t i = 0; i < v.MemberCount(); ++i) {
                const std::string& key = v.MemberKey(i);
                const json::Value* sub = properties != nullptr ? properties->Find(key) : nullptr;
                if (sub != nullptr) {
                    const std::string why = Walk(*sub, v.MemberValue(i), at + "." + key);
                    if (!why.empty()) return why;
                } else if (additional != nullptr && additional->IsBool() && !additional->AsBool()) {
                    return at + ": unexpected field \"" + key + "\"";
                }
            }
        }
        if (const json::Value* any = schema.Find("oneOf")) {
            int matches = 0;
            std::string firstWhy;
            for (const json::Value& option : any->Items()) {
                const std::string why = Walk(option, v, at);
                if (why.empty()) ++matches;
                else if (firstWhy.empty() || why.size() > firstWhy.size()) firstWhy = why;   // (the most specific complaint is usually the longest path)
            }
            if (matches != 1) return matches == 0 ? at + ": matches none of the alternatives; e.g. " + firstWhy : at + ": matches several alternatives";
        }
        return "";
    }

    const json::Value& root_;
};

}  // namespace

static void TestJsonSchemas() {
    const std::string serverText = ReadWholeFile(std::string(W2F_SCHEMA_DIR) + "/server-message.schema.json");
    const std::string clientText = ReadWholeFile(std::string(W2F_SCHEMA_DIR) + "/client-command.schema.json");
    json::Value serverSchema, clientSchema;
    std::string err;
    CHECK(json::Parse(serverText, serverSchema, &err) && json::Parse(clientText, clientSchema, &err));
    if (!serverSchema.IsObject() || !clientSchema.IsObject()) { std::printf("  cannot read the schemas: %s\n", err.c_str()); return; }
    const SchemaChecker server(serverSchema), client(clientSchema);
    std::map<std::string, int> validated;
    int problems = 0;
    const auto checkMessage = [&](const std::string& text) {
        const json::Value v = ParseJson(text);
        const std::string why = server.Check(serverSchema, v);
        if (!why.empty() && problems++ < 8) std::printf("  schema mismatch in a '%s' message: %s\n    %s\n", Str(v, "type").c_str(), why.c_str(), text.substr(0, 300).c_str());
        const std::string type = Str(v, "type");
        ++validated[type == "unit_event" || type == "gift_event" ? type + ":" + Str(v, "event") : type];
    };

    // 1. Every builder, called by hand with awkward arguments (all the optional fields, both sides of every enum).
    {
        Rig r(3, {}, sample::ProductionItemsPath());
        auto motherNature = w2f::LoadMotherNatureDatabaseFromFile(sample::ProductionMotherNaturePath(), r.items.get());
        const ChampionDefinition* champion = r.champions->Find(sample::kChampionAlesk);
        CHECK(champion != nullptr && motherNature != nullptr);
        UnitInstance unit;
        unit.id = (1u << 24) | 5u;
        unit.champion = champion;
        unit.starLevel = 2;
        unit.location = LocationType::Board;
        unit.x = 4;
        unit.y = 2;
        unit.items = {3, 7, 0};
        checkMessage(msg::Welcome(2, std::string(32, 'a'), true, 2, 8, 6, true));
        checkMessage(msg::Lobby({0, 1}, 8, 6));
        checkMessage(msg::Error("wrong_type", "detail", true, 12));
        checkMessage(msg::Error("invalid_json", "detail"));
        checkMessage(msg::Pong(true, 4));
        checkMessage(msg::Pong(false, 0));
        {
            msg::QueueInfo info;
            info.seats = 8;
            info.online = 3;
            checkMessage(msg::QueueStatus(info));
            info.reason = "match_over";
            checkMessage(msg::QueueStatus(info));
            info.searching = true;
            info.mode = QueueMode::Normal;
            info.inQueue = 2;
            info.waitedMs = 1500;
            info.fillMs = 30000;
            checkMessage(msg::QueueStatus(info));
            checkMessage(msg::MatchFound(QueueMode::Bots, 1, 7));
            checkMessage(msg::MatchFound(QueueMode::Normal, 3, 5));
        }
        Command command;
        command.type = CommandType::EquipItem;
        command.hasId = true;
        command.id = 9;
        checkMessage(msg::Result(command, ActionResult::NoItemsToRemove));
        command.hasId = false;
        for (int result = 0; result <= static_cast<int>(ActionResult::UnitInCombat); ++result) checkMessage(msg::Result(command, static_cast<ActionResult>(result)));
        IncomeBreakdown income;
        income.baseGold = 5;
        income.interestGold = 2;
        income.streakGold = 1;
        income.passiveXp = 2;
        checkMessage(msg::Income(3, 12, income));
        for (PveDropType type : {PveDropType::Gold, PveDropType::Champion, PveDropType::Item, PveDropType::None}) {
            PveDrop drop;
            drop.type = type;
            drop.gold = 3;
            drop.champion = 9001;
            drop.item = 4;
            checkMessage(msg::PveDropMsg(drop));
        }
        std::vector<GiftOffer> offers(5);
        offers[0].type = GiftType::Gold;   offers[0].gift = 102; offers[0].amount = 5;
        offers[1].type = GiftType::Xp;     offers[1].gift = 103; offers[1].amount = 4;
        offers[2].type = GiftType::Heal;   offers[2].gift = 104; offers[2].amount = 3;
        offers[3].type = GiftType::Item;   offers[3].gift = 101; offers[3].item = 3;
        offers[4].type = GiftType::Unit;   offers[4].gift = 105; offers[4].champion = champion;
        checkMessage(msg::GiftsOffered({offers[0], offers[3], offers[4], offers[1]}, motherNature.get()));   // (at most 4 offers: gift_index 0..3)
        for (const GiftOffer& offer : offers) {
            checkMessage(msg::GiftPicked(1, offer, motherNature.get(), false, 0));
            checkMessage(msg::GiftPicked(0, offer, motherNature.get(), true, 4));
        }
        checkMessage(msg::UnitBought(unit, 3));
        checkMessage(msg::UnitSold(unit, 2));
        UnitMove move;
        move.unit = unit;
        move.fromLocation = LocationType::Bench;
        move.fromX = 3;
        move.fromY = 0;
        checkMessage(msg::UnitMoved(move));
        UnitMerge merge;
        merge.upgraded = unit;
        merge.consumed = {(1u << 24) | 6u, (1u << 24) | 7u};
        merge.previousStarLevel = 1;
        merge.overflowItems = {4, 5};
        checkMessage(msg::UnitMerged(merge));
        checkMessage(msg::ItemEquipped(unit, 3));
        checkMessage(msg::ItemUnequipped(unit, 3));
        ItemCombination combination;
        combination.first = 3;
        combination.second = 3;
        combination.result = 24;
        checkMessage(msg::ItemsCombined(unit, combination));
        checkMessage(msg::ItemConsumed(unit, 50, {3, 4}));
        checkMessage(msg::BagItemsCombined(3, 3, 24));
        checkMessage(msg::PlayerDamaged(1, 7, 93));
        checkMessage(msg::PlayerEliminated(4, 6));
        checkMessage(msg::MatchStarted(r.data.config, 8, 0, 3, {1, 2, 3, 4, 5, 6, 7}));
        checkMessage(msg::Phase(r.data.config, MatchPhase::Combat, 5, 10, 600, 12345678901ull, false, false));
        checkMessage(msg::Phase(r.data.config, MatchPhase::MotherNature, 3, 0, 600, 0, true, true));
        checkMessage(msg::Catalog(*r.champions, r.items.get(), r.traits.get(), r.encounters.get(), r.data.config.combat, r.display.get()));
    }

    // 2. Whole matches: everything every client is sent (state, public_state, phases, fights, results, gifts, drops, events...).
    for (int variant = 0; variant < 2; ++variant) {
        std::unique_ptr<MotherNatureDatabase> mn;
        Rig r(3, [&](GameData& d, GameServerConfig& c) {
            c.bots = variant;   // one variant with only humans, one with a bot seat
            d.config.player.startingHealth = 30;
            d.config.damage.baseDamageByStage = {8, 12};
            d.config.match.shopClosedOpeningRounds = 1;   // the real opening rules for this test
            d.config.match.openingUnitCosts = {1};
            d.config.player.limitBoardToLevel = true;
        });
        mn = w2f::LoadMotherNatureDatabaseFromFile(sample::ProductionMotherNaturePath(), r.items.get());
        GameData withNature = r.data;
        withNature.motherNature = mn.get();
        // (The rig's server was built without Mother Nature: make a second server that has her, on the same fake transport.)
        r.server = std::make_unique<GameServer>(r.cfg, withNature, r.net);
        std::vector<ConnectionId> conns;
        for (int i = 0; i < 3 - variant; ++i) conns.push_back(r.Connect());
        std::vector<Bot> bots;
        for (ConnectionId c : conns) bots.emplace_back(r, c);
        for (int tick = 0; tick < 30000 && r.server->state() == GameServer::State::Running; ++tick) {
            for (Bot& bot : bots) bot.Poll();
            r.Tick();
            if (tick % 400 == 100) {   // now and then: state requests, fights of other players, and a few commands the engine will refuse
                for (ConnectionId c : conns) { r.Say(c, R"({"action": "get_state"})"); r.Say(c, R"({"action": "get_fight", "fight_index": 1})"); r.Say(c, R"({"action": "sell_unit", "unit_id": 99})"); }
                r.now += 1000;
            }
        }
        for (ConnectionId c : conns) {
            r.Say(c, R"({"action": "get_catalog"})");
            for (const std::string& text : r.net.sent[c]) checkMessage(text);
        }
    }
    for (const char* needed : {"welcome", "lobby", "catalog", "match_started", "phase", "state", "public_state", "income", "result", "error", "pong", "pve_drop", "gift_event:offered",
                               "gift_event:picked", "unit_event:bought", "unit_event:sold", "unit_event:moved", "unit_event:merged", "unit_event:item_equipped", "unit_event:item_unequipped",
                               "unit_event:items_combined", "unit_event:consumable_used", "combat_summary", "combat", "player_damaged", "player_eliminated", "match_over"}) {
        if (validated[needed] == 0) std::printf("  no '%s' message was validated\n", needed);
        CHECK(validated[needed] > 0 || std::string(needed) == "pong" || std::string(needed) == "error" || std::string(needed) == "match_over" || std::string(needed) == "player_eliminated");
    }
    // 2b. The sample fights handed to the UE5 developers (docs/sample_fights) are real messages: they must satisfy the schema too.
    for (const char* file : {"catalog", "01-melee-brawl", "02-ranged-projectiles-and-dots", "03-spell-areas", "04-overtime"}) {
        const std::string text = ReadWholeFile(std::string(W2F_SCHEMA_DIR) + "/../sample_fights/" + file + ".json");
        CHECK(!text.empty());
        if (!text.empty()) checkMessage(text);
    }
    CHECK(problems == 0);
    std::printf("  %zu kinds of message validated against docs/schemas/server-message.schema.json\n", validated.size());

    // 3. The commands: the schema accepts every command the parser accepts (over thousands of mutated messages), and refuses the malformed ones.
    {
        const char* valid[] = {R"({"action": "buy_unit", "shop_index": 2})", R"({"action":"buy_unit","shop_index":0,"id":17})", R"({"id": 0, "action": "reroll_shop"})", R"({"action": "buy_xp"})",
                               R"({"action": "pick_gift", "gift_index": 3})", R"({"action": "sell_unit", "unit_id": 16777217})", R"({"action": "move_unit", "unit_id": 5, "location": "bench", "x": 8})",
                               R"({"action": "move_unit", "unit_id": 5, "location": "board", "x": 6, "y": 3})", R"({"action": "equip_item", "unit_id": 5, "item_id": 3})",
                               R"({"action": "unequip_item", "unit_id": 5, "slot": 2})", R"({"action": "get_state"})", R"({"action": "get_fight", "fight_index": 7})", R"({"action": "get_catalog"})",
                               R"({"action": "ping", "id": 9007199254740991})", R"({"action": "queue", "mode": "bots"})", R"({"action": "queue", "mode": "normal", "id": 2})",
                               R"({"action": "leave_queue"})", R"({"action": "leave_match"})"};
        for (const char* text : valid) {
            const json::Value v = ParseJson(text);
            const std::string why = client.Check(clientSchema, v);
            if (!why.empty()) std::printf("  a valid command is refused by the schema: %s (%s)\n", text, why.c_str());
            CHECK(why.empty() && ParseCommand(text).ok);
        }
        const char* invalid[] = {R"({"action": "buy_unit"})", R"({"action": "buy_unit", "shop_index": -1})", R"({"action": "buy_unit", "shop_index": 64})", R"({"action": "buy_unit", "shop_index": 1.5})",
                                 R"({"action": "buy_unit", "shop_index": 1, "extra": 1})", R"({"action": "fly"})", R"({"action": "sell_unit", "unit_id": "1"})", R"({"action": "pick_gift", "gift_index": 4})",
                                 R"({"action": "move_unit", "unit_id": 5, "location": "moon", "x": 1})", R"({"action": "equip_item", "unit_id": 5, "item_id": 0})", R"({"action": "unequip_item", "unit_id": 5, "slot": 3})",
                                 R"({"action": "get_fight", "fight_index": 8})", R"({"action": "ping", "id": -1})", R"({"action": 5})", R"({})", R"([])",
                                 R"({"action": "queue"})", R"({"action": "queue", "mode": "ranked"})", R"({"action": "leave_queue", "mode": "bots"})"};
        for (const char* text : invalid) {
            json::Value v;
            std::string e;
            if (!json::Parse(text, v, &e)) continue;
            CHECK(!client.Check(clientSchema, v).empty() && !ParseCommand(text).ok);
        }
        Rng rng(4);
        int accepted = 0, mismatches = 0;
        for (int i = 0; i < 30000; ++i) {
            std::string text = valid[rng.NextBelow(static_cast<std::uint32_t>(sizeof(valid) / sizeof(valid[0])))];
            const int edits = 1 + static_cast<int>(rng.NextBelow(2));
            for (int k = 0; k < edits && !text.empty(); ++k) {
                const std::size_t at = rng.NextBelow(static_cast<std::uint32_t>(text.size()));
                if (rng.NextBelow(3) == 0) text.erase(at, 1);
                else text[at] = "0123456789 \":,{}"[rng.NextBelow(16)];
            }
            if (!ParseCommand(text).ok) continue;
            ++accepted;
            json::Value v;
            std::string e;
            if (!json::Parse(text, v, &e) || !client.Check(clientSchema, v).empty()) {   // the parser took it: the schema must too
                if (mismatches++ < 5) std::printf("  the parser accepts, the schema refuses: %s (%s)\n", text.c_str(), e.c_str());
            }
        }
        CHECK(accepted > 200 && mismatches == 0);
    }
}

static void TestOpeningRoundOverTheWire() {
    // The real rules: round 1 has no shop and a free unit for everyone, and the board holds `level` units. One human and three bots.
    Rig r(4, [](GameData& d, GameServerConfig& c) {
        c.bots = 3;
        d.config.match.shopClosedOpeningRounds = 1;
        d.config.match.openingUnitCosts = {1};
        d.config.player.limitBoardToLevel = true;
    });
    const ConnectionId a = r.Connect();
    r.Tick(3);
    const json::Value phase = r.Last(a, "phase");
    CHECK(Str(phase, "phase") == "Planning" && Num(phase, "round") == 1 && phase.Find("shop_closed") && phase.Find("shop_closed")->IsBool());
    CHECK(r.Last(a, "phase").Find("shop_closed")->AsBool() && !r.Last(a, "phase").Find("mother_nature")->AsBool());
    const json::Value state = r.Last(a, "state");
    long long owned = 0;
    for (const json::Value& u : state.Find("bench")->Items()) owned += u.IsNull() ? 0 : 1;
    CHECK(owned == 1 && state.Find("board")->Items().empty());   // the dealt unit waits on the bench
    for (const json::Value& slot : state.Find("shop")->Items()) {   // every shop slot is empty (0)
        long long id = -1;
        CHECK(slot.ToInt(id) && id == 0);
    }
    r.Say(a, R"({"action": "buy_unit", "shop_index": 0, "id": 5})");
    CHECK(Str(r.Last(a, "result"), "result") == "ShopClosed");
    r.Say(a, R"({"action": "reroll_shop", "id": 6})");
    CHECK(Str(r.Last(a, "result"), "result") == "ShopClosed");
    for (PlayerId bot = 1; bot <= 3; ++bot) {   // the bots field theirs at once
        const PlayerState* p = r.Match().Players().Get(bot);
        CHECK(p->Roster().Count() == 1 && p->Roster().BoardCount() == 1);
    }
    // The next round: the shop is open again, and the phase message says so.
    CHECK(r.RunUntil([&] { return r.Match().Round() == 2 && r.Match().Phase() == MatchPhase::Planning; }, 4000));
    r.Tick(2);
    CHECK(!r.Last(a, "phase").Find("shop_closed")->AsBool());
    long long filled = 0;
    const json::Value openState = r.Last(a, "state");   // (a named local: never range-for over a member of a temporary)
    for (const json::Value& slot : openState.Find("shop")->Items()) {
        long long id = 0;
        filled += slot.ToInt(id) && id != 0 ? 1 : 0;
    }
    CHECK(filled == r.Match().Config().shop.slotCount);
}

// ==== Phase C: crash recovery, private servers =====================================================================================

static void TestResumeAfterACrash() {
    const auto tweak = [](GameData& d, GameServerConfig& c) {
        c.bots = 2;
        d.config.match.shopClosedOpeningRounds = 1;
        d.config.match.openingUnitCosts = {1};
        d.config.player.limitBoardToLevel = true;
    };
    // Server A plays; every Planning phase hands its autosave (snapshot + seats) to the sink, like `--autosave` does.
    Rig a(4, tweak);
    struct Save { int round = 0; std::vector<std::uint8_t> snapshot; std::string seats; std::uint64_t hash = 0; };
    Save last;
    a.server->SetSnapshotSink([&](int round, const std::vector<std::uint8_t>& snapshot, const std::string& seats) {
        last = Save{round, snapshot, seats, a.server->match() != nullptr ? a.server->match()->StateHash() : 0};
    });
    const ConnectionId h0 = a.Connect();
    const ConnectionId h1 = a.Connect();
    std::vector<Bot> humans;
    humans.emplace_back(a, h0);
    humans.emplace_back(a, h1);
    CHECK(a.server->state() == GameServer::State::Running);
    const std::string token0 = Str(a.Last(h0, "welcome"), "token"), token1 = Str(a.Last(h1, "welcome"), "token");
    for (int tick = 0; tick < 20000 && last.round < 5; ++tick) {
        for (Bot& bot : humans) bot.Poll();
        a.Tick();
    }
    CHECK(last.round == 5 && !last.snapshot.empty() && last.hash != 0);
    // The sidecar is the small JSON the docs promise: tokens for the humans, none for the bots, the bots' state.
    const json::Value sidecar = ParseJson(last.seats);
    CHECK(Num(sidecar, "format") == 1 && Num(sidecar, "seats") == 4 && sidecar.Find("players")->Items().size() == 4 && sidecar.Find("bots")->Items().size() == 2);
    CHECK(Str(sidecar.Find("players")->Items()[0], "token") == token0 && Str(sidecar.Find("players")->Items()[1], "token") == token1 && Str(sidecar.Find("players")->Items()[2], "token").empty());
    int finishedCalls = 0;
    a.server->SetMatchFinishedHandler([&] { ++finishedCalls; });
    // A. the crash: a fresh server takes over from the saved pair and the humans reconnect with their old tokens.
    const std::string saved = last.seats;
    const std::vector<std::uint8_t> savedSnapshot = last.snapshot;
    const std::uint64_t savedHash = last.hash;
    Rig b(4, tweak);
    std::string error;
    CHECK(b.server->Resume(savedSnapshot, saved, &error));
    if (!error.empty()) std::printf("  %s\n", error.c_str());
    CHECK(b.server->state() == GameServer::State::Running && b.Match().Round() == 5 && b.Match().Phase() == MatchPhase::Planning && b.Match().StateHash() == savedHash);
    CHECK(b.server->bots() == 2 && b.server->connectedPlayers() == 0);
    // A stranger is refused; the two humans are welcomed back into THEIR seats and resynced.
    const ConnectionId stranger = b.Connect();
    CHECK(Str(b.Last(stranger, "error"), "code") == "match_in_progress");
    const ConnectionId back0 = b.Connect(token0);
    const ConnectionId back1 = b.Connect(token1);
    CHECK(Num(b.Last(back0, "welcome"), "player_id") == 0 && b.Last(back0, "welcome").Find("reconnected")->AsBool() && Num(b.Last(back1, "welcome"), "player_id") == 1);
    CHECK(Str(b.Last(back0, "phase"), "phase") == "Planning" && b.CountType(back0, "state") == 1 && b.CountType(back0, "public_state") == 1 && b.CountType(back0, "match_started") == 1);
    // B carries on exactly as A did: same ticks, same bots' decisions, same state (nobody touched anything in either).
    Rig& aa = a;
    for (int i = 0; i < 90; ++i) { aa.Tick(); b.Tick(); }
    CHECK(a.Match().Round() == b.Match().Round() && a.Match().Phase() == b.Match().Phase() && a.Match().StateHash() == b.Match().StateHash());
    // ... and the resumed bots really acted (they own units), so the check above compared something.
    CHECK(b.Match().Players().Get(2)->Roster().Count() > 0 && b.Match().Players().Get(3)->Roster().Count() > 0);
    for (int i = 0; i < 6000; ++i) { aa.Tick(); b.Tick(); }   // and through fights and rounds
    CHECK(a.Match().StateHash() == b.Match().StateHash() && b.Match().Round() > 5);
    // (When a match ends the handler fires exactly once -- the server tool deletes its autosave then.)
    CHECK(finishedCalls == 0);
    for (int i = 0; i < 90000 && a.server->state() == GameServer::State::Running; ++i) a.Tick();
    CHECK(a.server->state() == GameServer::State::Finished && finishedCalls == 1);

    // Refusals: each names its reason and leaves the server as it was.
    const auto refused = [&](Rig& fresh, const std::vector<std::uint8_t>& snapshot, const std::string& seats, const char* mention) {
        std::string why;
        const bool ok = !fresh.server->Resume(snapshot, seats, &why) && why.find(mention) != std::string::npos && fresh.server->state() == GameServer::State::Lobby && fresh.server->match() == nullptr;
        if (!ok) std::printf("  wanted a refusal mentioning '%s', got '%s'\n", mention, why.c_str());
        return ok;
    };
    {
        Rig fresh(4, tweak);
        CHECK(refused(fresh, savedSnapshot, "not json", "JSON"));
        CHECK(refused(fresh, savedSnapshot, "[]", "JSON"));
        CHECK(refused(fresh, std::vector<std::uint8_t>{1, 2, 3}, saved, "too short to be a snapshot"));
        std::string edited = saved;
        edited.replace(edited.find("\"seats\":4"), 9, "\"seats\":5");
        CHECK(refused(fresh, savedSnapshot, edited, "seats"));
        edited = saved;
        edited.replace(edited.find("\"bot\":true"), 10, "\"bot\":false");
        CHECK(refused(fresh, savedSnapshot, edited, "bots"));
        Rig otherRules(4, [&](GameData& d, GameServerConfig& c) { tweak(d, c); d.config.player.startingHealth = 77; });
        CHECK(refused(otherRules, savedSnapshot, saved, "different game configuration"));   // (the engine's own hash check: the rules changed)
        Rig noBots(4, [](GameData&, GameServerConfig&) {});
        CHECK(refused(noBots, savedSnapshot, saved, "bots"));
        Rig busy(4, tweak);
        busy.Connect();
        { std::string why; CHECK(!busy.server->Resume(savedSnapshot, saved, &why) && why.find("not fresh") != std::string::npos); }
    }
    // The single file: pack / unpack round trip, and it refuses what is not a resume file.
    {
        const std::vector<std::uint8_t> file = PackResumeFile(savedSnapshot, saved);
        std::vector<std::uint8_t> snapshot;
        std::string seats, why;
        CHECK(UnpackResumeFile(file, snapshot, seats, &why) && snapshot == savedSnapshot && seats == saved);
        CHECK(!UnpackResumeFile({}, snapshot, seats, &why) && !UnpackResumeFile(std::vector<std::uint8_t>{'X', '2', 'F', 'R', 0, 0, 0, 0}, snapshot, seats, &why));
        std::vector<std::uint8_t> cut(file.begin(), file.begin() + 20);
        CHECK(!UnpackResumeFile(cut, snapshot, seats, &why) && why.find("truncated") != std::string::npos);
    }
}

static void TestJoinCodeKeepsStrangersOut() {
    NetRig n(2, [](TcpServerConfig& c) { c.joinCode = "secret_42"; });
    const auto handshake = [&](const std::string& path) {
        RawClient c;
        std::string head;
        if (!c.Connect(n.tcp->port())) return std::string("no connection");
        c.SendRaw(RawClient::HandshakeRequest(path));
        n.PumpUntil([&] { return c.TakeHttpHead(head); });
        return head;
    };
    CHECK(handshake("/").find("403") != std::string::npos);                   // no code
    CHECK(handshake("/?code=wrong").find("403") != std::string::npos);        // wrong code
    CHECK(handshake("/?token=" + std::string(32, 'a')).find("403") != std::string::npos);   // a token alone is not enough
    CHECK(n.game->connectedPlayers() == 0);                                   // nobody got a seat
    RawClient a;
    CHECK(n.Open(a, "/?code=secret_42"));                                     // the right code: a seat, as usual
    json::Value welcome;
    CHECK(n.NextJson(a, welcome) && Str(welcome, "type") == "welcome");
    const std::string token = Str(welcome, "token");
    a.Drop();
    CHECK(n.PumpUntil([&] { return n.game->connectedPlayers() == 0; }));
    RawClient again;
    CHECK(n.Open(again, "/?code=secret_42&token=" + token));                  // a reconnect brings both
    CHECK(n.game->connectedPlayers() == 1);
    // Without a join code configured nothing changes: the query is ignored.
    NetRig open(2);
    RawClient c;
    CHECK(open.Open(c, "/?code=anything"));
}

// ==== the soak test: eight real clients on real sockets play a whole match, some of them dropping out and coming back ====================================

static void TestEightClientSoak() {
    const std::string serverText = ReadWholeFile(std::string(W2F_SCHEMA_DIR) + "/server-message.schema.json");
    json::Value schema;
    std::string err;
    CHECK(json::Parse(serverText, schema, &err));
    if (!schema.IsObject()) return;
    const SchemaChecker checker(schema);

    NetRig n(8, {}, [](GameData& d, GameServerConfig& c) {
        c.postMatchTicks = 90;
        d.config.match.motherNatureTicks = 40;
        d.config.match.planningTicks = 150;
        d.config.match.resolutionTicks = 30;
        d.config.player.startingHealth = 45;
        d.config.damage.baseDamageByStage = {0, 3, 4, 6, 8, 12};
        d.config.match.shopClosedOpeningRounds = 1;   // the real rules: opening round, board = level
        d.config.match.openingUnitCosts = {1};
        d.config.player.limitBoardToLevel = true;
    });
    auto motherNature = w2f::LoadMotherNatureDatabaseFromFile(sample::ProductionMotherNaturePath(), n.rig.items.get());
    GameData data = n.rig.data;
    data.motherNature = motherNature.get();
    n.game = std::make_unique<GameServer>(n.rig.cfg, data, *n.tcp);   // (the same server, now with Mother Nature)
    n.tcp->SetHandler(n.game.get());

    struct Client {
        RawClient c;
        std::string token;
        int seat = -1;
        json::Value state;
        bool haveState = false;
        std::string phase;
        int round = 0, plannedRound = 0, giftRound = 0, planningTicks = 0;
        std::deque<std::string> queue;
        bool sawOver = false, dropped = false, back = false;
        std::uint64_t dropAt = 0, backAt = 0;
        int messages = 0, errors = 0, badSchema = 0;
    };
    std::vector<std::unique_ptr<Client>> clients;
    for (int i = 0; i < 8; ++i) {
        clients.push_back(std::make_unique<Client>());
        CHECK(n.Open(clients.back()->c));
    }
    n.PumpUntil([&] { return n.game->state() == GameServer::State::Running; });
    CHECK(n.game->state() == GameServer::State::Running);

    int problems = 0, reconnects = 0;
    const auto pump = [&](Client& cl) {
        int op = 0;
        std::string payload;
        while (cl.c.TakeFrame(op, payload)) {
            if (op == 8) { cl.c.closed = true; continue; }
            if (op != 1) continue;
            ++cl.messages;
            const json::Value v = ParseJson(payload);
            const std::string type = Str(v, "type");
            const std::string why = checker.Check(schema, v);
            if (!why.empty()) { ++cl.badSchema; if (problems++ < 5) std::printf("  schema mismatch (%s): %s\n", type.c_str(), why.c_str()); }
            if (type == "welcome") { cl.seat = static_cast<int>(Num(v, "player_id")); cl.token = Str(v, "token"); }
            if (type == "error") { ++cl.errors; if (problems++ < 5) std::printf("  a client was told: %s\n", payload.c_str()); }
            if (type == "phase") {
                cl.phase = Str(v, "phase");
                if (Num(v, "round") != cl.round) cl.planningTicks = 0;
                cl.round = static_cast<int>(Num(v, "round"));
            }
            if (type == "state") { cl.state = v; cl.haveState = true; }
            if (type == "match_over") cl.sawOver = true;
        }
    };
    const auto act = [&](Client& cl, std::uint64_t tick) {
        if (cl.c.closed || cl.c.fd == kBadSock || !cl.haveState || cl.dropped) return;
        if (cl.phase == "Planning") ++cl.planningTicks;
        if (cl.phase == "MotherNature" && cl.giftRound != cl.round && cl.state.Find("gifts") && !cl.state.Find("gifts")->Items().empty() && !cl.state.Find("gift_settled")->AsBool()) {
            cl.giftRound = cl.round;
            cl.queue.push_back(R"({"action": "pick_gift", "gift_index": 0})");
        }
        if (cl.phase == "Planning" && cl.round != cl.plannedRound && cl.planningTicks == 6) {   // shop first ...
            const auto& shop = cl.state.Find("shop")->Items();
            for (std::size_t i = 0; i < shop.size(); ++i) {
                long long id = 0;
                if (shop[i].ToInt(id) && id != 0) cl.queue.push_back(R"({"action": "buy_unit", "shop_index": )" + std::to_string(i) + "}");
            }
            cl.queue.push_back(R"({"action": "buy_xp"})");
        }
        if (cl.phase == "Planning" && cl.round != cl.plannedRound && cl.planningTicks == 40) {   // ... then put the bench on the board, level allowing
            cl.plannedRound = cl.round;
            std::set<std::pair<int, int>> taken;
            for (const json::Value& u : cl.state.Find("board")->Items()) taken.insert({static_cast<int>(Num(u, "x")), static_cast<int>(Num(u, "y"))});
            int free = static_cast<int>(Num(cl.state, "level")) - static_cast<int>(taken.size());
            for (const json::Value& u : cl.state.Find("bench")->Items()) {
                if (!u.IsObject() || free <= 0) continue;
                for (int y = 3; y >= 0 && free > 0; --y) {
                    bool placed = false;
                    for (int x = 0; x < kBoardColumns && !placed; ++x) {
                        if (taken.count({x, y})) continue;
                        taken.insert({x, y});
                        cl.queue.push_back(R"({"action": "move_unit", "unit_id": )" + std::to_string(static_cast<long long>(Num(u, "id"))) + R"(, "location": "board", "x": )" + std::to_string(x) + R"(, "y": )" + std::to_string(y) + "}");
                        --free;
                        placed = true;
                    }
                    if (placed) break;
                }
            }
        }
        if (tick % 3 == static_cast<std::uint64_t>(cl.seat) % 3 && !cl.queue.empty()) {   // (three ticks between a client's commands: well inside the rate limit)
            cl.c.SendRaw(Masked(cl.queue.front()));
            cl.queue.pop_front();
        }
    };

    std::uint64_t tick = 0;
    bool integrity = true;
    for (; tick < 90000 && n.game->state() != GameServer::State::Lobby; ++tick) {
        n.now += 33;
        n.game->Tick(n.now);
        n.Step(0);
        for (auto& cl : clients) pump(*cl);
        for (auto& cl : clients) act(*cl, tick);
        // Three clients lose their connection in round 4's planning phase and return with their tokens 60 ticks (2 s) later.
        for (int i = 0; i < 8; i += 3) {
            Client& cl = *clients[static_cast<std::size_t>(i)];
            if (!cl.dropped && !cl.back && cl.round == 4 && cl.phase == "Planning" && cl.planningTicks > 50) {
                cl.c.Drop();
                cl.dropped = true;
                cl.dropAt = tick;
                cl.queue.clear();
            }
            if (cl.dropped && !cl.back && tick >= cl.dropAt + 60) {
                cl.c.in.clear();
                cl.c.closed = false;
                cl.haveState = false;
                if (n.Open(cl.c, "/?token=" + cl.token)) { cl.back = true; cl.dropped = false; ++reconnects; }
            }
        }
        if (tick % 200 == 0 && n.game->match() != nullptr) integrity = integrity && n.game->match()->VerifyPoolIntegrity() && n.game->match()->VerifyRosterLayouts();
        if (n.game->state() == GameServer::State::Finished) {   // (leave the post-match grace period running so every client can read match_over)
            bool all = true;
            for (auto& cl : clients) all = all && cl->sawOver;
            if (all) break;
        }
    }
    int finished = 0, reconnected = 0, badSchema = 0, errors = 0;
    std::size_t messages = 0;
    for (auto& cl : clients) {
        pump(*cl);
        finished += cl->sawOver ? 1 : 0;
        reconnected += cl->back ? 1 : 0;
        badSchema += cl->badSchema;
        errors += cl->errors;
        messages += static_cast<std::size_t>(cl->messages);
    }
    std::printf("  soak: %llu ticks, %zu messages to 8 clients, %d reconnects, longest match round %d\n", static_cast<unsigned long long>(tick), messages, reconnects, n.game->match() != nullptr ? n.game->match()->Round() : 0);
    CHECK(n.game->state() == GameServer::State::Finished || n.game->state() == GameServer::State::Lobby);
    CHECK(finished == 8);                       // everybody, the three who dropped out included, was told how it ended
    CHECK(reconnects == 3 && reconnected == 3);
    CHECK(integrity);                           // the shared pool and every roster balanced throughout
    CHECK(badSchema == 0 && errors == 0);       // every message matched the published schema; nobody was ever scolded
    CHECK(messages > 3000);
    // A reconnected client was resynced with its real state: its last private state agrees with the engine's books.
    if (n.game->match() != nullptr) {
        for (int i = 0; i < 8; i += 3) {
            const Client& cl = *clients[static_cast<std::size_t>(i)];
            if (cl.haveState && cl.state.Find("gold")) CHECK(Num(cl.state, "gold") == n.game->match()->Players().Get(static_cast<PlayerId>(cl.seat))->Gold());
        }
    }
}

static void TestCombineItemsAndPublicBenchOverTheProtocol() {
    Rig r(2, {}, sample::ProductionItemsPath());
    const ConnectionId a = r.Connect();
    const ConnectionId b = r.Connect();
    CHECK(r.RunUntil([&] { return IsPlanning(r); }, 200));
    PlayerState* player = const_cast<MatchManager*>(r.server->match())->PlayersMutable().Get(0);   // admin access: items normally arrive as PvE drops
    for (ItemId item : {3u, 3u, 9u}) CHECK(player->AddItemToBag(item));
    r.net.sent.clear();

    // combine_items: the answer, then a bag_event, then the new state; private to the owner.
    r.Say(a, R"({"action": "combine_items", "first": 3, "second": 3, "id": 5})");
    CHECK(Str(r.Last(a, "result"), "result") == "Ok" && Num(r.Last(a, "result"), "id") == 5);
    const json::Value event = r.Last(a, "bag_event");
    CHECK(Str(event, "event") == "combined" && Num(event, "first") == 3 && Num(event, "second") == 3 && Num(event, "result") == 24);
    std::vector<long long> bag;
    const json::Value state = r.Last(a, "state");   // a named local: never range-for over a member of a temporary (MSVC)
    for (const json::Value& item : state.Find("item_bag")->Items()) { long long id = 0; item.ToInt(id); bag.push_back(id); }
    CHECK((bag == std::vector<long long>{9, 24}));
    CHECK(r.CountType(b, "bag_event") == 0 && r.CountType(b, "state") == 0);
    // Refused: nothing to combine with; and the schema-level refusals (missing / unknown fields).
    r.net.sent.clear();
    r.Say(a, R"({"action": "combine_items", "first": 24, "second": 9, "id": 6})");
    CHECK(Str(r.Last(a, "result"), "result") == "InvalidItem" && r.CountType(a, "bag_event") == 0);
    r.Say(a, R"({"action": "combine_items", "first": 3})");
    CHECK(Str(r.Last(a, "error"), "code") == "missing_field");
    r.Say(a, R"({"action": "combine_items", "first": 3, "second": 3, "third": 3})");
    CHECK(Str(r.Last(a, "error"), "code") == "unknown_field");

    // The bench is public: player 1 sees player 0's bench units (and the private bag and gold stay private).
    r.Say(a, R"({"action": "buy_unit", "shop_index": 0})");
    r.Say(b, R"({"action": "get_state"})");
    const json::Value pub = r.Last(b, "public_state");
    bool sawBench = false;
    for (const json::Value& p : pub.Find("players")->Items()) {
        CHECK(p.Find("bench") != nullptr && p.Find("bench")->IsArray());
        if (Num(p, "player_id") == 0 && !p.Find("bench")->Items().empty()) sawBench = true;
    }
    CHECK(sawBench);


    // The shop lock: a boolean command, echoed in `state.shop_locked`.
    r.net.sent.clear();
    r.Say(a, R"({"action": "set_shop_lock", "locked": true, "id": 8})");
    CHECK(Str(r.Last(a, "result"), "result") == "Ok" && Num(r.Last(a, "result"), "id") == 8);
    CHECK(r.Last(a, "state").Find("shop_locked")->AsBool());
    r.Say(a, R"({"action": "set_shop_lock", "locked": false})");
    CHECK(!r.Last(a, "state").Find("shop_locked")->AsBool());
    r.Say(a, R"({"action": "set_shop_lock"})");
    CHECK(Str(r.Last(a, "error"), "code") == "missing_field");
    r.Say(a, R"({"action": "set_shop_lock", "locked": 1})");
    CHECK(Str(r.Last(a, "error"), "code") == "wrong_type");

    // Trait system v2 (revision 5): pick_trait_choice. Nothing is pending here, so the engine answers AlreadyPicked; the grammar is checked too.
    r.Say(a, R"({"action": "pick_trait_choice", "index": 0, "id": 9})");
    CHECK(Str(r.Last(a, "result"), "result") == "AlreadyPicked" && Num(r.Last(a, "result"), "id") == 9);
    r.Say(a, R"({"action": "pick_trait_choice", "index": 8})");
    CHECK(Str(r.Last(a, "error"), "code") == "out_of_range");
    r.Say(a, R"({"action": "pick_trait_choice"})");
    CHECK(Str(r.Last(a, "error"), "code") == "missing_field");
    r.Say(a, R"({"action": "get_state"})");
    CHECK(Str(*r.Last(a, "state").Find("trait_choice"), "kind") == "none" && r.Last(a, "state").Find("traits")->Find("star_dust") != nullptr);

    // The fighters' items travel with the fight: combat.unit_items maps a unit id to its item ids.
    const UnitId unit = player->Roster().Units()[0].id;
    CHECK(player->AddItemToBag(4));
    r.Say(a, R"({"action": "move_unit", "unit_id": )" + std::to_string(unit) + R"(, "location": "board", "x": 3, "y": 3})");
    r.Say(a, R"({"action": "equip_item", "unit_id": )" + std::to_string(unit) + R"(, "item_id": 4})");
    r.net.sent.clear();
    CHECK(r.RunUntil([&] { return r.server->match()->Phase() == MatchPhase::Combat; }, 5000));
    const json::Value combat = r.Last(a, "combat");
    const json::Value* unitItems = combat.Find("unit_items");
    CHECK(unitItems != nullptr && unitItems->IsObject());
    const json::Value* mine = unitItems != nullptr ? unitItems->Find(std::to_string(unit)) : nullptr;
    long long carried = 0;
    CHECK(mine != nullptr && mine->Items().size() == 1 && mine->Items()[0].ToInt(carried) && carried == 4);
}

static void TestCatalog() {
    Rig r(2);
    const ConnectionId a = r.Connect();   // works in the lobby, before any match
    r.Say(a, R"({"action": "get_catalog", "id": 4})");
    const json::Value catalog = r.Last(a, "catalog");
    CHECK(Str(catalog, "type") == "catalog");
    const json::Value* champions = catalog.Find("champions");
    const json::Value* items = catalog.Find("items");
    const json::Value* traits = catalog.Find("traits");
    CHECK(champions && items && traits && champions->Items().size() == r.champions->All().size() + r.encounters->Monsters().All().size() && items->Items().size() == r.items->All().size() &&
          traits->Items().size() == r.traits->All().size());
    bool alesk = false, sword = false, soulsSword = false, helios = false, monster = false;
    for (const json::Value& c : champions->Items()) {
        if (Num(c, "id") == 10001) monster = Str(c, "name") == "Gloop" && c.Find("monster") && c.Find("monster")->IsBool();
        if (Num(c, "id") == 9001) alesk = Str(c, "name") == "Alesk" && Num(c, "cost") == 4 && Str(c, "role") == "tank" && c.Find("traits")->Items().size() == 2 && c.Find("hp")->Items().size() == 3;
    }
    for (const json::Value& i : items->Items()) {
        if (Num(i, "id") == 3) sword = Str(i, "name") == "Coregons Sword" && i.Find("components")->Items().empty() && Num(*i.Find("stats"), "attack_damage") == 15;
        if (Num(i, "id") == 24) soulsSword = i.Find("components")->Items().size() == 2;
    }
    for (const json::Value& t : traits->Items()) helios = helios || (Str(t, "name") == "Helios" && !t.Find("breakpoints")->Items().empty());
    CHECK(alesk && sword && soulsSword && helios && monster);
    // The display text travels with the catalog: a flat key -> string map (data/text_en.json), so a client needs no hard-coded wording.
    const json::Value* text = catalog.Find("text");
    CHECK(text != nullptr && text->IsObject() && Str(*text, "language") == "en");
    const json::Value* entries = text != nullptr ? text->Find("entries") : nullptr;
    CHECK(entries != nullptr && entries->IsObject() && entries->MemberCount() == r.display->Entries().size());
    const json::Value* aleskName = entries != nullptr ? entries->Find("champion.9001.name") : nullptr;
    const json::Value* aleskDesc = entries != nullptr ? entries->Find("champion.9001.ability.desc") : nullptr;
    CHECK(aleskName != nullptr && aleskName->AsString() == "Alesk" && aleskDesc != nullptr && aleskDesc->AsString().size() > 20);
    // Identical for everyone, and the rate limiter counts it (5 tokens).
    const ConnectionId b = r.Connect();
    r.Say(b, R"({"action": "get_catalog"})");
    CHECK(r.CountType(b, "catalog") == 1 && r.Last(b, "catalog").Find("champions")->Items().size() == champions->Items().size());
    r.Say(a, R"({"action": "get_catalog", "extra": 1})");
    CHECK(Str(r.Last(a, "error"), "code") == "unknown_field");
}

int main() {
    struct Test { const char* name; void (*fn)(); };
    const Test tests[] = {
        {"Encoders: SHA-1, base64, hex, UTF-8", TestEncoders},
        {"WebSocket handshake", TestHandshake},
        {"WebSocket frames + fuzz", TestFrames},
        {"JSON writer", TestJsonWriter},
        {"Command validation + fuzz", TestCommandParsing},
        {"Lobby: seats, start at full, rejections", TestLobby},
        {"Reconnect: tokens, replacement, resync", TestReconnect},
        {"Command routing + engine results", TestCommandRouting},
        {"Item combination over the protocol", TestItemCombinationOverTheProtocol},
        {"Mother Nature over the protocol", TestMotherNatureOverTheProtocol},
        {"No Mother Nature data: nothing changes", TestNoMotherNatureData},
        {"Malformed traffic never reaches the engine", TestMalformedTrafficNeverReachesTheEngine},
        {"Rate limiting", TestRateLimiting},
        {"Privacy + event delivery over a whole match", TestPrivacyAndDelivery},
        {"Networked match == bare engine replay", TestNetworkedMatchEqualsBareEngine},
        {"Match end + lobby reset", TestMatchEndAndLobbyReset},
        {"GameServer fuzz (connections, garbage, time)", TestGameServerFuzz},
        {"Sockets: handshake, messages, close", TestSocketHandshakeAndMessages},
        {"Sockets: bad handshakes", TestSocketBadHandshakes},
        {"Sockets: protocol violations -> close codes", TestSocketProtocolViolations},
        {"Sockets: fragmented / coalesced delivery", TestSocketFragmentedDelivery},
        {"Sockets: abrupt disconnects", TestSocketAbruptDisconnects},
        {"Sockets: handshake / idle timeouts + pings", TestSocketTimeouts},
        {"Sockets: connection cap", TestSocketConnectionCap},
        {"Sockets: slow reader is dropped", TestSocketSlowReaderIsDropped},
        {"Sockets: two-player flow + token reconnect", TestSocketFullMatchFlow},
        {"Sockets: the real server loop starts and stops", TestSocketServerLoopRunsAndStops},
        {"Item Remover over the protocol + catalog flag", TestItemRemoverOverTheProtocol},
        {"combine_items + public bench over the protocol", TestCombineItemsAndPublicBenchOverTheProtocol},
        {"Combat phase length on the wire", TestCombatPhaseLengthOnTheWire},
        {"Bot seats: lobby, play, reset, determinism", TestBotSeats},
        {"JSON Schemas describe exactly what the server sends", TestJsonSchemas},
        {"Opening round + board = level, over the wire", TestOpeningRoundOverTheWire},
        {"Crash recovery: resume a server from its autosave", TestResumeAfterACrash},
        {"Join code keeps strangers out", TestJoinCodeKeepsStrangersOut},
        {"Soak: 8 real clients, a whole match, 3 reconnects", TestEightClientSoak},
        {"Catalog message", TestCatalog},
        {"Queue server: bots / normal queues, many matches, back to the home screen", TestQueueServer},
    };
    int failedTests = 0;
    for (const Test& t : tests) {
        const int before = g_failures;
        std::printf("[ RUN  ] %s\n", t.name);
        std::fflush(stdout);
        t.fn();
        if (g_failures == before) {
            std::printf("[  OK  ] %s\n", t.name);
        } else {
            std::printf("[ FAIL ] %s\n", t.name);
            ++failedTests;
        }
    }
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return failedTests == 0 ? 0 : 1;
}
