#include "w2f/ChampionDatabase.h"

#include <algorithm>
#include <functional>
#include <variant>

#include "w2f/Hash.h"
#include "w2f/Hex.h"

namespace w2f {

void ForEachSummonReference(const ChampionDefinition& def, const std::function<void(ChampionId)>& visit) {
    const auto scan = [&](const AbilityDefinition& a) {
        for (const AbilityEffect& e : a.effects) {
            if (const auto* summon = std::get_if<SummonEffect>(&e.payload)) visit(summon->champion);
        }
    };
    scan(def.ability);
    scan(def.passive);
    scan(def.onAttack);
    for (const AbilityDefinition& t : def.triggers) scan(t);
}

namespace {
bool ValidatePilot(const ChampionDefinition& def, std::string* why) {
    const PilotDefinition& p = def.pilot;
    if (!p.enabled) return true;
    if (p.hpPercent < 0 || p.hpPercent > 500) { *why = "pilot hpPercent must be 0..500"; return false; }
    for (const PilotBonus& bonus : p.bonuses) {
        if (bonus.traits.empty() || bonus.effects.empty()) { *why = "a pilot bonus needs traits and effects"; return false; }
        AbilityDefinition wrapper;   // validated as a passive of the unit
        wrapper.id = 1;
        wrapper.name = def.name + " pilot bonus";
        wrapper.trigger = CastTrigger::StartOfCombat;
        wrapper.effects = bonus.effects;
        if (!ValidateAbility(wrapper, def.stats.maxMana, why)) return false;
        for (const AbilityEffect& e : bonus.effects) {
            if (e.target.mode != TargetMode::Self) { *why = "pilot bonus effects target Self"; return false; }
        }
    }
    return true;
}
}  // namespace

std::unique_ptr<ChampionDatabase> ChampionDatabase::Create(std::vector<ChampionDefinition> definitions,
                                                           std::string* error) {
    auto fail = [error](const std::string& message) -> std::unique_ptr<ChampionDatabase> {
        if (error) *error = message;
        return nullptr;
    };

    std::sort(definitions.begin(), definitions.end(),
              [](const ChampionDefinition& a, const ChampionDefinition& b) { return a.id < b.id; });

    for (std::size_t i = 0; i < definitions.size(); ++i) {
        const ChampionDefinition& def = definitions[i];
        if (def.id == kInvalidChampionId) return fail("champion '" + def.name + "' has reserved id 0");
        if (def.cost < 1 || def.cost > kMaxCostTier) return fail("champion '" + def.name + "' has cost out of range");
        if (i > 0 && definitions[i - 1].id == def.id) return fail("duplicate champion id for '" + def.name + "'");
        const CombatStats& stats = def.stats;
        if (stats.IsCombatCapable()) {
            if (stats.attackSpeedMilli <= 0) return fail("champion '" + def.name + "' needs attackSpeedMilli > 0");
            if (stats.attackRange < 1) return fail("champion '" + def.name + "' needs attackRange >= 1");
            if (stats.startMana < 0 || stats.startMana > stats.maxMana) {
                return fail("champion '" + def.name + "': startMana must be between 0 and maxMana");
            }
            if (stats.maxMana < 0 || stats.manaRegenMilli < 0 || stats.attackSpreadTicks < 0 || stats.abilityPower < 0) {
                return fail("champion '" + def.name + "' has invalid mana / spread / ability power values");
            }
            for (int star = 0; star < kMaxStarLevel; ++star) {
                const auto s = static_cast<std::size_t>(star);
                if (stats.maxHp[s] <= 0 || stats.attackDamage[s] < 0 || stats.armor[s] < 0 || stats.magicResist[s] < 0 ||
                    stats.abilityDamage[s] < 0 || stats.critChance[s] < 0 || stats.critChance[s] > 100) {
                    return fail("champion '" + def.name + "' has invalid per-star stats");
                }
            }
        }
        std::string abilityError;
        if (def.ability.HasAbility() && def.ability.trigger == CastTrigger::StartOfCombat) {
            return fail("champion '" + def.name + "': a StartOfCombat ability belongs in 'passive', not 'ability'");
        }
        if (!ValidateAbility(def.ability, def.stats.maxMana, &abilityError)) {
            return fail("champion '" + def.name + "': " + abilityError);
        }
        if (def.ability.HasAbility() && def.ability.trigger == CastTrigger::OnBasicAttack) {
            return fail("champion '" + def.name + "': an OnBasicAttack ability belongs in 'onAttack', not 'ability'");
        }
        if (def.onAttack.HasAbility() && def.onAttack.trigger != CastTrigger::OnBasicAttack) {
            return fail("champion '" + def.name + "': 'onAttack' must use the OnBasicAttack trigger");
        }
        if (!ValidateAbility(def.onAttack, def.stats.maxMana, &abilityError)) {
            return fail("champion '" + def.name + "' onAttack: " + abilityError);
        }
        if (def.passive.HasAbility() && def.passive.trigger != CastTrigger::StartOfCombat) {
            return fail("champion '" + def.name + "': the 'passive' must use the StartOfCombat trigger");
        }
        if (!ValidateAbility(def.passive, def.stats.maxMana, &abilityError)) {
            return fail("champion '" + def.name + "' passive: " + abilityError);
        }
        for (const AbilityDefinition& hook : def.triggers) {
            if (!hook.HasAbility()) return fail("champion '" + def.name + "' has a trigger without an id");
            if (!IsHook(hook.trigger) && hook.trigger != CastTrigger::StartOfCombat) {
                return fail("champion '" + def.name + "': 'triggers' holds hooks (OnTake..., OnDealDamage, OnCast, ...); use 'ability' for Mana / EveryNthAttack spells and 'passive' for passives");
            }
            if (!ValidateAbility(hook, def.stats.maxMana, &abilityError)) return fail("champion '" + def.name + "' trigger: " + abilityError);
        }
        for (const AuraDefinition& aura : def.auras) {
            if (aura.radius < 1 || aura.radius > kArenaColumns + kArenaRows || aura.amount == 0) return fail("champion '" + def.name + "' has an invalid aura");
            if (aura.status == StatusType::Stun || aura.status == StatusType::Knockup || aura.status == StatusType::Burn || aura.status == StatusType::Tether) {
                return fail("champion '" + def.name + "': an aura cannot apply Stun, Knockup, Burn or Tether");
            }
        }
        if (def.summon && !def.traits.empty()) return fail("summon '" + def.name + "' has traits: summons take part in no synergy");
        if (def.summon && !def.stats.IsCombatCapable()) return fail("summon '" + def.name + "' has no combat stats");
        if ((def.summon ? 1 : 0) + (def.plant ? 1 : 0) + (def.special ? 1 : 0) > 1) return fail("champion '" + def.name + "' can only be one of summon / plant / special");
        if (def.teamSlots < 0 || def.teamSlots > 2) return fail("champion '" + def.name + "': teamSlots must be 0..2");
        if (def.plant && def.teamSlots != 0) return fail("plant '" + def.name + "' must take no board slot (teamSlots 0)");
        if (!def.plant && !def.summon && def.teamSlots == 0) return fail("champion '" + def.name + "': only plants take no board slot");
        if (def.price < 0 || def.price > 20 || (def.price != 0 && !def.special)) return fail("champion '" + def.name + "': a price (1..20) is only for special units");
        {
            std::string pilotError;
            if (!ValidatePilot(def, &pilotError)) return fail("champion '" + def.name + "': " + pilotError);
        }
        for (std::size_t t = 0; t < def.traits.size(); ++t) {
            if (def.traits[t].empty()) return fail("champion '" + def.name + "' has an empty trait name");
            for (std::size_t u = 0; u < t; ++u) {
                if (def.traits[u] == def.traits[t]) return fail("champion '" + def.name + "' lists trait '" + def.traits[t] + "' twice");
            }
        }
    }

    // Every Summon effect must name a champion that exists and is marked `summon`.
    const auto isSummon = [&definitions](ChampionId id) {
        for (const ChampionDefinition& d : definitions) {
            if (d.id == id) return d.summon;
        }
        return false;
    };
    for (const ChampionDefinition& def : definitions) {
        std::string bad;
        ForEachSummonReference(def, [&](ChampionId id) {
            if (!isSummon(id) && bad.empty()) bad = "champion '" + def.name + "' summons champion " + std::to_string(id) + ", which does not exist or is not marked \"summon\"";
        });
        if (!bad.empty()) return fail(bad);
    }

    // Private ctor: can't use make_unique.
    return std::unique_ptr<ChampionDatabase>(new ChampionDatabase(std::move(definitions)));
}

std::size_t ChampionDatabase::IndexOf(ChampionId id) const {
    auto it = std::lower_bound(definitions_.begin(), definitions_.end(), id,
                               [](const ChampionDefinition& d, ChampionId value) { return d.id < value; });
    if (it == definitions_.end() || it->id != id) return kNotFound;
    return static_cast<std::size_t>(it - definitions_.begin());
}

const ChampionDefinition* ChampionDatabase::Find(ChampionId id) const {
    const std::size_t index = IndexOf(id);
    return index == kNotFound ? nullptr : &definitions_[index];
}

std::uint64_t ChampionDatabase::ContentHash() const {
    Fnv1a h;
    h.AddInt(static_cast<std::int64_t>(definitions_.size()));
    for (const ChampionDefinition& d : definitions_) {
        h.Add(d.id);
        h.AddString(d.name);
        h.AddInt(d.cost);
        h.Add(static_cast<std::uint64_t>(d.role));
        h.Add(d.summon ? 1 : 0);
        h.AddInt(static_cast<std::int64_t>(d.traits.size()));
        for (const std::string& t : d.traits) h.AddString(t);
        const CombatStats& s = d.stats;
        for (const StarValue* values : {&s.maxHp, &s.armor, &s.magicResist, &s.attackDamage, &s.abilityDamage, &s.critChance}) {
            for (int v : *values) h.AddInt(v);
        }
        for (int v : {s.attackSpeedMilli, s.attackRange, s.abilityPower, s.maxMana, s.startMana, s.manaRegenMilli, s.attackSpreadTicks}) {
            h.AddInt(v);
        }
        if (s.attackType != DamageType::Physical) h.Add(0xA77AC0ull + static_cast<std::uint64_t>(s.attackType));   // (only when set: old data hashes as before)
        if (d.plant) h.Add(0x9A47ull);
        if (d.special) h.Add(0x59EC1A1ull + static_cast<std::uint64_t>(d.price));
        if (d.teamSlots != 1) h.AddInt(0x5107ll + d.teamSlots);
        if (d.stationary) h.Add(0x57A7ull);
        if (d.pilot.enabled) {
            h.AddInt(d.pilot.hpPercent);
            for (const PilotBonus& b : d.pilot.bonuses) {
                for (const std::string& t : b.traits) h.AddString(t);
                h.AddInt(static_cast<std::int64_t>(b.effects.size()));
            }
        }
        HashAbility(h, d.ability);
        HashAbility(h, d.passive);
        HashAbility(h, d.onAttack);
        h.AddInt(static_cast<std::int64_t>(d.triggers.size()));
        for (const AbilityDefinition& t : d.triggers) HashAbility(h, t);
        h.AddInt(static_cast<std::int64_t>(d.auras.size()));
        for (const AuraDefinition& a : d.auras) {
            h.Add(static_cast<std::uint64_t>(a.side));
            h.AddInt(a.radius);
            h.Add(static_cast<std::uint64_t>(a.status));
            h.AddInt(a.amount);
            h.Add(a.includeSelf ? 1 : 0);
        }
    }
    return h.value;
}

}  // namespace w2f
