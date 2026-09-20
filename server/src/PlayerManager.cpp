#include "w2f/PlayerManager.h"

#include "w2f/ShopManager.h"

namespace w2f {

PlayerManager::PlayerManager(int playerCount, const PlayerConfig& playerConfig, const ShopConfig& shopConfig,
                             SharedChampionPool& pool, std::uint64_t matchSeed, IPlayerListener* listener,
                             const ItemDatabase* items) {
    players_.reserve(static_cast<std::size_t>(playerCount));
    for (int i = 0; i < playerCount; ++i) {
        players_.push_back(
            std::make_unique<PlayerState>(static_cast<PlayerId>(i), playerConfig, shopConfig, pool, matchSeed, listener, items));
    }
}

int PlayerManager::AliveCount() const {
    int alive = 0;
    for (const auto& p : players_) alive += p->IsAlive() ? 1 : 0;
    return alive;
}

std::vector<PlayerId> PlayerManager::AlivePlayerIds() const {
    std::vector<PlayerId> ids;
    for (const auto& p : players_) {
        if (p->IsAlive()) ids.push_back(p->Id());
    }
    return ids;
}

PlayerState* PlayerManager::Get(PlayerId id) {
    return id < players_.size() ? players_[id].get() : nullptr;
}

const PlayerState* PlayerManager::Get(PlayerId id) const {
    return id < players_.size() ? players_[id].get() : nullptr;
}

void PlayerManager::GrantRoundIncome(int round) {
    for (auto& p : players_) p->GrantRoundIncome(round);
}

void PlayerManager::RefreshAllShops() {
    for (auto& p : players_) {
        if (p->IsAlive()) p->Shop().Refresh();
    }
}

void PlayerManager::CloseAllShops() {
    for (auto& p : players_) {
        if (p->IsAlive()) p->Shop().ReturnShopToPool();
    }
}

void PlayerManager::EliminatePlayer(PlayerId id, int placement) {
    if (PlayerState* p = Get(id)) p->Eliminate(placement);
}

}  // namespace w2f
