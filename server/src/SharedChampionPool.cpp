#include "w2f/SharedChampionPool.h"

#include <cassert>

namespace w2f {

namespace {
// Tier (cost) 1..kMaxCostTier -> array index 0..kMaxCostTier-1.
std::size_t TierIndex(int tier) { return static_cast<std::size_t>(tier - 1); }
}  // namespace

SharedChampionPool::SharedChampionPool(const ChampionDatabase& database, const PoolConfig& config)
    : database_(database) {
    const auto& all = database_.All();
    remaining_.resize(all.size());
    initial_.resize(all.size());
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (!all[i].IsPooled()) continue;   // summons, plants and special units are never sold: no copies exist (initial_ / remaining_ stay 0)
        const std::size_t tierIndex = TierIndex(all[i].cost);  // cost validated by ChampionDatabase::Create
        const int copies = config.copiesPerTier[tierIndex];
        initial_[i] = copies;
        remaining_[i] = copies;
        tierMembers_[tierIndex].push_back(i);
        tierRemaining_[tierIndex] += copies;
    }
}

const ChampionDefinition* SharedChampionPool::DrawFromTier(int tier, Rng& rng, const Allowed& allowed) {
    if (tier < 1 || tier > kMaxCostTier) return nullptr;
    const std::size_t tierIndex = TierIndex(tier);
    const int drawable = DrawableInTier(tier, allowed);
    if (drawable <= 0) return nullptr;

    std::uint32_t roll = rng.NextBelow(static_cast<std::uint32_t>(drawable));
    for (std::size_t index : tierMembers_[tierIndex]) {
        const ChampionDefinition& def = database_.All()[index];
        if (allowed ? !allowed(def) : def.unlock.Gated()) continue;
        const std::uint32_t copies = static_cast<std::uint32_t>(remaining_[index]);
        if (roll < copies) {
            --remaining_[index];
            --tierRemaining_[tierIndex];
            return &def;
        }
        roll -= copies;
    }
    assert(false && "tierRemaining_ out of sync with remaining_");
    return nullptr;
}

int SharedChampionPool::DrawableInTier(int tier, const Allowed& allowed) const {
    if (tier < 1 || tier > kMaxCostTier) return 0;
    int total = 0;
    for (std::size_t index : tierMembers_[TierIndex(tier)]) {
        const ChampionDefinition& def = database_.All()[index];
        if (allowed ? allowed(def) : !def.unlock.Gated()) total += remaining_[index];
    }
    return total;
}

bool SharedChampionPool::Return(const ChampionDefinition* champion, int copies) {
    if (champion == nullptr || copies <= 0) return false;
    const std::size_t index = database_.IndexOf(champion->id);
    if (index == ChampionDatabase::kNotFound) return false;
    if (remaining_[index] + copies > initial_[index]) return false;
    remaining_[index] += copies;
    tierRemaining_[TierIndex(champion->cost)] += copies;
    return true;
}

bool SharedChampionPool::Take(const ChampionDefinition* champion, int copies) {
    if (champion == nullptr || copies <= 0) return false;
    const std::size_t index = database_.IndexOf(champion->id);
    if (index == ChampionDatabase::kNotFound || remaining_[index] < copies) return false;
    remaining_[index] -= copies;
    tierRemaining_[TierIndex(champion->cost)] -= copies;
    return true;
}

bool SharedChampionPool::RestoreRemaining(const std::vector<int>& remaining) {
    if (remaining.size() != remaining_.size()) return false;
    for (std::size_t i = 0; i < remaining.size(); ++i) {
        if (remaining[i] < 0 || remaining[i] > initial_[i]) return false;
    }
    remaining_ = remaining;
    tierRemaining_.fill(0);
    const auto& all = database_.All();
    for (std::size_t i = 0; i < all.size(); ++i) tierRemaining_[TierIndex(all[i].cost)] += remaining_[i];
    return true;
}

int SharedChampionPool::Remaining(ChampionId id) const {
    const std::size_t index = database_.IndexOf(id);
    return index == ChampionDatabase::kNotFound ? 0 : remaining_[index];
}

int SharedChampionPool::InitialCopies(ChampionId id) const {
    const std::size_t index = database_.IndexOf(id);
    return index == ChampionDatabase::kNotFound ? 0 : initial_[index];
}

int SharedChampionPool::RemainingInTier(int tier) const {
    if (tier < 1 || tier > kMaxCostTier) return 0;
    return tierRemaining_[TierIndex(tier)];
}

int SharedChampionPool::CopiesForStarLevel(int starLevel) {
    int copies = 1;
    for (int s = 1; s < starLevel; ++s) copies *= 3;
    return copies;
}

}  // namespace w2f
