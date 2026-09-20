#pragma once

// Loads champion definitions (champions.json), trait / synergy definitions (traits.json) and items (items.json) from JSON.
// See docs/champions-json.md for the format of both.
//
// Strictness is deliberate, because the file is edited by hand by a designer:
//  * an unknown key is an error (a typo like "magicResit" would otherwise silently become 0);
//  * every error names the exact JSON path and the line / column in the file;
//  * decimals are converted with exact integer arithmetic (attackSpeed 0.78 -> 780), never floats;
//  * the result is then run through ChampionDatabase::Create, so the same validation applies as
//    for data built in code.
// Nothing is partially loaded: on any error you get an error message and no database.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "w2f/ChampionDatabase.h"
#include "w2f/MotherNature.h"
#include "w2f/Pve.h"
#include "w2f/Trait.h"

namespace w2f {

// Parses the document into definitions (no cross-champion validation).
bool ParseChampionsJson(std::string_view text, std::vector<ChampionDefinition>& out, std::string* error = nullptr);

// Parse + validate + build the database. nullptr and *error on failure.
std::unique_ptr<ChampionDatabase> LoadChampionDatabaseFromJson(std::string_view text, std::string* error = nullptr);

// Reads the file and does the same. This is what the server calls on start-up.
std::unique_ptr<ChampionDatabase> LoadChampionDatabaseFromFile(const std::string& path, std::string* error = nullptr);

// ---- traits.json ----
class TraitDatabase;
struct TraitDefinition;

bool ParseTraitsJson(std::string_view text, std::vector<TraitDefinition>& out, std::string* error = nullptr);
std::unique_ptr<TraitDatabase> LoadTraitDatabaseFromJson(std::string_view text, std::string* error = nullptr);
std::unique_ptr<TraitDatabase> LoadTraitDatabaseFromFile(const std::string& path, std::string* error = nullptr);

// ---- items.json ----
class ItemDatabase;
struct ItemDefinition;

bool ParseItemsJson(std::string_view text, std::vector<ItemDefinition>& out, std::string* error = nullptr);
std::unique_ptr<ItemDatabase> LoadItemDatabaseFromJson(std::string_view text, std::string* error = nullptr);
std::unique_ptr<ItemDatabase> LoadItemDatabaseFromFile(const std::string& path, std::string* error = nullptr);

// ---- pve.json ----
// The monsters (same format as champions, cost optional), the encounters that use them, and the default drop table.
struct PveFile {
    std::vector<ChampionDefinition> monsters;
    std::vector<EncounterDefinition> encounters;
    std::vector<PveDropEntry> defaultDrops;
};
bool ParsePveJson(std::string_view text, PveFile& out, std::string* error = nullptr);
// Parse + cross-validate against the real champions (id clashes) and items (drop lists). Either may be null.
std::unique_ptr<EncounterDatabase> LoadEncounterDatabaseFromJson(std::string_view text, const ChampionDatabase* champions,
                                                                  const ItemDatabase* items, std::string* error = nullptr);
std::unique_ptr<EncounterDatabase> LoadEncounterDatabaseFromFile(const std::string& path, const ChampionDatabase* champions,
                                                                  const ItemDatabase* items, std::string* error = nullptr);

// ---- mother_nature.json ----
// { "version": 1, "options": 2, "tiers": [ { "id", "name", "fromStage", "gifts": [ { "id", "name", "type", "weight", ... } ] } ] }
struct MotherNatureFile {
    int options = 2;
    std::vector<MotherNatureTier> tiers;
};
bool ParseMotherNatureJson(std::string_view text, MotherNatureFile& out, std::string* error = nullptr);
// Parse + validate against the item data (`items` may be null: item lists and classes are then not checked).
std::unique_ptr<MotherNatureDatabase> LoadMotherNatureDatabaseFromJson(std::string_view text, const ItemDatabase* items, std::string* error = nullptr);
std::unique_ptr<MotherNatureDatabase> LoadMotherNatureDatabaseFromFile(const std::string& path, const ItemDatabase* items, std::string* error = nullptr);

// Every trait tag an item grants must exist in the trait data (same typo protection as for champions).
class TraitDatabase;
bool ValidateItemTraits(const ItemDatabase& items, const TraitDatabase& traits, std::string* error = nullptr);

}  // namespace w2f
