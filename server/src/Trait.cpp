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

        if (!trait.paths.empty()) {
            if (!trait.breakpoints.empty()) return fail("trait '" + trait.name + "': give either \"breakpoints\" or \"paths\"");
            for (const TraitPath& path : trait.paths) {
                if (path.name.empty()) return fail("trait '" + trait.name + "': a path without a name");
                if (path.breakpoints.size() != trait.paths[0].breakpoints.size()) return fail("trait '" + trait.name + "': every path has the same breakpoints");
                for (std::size_t b = 0; b < path.breakpoints.size(); ++b) {
                    if (path.breakpoints[b].count != trait.paths[0].breakpoints[b].count) return fail("trait '" + trait.name + "': every path has the same breakpoint counts");
                }
            }
        }
        std::string moduleError;
        for (const TraitModule& m : trait.modules) {
            if (m.id == 0 || m.name.empty() || m.tier < 1 || m.tier > 3) return fail("trait '" + trait.name + "': a module needs an id, a name and a tier 1..3");
            if (m.ability.HasAbility() && !ValidateAbility(m.ability, 0, &moduleError)) return fail("trait '" + trait.name + "' module '" + m.name + "': " + moduleError);
            if (m.echo && (m.echoEveryTicks < 30 || m.echoEveryTicks > 3600)) return fail("trait '" + trait.name + "': an echo module repeats every 1..120 s");
            if (m.goldAfterCombat < 0 || m.goldAfterCombat > 50) return fail("trait '" + trait.name + "': goldAfterCombat must be 0..50");
            for (const TraitModule& other : trait.modules) {
                if (&other != &m && other.id == m.id) return fail("trait '" + trait.name + "': duplicate module id");
            }
        }
        if (!trait.modules.empty() && trait.invention == 0) return fail("trait '" + trait.name + "': modules need an \"invention\" summon to carry them");
        for (const TraitMutation& mutation : trait.mutations) {
            AbilityDefinition wrapper;
            wrapper.id = 1;
            wrapper.name = "trait '" + trait.name + "' mutation '" + mutation.name + "'";
            wrapper.trigger = CastTrigger::StartOfCombat;
            for (const auto* list : {&mutation.effects, &mutation.superEffects}) {
                wrapper.effects = *list;
                if (!wrapper.effects.empty() && !ValidateAbility(wrapper, 0, &moduleError)) return fail(moduleError);
                for (const AbilityEffect& e : *list) {
                    if (e.target.mode != TargetMode::Self) return fail(wrapper.name + ": mutation effects target Self");
                }
            }
            for (const auto* list : {&mutation.triggers, &mutation.superTriggers}) {
                for (const AbilityDefinition& hook : *list) {
                    if (!hook.HasAbility() || !IsHook(hook.trigger)) return fail(wrapper.name + ": mutation triggers are hooks with an id");
                    if (!ValidateAbility(hook, 0, &moduleError)) return fail(wrapper.name + ": " + moduleError);
                }
            }
        }

        std::vector<const std::vector<TraitBreakpoint>*> lists;
        if (trait.paths.empty()) lists.push_back(&trait.breakpoints);
        for (const TraitPath& path : trait.paths) lists.push_back(&path.breakpoints);
        for (const std::vector<TraitBreakpoint>* list : lists) {
        int lastCount = 0;
        for (const TraitBreakpoint& bp : *list) {
            if (bp.count <= lastCount) return fail("trait '" + trait.name + "': breakpoint counts must be >= 1 and strictly ascending");
            lastCount = bp.count;
            const bool economic = bp.xpAfterCombat != 0 || bp.takedownsPerGold != 0 || bp.starDust.Any() || bp.grantUnit.champion != 0 ||
                                  !bp.plants.empty() || bp.moduleTier != 0 || bp.mutationSlots != 0;
            if (bp.effects.empty() && bp.triggers.empty() && !economic) return fail("trait '" + trait.name + "': a breakpoint with no effects");
            if (bp.mutationSlots < -1 || bp.mutationSlots > 20 || (bp.mutationSlots != 0 && trait.mutations.empty())) {
                return fail("trait '" + trait.name + "': mutationSlots (-1 = all, 0..20) need \"mutations\"");
            }
            if (bp.xpAfterCombat < 0 || bp.xpAfterCombat > 100 || bp.takedownsPerGold < 0 || bp.takedownsPerGold > 100) {
                return fail("trait '" + trait.name + "': xpAfterCombat / takedownsPerGold out of range");
            }
            if (bp.starDust.multiplier < 1 || bp.starDust.multiplier > 10 || bp.starDust.onLoss < 0 || bp.starDust.perLossStreak < 0 || bp.starDust.perTakedown < 0 || bp.starDust.perCombat < 0) {
                return fail("trait '" + trait.name + "': invalid starDust");
            }
            if (bp.grantUnit.champion != 0 && (bp.grantUnit.afterCombats < 0 || bp.grantUnit.afterCombats > 50)) return fail("trait '" + trait.name + "': invalid grantUnit");
            if (bp.plantStar < 0 || bp.plantStar > kMaxStarLevel) return fail("trait '" + trait.name + "': plantStar must be 0..3");
            for (const PlantGrant& g : bp.plants) {
                if (g.champion == 0 || g.count < 1 || g.count > 4) return fail("trait '" + trait.name + "': a plant grant needs a champion and a count of 1..4");
            }
            if (bp.moduleTier < 0 || bp.moduleTier > 3 || (bp.moduleTier != 0 && trait.modules.empty())) return fail("trait '" + trait.name + "': moduleTier 1..3 needs \"modules\"");

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
                if (te.scope == TraitScope::Team) {
                    // Applied once, cast by the lowest-UnitId holder, so the effect may aim itself at a whole side.
                    const TargetMode mode = te.effect.target.mode;
                    if (mode != TargetMode::Self && mode != TargetMode::AllEnemies && mode != TargetMode::AllAllies) {
                        return fail("trait '" + trait.name + "': a scope-Team effect targets Self, AllEnemies or AllAllies");
                    }
                    if (std::holds_alternative<SummonEffect>(te.effect.payload) && mode != TargetMode::Self) {
                        return fail("trait '" + trait.name + "': a Summon effect targets Self");
                    }
                } else if (te.effect.target.mode != TargetMode::Self) {
                    return fail("trait '" + trait.name + "': synergy effects apply to each unit itself (target Self); use \"scope\" to pick who");
                }
                wrapper.effects.push_back(te.effect);
            }
            std::string effectError;
            if (!wrapper.effects.empty() && !ValidateAbility(wrapper, 0, &effectError)) return fail(effectError);
            for (const TraitTrigger& tt : bp.triggers) {
                if (!tt.ability.HasAbility()) return fail("trait '" + trait.name + "': a trigger without an id");
                if (tt.scope == TraitScope::Team && tt.ability.trigger != CastTrigger::OnTeamHpLoss) {
                    return fail("trait '" + trait.name + "': a scope-Team trigger (once for the whole team) must use OnTeamHpLoss; otherwise use AllAllies or TraitHolders");
                }
                if (!IsHook(tt.ability.trigger)) return fail("trait '" + trait.name + "': synergy triggers must be hooks (OnDealDamage, OnAllyDealDamage, ...)");
                if (!ValidateAbility(tt.ability, 0, &effectError)) return fail("trait '" + trait.name + "': " + effectError);
            }
        }
        }
    }
    return std::unique_ptr<TraitDatabase>(new TraitDatabase(std::move(definitions)));
}

int ActiveTier(const std::vector<TraitBreakpoint>& breakpoints, int count, int emblemHolders) {
    int tier = 0;
    for (std::size_t i = 0; i < breakpoints.size(); ++i) {
        if (count >= breakpoints[i].count) tier = static_cast<int>(i) + 1;
    }
    const bool prismatic = breakpoints.size() >= kPrismaticMinBreakpoints && tier == static_cast<int>(breakpoints.size());
    if (prismatic && emblemHolders <= 0) --tier;   // the prismatic tier needs an emblem
    return tier;
}

bool UnitHasTrait(const TraitCountUnit& unit, const std::string& trait) {
    if (unit.champion == nullptr) return false;
    return std::find(unit.champion->traits.begin(), unit.champion->traits.end(), trait) != unit.champion->traits.end() ||
           std::find(unit.extraTraits.begin(), unit.extraTraits.end(), trait) != unit.extraTraits.end();
}

int CountTraitHolders(const std::vector<TraitCountUnit>& units, const std::string& trait) {
    std::vector<ChampionId> counted;
    for (const TraitCountUnit& u : units) {
        if (u.champion == nullptr || u.champion->summon || u.champion->plant || !UnitHasTrait(u, trait)) continue;
        if (std::find(counted.begin(), counted.end(), u.champion->id) == counted.end()) counted.push_back(u.champion->id);
    }
    return static_cast<int>(counted.size());
}

int CountEmblemHolders(const std::vector<TraitCountUnit>& units, const std::string& trait) {
    std::vector<ChampionId> counted;
    for (const TraitCountUnit& u : units) {
        if (u.champion == nullptr || u.champion->summon || u.champion->plant) continue;
        const bool own = std::find(u.champion->traits.begin(), u.champion->traits.end(), trait) != u.champion->traits.end();
        const bool granted = std::find(u.extraTraits.begin(), u.extraTraits.end(), trait) != u.extraTraits.end();
        if (own || !granted) continue;
        if (std::find(counted.begin(), counted.end(), u.champion->id) == counted.end()) counted.push_back(u.champion->id);
    }
    return static_cast<int>(counted.size());
}

std::vector<TraitCountUnit> BoardTraitUnits(const std::vector<UnitInstance>& roster, const ItemDatabase* items) {
    std::vector<TraitCountUnit> out;
    for (const UnitInstance& unit : roster) {
        if (unit.location != LocationType::Board || unit.champion == nullptr) continue;
        TraitCountUnit u;
        u.champion = unit.champion;
        u.starLevel = unit.starLevel;
        if (items != nullptr) {
            for (ItemId item : unit.items) {
                const ItemDefinition* def = item != 0 ? items->Find(item) : nullptr;
                if (def == nullptr) continue;
                for (const std::string& t : def->grantsTraits) u.extraTraits.push_back(t);
            }
        }
        out.push_back(std::move(u));
    }
    return out;
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
            for (const TraitModule& m : trait.modules) {
                if (!check(m.ability, "trait '" + trait.name + "' module", why)) { if (error) *error = why; return false; }
            }
            if (trait.invention != 0) {
                const ChampionDefinition* c = champions.Find(trait.invention);
                if (c == nullptr || !c->summon) { if (error) *error = "trait '" + trait.name + "': its invention is not a summon"; return false; }
            }
            std::vector<const TraitBreakpoint*> all;
            for (const TraitBreakpoint& bp : trait.breakpoints) all.push_back(&bp);
            for (const TraitPath& path : trait.paths) {
                for (const TraitBreakpoint& bp : path.breakpoints) all.push_back(&bp);
            }
            for (const TraitBreakpoint* bpp : all) {
                const TraitBreakpoint& bp = *bpp;
                for (const PlantGrant& g : bp.plants) {
                    const ChampionDefinition* c = champions.Find(g.champion);
                    if (c == nullptr || !c->plant) { if (error) *error = "trait '" + trait.name + "' grants plant " + std::to_string(g.champion) + ", which is not a plant"; return false; }
                }
                if (bp.grantUnit.champion != 0) {
                    const ChampionDefinition* c = champions.Find(bp.grantUnit.champion);
                    if (c == nullptr || !c->special) { if (error) *error = "trait '" + trait.name + "' grants unit " + std::to_string(bp.grantUnit.champion) + ", which is not special"; return false; }
                }
                AbilityDefinition wrapper;
                for (const TraitEffect& te : bp.effects) wrapper.effects.push_back(te.effect);
                if (!check(wrapper, "trait '" + trait.name + "'", why)) { if (error) *error = why; return false; }
                for (const TraitTrigger& tt : bp.triggers) {
                    if (!check(tt.ability, "trait '" + trait.name + "'", why)) { if (error) *error = why; return false; }
                }
            }
            if (trait.queen.champion != 0) {
                const ChampionDefinition* c = champions.Find(trait.queen.champion);
                if (c == nullptr || !c->special) { if (error) *error = "trait '" + trait.name + "': the queen is not a special unit"; return false; }
            }
        }
    }
    return true;
}

}  // namespace w2f
