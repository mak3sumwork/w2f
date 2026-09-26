#pragma once

// The single, finite pool of champion copies shared by all players. Every copy is in
// exactly one place at any time: the pool, a shop slot, or a player's roster.
// Conservation of that invariant is what MatchManager::VerifyPoolIntegrity() checks.

#include <array>
#include <cstddef>
#include <vector>

#include "w2f/ChampionDatabase.h"
#include "w2f/Config.h"
#include "w2f/Rng.h"

namespace w2f {

class SharedChampionPool {
public:
    // `database` must outlive the pool.
    SharedChampionPool(const ChampionDatabase& database, const PoolConfig& config);

    SharedChampionPool(const SharedChampionPool&) = delete;
    SharedChampionPool& operator=(const SharedChampionPool&) = delete;

    // Removes one random copy from the given tier and returns its definition, or nullptr if
    // the tier is sold out. Each remaining *copy* is equally likely, so a champion with
    // more copies left is proportionally more likely to be drawn.
    const ChampionDefinition* DrawFromTier(int tier, Rng& rng);

    // Puts copies back. Returns false (and changes nothing) if that would exceed the
    // champion's original supply -- that always indicates a bookkeeping bug in the caller.
    bool Return(const ChampionDefinition* champion, int copies = 1);
    // Checks `copies` of one specific champion out of the pool (admin tools, tests, scripted setups). False (and nothing changes) if not enough remain.
    bool Take(const ChampionDefinition* champion, int copies = 1);

    // Snapshot support: replaces the remaining-copy counts (parallel to Database().All(), so one per champion).
    // Fails without changing anything unless every count is within [0, initial supply] and the size matches.
    bool RestoreRemaining(const std::vector<int>& remaining);

    int Remaining(ChampionId id) const;
    int InitialCopies(ChampionId id) const;
    int RemainingInTier(int tier) const;

    // Copies a unit is worth when returned: 1 star = 1, 2 star = 3, 3 star = 9.
    static int CopiesForStarLevel(int starLevel);

    const ChampionDatabase& Database() const { return database_; }

private:
    const ChampionDatabase& database_;
    std::vector<int> remaining_;  // parallel to database_.All()
    std::vector<int> initial_;
    std::array<std::vector<std::size_t>, kMaxCostTier> tierMembers_;  // database indices, ascending
    std::array<int, kMaxCostTier> tierRemaining_{};
};

}  // namespace w2f
