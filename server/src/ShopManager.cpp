#include "w2f/ShopManager.h"

#include <cassert>

#include "w2f/PlayerState.h"

namespace w2f {

ShopManager::ShopManager(PlayerState& owner, SharedChampionPool& pool, const ShopConfig& config, Rng rng)
    : owner_(owner),
      pool_(pool),
      config_(config),
      rng_(rng),
      slots_(static_cast<std::size_t>(config.slotCount), nullptr) {}

ActionResult ShopManager::TryReroll() {
    if (!owner_.IsAlive()) return ActionResult::PlayerEliminated;
    if (!owner_.TrySpendGold(config_.rerollCost)) return ActionResult::NotEnoughGold;
    Refresh();
    return ActionResult::Ok;
}

void ShopManager::Refresh() {
    // Old offer goes back first, so a reroll can legitimately show the same champion again.
    ReturnShopToPool();
    for (auto& slot : slots_) slot = DrawOne();
}

ActionResult ShopManager::TryBuy(std::size_t slot) {
    if (!owner_.IsAlive()) return ActionResult::PlayerEliminated;
    if (slot >= slots_.size()) return ActionResult::InvalidSlot;
    const ChampionDefinition* champion = slots_[slot];
    if (champion == nullptr) return ActionResult::EmptySlot;
    // Room is checked before gold so a rejected purchase changes nothing. A full roster still
    // accepts a copy that completes a merge.
    if (!owner_.CanAcquire(champion)) return ActionResult::RosterFull;
    if (owner_.Gold() < champion->cost) return ActionResult::NotEnoughGold;

    const bool paid = owner_.TrySpendGold(champion->cost);
    assert(paid);
    (void)paid;
    const ActionResult acquired = owner_.AcquireUnit(champion, champion->cost);
    assert(acquired == ActionResult::Ok);
    (void)acquired;
    slots_[slot] = nullptr;  // The copy stays checked out of the pool; it is now a unit (or part of a merged one).
    return ActionResult::Ok;
}

bool ShopManager::RestoreState(const std::vector<const ChampionDefinition*>& slots, const RngState& rng) {
    if (slots.size() != slots_.size() || !rng.IsValid()) return false;
    slots_ = slots;
    rng_.SetState(rng);
    return true;
}

void ShopManager::ReturnShopToPool() {
    for (auto& slot : slots_) {
        if (slot != nullptr) {
            const bool returned = pool_.Return(slot, 1);
            assert(returned);
            (void)returned;
            slot = nullptr;
        }
    }
}

const ChampionDefinition* ShopManager::DrawOne() {
    // Pick a tier from the level's odds, ignoring tiers the pool has run dry on, then pick a
    // champion inside that tier. Integer weights only -> fully deterministic.
    const auto& odds = config_.dropRatesByLevel[static_cast<std::size_t>(owner_.Level() - 1)];

    int weights[kMaxCostTier];
    std::uint32_t totalWeight = 0;
    for (int t = 0; t < kMaxCostTier; ++t) {
        weights[t] = pool_.RemainingInTier(t + 1) > 0 ? odds[static_cast<std::size_t>(t)] : 0;
        totalWeight += static_cast<std::uint32_t>(weights[t]);
    }
    if (totalWeight > 0) {
        std::uint32_t roll = rng_.NextBelow(totalWeight);
        for (int t = 0; t < kMaxCostTier; ++t) {
            const std::uint32_t weight = static_cast<std::uint32_t>(weights[t]);
            if (roll < weight) {
                if (const ChampionDefinition* drawn = pool_.DrawFromTier(t + 1, rng_)) return drawn;
                break;  // (cannot happen: the tier had copies) -- fall back rather than hand out a hole
            }
            roll -= weight;
        }
    }

    // Every tier this level can roll is sold out. Rather than leave the slot empty while the pool still holds copies, roll a different tier:
    // the nearest one with stock to the level's most likely tier (the cheaper one on a tie). Only when the WHOLE pool is empty is the slot
    // left empty -- and an empty slot is a valid, buyable-as-nothing state (TryBuy answers EmptySlot), never a crash or a wait.
    int favourite = 0;
    for (int t = 1; t < kMaxCostTier; ++t) {
        if (odds[static_cast<std::size_t>(t)] > odds[static_cast<std::size_t>(favourite)]) favourite = t;
    }
    for (int distance = 0; distance < kMaxCostTier; ++distance) {
        for (int direction : {-1, +1}) {
            if (distance == 0 && direction == +1) continue;
            const int t = favourite + direction * distance;
            if (t < 0 || t >= kMaxCostTier || pool_.RemainingInTier(t + 1) <= 0) continue;
            if (const ChampionDefinition* drawn = pool_.DrawFromTier(t + 1, rng_)) return drawn;
        }
    }
    return nullptr;
}

}  // namespace w2f
