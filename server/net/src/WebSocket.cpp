#include "w2f/net/WebSocket.h"

#include <cctype>
#include <map>

#include "w2f/net/Encoding.h"

namespace w2f::net {

namespace {

constexpr std::size_t kMaxHeaders = 64;
constexpr std::size_t kMaxTargetBytes = 2048;
constexpr const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

bool IsTokenChar(char c) {
    if (std::isalnum(static_cast<unsigned char>(c))) return true;
    for (char t : std::string_view("!#$%&'*+-.^_`|~")) {
        if (c == t) return true;
    }
    return false;
}

// Does a comma-separated header value contain `token` (case-insensitive)?
bool HasToken(std::string_view value, std::string_view token) {
    std::size_t start = 0;
    while (start <= value.size()) {
        std::size_t end = value.find(',', start);
        if (end == std::string_view::npos) end = value.size();
        if (Lower(Trim(value.substr(start, end - start))) == token) return true;
        start = end + 1;
    }
    return false;
}

HandshakeResult Bad(int status, const char* reason) {
    HandshakeResult r;
    r.status = HandshakeStatus::Bad;
    r.httpStatus = status;
    r.reason = reason;
    return r;
}

}  // namespace

std::string AcceptKey(std::string_view key) {
    const auto digest = Sha1(std::string(key) + kGuid);
    return Base64Encode(digest.data(), digest.size());
}

HandshakeResult ParseHandshake(std::string_view buffer) {
    const std::size_t end = buffer.find("\r\n\r\n");
    if (end == std::string_view::npos) {
        return buffer.size() > kMaxHandshakeBytes ? Bad(431, "request header too large") : HandshakeResult{};
    }
    if (end + 4 > kMaxHandshakeBytes) return Bad(431, "request header too large");
    const std::string_view head = buffer.substr(0, end);
    for (char c : head) {
        const auto u = static_cast<unsigned char>(c);
        if (u == 0 || u == 0x7F || (u < 0x20 && c != '\r' && c != '\n' && c != '\t')) return Bad(400, "control character in request");
    }

    // Split into lines; only CRLF may end a line.
    std::vector<std::string_view> lines;
    std::size_t pos = 0;
    while (pos <= head.size()) {
        std::size_t eol = head.find("\r\n", pos);
        if (eol == std::string_view::npos) eol = head.size();
        const std::string_view line = head.substr(pos, eol - pos);
        if (line.find('\n') != std::string_view::npos || line.find('\r') != std::string_view::npos) return Bad(400, "malformed line ending");
        lines.push_back(line);
        pos = eol + 2;
    }
    if (lines.empty() || lines[0].empty()) return Bad(400, "empty request");

    // Request line: GET <target> HTTP/1.1
    const std::string_view request = lines[0];
    const std::size_t sp1 = request.find(' ');
    const std::size_t sp2 = sp1 == std::string_view::npos ? sp1 : request.find(' ', sp1 + 1);
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos || request.find(' ', sp2 + 1) != std::string_view::npos) return Bad(400, "malformed request line");
    if (request.substr(0, sp1) != "GET") return Bad(405, "only GET is supported");
    if (request.substr(sp2 + 1) != "HTTP/1.1") return Bad(400, "HTTP/1.1 is required");
    const std::string_view target = request.substr(sp1 + 1, sp2 - sp1 - 1);
    if (target.empty() || target[0] != '/' || target.size() > kMaxTargetBytes) return Bad(400, "bad request target");

    std::map<std::string, std::string> headers;
    if (lines.size() - 1 > kMaxHeaders) return Bad(431, "too many headers");
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const std::string_view line = lines[i];
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) return Bad(400, "malformed header");
        const std::string_view name = line.substr(0, colon);
        for (char c : name) {
            if (!IsTokenChar(c)) return Bad(400, "malformed header name");
        }
        const std::string lname = Lower(name);
        const std::string value(Trim(line.substr(colon + 1)));
        auto [it, inserted] = headers.emplace(lname, value);
        if (!inserted) {
            // Repeating a header that decides the handshake is ambiguous; others are merged as HTTP allows.
            if (lname == "sec-websocket-key" || lname == "sec-websocket-version" || lname == "upgrade" || lname == "host") return Bad(400, "duplicated header");
            it->second += "," + value;
        }
    }

    if (headers.count("content-length") && headers["content-length"] != "0") return Bad(400, "a request body is not allowed");
    if (headers.count("transfer-encoding")) return Bad(400, "a request body is not allowed");
    if (!headers.count("upgrade") || !HasToken(headers["upgrade"], "websocket")) return Bad(400, "not a WebSocket upgrade request");
    if (!headers.count("connection") || !HasToken(headers["connection"], "upgrade")) return Bad(400, "Connection: Upgrade is required");
    if (!headers.count("sec-websocket-version")) return Bad(400, "Sec-WebSocket-Version is required");
    if (headers["sec-websocket-version"] != "13") return Bad(426, "unsupported WebSocket version");
    if (!headers.count("sec-websocket-key")) return Bad(400, "Sec-WebSocket-Key is required");
    std::string decoded;
    if (!Base64Decode(headers["sec-websocket-key"], decoded) || decoded.size() != 16) return Bad(400, "Sec-WebSocket-Key must be 16 bytes, base64");

    HandshakeResult ok;
    ok.status = HandshakeStatus::Ok;
    ok.consumed = end + 4;
    ok.key = headers["sec-websocket-key"];
    const std::size_t q = target.find('?');
    ok.path = std::string(target.substr(0, q));
    if (q != std::string_view::npos) ok.query = std::string(target.substr(q + 1));
    return ok;
}

std::string BuildHandshakeResponse(std::string_view key) {
    return "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + AcceptKey(key) + "\r\n\r\n";
}

std::string BuildHttpError(int status, std::string_view reason) {
    const char* text = "Bad Request";
    switch (status) {
        case 405: text = "Method Not Allowed"; break;
        case 426: text = "Upgrade Required"; break;
        case 431: text = "Request Header Fields Too Large"; break;
        case 503: text = "Service Unavailable"; break;
        default: break;
    }
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + text + "\r\nConnection: close\r\nContent-Length: 0\r\n";
    if (status == 426) out += "Sec-WebSocket-Version: 13\r\n";
    if (!reason.empty()) {
        std::string safe;
        for (char c : reason) safe.push_back((c >= 0x20 && c < 0x7F) ? c : '?');
        out += "X-Reason: " + safe + "\r\n";
    }
    return out + "\r\n";
}

std::string QueryParam(std::string_view query, std::string_view name) {
    std::size_t pos = 0;
    while (pos <= query.size()) {
        std::size_t amp = query.find('&', pos);
        if (amp == std::string_view::npos) amp = query.size();
        const std::string_view pair = query.substr(pos, amp - pos);
        const std::size_t eq = pair.find('=');
        if (eq != std::string_view::npos && pair.substr(0, eq) == name) return std::string(pair.substr(eq + 1));
        pos = amp + 1;
    }
    return {};
}

// ---- frames ----------------------------------------------------------------------------------

bool WebSocketParser::Fail(std::vector<WsEvent>& out, std::uint16_t code, const char* why) {
    WsEvent e;
    e.kind = WsEvent::Kind::Error;
    e.code = code;
    e.reason = why;
    out.push_back(std::move(e));
    dead_ = true;
    buffer_.clear();
    message_.clear();
    return false;
}

bool WebSocketParser::Feed(const std::uint8_t* data, std::size_t size, std::vector<WsEvent>& out) {
    if (dead_) return false;
    buffer_.append(reinterpret_cast<const char*>(data), size);
    std::size_t pos = 0;
    for (;;) {
        const std::size_t avail = buffer_.size() - pos;
        if (avail < 2) break;
        const auto b0 = static_cast<std::uint8_t>(buffer_[pos]);
        const auto b1 = static_cast<std::uint8_t>(buffer_[pos + 1]);
        const bool fin = (b0 & 0x80) != 0;
        if ((b0 & 0x70) != 0) return Fail(out, 1002, "reserved bits are set (no extensions were negotiated)");
        const std::uint8_t opcode = b0 & 0x0F;
        if (!(b1 & 0x80)) return Fail(out, 1002, "client frames must be masked");
        const std::uint8_t len7 = b1 & 0x7F;
        const std::size_t extBytes = len7 == 126 ? 2 : len7 == 127 ? 8 : 0;
        if (avail < 2 + extBytes) break;

        std::uint64_t length = len7;
        if (extBytes > 0) {
            length = 0;
            for (std::size_t i = 0; i < extBytes; ++i) length = (length << 8) | static_cast<std::uint8_t>(buffer_[pos + 2 + i]);
            if (extBytes == 8 && (length >> 63) != 0) return Fail(out, 1002, "64-bit length has its top bit set");
            if (extBytes == 2 && length < 126) return Fail(out, 1002, "length not minimally encoded");
            if (extBytes == 8 && length < 65536) return Fail(out, 1002, "length not minimally encoded");
        }

        const bool control = (opcode & 0x08) != 0;
        const bool known = opcode == 0 || opcode == 1 || opcode == 2 || opcode == 8 || opcode == 9 || opcode == 10;
        if (!known) return Fail(out, 1002, "unknown opcode");
        if (control) {
            if (length > 125) return Fail(out, 1002, "control frame too large");
            if (!fin) return Fail(out, 1002, "control frames cannot be fragmented");
        } else {
            if (opcode == 2) return Fail(out, 1003, "binary messages are not supported");
            if (opcode == 0 && !fragmenting_) return Fail(out, 1002, "continuation without a message to continue");
            if (opcode == 1 && fragmenting_) return Fail(out, 1002, "a new message started before the previous one finished");
            const std::uint64_t so_far = fragmenting_ ? message_.size() : 0;
            if (so_far + length > maxMessage_) return Fail(out, 1009, "message too large");
        }

        const std::size_t headerBytes = 2 + extBytes + 4;
        if (avail < headerBytes + length) break;   // the payload is bounded by the checks above, so waiting cannot grow without limit

        const std::size_t maskAt = pos + 2 + extBytes;
        const std::size_t payloadAt = pos + headerBytes;
        std::string payload(static_cast<std::size_t>(length), '\0');
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(static_cast<std::uint8_t>(buffer_[payloadAt + i]) ^ static_cast<std::uint8_t>(buffer_[maskAt + (i & 3)]));
        }
        pos += headerBytes + payload.size();

        if (control) {
            WsEvent e;
            if (opcode == 9) {
                e.kind = WsEvent::Kind::Ping;
                e.payload = std::move(payload);
                out.push_back(std::move(e));
            } else if (opcode == 10) {
                e.kind = WsEvent::Kind::Pong;
                e.payload = std::move(payload);
                out.push_back(std::move(e));
            } else {   // Close
                e.kind = WsEvent::Kind::Close;
                e.code = 1005;
                if (payload.size() == 1) return Fail(out, 1002, "close payload of one byte");
                if (payload.size() >= 2) {
                    e.code = static_cast<std::uint16_t>((static_cast<std::uint8_t>(payload[0]) << 8) | static_cast<std::uint8_t>(payload[1]));
                    e.reason = payload.substr(2);
                    const bool valid = (e.code >= 1000 && e.code <= 1003) || (e.code >= 1007 && e.code <= 1011) || (e.code >= 3000 && e.code <= 4999);
                    if (!valid) return Fail(out, 1002, "invalid close code");
                    if (!IsValidUtf8(e.reason)) return Fail(out, 1007, "close reason is not UTF-8");
                }
                out.push_back(std::move(e));
                dead_ = true;
                buffer_.clear();
                message_.clear();
                return false;
            }
            continue;
        }

        if (opcode == 1) {
            message_ = std::move(payload);
            fragmenting_ = !fin;
        } else {   // continuation
            message_ += payload;
            fragmenting_ = !fin;
        }
        if (fin) {
            if (!IsValidUtf8(message_)) return Fail(out, 1007, "text message is not valid UTF-8");
            WsEvent e;
            e.kind = WsEvent::Kind::Text;
            e.payload = std::move(message_);
            message_.clear();
            out.push_back(std::move(e));
        }
    }
    buffer_.erase(0, pos);
    return true;
}

// ---- encoding --------------------------------------------------------------------------------

namespace {
void AppendHeader(std::string& out, WsOpcode opcode, std::size_t size, bool fin, bool masked) {
    out.push_back(static_cast<char>((fin ? 0x80 : 0x00) | static_cast<std::uint8_t>(opcode)));
    const std::uint8_t maskBit = masked ? 0x80 : 0x00;
    if (size < 126) {
        out.push_back(static_cast<char>(maskBit | size));
    } else if (size < 65536) {
        out.push_back(static_cast<char>(maskBit | 126));
        out.push_back(static_cast<char>((size >> 8) & 0xFF));
        out.push_back(static_cast<char>(size & 0xFF));
    } else {
        out.push_back(static_cast<char>(maskBit | 127));
        for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>((static_cast<std::uint64_t>(size) >> (8 * i)) & 0xFF));
    }
}
}  // namespace

std::string EncodeFrame(WsOpcode opcode, std::string_view payload, bool fin) {
    std::string out;
    out.reserve(payload.size() + 10);
    AppendHeader(out, opcode, payload.size(), fin, false);
    out.append(payload);
    return out;
}

std::string EncodeClose(std::uint16_t code, std::string_view reason) {
    std::string payload;
    payload.push_back(static_cast<char>(code >> 8));
    payload.push_back(static_cast<char>(code & 0xFF));
    payload.append(reason.substr(0, 120));   // control payloads are at most 125 bytes
    return EncodeFrame(WsOpcode::Close, payload);
}

std::string EncodeMaskedFrame(WsOpcode opcode, std::string_view payload, const std::uint8_t mask[4], bool fin) {
    std::string out;
    out.reserve(payload.size() + 14);
    AppendHeader(out, opcode, payload.size(), fin, true);
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>(mask[i]));
    for (std::size_t i = 0; i < payload.size(); ++i) out.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ mask[i & 3]));
    return out;
}

}  // namespace w2f::net
