#pragma once

// Per-player shop. Owned by (and attached to) one PlayerState. Draws champions out of the
// SharedChampionPool using the drop rates for the owner's *current* level.
//
// A champion sitting in a shop slot is "checked out" of the pool. Slots that are
// replaced by a refresh/reroll, or left unbought when the player is eliminated, go back.

#include <vector>

#include "w2f/ChampionDatabase.h"
#include "w2f/Config.h"
#include "w2f/Rng.h"
#include "w2f/SharedChampionPool.h"

namespace w2f {

class PlayerState;

class ShopManager {
public:
    // `owner` and `pool` must outlive the shop (PlayerState guarantees this for `owner`).
    ShopManager(PlayerState& owner, SharedChampionPool& pool, const ShopConfig& config, Rng rng);

    ShopManager(const ShopManager&) = delete;
    ShopManager& operator=(const ShopManager&) = delete;

    // Pay rerollCost gold, return the current offer to the pool, draw a new one.
    ActionResult TryReroll();

    // Free refresh (start-of-round). Same pool behaviour as a reroll, no gold involved.
    void Refresh();

    // Buy the champion in `slot`: pays its cost (a special unit's price) and moves it into the owner's roster.
    ActionResult TryBuy(std::size_t slot);

    // Puts a special unit (never in the pool: the Phaisa Queen) in the first slot; whatever was there goes back to the pool. False if it is already offered.
    bool OfferSpecial(const ChampionDefinition* champion);

    // Return every unbought champion to the pool and empty the shop (used on elimination).
    void ReturnShopToPool();

    // nullptr = empty slot (bought, or pool had nothing to offer).
    const std::vector<const ChampionDefinition*>& Slots() const { return slots_; }

    int RerollCost() const { return config_.rerollCost; }

    // Snapshot support. The draw stream is part of the state: two shops with the same slots but different generators
    // would deal different offers on the next refresh.
    RngState GetRngState() const { return rng_.GetState(); }
    // Replaces the offer WITHOUT touching the pool (the pool's counts are restored separately). The slot count must
    // match the configuration.
    bool RestoreState(const std::vector<const ChampionDefinition*>& slots, const RngState& rng);

private:
    const ChampionDefinition* DrawOne();

    PlayerState& owner_;
    SharedChampionPool& pool_;
    ShopConfig config_;
    Rng rng_;
    std::vector<const ChampionDefinition*> slots_;
};

}  // namespace w2f
