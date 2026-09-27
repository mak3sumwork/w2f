#include "w2f/net/Encoding.h"

namespace w2f::net {

namespace {
std::uint32_t Rotl(std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

void Sha1Block(std::uint32_t h[5], const std::uint8_t* block) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) w[i] = Rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
        std::uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
        const std::uint32_t t = Rotl(a, 5) + f + e + k + w[i];
        e = d; d = c; c = Rotl(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
int B64Index(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
}  // namespace

std::array<std::uint8_t, 20> Sha1(std::string_view data) {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    std::size_t remaining = data.size();
    while (remaining >= 64) {
        Sha1Block(h, p);
        p += 64;
        remaining -= 64;
    }
    std::uint8_t tail[128] = {};
    for (std::size_t i = 0; i < remaining; ++i) tail[i] = p[i];
    tail[remaining] = 0x80;
    const std::size_t tailSize = remaining < 56 ? 64 : 128;
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    for (int i = 0; i < 8; ++i) tail[tailSize - 1 - static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(bits >> (8 * i));
    Sha1Block(h, tail);
    if (tailSize == 128) Sha1Block(h, tail + 64);
    std::array<std::uint8_t, 20> out{};
    for (std::size_t i = 0; i < 5; ++i) {
        for (std::size_t j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<std::uint8_t>(h[i] >> (24 - 8 * j));
    }
    return out;
}

namespace {
constexpr std::uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
std::uint32_t Rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void Sha256Block(std::uint32_t h[8], const std::uint8_t* block) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = hh + S1 + ch + kSha256K[i] + w[i];
        const std::uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
}  // namespace

std::array<std::uint8_t, 32> Sha256(std::string_view data) {
    std::uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    std::size_t remaining = data.size();
    while (remaining >= 64) {
        Sha256Block(h, p);
        p += 64;
        remaining -= 64;
    }
    std::uint8_t tail[128] = {};
    for (std::size_t i = 0; i < remaining; ++i) tail[i] = p[i];
    tail[remaining] = 0x80;
    const std::size_t tailSize = remaining < 56 ? 64 : 128;
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    for (int i = 0; i < 8; ++i) tail[tailSize - 1 - static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(bits >> (8 * i));
    Sha256Block(h, tail);
    if (tailSize == 128) Sha256Block(h, tail + 64);
    std::array<std::uint8_t, 32> out{};
    for (std::size_t i = 0; i < 8; ++i) {
        for (std::size_t j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<std::uint8_t>(h[i] >> (24 - 8 * j));
    }
    return out;
}

std::array<std::uint8_t, 32> HmacSha256(std::string_view key, std::string_view message) {
    std::string k(key);
    if (k.size() > 64) {
        const auto d = Sha256(k);
        k.assign(reinterpret_cast<const char*>(d.data()), d.size());
    }
    k.resize(64, '\0');
    std::string inner(64, '\0'), outer(64, '\0');
    for (std::size_t i = 0; i < 64; ++i) {
        inner[i] = static_cast<char>(static_cast<std::uint8_t>(k[i]) ^ 0x36u);
        outer[i] = static_cast<char>(static_cast<std::uint8_t>(k[i]) ^ 0x5cu);
    }
    inner.append(message);
    const auto ih = Sha256(inner);
    outer.append(reinterpret_cast<const char*>(ih.data()), ih.size());
    return Sha256(outer);
}

std::string Pbkdf2Sha256(std::string_view password, std::string_view salt, int iterations, std::size_t length) {
    std::string out;
    for (std::uint32_t block = 1; out.size() < length; ++block) {
        std::string s(salt);
        for (int i = 3; i >= 0; --i) s.push_back(static_cast<char>((block >> (8 * i)) & 0xFFu));
        auto u = HmacSha256(password, s);
        auto t = u;
        for (int i = 1; i < iterations; ++i) {
            u = HmacSha256(password, std::string_view(reinterpret_cast<const char*>(u.data()), u.size()));
            for (std::size_t j = 0; j < t.size(); ++j) t[j] ^= u[j];
        }
        out.append(reinterpret_cast<const char*>(t.data()), t.size());
    }
    out.resize(length);
    return out;
}

bool ConstantTimeEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    unsigned diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned>(static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]));
    return diff == 0;
}

std::string Base64Encode(const std::uint8_t* data, std::size_t size) {
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (std::size_t i = 0; i < size; i += 3) {
        const std::uint32_t b0 = data[i];
        const std::uint32_t b1 = i + 1 < size ? data[i + 1] : 0;
        const std::uint32_t b2 = i + 2 < size ? data[i + 2] : 0;
        const std::uint32_t v = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(i + 1 < size ? kB64[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < size ? kB64[v & 63] : '=');
    }
    return out;
}

bool Base64Decode(std::string_view in, std::string& out) {
    out.clear();
    if (in.size() % 4 != 0) return false;
    for (std::size_t i = 0; i < in.size(); i += 4) {
        int v[4];
        int pad = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            const char c = in[i + j];
            if (c == '=') {
                if (i + 4 != in.size() || j < 2) return false;   // padding only at the very end, at most two
                ++pad;
                v[j] = 0;
            } else {
                if (pad > 0) return false;                        // data after padding
                v[j] = B64Index(c);
                if (v[j] < 0) return false;
            }
        }
        const std::uint32_t n = (static_cast<std::uint32_t>(v[0]) << 18) | (static_cast<std::uint32_t>(v[1]) << 12) |
                                (static_cast<std::uint32_t>(v[2]) << 6) | static_cast<std::uint32_t>(v[3]);
        out.push_back(static_cast<char>((n >> 16) & 0xFF));
        if (pad < 2) out.push_back(static_cast<char>((n >> 8) & 0xFF));
        if (pad < 1) out.push_back(static_cast<char>(n & 0xFF));
    }
    return true;
}

std::string HexEncode(const std::uint8_t* data, std::size_t size) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 15]);
    }
    return out;
}

bool IsLowerHex(std::string_view s) {
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool IsValidUtf8(std::string_view s) {
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        const auto c = static_cast<std::uint8_t>(s[i]);
        if (c < 0x80) { ++i; continue; }
        std::size_t extra;
        std::uint32_t cp, minimum;
        if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1Fu; minimum = 0x80; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0Fu; minimum = 0x800; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07u; minimum = 0x10000; }
        else return false;
        if (i + extra >= n) return false;   // truncated sequence
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto cc = static_cast<std::uint8_t>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += extra + 1;
    }
    return true;
}

}  // namespace w2f::net
