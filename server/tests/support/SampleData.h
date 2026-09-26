#pragma once

// Test / demo fixtures. NOT part of the server library. Real champion data lives in JSON
// (data/champions.json, loaded through ChampionLoader); this file only
//   * locates the JSON files,
//   * adds 25 generic ability-less filler champions so the shop has something to sell at every cost, and
//   * gives tests stable handles (ids) for the roster champions.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "w2f/ChampionDatabase.h"
#include "w2f/ChampionLoader.h"
#include "w2f/Trait.h"

#ifndef W2F_DATA_DIR
#define W2F_DATA_DIR "data"
#endif
#ifndef W2F_TEST_DATA_DIR
#define W2F_TEST_DATA_DIR "tests/data"
#endif

namespace sample {

using w2f::ChampionDefinition;
using w2f::ChampionRole;

// Ids of the roster champions (same in both JSON files) and of their abilities.
constexpr w2f::ChampionId kChampionAlesk = 9001;
constexpr w2f::ChampionId kChampionBaira = 9002;
constexpr w2f::ChampionId kChampionCyla = 9003;
constexpr w2f::ChampionId kChampionDyno = 9004;
constexpr w2f::ChampionId kChampionFaire = 9005;
constexpr w2f::ChampionId kChampionLes = 9006;
constexpr w2f::ChampionId kChampionLum = 9007;
constexpr w2f::ChampionId kChampionAstra = 9008;
constexpr w2f::ChampionId kChampionLunis = 9009;
constexpr w2f::ChampionId kChampionSoul = 9010;
constexpr w2f::ChampionId kChampionMyna = 9011;
constexpr w2f::ChampionId kChampionVega = 9012;
constexpr w2f::AbilityId kAbilityAesa = 1;
constexpr w2f::AbilityId kAbilityArdeatsDestiny = 2;
constexpr w2f::AbilityId kAbilityRocketStrike = 3;
constexpr w2f::AbilityId kAbilityDynoShield = 4;
constexpr w2f::AbilityId kAbilityExploit = 5;

inline std::string ProductionDataPath() { return std::string(W2F_DATA_DIR) + "/champions.json"; }
// The item tests used to run on a separate expressiveness-proof file; the real data/items.json now carries every item of the design doc.
inline std::string Phase10ItemsPath() { return std::string(W2F_DATA_DIR) + "/items.json"; }
inline std::string ProductionPvePath() { return std::string(W2F_DATA_DIR) + "/pve.json"; }
inline std::string ProductionItemsPath() { return std::string(W2F_DATA_DIR) + "/items.json"; }
inline std::string ProductionMotherNaturePath() { return std::string(W2F_DATA_DIR) + "/mother_nature.json"; }
inline std::string ProductionTextPath() { return std::string(W2F_DATA_DIR) + "/text_en.json"; }
inline std::string ProductionTraitsPath() { return std::string(W2F_DATA_DIR) + "/traits.json"; }
// The designer's numbers, frozen before the balance pass (see the header of tests/data/designer_spec_champions.json): what the mechanics tests use.
inline std::string SpecChampionsPath() { return std::string(W2F_TEST_DATA_DIR) + "/designer_spec_champions.json"; }
inline std::string SpecTraitsPath() { return std::string(W2F_TEST_DATA_DIR) + "/designer_spec_traits.json"; }
inline std::string LegacyRosterPath() { return std::string(W2F_TEST_DATA_DIR) + "/phase4_roster.json"; }
inline std::string OldBairaPath() { return std::string(W2F_TEST_DATA_DIR) + "/baira_until_2026_09.json"; }   // her pre-Sept-2026 design, for the mechanics tests
inline std::string OldCylaPath() { return std::string(W2F_TEST_DATA_DIR) + "/cyla_until_2026_09.json"; }     // her mana rocket, before Fishbones
inline std::string LegacyTraitsPath() { return std::string(W2F_TEST_DATA_DIR) + "/traits_until_2026_09.json"; }   // the synergies before trait system v2

inline ChampionDefinition Def(w2f::ChampionId id, const char* name, int cost) {
    ChampionDefinition d;
    d.id = id;
    d.name = name;
    d.cost = cost;
    return d;
}

// Star scaling: x1.0 / x1.8 / x3.24 (integer percent).
inline std::array<int, w2f::kMaxStarLevel> ByStar(int base) {
    return {{base, base * 180 / 100, base * 324 / 100}};
}

// Loads a JSON roster file into definitions; prints the loader's error and returns {} on failure.
inline std::vector<ChampionDefinition> LoadRosterFile(const std::string& path) {
    std::string error;
    auto db = w2f::LoadChampionDatabaseFromFile(path, &error);
    if (!db) {
        std::printf("LoadRosterFile failed: %s\n", error.c_str());
        return {};
    }
    return db->All();
}

// The production trait / synergy data (data/traits.json). Prints the loader's error and returns nullptr on failure.
// The synergies as they were until September 2026 (the mechanics tests written against them still run on them).
inline std::unique_ptr<w2f::TraitDatabase> LoadLegacyTraits() {
    std::string error;
    auto traits = w2f::LoadTraitDatabaseFromFile(LegacyTraitsPath(), &error);
    if (!traits) std::printf("LoadLegacyTraits failed: %s\n", error.c_str());
    return traits;
}

inline std::unique_ptr<w2f::TraitDatabase> LoadProductionTraits() {
    std::string error;
    auto traits = w2f::LoadTraitDatabaseFromFile(ProductionTraitsPath(), &error);
    if (!traits) std::printf("LoadProductionTraits failed: %s\n", error.c_str());
    return traits;
}

inline std::unique_ptr<w2f::TraitDatabase> LoadSpecTraits() {
    std::string error;
    auto traits = w2f::LoadTraitDatabaseFromFile(SpecTraitsPath(), &error);
    if (!traits) std::printf("LoadSpecTraits failed: %s\n", error.c_str());
    return traits;
}

// The frozen Phase 4 champions (tests/data/phase4_roster.json): what the mechanics tests are written against.
inline ChampionDefinition Legacy(w2f::ChampionId id) {
    static const std::vector<ChampionDefinition> roster = LoadRosterFile(LegacyRosterPath());
    for (const ChampionDefinition& d : roster) {
        if (d.id == id) return d;
    }
    std::printf("Legacy(): no champion %u in %s\n", id, LegacyRosterPath().c_str());
    return ChampionDefinition{};
}

// 25 generic ability-less champions (5 per cost tier, ids cost*100+n) + the CURRENT production roster from
// data/champions.json. Per generic tier: two tanks, two melee damage dealers, one ranged.
inline std::unique_ptr<w2f::ChampionDatabase> MakeCombatDatabase(const std::string& rosterPath = SpecChampionsPath()) {
    std::vector<ChampionDefinition> defs;
    for (int cost = 1; cost <= w2f::kMaxCostTier; ++cost) {
        for (int n = 0; n < 5; ++n) {
            ChampionDefinition d = Def(static_cast<w2f::ChampionId>(cost * 100 + n),
                                       ("T" + std::to_string(cost) + "_" + std::to_string(n)).c_str(), cost);
            if (n < 2) {  // tank
                d.role = ChampionRole::Tank;
                d.stats.maxHp = ByStar(350 + 120 * cost);
                d.stats.armor = ByStar(30 + 6 * cost);
                d.stats.attackDamage = ByStar(28 + 5 * cost);
                d.stats.attackSpeedMilli = 800;
            } else {
                d.role = ChampionRole::Damage;
                d.stats.maxHp = ByStar(220 + 70 * cost);
                d.stats.armor = ByStar(12 + 3 * cost);
                d.stats.attackDamage = ByStar(40 + 12 * cost);
                d.stats.attackSpeedMilli = 900 + 100 * (n - 2);
                d.stats.attackRange = (n == 4) ? 3 : 1;
            }
            defs.push_back(d);
        }
    }
    std::vector<ChampionDefinition> roster = LoadRosterFile(rosterPath);
    if (roster.empty()) return nullptr;   // the loader already printed why
    for (ChampionDefinition& d : roster) defs.push_back(std::move(d));
    std::string error;
    auto db = w2f::ChampionDatabase::Create(std::move(defs), &error);
    if (!db) std::printf("MakeCombatDatabase failed: %s\n", error.c_str());
    return db;
}

}  // namespace sample
