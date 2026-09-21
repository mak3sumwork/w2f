#include "w2f/Item.h"

#include <algorithm>
#include <set>

#include "w2f/Hash.h"
#include "w2f/Hex.h"

namespace w2f {

std::unique_ptr<ItemDatabase> ItemDatabase::Create(std::vector<ItemDefinition> definitions, std::string* error) {
    auto fail = [error](const std::string& message) -> std::unique_ptr<ItemDatabase> {
        if (error) *error = message;
        return nullptr;
    };
    std::sort(definitions.begin(), definitions.end(), [](const ItemDefinition& a, const ItemDefinition& b) { return a.id < b.id; });
    const auto find = [&definitions](ItemId id) -> const ItemDefinition* {
        for (const ItemDefinition& d : definitions) {
            if (d.id == id) return &d;
        }
        return nullptr;
    };
    std::set<ItemId> ingredients;
    std::set<std::pair<ItemId, ItemId>> recipes;
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        const ItemDefinition& item = definitions[i];
        if (item.id == 0) return fail("item '" + item.name + "' has reserved id 0");
        if (item.name.empty()) return fail("an item has an empty name");
        if (i > 0 && definitions[i - 1].id == item.id) return fail("duplicate item id for '" + item.name + "'");
        const ItemStats& s = item.stats;
        if (s.maxHp < -100000 || s.maxHp > 100000 || s.armor < -10000 || s.armor > 10000 || s.magicResist < -10000 || s.magicResist > 10000 ||
            s.attackDamage < -10000 || s.attackDamage > 10000 || s.abilityDamage < -10000 || s.abilityDamage > 10000 ||
            s.attackSpeedPercent < -90 || s.attackSpeedPercent > 1000 || s.critChance < -100 || s.critChance > 100 ||
            s.startMana < 0 || s.startMana > 1000 || s.manaRegenMilli < -10000 || s.manaRegenMilli > 20000) {
            return fail("item '" + item.name + "' has a stat out of range");
        }
        for (std::size_t t = 0; t < item.grantsTraits.size(); ++t) {
            if (item.grantsTraits[t].empty()) return fail("item '" + item.name + "' grants an empty trait name");
            for (std::size_t u = 0; u < t; ++u) {
                if (item.grantsTraits[u] == item.grantsTraits[t]) return fail("item '" + item.name + "' lists trait '" + item.grantsTraits[t] + "' twice");
            }
        }
        if ((item.components[0] == 0) != (item.components[1] == 0)) return fail("item '" + item.name + "' needs two components (or none)");
        if (item.IsConsumable() && (item.IsCombined() || !item.stats.IsEmpty() || !item.grantsTraits.empty() || !item.abilities.empty() || !item.auras.empty())) {
            return fail("item '" + item.name + "' is a consumable: it cannot be crafted and has no stats, traits, abilities or auras");
        }
        if (item.IsCombined()) {
            for (ItemId c : item.components) {
                if (c == item.id) return fail("item '" + item.name + "' is made from itself");
                const ItemDefinition* component = find(c);
                if (component == nullptr) return fail("item '" + item.name + "' is made from item " + std::to_string(c) + ", which does not exist");
                if (component->IsCombined()) return fail("item '" + item.name + "' is made from '" + component->name + "', which is itself a combination: only base components combine");
                ingredients.insert(c);
            }
            const auto key = std::minmax(item.components[0], item.components[1]);
            if (!recipes.insert({key.first, key.second}).second) return fail("item '" + item.name + "': another item is already made from the same two components");
        }
        for (const AbilityDefinition& ability : item.abilities) {
            if (!ability.HasAbility()) return fail("item '" + item.name + "' has an ability without an id");
            std::string abilityError;
            if (!ValidateAbility(ability, 0, &abilityError)) return fail("item '" + item.name + "': " + abilityError);
        }
        for (const AuraDefinition& aura : item.auras) {
            if (aura.radius < 1 || aura.radius > kArenaColumns + kArenaRows) return fail("item '" + item.name + "': an aura radius must be 1..15 hexes");
            if (aura.amount == 0) return fail("item '" + item.name + "': an aura with amount 0 does nothing");
            if (aura.status == StatusType::Stun || aura.status == StatusType::Knockup || aura.status == StatusType::Burn || aura.status == StatusType::Tether) {
                return fail("item '" + item.name + "': an aura cannot apply Stun, Knockup, Burn or Tether");
            }
            if (aura.includeSelf && aura.side != TargetSide::Allies) return fail("item '" + item.name + "': includeSelf only applies to an Allies aura");
        }
    }
    for (const ItemDefinition& item : definitions) {
        // A plain component (an Omnilium Seed) may have no effect of its own -- but only if some recipe uses it.
        if (!item.HasEffect() && !item.IsCombined() && !item.IsConsumable() && ingredients.count(item.id) == 0) {
            return fail("item '" + item.name + "' does nothing (no stats, traits, abilities or auras) and is not used in any recipe");
        }
    }
    return std::unique_ptr<ItemDatabase>(new ItemDatabase(std::move(definitions)));
}

const ItemDefinition* ItemDatabase::Find(ItemId id) const {
    auto it = std::lower_bound(definitions_.begin(), definitions_.end(), id, [](const ItemDefinition& d, ItemId v) { return d.id < v; });
    return (it == definitions_.end() || it->id != id) ? nullptr : &*it;
}

const ItemDefinition* ItemDatabase::FindCombination(ItemId a, ItemId b) const {
    if (a == 0 || b == 0) return nullptr;
    for (const ItemDefinition& d : definitions_) {
        if (!d.IsCombined()) continue;
        if ((d.components[0] == a && d.components[1] == b) || (d.components[0] == b && d.components[1] == a)) return &d;
    }
    return nullptr;
}

bool ItemDatabase::IsComponent(ItemId id) const {
    for (const ItemDefinition& d : definitions_) {
        if (d.IsCombined() && (d.components[0] == id || d.components[1] == id)) return true;
    }
    return false;
}

std::uint64_t ItemDatabase::ContentHash() const {
    Fnv1a h;
    h.AddInt(static_cast<std::int64_t>(definitions_.size()));
    for (const ItemDefinition& d : definitions_) {
        h.Add(d.id);
        h.AddString(d.name);
        const ItemStats& s = d.stats;
        for (int v : {s.maxHp, s.armor, s.magicResist, s.attackDamage, s.abilityDamage, s.attackSpeedPercent, s.critChance, s.startMana, s.manaRegenMilli}) h.AddInt(v);
        h.Add(d.components[0]);
        h.Add(d.components[1]);
        h.Add(static_cast<std::uint64_t>(d.use));
        h.AddInt(static_cast<std::int64_t>(d.abilities.size()));
        for (const AbilityDefinition& a : d.abilities) HashAbility(h, a);
        h.AddInt(static_cast<std::int64_t>(d.auras.size()));
        for (const AuraDefinition& a : d.auras) {
            h.Add(static_cast<std::uint64_t>(a.side));
            h.AddInt(a.radius);
            h.Add(static_cast<std::uint64_t>(a.status));
            h.AddInt(a.amount);
            h.Add(a.includeSelf ? 1 : 0);
        }
        h.AddInt(static_cast<std::int64_t>(d.grantsTraits.size()));
        for (const std::string& t : d.grantsTraits) h.AddString(t);
    }
    return h.value;
}

}  // namespace w2f
