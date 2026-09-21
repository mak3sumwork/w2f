#include "w2f/AIBotController.h"

#include <algorithm>
#include <numeric>
#include <string>
#include <tuple>
#include <vector>

namespace w2f {

namespace {
// Fill order for a row: centre outward, so formations are compact rather than hugging one edge.
constexpr int kColumnOrder[kBoardColumns] = {3, 2, 4, 1, 5, 0, 6};
// Row preference (board frame: y = 0 back row, y = kBoardRows-1 front row).
constexpr int kFrontFirst[kBoardRows] = {3, 2, 1, 0};
constexpr int kBackFirst[kBoardRows] = {0, 1, 2, 3};

// How well an item's stats suit a champion, in rough "worth" points (a +15 attack damage component is ~45, a +25 armor one ~50).
// Tanks want durability; a champion whose spell out-scales its attack (and that has mana) wants ability damage and mana regeneration;
// every other damage dealer wants attack damage, attack speed and crit. Each also values the other kinds a little.
int ItemFit(const ChampionDefinition& champion, const ItemStats& s) {
    const int physical = s.attackDamage * 3 + s.attackSpeedPercent * 2 + s.critChance * 2;
    const int magical = s.abilityDamage * 3 + s.manaRegenMilli / 40 + s.startMana;
    const int defence = s.maxHp / 4 + s.armor * 2 + s.magicResist * 2;
    if (champion.role == ChampionRole::Tank) return defence * 2 + (physical + magical) / 4;
    const bool caster = champion.stats.maxMana > 0 && champion.stats.abilityDamage[0] >= champion.stats.attackDamage[0];
    return (caster ? magical * 2 + physical / 2 : physical * 2 + magical / 2) + defence / 3;
}

bool HasTrait(const UnitInstance& unit, const ItemDatabase& items, const std::string& trait) {
    if (std::find(unit.champion->traits.begin(), unit.champion->traits.end(), trait) != unit.champion->traits.end()) return true;
    for (ItemId held : unit.items) {
        const ItemDefinition* def = held != 0 ? items.Find(held) : nullptr;
        if (def != nullptr && std::find(def->grantsTraits.begin(), def->grantsTraits.end(), trait) != def->grantsTraits.end()) return true;
    }
    return false;
}

// Points for putting `item` on `unit`, or a negative number when it should not go there at all: no free slot and no recipe to make room,
// a trait item the unit already has, or an item with no effect of its own (the Omnilium Seed) that would not combine into something.
constexpr int kNotWanted = -1000000;
int ScoreItemOnUnit(const UnitInstance& unit, const ItemDefinition& item, const ItemDatabase& items) {
    const ItemDefinition* result = nullptr;   // what equipping would leave behind, if it combines
    for (ItemId held : unit.items) {
        if (held == 0) continue;
        const ItemDefinition* combined = items.FindCombination(held, item.id);
        if (combined != nullptr) {
            const ItemDefinition* current = result;
            if (current == nullptr || ItemFit(*unit.champion, combined->stats) > ItemFit(*unit.champion, current->stats)) result = combined;
        }
    }
    if (result == nullptr && unit.ItemCount() >= kMaxItemsPerUnit) return kNotWanted;
    if (result == nullptr && !item.HasEffect()) return kNotWanted;
    const ItemDefinition& landing = result != nullptr ? *result : item;
    int points = ItemFit(*unit.champion, landing.stats);
    for (const std::string& trait : landing.grantsTraits) {
        if (HasTrait(unit, items, trait)) return kNotWanted;   // an emblem for a trait it already has
        points += 25;
    }
    if (result != nullptr) points += 15;   // finishing an item beats starting one
    return points * 4 + unit.champion->cost * unit.starLevel * 3;   // the same item does more on a stronger unit
}
}  // namespace

AIBotController::AIBotController(PlayerId player, std::uint64_t matchSeed, const BotProfile& profile)
    : player_(player), rng_(matchSeed, kRngStreamBotBase + player), profile_(profile) {}

void AIBotController::Tick(MatchManager& match) {
    if (match.Phase() == MatchPhase::MotherNature) {
        PickGift(match);
        return;
    }
    if (match.Phase() != MatchPhase::Planning) return;
    if (match.Round() == lastActedRound_) return;  // already played this round's planning
    const PlayerState* self = match.Players().Get(player_);
    if (self == nullptr || !self->IsAlive()) return;
    lastActedRound_ = match.Round();

    BuyExperience(match);
    BuyUnits(match);
    PlaceUnits(match);
    EquipItems(match);
}

// Mother Nature: take the most useful gift on offer -- a unit first, then an item, gold, XP and healing last. (No randomness, so a bot that
// is restored from a snapshot picks the same thing.) Does nothing once it has picked.
void AIBotController::PickGift(MatchManager& match) {
    const PlayerState* self = match.Players().Get(player_);
    if (self == nullptr || !self->IsAlive() || match.GiftSettled(player_)) return;
    const std::vector<GiftOffer>& offers = match.GiftOffers(player_);
    const auto rank = [](GiftType type) {
        switch (type) {
            case GiftType::Unit: return 4;
            case GiftType::Item: return 3;
            case GiftType::Gold: return 2;
            case GiftType::Xp: return 1;
            case GiftType::Heal: return 0;
        }
        return 0;
    };
    std::size_t best = 0;
    for (std::size_t i = 1; i < offers.size(); ++i) {
        if (rank(offers[i].type) > rank(offers[best].type)) best = i;
    }
    if (!offers.empty()) match.TryPickGift(player_, best);
}

void AIBotController::BuyExperience(MatchManager& match) {
    const PlayerState* self = match.Players().Get(player_);
    while (self->Gold() > profile_.levelUpAboveGold) {
        if (match.TryBuyXp(player_) != ActionResult::Ok) break;  // max level (or anything unexpected)
    }
}

void AIBotController::BuyUnits(MatchManager& match) {
    const PlayerState* self = match.Players().Get(player_);
    const std::size_t slotCount = self->Shop().Slots().size();

    std::vector<std::size_t> order(slotCount);
    std::iota(order.begin(), order.end(), std::size_t{0});
    rng_.Shuffle(order);  // "random units": the visit order is random...
    // ...except that another copy of something it already owns is always looked at first (it leads to a star-up).
    const auto owned = [&](std::size_t slot) {
        const ChampionDefinition* offered = self->Shop().Slots()[slot];
        if (offered == nullptr) return false;
        for (const UnitInstance& unit : self->Roster().Units()) {
            if (unit.champion == offered) return true;
        }
        return false;
    };
    std::stable_partition(order.begin(), order.end(), owned);

    for (std::size_t slot : order) {
        const ChampionDefinition* offered = self->Shop().Slots()[slot];
        if (offered == nullptr) continue;
        if (self->Gold() - offered->cost < profile_.reserveGold) continue;  // not spare gold
        if (!self->CanAcquire(offered) && !MakeRoomFor(match, *offered)) continue;
        match.TryBuyShopUnit(player_, slot);  // anything unexpected (a closed shop ...) is simply skipped
    }
}

const UnitInstance* AIBotController::WeakestSellable(const UnitRoster& roster) const {
    const UnitInstance* weakest = nullptr;
    std::tuple<int, int, int, UnitId> weakestKey{};
    for (const UnitInstance& unit : roster.Units()) {
        if (unit.starLevel > 1 || roster.CountOf(unit.champion, unit.starLevel) >= 2) continue;   // keep upgraded units and merge candidates
        // Items last, then bench before board (the bench is what blocks buying), then the cheapest, then the lowest id (a total order).
        const std::tuple<int, int, int, UnitId> key{unit.ItemCount(), unit.location == LocationType::Board ? 1 : 0, unit.champion->cost, unit.id};
        if (weakest == nullptr || key < weakestKey) {
            weakest = &unit;
            weakestKey = key;
        }
    }
    return weakest;
}

bool AIBotController::MakeRoomFor(MatchManager& match, const ChampionDefinition& offered) {
    const PlayerState* self = match.Players().Get(player_);
    const UnitInstance* weakest = WeakestSellable(self->Roster());
    if (weakest == nullptr || weakest->champion == &offered) return false;
    const int have = weakest->champion->cost;
    const bool worthIt = offered.cost > have || (offered.cost == have && self->Roster().CountOf(&offered, 1) >= 1);
    if (!worthIt) return false;
    if (match.TrySellUnit(player_, weakest->id) != ActionResult::Ok) return false;
    return self->CanAcquire(&offered);
}

void AIBotController::PlaceUnits(MatchManager& match) {
    const PlayerState* self = match.Players().Get(player_);
    const UnitRoster& roster = self->Roster();

    for (int slot = 0; slot < kBenchSlots; ++slot) {
        if (roster.BoardCount() >= roster.BoardCapacity()) return;  // board is at its unit limit
        const UnitInstance* unit = roster.BenchAt(slot);
        if (unit == nullptr) continue;

        const int* rows = unit->champion->role == ChampionRole::Tank ? kFrontFirst : kBackFirst;
        bool placed = false;
        for (int r = 0; r < kBoardRows && !placed; ++r) {
            for (int c = 0; c < kBoardColumns && !placed; ++c) {
                const int y = rows[r];
                const int x = kColumnOrder[c];
                if (roster.BoardAt(x, y) != nullptr) continue;
                placed = match.TryMoveUnit(player_, unit->id, LocationType::Board, x, y) == ActionResult::Ok;
            }
        }
        if (!placed) return;  // board completely full
    }
}

void AIBotController::EquipItems(MatchManager& match) {
    const ItemDatabase* items = match.Items();
    const PlayerState* self = match.Players().Get(player_);
    if (items == nullptr) return;

    const std::vector<ItemId> bag = self->ItemBag();   // a copy: equipping (and combining) changes the bag as we go
    for (ItemId id : bag) {
        const ItemDefinition* item = items->Find(id);
        if (item == nullptr) continue;
        const UnitInstance* best = nullptr;
        int bestScore = kNotWanted;
        for (const UnitInstance& unit : self->Roster().Units()) {
            if (unit.location != LocationType::Board) continue;   // only what fights is worth equipping
            const int score = ScoreItemOnUnit(unit, *item, *items);
            if (score > bestScore || (score == bestScore && best != nullptr && unit.id < best->id)) {
                best = &unit;
                bestScore = score;
            }
        }
        if (best != nullptr && bestScore > kNotWanted) match.TryEquipItem(player_, best->id, id);
    }
}

}  // namespace w2f
