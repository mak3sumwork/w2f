#pragma once

// RFC 6455 WebSocket, server side, with nothing but the standard library. Pure byte-in / byte-out: no sockets and no clock,
// so every rule here is tested by feeding it bytes.
//
// Deliberately strict -- everything a well-behaved client (a browser, Unreal's WebSockets module, any library) does is
// accepted, and everything else is refused with the close code the RFC prescribes:
//   * the handshake must be a complete, well-formed HTTP/1.1 GET upgrade (at most 8 KiB, 64 headers, no body);
//   * client frames must be masked, reserved bits must be 0 (no extensions), opcodes must be known;
//   * control frames are at most 125 bytes and never fragmented; data frames may be fragmented but not interleaved;
//   * a message may not exceed the configured size (checked from the frame HEADER, before the payload is buffered);
//   * text must be valid UTF-8; binary is not supported by this protocol.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace w2f::net {

constexpr std::size_t kMaxHandshakeBytes = 8192;

enum class HandshakeStatus { NeedMore, Ok, Bad };

struct HandshakeResult {
    HandshakeStatus status = HandshakeStatus::NeedMore;
    std::size_t consumed = 0;   // Ok: bytes of the request (anything after them is already WebSocket data)
    std::string path;           // Ok: request target without the query
    std::string query;          // Ok: after '?', without it
    std::string key;            // Ok: Sec-WebSocket-Key
    int httpStatus = 400;       // Bad: the status to answer with (400, 426 wrong version, 431 too large, 405 method)
    std::string reason;         // Bad: a short human-readable cause
};

// `buffer` is everything received so far on the connection. Call again with more data while NeedMore.
HandshakeResult ParseHandshake(std::string_view buffer);
std::string BuildHandshakeResponse(std::string_view key);
// A complete, closing HTTP error response (no body). 426 also advertises the supported version.
std::string BuildHttpError(int status, std::string_view reason);
// Value of `name` in an "a=1&b=2" query, or "" (no percent-decoding: the only parameter used is a hex token).
std::string QueryParam(std::string_view query, std::string_view name);
// The Sec-WebSocket-Accept value for a key (RFC 6455 section 4.2.2).
std::string AcceptKey(std::string_view key);

enum class WsOpcode : std::uint8_t { Continuation = 0, Text = 1, Binary = 2, Close = 8, Ping = 9, Pong = 10 };

struct WsEvent {
    enum class Kind { Text, Ping, Pong, Close, Error };
    Kind kind = Kind::Text;
    std::string payload;         // Text: the message. Ping / Pong: the payload.
    std::uint16_t code = 0;      // Close: the code the peer sent (1005 = none). Error: the close code to answer with.
    std::string reason;          // Close: the peer's reason. Error: what was wrong.
};

class WebSocketParser {
public:
    explicit WebSocketParser(std::size_t maxMessageBytes) : maxMessage_(maxMessageBytes) {}

    // Feeds received bytes; appends the events they complete. Returns false once the stream is unusable (after an Error or
    // a Close event): the last event says why, and further input is ignored.
    bool Feed(const std::uint8_t* data, std::size_t size, std::vector<WsEvent>& out);
    bool dead() const { return dead_; }

private:
    bool Fail(std::vector<WsEvent>& out, std::uint16_t code, const char* why);

    std::size_t maxMessage_;
    std::string buffer_;      // bytes of a frame not complete yet
    std::string message_;     // a fragmented message being assembled
    bool fragmenting_ = false;
    bool dead_ = false;
};

// Server -> client frames are never masked.
std::string EncodeFrame(WsOpcode opcode, std::string_view payload, bool fin = true);
std::string EncodeClose(std::uint16_t code, std::string_view reason = {});
// Client -> server frames (used by tests and by any C++ client): masked with `mask`.
std::string EncodeMaskedFrame(WsOpcode opcode, std::string_view payload, const std::uint8_t mask[4], bool fin = true);

}  // namespace w2f::net
