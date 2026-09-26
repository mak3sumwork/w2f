#pragma once

// Immutable registry of champion definitions. The rest of the server refers to champions
// only through `const ChampionDefinition*` (or ChampionId), never through concrete
// champion classes, so stats / abilities / traits can be added to the definition later
// and loaded from data without touching the economy code.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "w2f/Ability.h"
#include "w2f/Types.h"

namespace w2f {

// Where a champion prefers to stand. Used by AI bots (and later by any auto-placement).
enum class ChampionRole : std::uint8_t { Damage, Tank };

// Base combat stats, all integers. Per-star arrays are indexed by (starLevel - 1).
// A champion with maxHp[0] == 0 has no combat stats and is not fielded in fights.
struct CombatStats {
    StarValue maxHp{};
    StarValue armor{};
    StarValue magicResist{};
    StarValue attackDamage{};
    StarValue abilityDamage{};  // base "ability damage" from the design sheet; ability formulas read it via SelfAbilityDamage
    StarValue critChance{};     // percent chance for an attack to crit (needs RNG; 0 = never rolls)
    int attackSpeedMilli = 1000;  // attacks per second * 1000 (1000 = 1.00/s, 1180 = 1.18/s)
    int attackRange = 1;          // hexes; 1 = melee
    DamageType attackType = DamageType::Physical;   // what a basic attack deals (Magic for the Souls)
    int abilityPower = 100;       // percent multiplier applied to abilityDamage (100 = x1.0)

    // Mana. maxMana is in whole mana; 0 means the champion does not use mana at all.
    int maxMana = 0;
    int startMana = 0;       // mana the unit begins the fight with (whole mana; the sheet's "0/60" is start/max)
    int manaRegenMilli = 0;  // passive mana per second * 1000 (1000 = +1.0 mana/s)

    // > 0: every basic attack's damage is dealt spread evenly over this many ticks instead of
    // landing at once (Faire's "exploit"). 0 = instant.
    int attackSpreadTicks = 0;

    // Presentation (never changes the fight; not part of the content hash). -1 = not set in the data: the CombatConfig defaults apply.
    //  attackWindupTicks: how long before an attack LANDS (the Attack event's tick) its swing animation should start.
    //  projectileSpeedMilli: hexes per second x 1000 of the attack's projectile; 0 = a melee blow / instant hit; default: ranged champions (range >= 2) shoot, melee ones do not.
    int attackWindupTicks = -1;
    int projectileSpeedMilli = -1;

    bool IsCombatCapable() const { return maxHp[0] > 0; }

    // Attack cooldown in simulation ticks: round(kTicksPerSecond * 1000 / attackSpeedMilli), min 1.
    // 1.00/s -> 30 ticks, 2.00/s -> 15, 1.18/s -> 25 (quantised to the tick grid).
    constexpr int AttackIntervalTicks() const {
        if (attackSpeedMilli <= 0) return kTicksPerSecond * 10;  // guard: validated data never gets here
        const int ticks = (kTicksPerSecond * 1000 + attackSpeedMilli / 2) / attackSpeedMilli;
        return ticks < 1 ? 1 : ticks;
    }
};

// Hexa's cockpit: at the start of combat the ally standing on the hex directly BEHIND the unit (toward its own back row) climbs in. It leaves the
// fight (untargetable, inactive: status Piloting) and the unit gains `hpPercent`% of the pilot's max HP plus the bonuses whose `traits` the pilot
// carries (the first match only; effects run as the unit's own, at its star). When the unit dies the pilot ejects: it is back, on its hex, as it went in.
struct PilotBonus {
    std::vector<std::string> traits;
    std::vector<AbilityEffect> effects;   // target Self
};
struct PilotDefinition {
    bool enabled = false;
    int hpPercent = 80;
    std::vector<PilotBonus> bonuses;
};

struct ChampionDefinition {
    ChampionId id = kInvalidChampionId;
    std::string name;
    int cost = 1;  // 1..kMaxCostTier; also selects the pool tier.
    ChampionRole role = ChampionRole::Damage;
    // Trait / origin tags (e.g. "Helios"). Data only for now: no trait logic exists yet.
    std::vector<std::string> traits;
    CombatStats stats;
    AbilityDefinition ability;  // the active spell; default-constructed = none
    AbilityDefinition passive;  // trigger StartOfCombat; default-constructed = none
    AbilityDefinition onAttack; // trigger OnBasicAttack: a rider on every basic attack (e.g. Soul's shield refill); none by default
    std::vector<AbilityDefinition> triggers;  // hooks (OnTakeAbilityDamage, OnAllyDealDamage, ...): silent, always-on reactions; see Ability.h
    std::vector<AuraDefinition> auras;        // standing effects on the units around it
    // A summon is only ever created by a SummonEffect: it is never sold in the shop, is not in the champion pool, and lives only for the
    // fight. It must have no traits. (`cost` is ignored.)
    bool summon = false;
    // Trait system v2. A PLANT (the Nature trait's) is granted by the synergy: it stands on the board, fights, never goes to the bench, cannot be
    // sold and takes no board slot. A SPECIAL unit (the Rift Herald, the Phaisa Queen) is owned like a champion but never comes from the pool:
    // it is granted or offered by a trait and sells for `price` (default: its cost) without returning copies anywhere.
    bool plant = false;
    bool special = false;
    int price = 0;              // special units: what it costs / sells for (0 = `cost`)
    int teamSlots = 1;          // board slots it takes (plants 0; the Queen 2)
    bool stationary = false;    // never walks and never basic-attacks (it still casts) unless Awakened
    PilotDefinition pilot;      // Hexa
    // An UNLOCKABLE champion (like TFT's T-Hex): its copies sit in the pool but nothing hands them out -- no shop, drop or gift -- until a player
    // meets the rule; from then on it can appear in THAT player's shop. Rule: the star levels of the `trait` units on the board add up to
    // `starLevel` or more while the player is level `playerLevel` or higher (demo 1.1, FEEDBACK V1).
    struct UnlockRule {
        std::string trait;
        int starLevel = 0;
        int playerLevel = 0;
        bool Gated() const { return !trait.empty(); }
    } unlock;

    // Sold in the shop and counted in the shared pool.
    bool IsPooled() const { return !summon && !plant && !special; }
    int Price() const { return price > 0 ? price : cost; }
};

// Calls `visit` with the champion id of every SummonEffect in any of the champion's abilities, passives, riders and triggers.
void ForEachSummonReference(const ChampionDefinition& def, const std::function<void(ChampionId)>& visit);

class ChampionDatabase {
public:
    static constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

    // Validates (non-zero unique ids, cost in range) and returns nullptr on failure.
    // Definitions are stored sorted by id, so iteration order (and therefore every pool
    // draw) is identical no matter what order the data was loaded in.
    static std::unique_ptr<ChampionDatabase> Create(std::vector<ChampionDefinition> definitions,
                                                    std::string* error = nullptr);

    ChampionDatabase(const ChampionDatabase&) = delete;
    ChampionDatabase& operator=(const ChampionDatabase&) = delete;

    // Pointers returned here stay valid for the lifetime of the database.
    const ChampionDefinition* Find(ChampionId id) const;
    std::size_t IndexOf(ChampionId id) const;  // kNotFound if unknown
    const std::vector<ChampionDefinition>& All() const { return definitions_; }

    // Hash of what a saved match depends on: every champion's identity (id, name, cost, role, traits), its base stats and
    // which abilities it has (id, trigger, shape). A snapshot records it so it is not restored against different data.
    // It does not cover the numbers inside an ability's effects: retuning a spell keeps old snapshots restorable.
    std::uint64_t ContentHash() const;

private:
    explicit ChampionDatabase(std::vector<ChampionDefinition> definitions)
        : definitions_(std::move(definitions)) {}

    std::vector<ChampionDefinition> definitions_;  // sorted by id, never mutated
};

}  // namespace w2f
