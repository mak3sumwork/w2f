#pragma once

// PvE rounds: every player fights the same preset board of monsters instead of another player. A win pays out one random
// drop (gold, a champion from the shared pool, or an item). The data lives in pve.json (docs/champions-json.md).
//
// Monsters are ordinary ChampionDefinitions (same stats / abilities format) kept in their OWN database: they are never in
// the shop pool, never owned by a player, and their ids must not collide with a real champion's.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "w2f/ChampionDatabase.h"
#include "w2f/Combat.h"
#include "w2f/Item.h"

namespace w2f {

// Monster units get ids kMonsterUnitBase + 1, + 2, ... (players' ids have a high byte of 1..8, so these can never clash).
constexpr UnitId kMonsterUnitBase = 0xF0000000u;

// Where a monster stands, in the same board coordinates a player uses for THEIR board (x 0..6; y 0..3, y = 3 is the front
// row, nearest the player) -- the fight mirrors it to the far side of the arena exactly like an opponent's board.
struct MonsterPlacement {
    ChampionId monster = kInvalidChampionId;
    int starLevel = 1;
    int x = 0;
    int y = 0;
};

// One line of a drop table. The MatchManager picks a line by weight (lines that cannot pay out right now -- no items
// loaded, every listed tier sold out -- are skipped) and rolls its details.
struct PveDropEntry {
    PveDropType type = PveDropType::Gold;   // Gold, Champion or Item
    int weight = 1;                         // > 0
    int minGold = 0;                        // Gold: the amount is uniform in [minGold, maxGold]
    int maxGold = 0;
    std::vector<int> tiers;                 // Champion: cost tiers it may come from (drawn from the shared pool)
    std::vector<ItemId> items;              // Item: which items (empty = any item in items.json)
};

struct EncounterDefinition {
    std::uint32_t id = 0;   // > 0, unique; appears in the Matchup
    std::string name;
    // Which rounds it is used for. 0 = any. (stage 1, round 2) is round 1-2; (stage 0, round 7) is X-7 of every stage.
    int stage = 0;
    int round = 0;
    std::vector<MonsterPlacement> units;
    std::vector<PveDropEntry> drops;   // empty = the file's default drops
};

class EncounterDatabase {
public:
    // Validates everything: unique ids, monsters defined and disjoint from `champions`, cells in range and unique, drop tables
    // sensible (tiers 1..5, item ids known to `items`), at least one drop table for every encounter. `champions` and `items` are
    // only used for those checks (either may be null: item lists are then rejected and id clashes go unchecked).
    static std::unique_ptr<EncounterDatabase> Create(std::vector<ChampionDefinition> monsters, std::vector<EncounterDefinition> encounters,
                                                     std::vector<PveDropEntry> defaultDrops, const ChampionDatabase* champions,
                                                     const ItemDatabase* items, std::string* error = nullptr);

    EncounterDatabase(const EncounterDatabase&) = delete;
    EncounterDatabase& operator=(const EncounterDatabase&) = delete;

    const ChampionDatabase& Monsters() const { return *monsters_; }
    const EncounterDefinition* Find(std::uint32_t id) const;
    const std::vector<EncounterDefinition>& All() const { return encounters_; }   // sorted by id
    const std::vector<PveDropEntry>& DefaultDrops() const { return defaultDrops_; }
    const std::vector<PveDropEntry>& DropsFor(const EncounterDefinition& e) const { return e.drops.empty() ? defaultDrops_ : e.drops; }

    // The encounter for round `roundInStage` of `stage`: among those that match, the most specific (exact stage and round beats
    // exact stage beats exact round beats "any"); if several tie, `roll` picks one. nullptr if none match.
    const EncounterDefinition* Select(int stage, int roundInStage, std::uint32_t roll) const;

    // Everything a snapshot / desync check needs to know the PvE data has not changed.
    std::uint64_t ContentHash() const;

private:
    EncounterDatabase() = default;
    std::unique_ptr<ChampionDatabase> monsters_;
    std::vector<EncounterDefinition> encounters_;
    std::vector<PveDropEntry> defaultDrops_;
};

}  // namespace w2f
