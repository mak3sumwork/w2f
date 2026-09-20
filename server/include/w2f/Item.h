#pragma once

// Items: data-driven equipment. An item grants FLAT stats (+15 attack damage, +200 HP, ...) and/or trait tags
// ("Coregons Emblem" makes its holder count as a Coregons for synergies). At the start of a fight each unit's items are
// applied as permanent statuses, so they show up in the event stream exactly like passives do.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "w2f/Ability.h"
#include "w2f/Types.h"

namespace w2f {

// All flat, integer, and applied as-is regardless of star level.
struct ItemStats {
    int maxHp = 0;
    int armor = 0;
    int magicResist = 0;
    int attackDamage = 0;
    int abilityDamage = 0;       // adds to the champion's per-star abilityDamage (its "AP") before the ability-power multiplier
    int attackSpeedPercent = 0;  // +% attack speed (adds up with other attack-speed bonuses)
    int critChance = 0;          // + percentage points
    int startMana = 0;           // + mana at the start of the fight (whole mana, same scale as maxMana)
    int manaRegenMilli = 0;      // + mana per second, in thousandths (1000 = "+1 Mana Regen")

    bool IsEmpty() const {
        return maxHp == 0 && armor == 0 && magicResist == 0 && attackDamage == 0 && abilityDamage == 0 &&
               attackSpeedPercent == 0 && critChance == 0 && startMana == 0 && manaRegenMilli == 0;
    }
};

struct ItemDefinition {
    ItemId id = 0;  // non-zero, unique
    std::string name;
    ItemStats stats;
    std::vector<std::string> grantsTraits;  // trait tags the holder gains (must exist in the trait data)

    // Combination: both non-zero = this item is what two BASE components turn into. A unit holding one component that is given the
    // other (in either order) consumes both and gets this item instead -- see ItemDatabase::FindCombination.
    std::array<ItemId, 2> components{};
    // Triggered effects while the item is carried: each is a hook (OnTakeBasicAttackDamage, OnDealDamage, EveryNthAttack, OnCast ...)
    // or a StartOfCombat passive; Mana triggers are not allowed (an item has no mana bar of its own).
    std::vector<AbilityDefinition> abilities;
    // Standing effects on the units around the holder ("enemies within 2 hexes: magic resist -30%").
    std::vector<AuraDefinition> auras;

    bool IsCombined() const { return components[0] != 0 && components[1] != 0; }
    bool HasEffect() const { return !stats.IsEmpty() || !grantsTraits.empty() || !abilities.empty() || !auras.empty(); }
};

class ItemDatabase {
public:
    // Validates ids / names / stat ranges / trait lists. nullptr and *error on failure.
    static std::unique_ptr<ItemDatabase> Create(std::vector<ItemDefinition> definitions, std::string* error = nullptr);

    ItemDatabase(const ItemDatabase&) = delete;
    ItemDatabase& operator=(const ItemDatabase&) = delete;

    const ItemDefinition* Find(ItemId id) const;
    // What `a` and `b` combine into (either order), or nullptr. Only base components combine; a finished item never does.
    const ItemDefinition* FindCombination(ItemId a, ItemId b) const;
    // Is this item one of the two ingredients of some recipe?
    bool IsComponent(ItemId id) const;
    const std::vector<ItemDefinition>& All() const { return definitions_; }  // sorted by id
    // Hash of everything in the database. A snapshot records it to refuse restoring against different data.
    std::uint64_t ContentHash() const;

private:
    explicit ItemDatabase(std::vector<ItemDefinition> definitions) : definitions_(std::move(definitions)) {}
    std::vector<ItemDefinition> definitions_;
};

}  // namespace w2f
