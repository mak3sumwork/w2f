#pragma once

// Notifications about a single player's units, for the UE5 wrapper / UI / VFX.
//
// IMatchListener extends this interface, so a wrapper registers ONE listener with the
// MatchManager and receives both match-level and player-level events. Standalone
// PlayerStates (e.g. in tests) can be given an IPlayerListener directly.
//
// Rules for implementers:
//  - Callbacks run synchronously inside the action that caused them. Do not call back into
//    the server from a callback; copy what you need and act later.
//  - Unit references are only valid for the duration of the call.
//  - Player-level events fire in the order things happened, e.g. a purchase that completes a
//    merge produces OnUnitBought and then OnUnitMerged (once per merge step in a cascade).

#include <vector>

#include "w2f/Types.h"
#include "w2f/Unit.h"

namespace w2f {

// What a player was paid at the start of a round (streak gold is the win / loss streak bonus).
struct IncomeBreakdown {
    int baseGold = 0;
    int interestGold = 0;
    int streakGold = 0;
    int passiveXp = 0;
    int TotalGold() const { return baseGold + interestGold + streakGold; }
};

class IPlayerListener {
public:
    virtual ~IPlayerListener() = default;

    // Round income was paid (fires for every living player, even when it is all zero).
    virtual void OnIncomeGranted(PlayerId /*player*/, int /*round*/, const IncomeBreakdown& /*income*/) {}

    // A unit entered the player's roster. `unit` is as first placed (star level as acquired,
    // before any merge). `goldSpent` is 0 for free units (carousel / awards).
    virtual void OnUnitBought(PlayerId /*player*/, const UnitInstance& /*unit*/, int /*goldSpent*/) {}

    // `unit` is its state just before removal; the copies are already back in the pool.
    virtual void OnUnitSold(PlayerId /*player*/, const UnitInstance& /*unit*/, int /*goldGained*/) {}

    // Fired once per unit that changed position: a swap produces two calls.
    virtual void OnUnitMoved(PlayerId /*player*/, const UnitMove& /*move*/) {}

    virtual void OnUnitMerged(PlayerId /*player*/, const UnitMerge& /*merge*/) {}

    // An item moved from the player's item bag onto a unit / back off it into the bag. `unit` is its state afterwards.
    virtual void OnItemEquipped(PlayerId /*player*/, const UnitInstance& /*unit*/, ItemId /*item*/) {}
    virtual void OnItemUnequipped(PlayerId /*player*/, const UnitInstance& /*unit*/, ItemId /*item*/) {}
    // Two items on a unit turned into one (right after OnItemEquipped for the second, or after OnUnitMerged for items carried over
    // by a merge). `unit` is its state afterwards: the result already sits in the slot the first item had.
    virtual void OnItemsCombined(PlayerId /*player*/, const UnitInstance& /*unit*/, const ItemCombination& /*combination*/) {}
    // A consumable (the Item Remover) was used on `unit` and is gone from the bag. `returned` are the items that came off the unit, in slot order (each was
    // also announced by OnItemUnequipped just before). `unit` is its state afterwards.
    virtual void OnItemConsumed(PlayerId /*player*/, const UnitInstance& /*unit*/, ItemId /*consumable*/, const std::vector<ItemId>& /*returned*/) {}
};

}  // namespace w2f
