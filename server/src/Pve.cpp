#include "w2f/Pve.h"

#include <algorithm>
#include <set>

#include "w2f/Hash.h"

namespace w2f {

namespace {

bool ValidateDrops(const std::vector<PveDropEntry>& drops, const std::string& where, const ItemDatabase* items, std::string* error) {
    const auto fail = [&](const std::string& message) {
        if (error) *error = where + ": " + message;
        return false;
    };
    for (const PveDropEntry& d : drops) {
        if (d.weight <= 0) return fail("a drop needs a weight > 0");
        switch (d.type) {
            case PveDropType::Gold:
                if (d.minGold < 0 || d.maxGold < d.minGold) return fail("a gold drop needs 0 <= minGold <= maxGold");
                if (d.maxGold > 1000) return fail("a gold drop above 1000 gold looks like a typo");
                break;
            case PveDropType::Champion:
                if (d.tiers.empty()) return fail("a champion drop needs at least one tier");
                for (int tier : d.tiers) {
                    if (tier < 1 || tier > kMaxCostTier) return fail("a champion drop's tier must be 1.." + std::to_string(kMaxCostTier));
                }
                break;
            case PveDropType::Item:
                for (ItemId item : d.items) {
                    if (items == nullptr || items->Find(item) == nullptr) return fail("an item drop lists item " + std::to_string(item) + ", which is not in the item data");
                }
                break;
            case PveDropType::None: return fail("a drop cannot have the type None");
        }
    }
    return true;
}

}  // namespace

std::unique_ptr<EncounterDatabase> EncounterDatabase::Create(std::vector<ChampionDefinition> monsters, std::vector<EncounterDefinition> encounters,
                                                             std::vector<PveDropEntry> defaultDrops, const ChampionDatabase* champions,
                                                             const ItemDatabase* items, std::string* error) {
    const auto fail = [error](const std::string& message) -> std::unique_ptr<EncounterDatabase> {
        if (error) *error = message;
        return nullptr;
    };
    for (const ChampionDefinition& m : monsters) {
        if (champions != nullptr && champions->Find(m.id) != nullptr) return fail("monster '" + m.name + "' has the same id as a champion");
        if (!m.stats.IsCombatCapable()) return fail("monster '" + m.name + "' has no combat stats");
        if (!m.traits.empty()) return fail("monster '" + m.name + "' lists traits: monsters take part in no synergy");
    }
    std::string inner;
    auto monsterDb = ChampionDatabase::Create(std::move(monsters), &inner);
    if (!monsterDb) return fail("monsters: " + inner);

    std::sort(encounters.begin(), encounters.end(), [](const EncounterDefinition& a, const EncounterDefinition& b) { return a.id < b.id; });
    if (!ValidateDrops(defaultDrops, "default drops", items, &inner)) return fail(inner);
    for (std::size_t i = 0; i < encounters.size(); ++i) {
        const EncounterDefinition& e = encounters[i];
        const std::string where = "encounter '" + e.name + "'";
        if (e.id == 0) return fail(where + " has reserved id 0");
        if (i > 0 && encounters[i - 1].id == e.id) return fail(where + " reuses an encounter id");
        if (e.name.empty()) return fail("an encounter has an empty name");
        if (e.stage < 0 || e.round < 0) return fail(where + ": stage and round must be >= 0 (0 = any)");
        if (e.units.empty()) return fail(where + " has no monsters");
        std::set<std::pair<int, int>> cells;
        for (const MonsterPlacement& u : e.units) {
            if (monsterDb->Find(u.monster) == nullptr) return fail(where + " uses monster " + std::to_string(u.monster) + ", which is not defined");
            if (u.starLevel < 1 || u.starLevel > kMaxStarLevel) return fail(where + ": star level must be 1.." + std::to_string(kMaxStarLevel));
            if (u.x < 0 || u.x >= kBoardColumns || u.y < 0 || u.y >= kBoardRows) return fail(where + ": a monster stands outside the board (x 0.." + std::to_string(kBoardColumns - 1) + ", y 0.." + std::to_string(kBoardRows - 1) + ")");
            if (!cells.insert({u.x, u.y}).second) return fail(where + ": two monsters on the same cell");
        }
        if (e.drops.empty() && defaultDrops.empty()) return fail(where + " has no drops and there are no default drops");
        if (!ValidateDrops(e.drops, where, items, &inner)) return fail(inner);
    }
    std::unique_ptr<EncounterDatabase> db(new EncounterDatabase());
    db->monsters_ = std::move(monsterDb);
    db->encounters_ = std::move(encounters);
    db->defaultDrops_ = std::move(defaultDrops);
    return db;
}

const EncounterDefinition* EncounterDatabase::Find(std::uint32_t id) const {
    for (const EncounterDefinition& e : encounters_) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

const EncounterDefinition* EncounterDatabase::Select(int stage, int roundInStage, std::uint32_t roll) const {
    int bestScore = -1;
    std::vector<const EncounterDefinition*> best;
    for (const EncounterDefinition& e : encounters_) {
        if ((e.stage != 0 && e.stage != stage) || (e.round != 0 && e.round != roundInStage)) continue;
        // Exact stage beats exact round, and both beat neither.
        const int score = (e.stage != 0 ? 2 : 0) + (e.round != 0 ? 1 : 0);
        if (score > bestScore) {
            bestScore = score;
            best.clear();
        }
        if (score == bestScore) best.push_back(&e);
    }
    return best.empty() ? nullptr : best[roll % best.size()];
}

std::uint64_t EncounterDatabase::ContentHash() const {
    Fnv1a h;
    h.Add(monsters_->ContentHash());
    const auto addDrops = [&h](const std::vector<PveDropEntry>& drops) {
        h.AddInt(static_cast<std::int64_t>(drops.size()));
        for (const PveDropEntry& d : drops) {
            h.Add(static_cast<std::uint64_t>(d.type));
            h.AddInt(d.weight);
            h.AddInt(d.minGold);
            h.AddInt(d.maxGold);
            h.AddInt(static_cast<std::int64_t>(d.tiers.size()));
            for (int t : d.tiers) h.AddInt(t);
            h.AddInt(static_cast<std::int64_t>(d.items.size()));
            for (ItemId i : d.items) h.Add(i);
        }
    };
    h.AddInt(static_cast<std::int64_t>(encounters_.size()));
    for (const EncounterDefinition& e : encounters_) {
        h.Add(e.id);
        h.AddString(e.name);
        h.AddInt(e.stage);
        h.AddInt(e.round);
        h.AddInt(static_cast<std::int64_t>(e.units.size()));
        for (const MonsterPlacement& u : e.units) {
            h.Add(u.monster);
            h.AddInt(u.starLevel);
            h.AddInt(u.x);
            h.AddInt(u.y);
        }
        addDrops(e.drops);
    }
    addDrops(defaultDrops_);
    return h.value;
}

}  // namespace w2f
