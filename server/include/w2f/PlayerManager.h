#pragma once

// Owns the PlayerState for every seat in the match and handles the operations that span
// players (round income, shop refresh, elimination). PlayerIds are seat indices 0..N-1.

#include <memory>
#include <vector>

#include "w2f/Config.h"
#include "w2f/PlayerState.h"
#include "w2f/SharedChampionPool.h"

namespace w2f {

class PlayerManager {
public:
    // `pool` must outlive the manager. `listener` (optional) is attached to every player.
    PlayerManager(int playerCount, const PlayerConfig& playerConfig, const ShopConfig& shopConfig,
                  SharedChampionPool& pool, std::uint64_t matchSeed, IPlayerListener* listener = nullptr,
                  const ItemDatabase* items = nullptr);

    PlayerManager(const PlayerManager&) = delete;
    PlayerManager& operator=(const PlayerManager&) = delete;

    int PlayerCount() const { return static_cast<int>(players_.size()); }
    int AliveCount() const;
    std::vector<PlayerId> AlivePlayerIds() const;  // ascending id order

    // nullptr for an out-of-range id. Pointers are stable for the manager's lifetime.
    PlayerState* Get(PlayerId id);
    const PlayerState* Get(PlayerId id) const;

    void GrantRoundIncome(int round);  // every living player
    void RefreshAllShops();            // every living player, ascending id order
    void EliminatePlayer(PlayerId id, int placement);

private:
    std::vector<std::unique_ptr<PlayerState>> players_;
};

}  // namespace w2f
