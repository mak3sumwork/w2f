#include "w2f/AIBotController.h"

#include <algorithm>
#include <map>
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

// ---- synergies and unit strength ------------------------------------------------------------------------------------------------
// A synergy is worth kTierValue points per breakpoint reached, growing in a straight line between one breakpoint and the next (so a unit
// that gets a team from 1 to 2 of a 4-trait is worth something before the 4 is there).
constexpr int kTierValue = 12;

// trait tag -> the DIFFERENT champions carrying it (two copies of one champion count once, like the engine's synergies)
using TagSets = std::map<std::string, std::vector<ChampionId>>;

void CollectTags(const ChampionDefinition& champion, const ItemDatabase* items, const std::array<ItemId, kMaxItemsPerUnit>* held, std::vector<std::string>& out) {
    out = champion.traits;
    if (items == nullptr || held == nullptr) return;
    for (ItemId id : *held) {
        const ItemDefinition* def = id != 0 ? items->Find(id) : nullptr;
        if (def == nullptr) continue;
        for (const std::string& tag : def->grantsTraits) {
            if (std::find(out.begin(), out.end(), tag) == out.end()) out.push_back(tag);
        }
    }
}

int SynergyValue(const TraitDatabase* traits, const std::string& tag, int count) {
    const TraitDefinition* def = traits != nullptr && count > 0 ? traits->FindByName(tag) : nullptr;
    if (def == nullptr) return 0;
    int reachedCount = 0;
    int reachedValue = 0;
    int tier = 0;
    for (const TraitBreakpoint& bp : def->Breakpoints(0)) {   // (a trait with paths: every path has the same counts)
        ++tier;
        if (count >= bp.count) {
            reachedCount = bp.count;
            reachedValue = kTierValue * tier;
            continue;
        }
        return reachedValue + kTierValue * (count - reachedCount) / (bp.count - reachedCount);
    }
    return reachedValue;
}

bool Carries(const TagSets& sets, const std::string& tag, ChampionId champion) {
    const auto found = sets.find(tag);
    return found != sets.end() && std::find(found->second.begin(), found->second.end(), champion) != found->second.end();
}

// What adding `champion` (carrying `tags`) to a team with these `sets` is worth in synergy points.
int SynergyGain(const TraitDatabase* traits, const TagSets& sets, ChampionId champion, const std::vector<std::string>& tags) {
    int gain = 0;
    for (const std::string& tag : tags) {
        if (Carries(sets, tag, champion)) continue;
        const auto found = sets.find(tag);
        const int count = found == sets.end() ? 0 : static_cast<int>(found->second.size());
        gain += SynergyValue(traits, tag, count + 1) - SynergyValue(traits, tag, count);
    }
    return gain;
}

void AddToSets(TagSets& sets, ChampionId champion, const std::vector<std::string>& tags) {
    for (const std::string& tag : tags) {
        std::vector<ChampionId>& members = sets[tag];
        if (std::find(members.begin(), members.end(), champion) == members.end()) members.push_back(champion);
    }
}

// A unit's own strength: cost x star (a 2-star is worth about three copies, a 3-star about nine), a little more for each item it carries.
int UnitPower(const UnitInstance& unit) {
    static constexpr int kStarFactor[kMaxStarLevel] = {1, 3, 9};
    const int star = std::max(1, std::min(kMaxStarLevel, unit.starLevel));
    return unit.champion->cost * 4 * kStarFactor[star - 1] + unit.ItemCount() * 3;
}

// The `capacity` units to field: repeatedly the one whose strength plus the synergy it adds to those already chosen is highest (ties: lowest id).
std::vector<const UnitInstance*> ChooseBoard(const UnitRoster& roster, int capacity, const ItemDatabase* items, const TraitDatabase* traits) {
    std::vector<const UnitInstance*> remaining;
    for (const UnitInstance& unit : roster.Units()) {
        if (!unit.champion->plant) remaining.push_back(&unit);   // plants stand where the Nature trait put them: never chosen, never moved
    }
    std::vector<const UnitInstance*> chosen;
    TagSets sets;
    std::vector<std::string> tags;
    int used = 0;   // board slots (the Phaisa Queen takes 2)
    while (used < capacity && !remaining.empty()) {
        remaining.erase(std::remove_if(remaining.begin(), remaining.end(), [&](const UnitInstance* u) { return used + u->champion->teamSlots > capacity; }),
                        remaining.end());
        if (remaining.empty()) break;
        std::size_t best = 0;
        int bestScore = 0;
        for (std::size_t i = 0; i < remaining.size(); ++i) {
            CollectTags(*remaining[i]->champion, items, &remaining[i]->items, tags);
            const int score = UnitPower(*remaining[i]) + SynergyGain(traits, sets, remaining[i]->champion->id, tags) * 3 / 2;
            if (i == 0 || score > bestScore || (score == bestScore && remaining[i]->id < remaining[best]->id)) {
                best = i;
                bestScore = score;
            }
        }
        CollectTags(*remaining[best]->champion, items, &remaining[best]->items, tags);
        AddToSets(sets, remaining[best]->champion->id, tags);
        used += remaining[best]->champion->teamSlots;
        chosen.push_back(remaining[best]);
        remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(best));
    }
    return chosen;
}
}  // namespace

AIBotController::AIBotController(PlayerId player, std::uint64_t matchSeed, const BotProfile& profile, const TraitDatabase* traits)
    : player_(player), rng_(matchSeed, kRngStreamBotBase + player), profile_(profile), traits_(traits) {}

void AIBotController::Tick(MatchManager& match) {
    if (match.Phase() == MatchPhase::MotherNature) {
        PickGift(match);
        return;
    }
    AnswerTraitChoice(match);
    if (match.Phase() != MatchPhase::Planning) return;
    if (match.Round() == lastActedRound_) return;  // already played this round's planning
    const PlayerState* self = match.Players().Get(player_);
    if (self == nullptr || !self->IsAlive()) return;
    lastActedRound_ = match.Round();

    BuyExperience(match);
    BuyUnits(match);
    ArrangeBoard(match);
    EquipItems(match);
}

// Mother Nature: take the most useful gift on offer -- a unit it has room for first, then an item, gold, XP; healing only when it is hurt (then
// it comes right after the unit). A unit it cannot receive would be paid as gold, so it counts as gold. (No randomness, so a bot that is
// restored from a snapshot picks the same thing.) Does nothing once it has picked.
void AIBotController::PickGift(MatchManager& match) {
    const PlayerState* self = match.Players().Get(player_);
    if (self == nullptr || !self->IsAlive() || match.GiftSettled(player_)) return;
    const std::vector<GiftOffer>& offers = match.GiftOffers(player_);
    const auto rank = [&](const GiftOffer& offer) {
        switch (offer.type) {
            case GiftType::Unit: return self->CanAcquire(offer.champion) ? 8 : 4;
            case GiftType::Item: return 6;
            case GiftType::Gold: return 4;
            case GiftType::Xp: return 2;
            case GiftType::Heal: return self->Health() <= profile_.lowHealth ? 7 : 0;
        }
        return 0;
    };
    std::size_t best = 0;
    for (std::size_t i = 1; i < offers.size(); ++i) {
        if (rank(offers[i]) > rank(offers[best])) best = i;
    }
    if (!offers.empty()) match.TryPickGift(player_, best);
}

// Trait system v2: a Hexagon module offer takes the first option; a Najmi cash-out is taken once it is worth it (300+ star dust: a choice of
// completed items), or at 200+ when the bot is in trouble; otherwise it is left on offer and the bank keeps growing. Deterministic.
void AIBotController::AnswerTraitChoice(MatchManager& match) {
    const TraitChoice& choice = match.PendingTraitChoice(player_);
    if (!choice.Pending()) return;
    const PlayerState* self = match.Players().Get(player_);
    if (self == nullptr || !self->IsAlive()) return;
    if (choice.kind == TraitChoiceKind::Module) {
        match.TryPickTraitChoice(player_, 0);
        return;
    }
    const bool take = choice.tier >= 3 || (choice.tier >= 2 && self->Health() <= profile_.lowHealth);
    if (take) match.TryPickTraitChoice(player_, 0);
}

void AIBotController::BuyExperience(MatchManager& match) {
    const PlayerState* self = match.Players().Get(player_);
    while (self->Gold() > profile_.levelUpAboveGold) {
        if (match.TryBuyXp(player_) != ActionResult::Ok) break;  // max level (or anything unexpected)
    }
}

int AIBotController::KeepGold(const MatchManager& match) const {
    const PlayerState* self = match.Players().Get(player_);
    if (self->Health() <= profile_.lowHealth) return profile_.reserveGold;   // in trouble: spend it
    const int stage = match.CurrentStageRound().stage;
    return std::max(profile_.reserveGold, std::min(profile_.maxInterestGold, (stage - 1) * profile_.interestStepGold));
}

void AIBotController::LevelToTarget(MatchManager& match, int keepGold) {
    const PlayerState* self = match.Players().Get(player_);
    const int stage = std::max(1, match.CurrentStageRound().stage);
    const int target = profile_.targetLevelByStage[static_cast<std::size_t>(std::min<int>(stage, static_cast<int>(profile_.targetLevelByStage.size())) - 1)];
    const int cost = match.Config().player.buyXpCost;
    while (self->Level() < target && self->Gold() - cost >= keepGold) {
        if (match.TryBuyXp(player_) != ActionResult::Ok) break;
    }
}

void AIBotController::BuyUnits(MatchManager& match) {
    const PlayerState* self = match.Players().Get(player_);
    const int keep = KeepGold(match);
    BuyWantedUnits(match, keep);
    LevelToTarget(match, keep);
    // The gold above its savings goes into rerolls, looking for more copies and for the units its synergies want.
    const int stage = match.CurrentStageRound().stage;
    for (int rolls = 0; rolls < profile_.maxRerollsPerRound && stage >= 2; ++rolls) {
        if (self->Gold() - self->Shop().RerollCost() < std::max(keep, profile_.reserveGold)) break;
        if (self->Roster().BenchCount() >= kBenchSlots) break;   // nowhere to put what it would find
        if (match.TryRerollShop(player_) != ActionResult::Ok) break;   // (a closed shop, say)
        BuyWantedUnits(match, keep);
    }
}

void AIBotController::BuyWantedUnits(MatchManager& match, int keepGold) {
    const PlayerState* self = match.Players().Get(player_);
    const ItemDatabase* items = match.Items();
    const std::size_t slotCount = self->Shop().Slots().size();
    std::vector<std::string> tags;
    for (;;) {
        const UnitRoster& roster = self->Roster();
        const int capacity = roster.BoardCapacity();
        // The team as it would be fielded now: what a new champion could add to.
        TagSets sets;
        for (const UnitInstance* unit : ChooseBoard(roster, capacity, items, traits_)) {
            CollectTags(*unit->champion, items, &unit->items, tags);
            AddToSets(sets, unit->champion->id, tags);
        }
        const bool needBodies = static_cast<int>(roster.Count()) < capacity + profile_.benchSlack;
        const UnitInstance* weakest = WeakestSellable(roster);

        std::vector<std::size_t> order(slotCount);
        std::iota(order.begin(), order.end(), std::size_t{0});
        rng_.Shuffle(order);  // equally good offers are taken in a random order
        std::size_t pickSlot = slotCount;
        int pickScore = 0;
        for (std::size_t slot : order) {
            const ChampionDefinition* offered = self->Shop().Slots()[slot];
            if (offered == nullptr) continue;
            const int copies = roster.CountOf(offered, 1);
            const int limit = std::max(profile_.reserveGold, copies >= 2 ? 0 : keepGold);   // completing a star-up may dig into the savings
            if (self->Gold() - offered->cost < limit) continue;
            CollectTags(*offered, nullptr, nullptr, tags);
            const int gain = SynergyGain(traits_, sets, offered->id, tags);
            const bool upgrade = weakest != nullptr && weakest->champion != offered && offered->cost > weakest->champion->cost;
            if (copies == 0 && gain <= 0 && !needBodies && !upgrade) continue;   // nothing it wants about this one
            if (!self->CanAcquire(offered) && SellCandidateFor(roster, *offered) == nullptr) continue;
            const int score = (copies >= 2 ? 60 : copies == 1 ? 14 : 0) + gain * 3 / 2 + offered->cost * 4;
            if (pickSlot == slotCount || score > pickScore) {
                pickSlot = slot;
                pickScore = score;
            }
        }
        if (pickSlot == slotCount) return;
        const ChampionDefinition* pick = self->Shop().Slots()[pickSlot];
        if (!self->CanAcquire(pick) && !MakeRoomFor(match, *pick)) return;
        if (match.TryBuyShopUnit(player_, pickSlot) != ActionResult::Ok) return;   // anything unexpected (a closed shop ...) ends the visit
    }
}

const UnitInstance* AIBotController::WeakestSellable(const UnitRoster& roster) const {
    const UnitInstance* weakest = nullptr;
    std::tuple<int, int, int, UnitId> weakestKey{};
    for (const UnitInstance& unit : roster.Units()) {
        if (!unit.champion->IsPooled()) continue;   // plants cannot be sold; special units (the Rift Herald, the Queen) are worth keeping
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

const UnitInstance* AIBotController::SellCandidateFor(const UnitRoster& roster, const ChampionDefinition& offered) const {
    const UnitInstance* weakest = WeakestSellable(roster);
    if (weakest == nullptr || weakest->champion == &offered) return nullptr;
    const int have = weakest->champion->cost;
    const bool worthIt = offered.cost > have || (offered.cost == have && roster.CountOf(&offered, 1) >= 1);
    return worthIt ? weakest : nullptr;
}

bool AIBotController::MakeRoomFor(MatchManager& match, const ChampionDefinition& offered) {
    const PlayerState* self = match.Players().Get(player_);
    const UnitInstance* sell = SellCandidateFor(self->Roster(), offered);
    if (sell == nullptr) return false;
    if (match.TrySellUnit(player_, sell->id) != ActionResult::Ok) return false;
    return self->CanAcquire(&offered);
}

void AIBotController::ArrangeBoard(MatchManager& match) {
    const PlayerState* self = match.Players().Get(player_);
    const UnitRoster& roster = self->Roster();
    const int capacity = std::min(roster.BoardCapacity(), kBoardRows * kBoardColumns);
    std::vector<UnitId> chosen;
    for (const UnitInstance* unit : ChooseBoard(roster, capacity, match.Items(), traits_)) chosen.push_back(unit->id);
    const auto isChosen = [&chosen](UnitId id) { return std::find(chosen.begin(), chosen.end(), id) != chosen.end(); };

    for (UnitId id : chosen) {
        const UnitInstance* unit = roster.Find(id);
        if (unit == nullptr || unit->location == LocationType::Board) continue;
        if (roster.BoardCount() >= capacity) {
            // The board is full: the unit takes the place of the weakest fielded unit that did not make the cut.
            const UnitInstance* out = nullptr;
            for (const UnitInstance& other : roster.Units()) {
                if (other.location != LocationType::Board || isChosen(other.id) || other.champion->plant) continue;
                if (out == nullptr || UnitPower(other) < UnitPower(*out) || (UnitPower(other) == UnitPower(*out) && other.id < out->id)) out = &other;
            }
            if (out != nullptr) match.TryMoveUnit(player_, id, LocationType::Board, out->x, out->y);
            continue;
        }
        const int* rows = unit->champion->role == ChampionRole::Tank ? kFrontFirst : kBackFirst;
        bool placed = false;
        for (int r = 0; r < kBoardRows && !placed; ++r) {
            for (int c = 0; c < kBoardColumns && !placed; ++c) {
                const int y = rows[r];
                const int x = kColumnOrder[c];
                if (roster.BoardAt(x, y) != nullptr) continue;
                placed = match.TryMoveUnit(player_, id, LocationType::Board, x, y) == ActionResult::Ok;
            }
        }
    }
}

void AIBotController::EquipItems(MatchManager& match) {
    const ItemDatabase* items = match.Items();
    const PlayerState* self = match.Players().Get(player_);
    if (items == nullptr) return;

    const std::vector<ItemId> bag = self->ItemBag();   // a copy: equipping (and combining) changes the bag as we go
    for (ItemId id : bag) {
        const ItemDefinition* item = items->Find(id);
        if (item == nullptr || item->IsConsumable()) continue;   // (it keeps its Item Removers: it never takes its own items off)
        const UnitInstance* best = nullptr;
        int bestScore = kNotWanted;
        for (const UnitInstance& unit : self->Roster().Units()) {
            if (unit.location != LocationType::Board || unit.champion->plant) continue;   // only what fights is worth equipping (plants carry nothing)
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
