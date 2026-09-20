#pragma once

// Match snapshots: the ENTIRE authoritative state of a MatchManager as a byte buffer, and back.
//
//   std::vector<std::uint8_t> bytes = match.Snapshot();
//   auto again = MatchManager::Restore(bytes, config, database, std::move(simulator), &error, items);
//
// Uses: a reconnecting client's server-side resume, rollback while debugging, "save the match at round 12 and replay
// from there", regression fixtures. Restoring is PERFECT: the restored match has the same StateHash, produces the same
// bytes if snapshotted again, and every later tick, action and fight is identical to the original's.
//
// Format (binary, little-endian, fixed-width integers only, so the same bytes on every platform and compiler):
//
//   header   magic "W2FS", format version, match seed, hashes of the game config / champion / item / PvE / Mother Nature data the
//            snapshot was taken under, the StateHash at that moment, phase, round, ticks in phase, player count
//   body     match RNG state, pool counts, every player (economy, units in roster order, item bag, shop offer + shop
//            RNG, Mother Nature offers), this round's matchups, and every fight's outcome INCLUDING its event log (a viewer that reconnects
//            mid-round needs the stream it missed, and Resolution needs the results)
//   trailer  FNV-1a over everything before it
//
// Restore never trusts the buffer. It checks the trailer, every count and range, that every id resolves in the loaded
// data, the roster/pool invariants, and finally that recomputing StateHash() gives the value stored in the header, so a
// buffer that decodes cleanly but describes an impossible match is refused. It returns nullptr and an error message; it
// never crashes on garbage (no exceptions, no unchecked reads).
//
// NOT inside a snapshot (they are not match state): the listeners, the combat simulator object (the caller passes one
// to Restore), the champion/item/trait data (only hashes of the first two are recorded), and AI bots / other clients
// (save those with AIBotController::GetState).

#include <cstdint>
#include <string>
#include <vector>

#include "w2f/Types.h"

namespace w2f {

// MotherNature: the gift phase that opens every 3rd round (see MotherNature.h). It took the place of the old Draft phase.
enum class MatchPhase : std::uint8_t { NotStarted, MotherNature, Planning, Combat, Resolution, MatchOver };

constexpr std::uint32_t kSnapshotMagic = 0x53463257u;  // bytes 'W' '2' 'F' 'S'
constexpr std::uint32_t kSnapshotVersion = 3;

// What a snapshot says about itself, readable without restoring it.
struct SnapshotInfo {
    std::uint32_t version = 0;
    std::uint64_t seed = 0;
    std::uint64_t configHash = 0;
    std::uint64_t championHash = 0;
    std::uint64_t itemHash = 0;   // 0 when the match ran without an item database
    std::uint64_t encounterHash = 0;   // 0 when the match ran without PvE data
    std::uint64_t motherNatureHash = 0;   // 0 when the match ran without Mother Nature data
    std::uint64_t stateHash = 0;  // MatchManager::StateHash() when it was taken
    std::uint8_t phase = 0;       // a MatchPhase value
    int round = 0;
    int ticksInPhase = 0;
    int playerCount = 0;
};

// Reads and verifies just the header and trailer (magic, version, checksum). False + *error if the buffer is not an
// intact snapshot of a version this build understands.
bool ReadSnapshotInfo(const std::vector<std::uint8_t>& bytes, SnapshotInfo& out, std::string* error = nullptr);

struct RestoreOptions {
    // Refuse to restore against a game config / champion / item / PvE data whose hash differs from the one recorded.
    // Turn off to deliberately replay an old snapshot under retuned data (debugging): the match then continues under
    // the NEW rules, and units whose champion no longer exists are still rejected.
    bool requireMatchingData = true;
};

}  // namespace w2f
