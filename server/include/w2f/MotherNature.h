#pragma once

// Mother Nature: the game's replacement for the carousel and for augments. Every `MatchConfig::motherNatureEveryRounds` rounds (3 by
// default) the round opens with a MotherNature phase INSTEAD of a shop: every living player is offered a few random gifts (2 by default)
// and may take exactly one, for free. The shop stays closed for the whole of that round.
//
// The gifts live in data/mother_nature.json (docs/champions-json.md): tiers by stage, and for each tier a weighted list of gift kinds.
//   Gold    +N gold                      Xp      +N XP (may level the player up)
//   Heal    +N player HP (never above the starting health)
//   Item    one item: a random one of an explicit list, or of a class derived from items.json --
//           Component (a base item that is an ingredient; not the Seed), Legendary (a finished non-emblem item), Emblem, or Any
//   Unit    one champion of one of the listed costs; the copy is checked out of the shared pool while it is on offer, exactly like a
//           shop slot, and goes back if it is not the one picked
// An offer is CONCRETE: the player sees "Omnilium Heart" or "Soul, 2-cost", not "an item".

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "w2f/ChampionDatabase.h"
#include "w2f/Item.h"
#include "w2f/Types.h"

namespace w2f {

enum class GiftType : std::uint8_t { Gold, Xp, Heal, Item, Unit };
enum class ItemClass : std::uint8_t { Any, Component, Legendary, Emblem };

constexpr const char* ToString(GiftType t) {
    switch (t) {
        case GiftType::Gold: return "gold";
        case GiftType::Xp: return "xp";
        case GiftType::Heal: return "heal";
        case GiftType::Item: return "item";
        case GiftType::Unit: return "unit";
    }
    return "?";
}

// A Unit gift whose cost tiers depend on the stage: from `fromStage` on (until a later entry takes over) the unit is drawn from one of `costs`.
struct StageCosts {
    int fromStage = 1;
    std::vector<int> costs;
};

struct GiftDefinition {
    std::uint32_t id = 0;        // > 0, unique across the whole file; offers refer to it
    std::string name;            // "Mother's Blessing"
    GiftType type = GiftType::Gold;
    int weight = 1;              // > 0: relative chance among the tier's gifts
    int amount = 0;              // Gold / Xp / Heal: how much
    ItemClass itemClass = ItemClass::Any;   // Item without an explicit `items` list
    std::vector<ItemId> items;   // Item: explicit candidates (overrides itemClass)
    std::vector<int> costs;      // Unit: the cost tiers it may come from (the same at every stage) ...
    std::vector<StageCosts> costsByStage;   // ... or, instead, tiers that SCALE with the stage: the entry with the largest fromStage <= the stage applies

    // The cost tiers a Unit gift draws from at `stage`.
    const std::vector<int>& CostsAt(int stage) const {
        const std::vector<int>* best = &costs;
        for (const StageCosts& entry : costsByStage) {
            if (entry.fromStage <= stage) best = &entry.costs;
        }
        return *best;
    }
};

struct MotherNatureTier {
    int id = 0;                  // > 0, unique; the designer's number ("Tier 1", "Tier 3": gaps are allowed)
    std::string name;
    int fromStage = 1;           // used from this stage on, until a tier with a larger fromStage takes over
    std::vector<GiftDefinition> gifts;
};

// One concrete option a player is offered (or has been given).
struct GiftOffer {
    std::uint32_t gift = 0;                        // the GiftDefinition it came from
    GiftType type = GiftType::Gold;
    int amount = 0;                                // Gold / Xp / Heal
    ItemId item = 0;                               // Item
    const ChampionDefinition* champion = nullptr;  // Unit: checked out of the pool while on offer
};

class MotherNatureDatabase {
public:
    // Validates everything: unique tier / gift ids, the first tier starts at stage 1, positive weights and amounts, item ids known to `items`
    // and every item class non-empty (when `items` is given), unit costs 1..5. `options` is how many gifts a player is shown (1..4).
    static std::unique_ptr<MotherNatureDatabase> Create(int options, std::vector<MotherNatureTier> tiers, const ItemDatabase* items,
                                                        std::string* error = nullptr);

    MotherNatureDatabase(const MotherNatureDatabase&) = delete;
    MotherNatureDatabase& operator=(const MotherNatureDatabase&) = delete;

    int Options() const { return options_; }
    const std::vector<MotherNatureTier>& Tiers() const { return tiers_; }   // ascending fromStage
    // The tier in force at `stage` (never null: the first tier starts at stage 1).
    const MotherNatureTier& TierFor(int stage) const;
    const GiftDefinition* FindGift(std::uint32_t id) const;
    std::uint64_t ContentHash() const;

private:
    MotherNatureDatabase() = default;
    int options_ = 2;
    std::vector<MotherNatureTier> tiers_;
};

// The items a gift may hand out: its explicit list, else every item of its class in `items` (ascending id).
std::vector<ItemId> GiftItemChoices(const GiftDefinition& gift, const ItemDatabase& items);
// The class an item belongs to. Components are base items that are an ingredient of a recipe and do something (the Omnilium Seed does not);
// Legendary = a finished item that grants no trait; Emblem = a finished item that grants a trait. Anything else (a base item that is no
// ingredient) counts only as `Any`.
bool ItemIsOfClass(const ItemDatabase& items, const ItemDefinition& item, ItemClass cls);

}  // namespace w2f
