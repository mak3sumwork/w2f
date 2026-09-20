#pragma once

// Small self-contained encoders the WebSocket layer needs (so the server has no third-party dependency):
// SHA-1 (only for the WebSocket handshake -- it is NOT used for anything security-sensitive), base64, hex, UTF-8 validation.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace w2f::net {

std::array<std::uint8_t, 20> Sha1(std::string_view data);

std::string Base64Encode(const std::uint8_t* data, std::size_t size);
inline std::string Base64Encode(std::string_view s) { return Base64Encode(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()); }
// Strict: standard alphabet, correct '=' padding, nothing else. False on any deviation.
bool Base64Decode(std::string_view in, std::string& out);

std::string HexEncode(const std::uint8_t* data, std::size_t size);
bool IsLowerHex(std::string_view s);

// Well-formed UTF-8 only: no overlong forms, no surrogates, nothing above U+10FFFF, no truncated sequences.
bool IsValidUtf8(std::string_view s);

}  // namespace w2f::net
