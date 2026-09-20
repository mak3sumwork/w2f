#pragma once

// FNV-1a, 64-bit, fed fixed-width little-endian words so the result is identical on every
// platform. Used for state hashes (desync detection) and combat-log checksums.

#include <cstdint>
#include <string>

namespace w2f {

struct Fnv1a {
    std::uint64_t value = 0xcbf29ce484222325ull;

    // The textbook per-byte step (used for byte buffers; Add() feeds a whole word).
    void AddByte(std::uint8_t b) {
        value ^= b;
        value *= 0x100000001b3ull;
    }
    void Add(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            value ^= (v >> (8 * i)) & 0xFFu;
            value *= 0x100000001b3ull;
        }
    }
    void AddInt(std::int64_t v) { Add(static_cast<std::uint64_t>(v)); }
    void AddString(const std::string& text) {
        Add(text.size());
        for (char c : text) Add(static_cast<unsigned char>(c));
    }
};

}  // namespace w2f
