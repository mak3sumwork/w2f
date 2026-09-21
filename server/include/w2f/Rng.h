#pragma once

// Deterministic random number generator (xoshiro256**, seeded via SplitMix64).
//
// Why not <random>? The standard *engines* are portable, but the standard
// *distributions* (uniform_int_distribution etc.) are implementation-defined, so the
// same seed can produce different results on MSVC / Clang / GCC. Server and client
// (or two servers replaying a match) must agree bit-for-bit, so we own the algorithm.

#include <cstdint>
#include <utility>
#include <vector>

namespace w2f {

// Independent streams derived from the single match seed. Giving each consumer its own
// stream means one player's shop activity cannot perturb another consumer's rolls.
constexpr std::uint64_t kRngStreamMatch = 0;
constexpr std::uint64_t kRngStreamShopBase = 100;    // + PlayerId
constexpr std::uint64_t kRngStreamCombatBase = 1000; // + round number
constexpr std::uint64_t kRngStreamBotBase = 2000;    // + PlayerId
constexpr std::uint64_t kRngStreamPveBase = 3000;    // + round number: which encounter a PvE round uses
constexpr std::uint64_t kRngStreamPveDropBase = 4000; // + round number: the drops that round's winners get
constexpr std::uint64_t kRngStreamMotherNatureBase = 5000; // + round number: the gifts Mother Nature offers that round
constexpr std::uint64_t kRngStreamOpening = 6000;     // the units dealt to every player before round 1

// The full generator state, for snapshots. All-zero is not a valid xoshiro state (it would emit zeros forever).
struct RngState {
    std::uint64_t words[4] = {0, 0, 0, 0};
    bool IsValid() const { return (words[0] | words[1] | words[2] | words[3]) != 0; }
    bool operator==(const RngState& o) const {
        return words[0] == o.words[0] && words[1] == o.words[1] && words[2] == o.words[2] && words[3] == o.words[3];
    }
};

class Rng {
public:
    Rng() : Rng(0, 0) {}

    explicit Rng(std::uint64_t seed, std::uint64_t stream = 0) {
        std::uint64_t sm = Mix(seed) + Mix(stream ^ 0xD1B54A32D192ED03ull);
        for (auto& word : state_) word = SplitMix64(sm);
    }

    RngState GetState() const {
        RngState s;
        for (int i = 0; i < 4; ++i) s.words[i] = state_[i];
        return s;
    }
    void SetState(const RngState& s) {
        for (int i = 0; i < 4; ++i) state_[i] = s.words[i];
    }

    std::uint64_t Next64() {
        const std::uint64_t result = Rotl(state_[1] * 5, 7) * 9;
        const std::uint64_t t = state_[1] << 17;
        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];
        state_[2] ^= t;
        state_[3] = Rotl(state_[3], 45);
        return result;
    }

    std::uint32_t Next32() { return static_cast<std::uint32_t>(Next64() >> 32); }

    // Uniform integer in [0, bound). Rejection sampling: no modulo bias. bound must be > 0.
    std::uint32_t NextBelow(std::uint32_t bound) {
        const std::uint32_t threshold = (std::uint32_t(0) - bound) % bound;  // 2^32 mod bound
        for (;;) {
            const std::uint32_t r = Next32();
            if (r >= threshold) return r % bound;
        }
    }

    // Fisher-Yates. Result depends only on generator state and input order.
    template <typename T>
    void Shuffle(std::vector<T>& v) {
        for (std::size_t i = v.size(); i > 1; --i) {
            const std::size_t j = NextBelow(static_cast<std::uint32_t>(i));
            std::swap(v[i - 1], v[j]);
        }
    }

private:
    static std::uint64_t Rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

    static std::uint64_t SplitMix64(std::uint64_t& x) {
        std::uint64_t z = (x += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    static std::uint64_t Mix(std::uint64_t x) { return SplitMix64(x); }

    std::uint64_t state_[4];
};

}  // namespace w2f
