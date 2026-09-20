#include "w2f/Trait.h"

#include <algorithm>

#include "w2f/Item.h"
#include <variant>

namespace w2f {

std::unique_ptr<TraitDatabase> TraitDatabase::Create(std::vector<TraitDefinition> definitions, std::string* error) {
    auto fail = [error](const std::string& message) -> std::unique_ptr<TraitDatabase> {
        if (error) *error = message;
        return nullptr;
    };
    std::sort(definitions.begin(), definitions.end(),
              [](const TraitDefinition& a, const TraitDefinition& b) { return a.id < b.id; });

    for (std::size_t i = 0; i < definitions.size(); ++i) {
        const TraitDefinition& trait = definitions[i];
        if (trait.id == 0) return fail("trait '" + trait.name + "' has reserved id 0");
        if (trait.name.empty()) return fail("a trait has an empty name");
        if (i > 0 && definitions[i - 1].id == trait.id) return fail("duplicate trait id for '" + trait.name + "'");
        for (std::size_t j = 0; j < i; ++j) {
            if (definitions[j].name == trait.name) return fail("duplicate trait name '" + trait.name + "'");
        }

        int lastCount = 0;
        for (const TraitBreakpoint& bp : trait.breakpoints) {
            if (bp.count <= lastCount) return fail("trait '" + trait.name + "': breakpoint counts must be >= 1 and strictly ascending");
            lastCount = bp.count;
            if (bp.effects.empty() && bp.triggers.empty()) return fail("trait '" + trait.name + "': a breakpoint with no effects");

            // Reuse the ability validation for the payload details.
            AbilityDefinition wrapper;
            wrapper.id = 1;
            wrapper.name = "trait '" + trait.name + "'";
            wrapper.trigger = CastTrigger::StartOfCombat;
            for (const TraitEffect& te : bp.effects) {
                const bool allowed = std::holds_alternative<StatusEffect>(te.effect.payload) ||
                                     std::holds_alternative<ShieldEffect>(te.effect.payload) ||
                                     std::holds_alternative<HealEffect>(te.effect.payload) ||
                                     std::holds_alternative<SummonEffect>(te.effect.payload) ||
                                     std::holds_alternative<ManaEffect>(te.effect.payload);
                if (!allowed) return fail("trait '" + trait.name + "': synergy effects may only be Status, Shield, Heal, Mana or Summon");
                if (te.effect.target.mode != TargetMode::Self) {
                    return fail("trait '" + trait.name + "': synergy effects apply to each unit itself (target Self); use \"scope\" to pick who");
                }
                if ((te.scope == TraitScope::Team) != std::holds_alternative<SummonEffect>(te.effect.payload) &&
                    te.scope == TraitScope::Team) {
                    return fail("trait '" + trait.name + "': scope Team only applies to Summon effects");
                }
                wrapper.effects.push_back(te.effect);
            }
            std::string effectError;
            if (!wrapper.effects.empty() && !ValidateAbility(wrapper, 0, &effectError)) return fail(effectError);
            for (const TraitTrigger& tt : bp.triggers) {
                if (tt.scope == TraitScope::Team) return fail("trait '" + trait.name + "': a trigger's scope is AllAllies or TraitHolders");
                if (!tt.ability.HasAbility()) return fail("trait '" + trait.name + "': a trigger without an id");
                if (!IsHook(tt.ability.trigger)) return fail("trait '" + trait.name + "': synergy triggers must be hooks (OnDealDamage, OnAllyDealDamage, ...)");
                if (!ValidateAbility(tt.ability, 0, &effectError)) return fail("trait '" + trait.name + "': " + effectError);
            }
        }
    }
    return std::unique_ptr<TraitDatabase>(new TraitDatabase(std::move(definitions)));
}

const TraitDefinition* TraitDatabase::FindByName(const std::string& name) const {
    for (const TraitDefinition& t : definitions_) {
        if (t.name == name) return &t;
    }
    return nullptr;
}

const TraitDefinition* TraitDatabase::FindById(TraitId id) const {
    for (const TraitDefinition& t : definitions_) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

bool ValidateChampionTraits(const ChampionDatabase& champions, const TraitDatabase& traits, std::string* error) {
    for (const ChampionDefinition& champion : champions.All()) {
        for (const std::string& name : champion.traits) {
            if (traits.FindByName(name) == nullptr) {
                if (error) *error = "champion '" + champion.name + "' has trait '" + name + "', which is not defined in the trait data";
                return false;
            }
        }
    }
    return true;
}

bool ValidateItemTraits(const ItemDatabase& items, const TraitDatabase& traits, std::string* error) {
    for (const ItemDefinition& item : items.All()) {
        for (const std::string& name : item.grantsTraits) {
            if (traits.FindByName(name) == nullptr) {
                if (error) *error = "item '" + item.name + "' grants trait '" + name + "', which is not defined in the trait data";
                return false;
            }
        }
    }
    return true;
}

bool ValidateSummonReferences(const ChampionDatabase& champions, const ItemDatabase* items, const TraitDatabase* traits, std::string* error) {
    const auto check = [&](const AbilityDefinition& a, const std::string& owner, std::string& why) {
        for (const AbilityEffect& e : a.effects) {
            const auto* summon = std::get_if<SummonEffect>(&e.payload);
            if (summon == nullptr) continue;
            const ChampionDefinition* c = champions.Find(summon->champion);
            if (c == nullptr || !c->summon) {
                why = owner + " summons champion " + std::to_string(summon->champion) + ", which does not exist or is not marked \"summon\"";
                return false;
            }
        }
        return true;
    };
    std::string why;
    if (items != nullptr) {
        for (const ItemDefinition& item : items->All()) {
            for (const AbilityDefinition& a : item.abilities) {
                if (!check(a, "item '" + item.name + "'", why)) { if (error) *error = why; return false; }
            }
        }
    }
    if (traits != nullptr) {
        for (const TraitDefinition& trait : traits->All()) {
            for (const TraitBreakpoint& bp : trait.breakpoints) {
                AbilityDefinition wrapper;
                for (const TraitEffect& te : bp.effects) wrapper.effects.push_back(te.effect);
                if (!check(wrapper, "trait '" + trait.name + "'", why)) { if (error) *error = why; return false; }
                for (const TraitTrigger& tt : bp.triggers) {
                    if (!check(tt.ability, "trait '" + trait.name + "'", why)) { if (error) *error = why; return false; }
                }
            }
        }
    }
    return true;
}

}  // namespace w2f
