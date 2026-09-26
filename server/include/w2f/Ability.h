#pragma once

// Data format for champion abilities. Everything here is plain data: an ability is a trigger
// plus a list of effect *primitives*, and every number in it is either a per-star constant or a
// small formula over the caster's / target's stats. There is deliberately no per-champion code:
// Alesk, Baira, Cyla, Dyno and Faire are all just different ways of plugging these together
// (see data/champions.json, read by ChampionLoader), and a new champion needs no C++ at all -- only data.
//
//   AbilityDefinition
//     trigger        when it fires: mana full, every Nth basic attack, once at the start of combat, or one of the
//                    combat HOOKS (being hit, dealing damage, an ally dealing damage, HP dropping low ...)
//                    (a champion's `ability` is its active spell; its `passive` uses StartOfCombat; items, synergies and
//                    a champion's `triggers` use hooks)
//     effects[]      what it does, each with a TargetSpec and an optional delay
//       DamageEffect   Physical / Magic / True damage
//       ShieldEffect   absorbs damage for a while (optionally also reduces damage taken)
//       StatusEffect   Stun, or a +/- percent change to AD / attack speed / armor / MR ...
//       DotEffect      damage over time (Burn), with a stacking bonus
//       HealEffect     restores HP (reduced by the target's Wound status)
//       TeleportEffect an assassin jump / blink / dash
//       DisplaceEffect pull or knock back
//       ManaEffect     grants mana
//       SummonEffect   spawns units for the caster's team in the middle of a fight
//
// All values are integers. Percentages are whole percents (20 = 20%). Anything that varies by
// star level is a StarValue, indexed by (starLevel - 1).

#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "w2f/Types.h"

namespace w2f {

using AbilityId = std::uint32_t;
constexpr AbilityId kNoAbility = 0;

using StarValue = std::array<int, kMaxStarLevel>;
constexpr StarValue Same(int value) { return {{value, value, value}}; }

enum class DamageType : std::uint8_t { Physical, Magic, True };

// What a status does to its holder. Also identifies the status in the event stream so the
// client can pick the right particle effect / icon.
enum class StatusType : std::uint8_t {
    Stun,          // cannot move, attack or cast
    Burn,          // produced by DotEffect only (damage over time)
    AttackDamage,  // +/- percent of base attack damage
    AttackSpeed,   // +/- percent attack speed
    Armor,         // +/- percent armor
    MagicResist,   // +/- percent magic resist
    MaxHp,         // +/- percent of base max HP (current HP moves by the same amount, like a level-up of health)
    Wound,         // healing received is reduced by this percent (the strongest Wound on a unit applies).
    InflictsWound, // attacker-side marker: damage-over-time effects THIS unit applies also inflict Wound of this %
                   // on their victims for as long as the DoT runs. (How Baira's passive works.)
    DamageAmp,     // +/- percent damage DEALT: every hit this unit deals is multiplied before armor / resist
    Root,          // cannot move (it can still attack and cast)
    Knockup,       // airborne: like Stun, cannot move / attack / cast. (Only the disable is simulated, not the displacement.)
    CcImmunity,    // ignores Stun / Root / Knockup that OTHER units apply while it lasts (its own effects on itself still work)
    DamageTakenRegen,   // heals this % of every hit's damage taken (after armor / reduction, before shields absorb it)
    BonusAttackDamage,  // + FLAT attack damage. Its size is a formula (StatusEffect::value), evaluated when it is applied.
    Tether,        // holder-side: `percent` of every hit the holder takes (after armor / resist) is redirected to the status's
                   // SOURCE unit instead, as true damage. Nothing is redirected once the source is dead.
    Untargetable,  // enemies cannot target this unit at all: no attacks, no spells, no area effects
    AggroDrop,     // enemies' AUTOMATIC targeting skips this unit (they retarget); area effects can still hit it
    BonusArmor,          // the flat family: + / - points, from a `value` formula (or an item)
    BonusMagicResist,
    BonusMaxHp,          // like MaxHp, current HP moves with it
    BonusAbilityDamage,  // adds to the champion's per-star abilityDamage (its "AP") before the ability-power multiplier
    BonusCritChance,     // + percentage points of crit chance
    AbilityCrit,         // the holder's DAMAGE ABILITIES can critically strike (they roll its crit chance, like a basic attack does)
    CritDamage,          // + percentage points added to the holder's crit bonus (a crit deals base + critBonusPercent + this)
    CritDamageTakenReduction,  // % of the crit BONUS the holder does not take (50: a 21% crit bonus becomes 10.5%)
    BonusManaRegen,      // flat family: + thousandths of a mana per second (1000 = +1 mana/s)
    AbilityPower,        // +/- percent of the holder's ability power (multiplies its ability damage stat, like a +30% AP item)
    SpellShield,         // the next enemy ABILITY hit on the holder is blocked completely; the status is used up
    Blind,               // the holder cannot BASIC ATTACK (it can still walk and cast)
    DamageTaken,         // +/- percent damage TAKEN: every hit the holder suffers is multiplied after armor / resist (frenzy: +10)
    BonusMaxMana,        // flat family: + whole mana on the holder's mana bar (a cast needs that much more). Permanent only; no mana bar = no effect
    ExecuteBelow,        // holder-side: the holder dies when its HP is below `percent` of its max HP (checked every tick, shields do not help)
    HpPerSecond,         // every second the holder heals `percent`% of its max HP (negative: takes that much TRUE damage, credited to the
                         // status's source without counting as damage dealt for formulas)
    EmpoweredAttack,     // charges (`percent` = how many) that riders marked `requiresCharge` spend, one per basic attack
    // Typed damage over time: only ever the `visual` of a DoT effect (a viewer picks the VFX / icon from it). They behave exactly like Burn.
    Poison,              // a toxin: green cloud / drip
    Bleed,               // a wound: red drip
    Drain,               // life drain: the source heals from each tick
    // ---- Trait system v2 (September 2026) ----
    ManaCost,            // +/- percent of the holder's ORIGINAL max mana (-10 = the bar is 10% shorter: Helios's Rally). Permanent only; never below 30%
    HealingAmp,          // +/- percent to every heal and shield the holder RECEIVES (Tuned Oscillator)
    Omnivamp,            // the holder heals this % of all damage it deals (after mitigation)
    Awakened,            // a stationary unit (a plant) may walk and attack while it has this ("the forest comes to life")
    Piloting,            // inside Hexa: cannot act and cannot be affected by anything; ends when Hexa dies (the pilot ejects)
};
constexpr bool IsFlatBonusStatus(StatusType t) {
    return t == StatusType::BonusAttackDamage || t == StatusType::BonusArmor || t == StatusType::BonusMagicResist ||
           t == StatusType::BonusMaxHp || t == StatusType::BonusAbilityDamage || t == StatusType::BonusCritChance ||
           t == StatusType::BonusManaRegen || t == StatusType::BonusMaxMana;
}
// New values are only ever appended: the numbers appear in the event stream.

// ---- Numbers as formulas ------------------------------------------------------------------

// Something a formula can read. All values are integers at the time of use.
enum class StatSource : std::uint8_t {
    None,
    SelfMaxHp,
    SelfCurrentHp,
    SelfAttackDamage,        // current attack damage (including status modifiers)
    SelfAbilityDamage,       // the champion's per-star ability damage stat x its ability power %
    SelfArmor,               // current armor (including status modifiers)
    SelfMagicResist,         // current magic resist (including status modifiers)
    TargetMaxHp,             // the unit each effect instance is applied to
    TargetCurrentHp,
    DamageDealtInWindow,     // damage this caster dealt during the last `windowTicks`
    RawDamageDealtToTarget,  // raw (pre-armor) basic-attack damage dealt to the cast target since
                             // the caster's previous cast (resets on cast or target switch)
    DamageDealtToTargetInWindow,  // damage (after armor / resist, from any source) this caster dealt to the
                             // CAST TARGET during the last `windowTicks`
    CastTargetAttackDamage,  // the cast target's current attack damage (for "steal X% of the target's AD")
    CastTargetArmor,         // the cast target's current armor
    TriggerDamage,           // hook triggers only: the damage (after mitigation) of the hit that fired the trigger
    HighestAllyMaxHp,        // the largest max HP among the caster's living allies (summons excluded): "25% of your tankiest ally"
    PlayerLevel,             // the level of the player the caster fights for (1 for monsters and bare RunFight calls)
    TraitGold,               // gold the caster's player has earned so far through the trait's gold path (Selini's Prosperity)
    TraitStarLevel,          // the sum of the star levels of the caster's team's units that carry the term's `trait` (summons / plants excluded)
    TargetCastCount,         // how many times the unit the effect lands on has cast this fight
};

// Contributes  source * percent[star] / divisor  to an Amount. The divisor is 100 (whole percents) unless the data says "permille"
// (divisor 1000): "0.5% of the target's max HP" is 5 permille.
struct ScalingTerm {
    StatSource source = StatSource::None;
    StarValue percent{};
    int windowTicks = 0;  // DamageDealtInWindow only
    int divisor = 100;
    std::string trait{};  // TraitStarLevel only: which trait's holders count
};

// value = flat[star] + sum(term.source * term.percent[star] / 100)   (integer math, floors)
struct Amount {
    StarValue flat{};
    std::vector<ScalingTerm> terms;
};

inline ScalingTerm Scale(StatSource source, StarValue percent, int windowTicks = 0) {
    ScalingTerm t;
    t.source = source;
    t.percent = percent;
    t.windowTicks = windowTicks;
    return t;
}
inline Amount FlatAmount(StarValue flat) {
    Amount a;
    a.flat = flat;
    return a;
}
inline Amount ScaledAmount(std::vector<ScalingTerm> terms, StarValue flat = StarValue{}) {
    Amount a;
    a.flat = flat;
    a.terms = std::move(terms);
    return a;
}

// ---- Targeting ----------------------------------------------------------------------------

enum class TargetMode : std::uint8_t {
    Self,
    CurrentTarget,     // the enemy the caster was attacking when it cast
    AreaAroundTarget,  // units within `radius` hexes of the cast target's position
    AreaAroundSelf,    // units within `radius` hexes of the caster
    ClosestEnemies,    // the `count` living enemies nearest the caster (ties: lowest UnitId)
    LineBehindTarget,  // the units standing on the `radius` hexes in a straight line behind the cast target, as seen
                       // from the caster (a punch that knocks the target back into whoever is behind it)
    AlliesInStartLine, // allies that began the fight in the caster's team's BUSIEST arena row (the row holding the most
                       // of its units when combat started; a tie goes to the row nearest the middle)
    HighestDamageAlly, // the ally (not the caster) that dealt the most damage in the last `windowTicks`; ties -> higher attack damage, then lower UnitId
    LowestHpAlly,      // the living ally (the caster included) with the least current HP
    HighestHpEnemyNearTarget,  // the enemy with the most current HP within `radius` hexes of the cast target (a "targeted zone")
    RandomEnemy,       // one living enemy chosen with the fight's deterministic RNG
    ConeTowardTarget,  // enemies in a 120-degree cone from the caster toward the cast target, `radius` hexes long
    TriggerAttacker,   // hook triggers only: the unit that dealt the damage that fired the trigger (for OnDealDamage, the holder)
    TriggerVictim,     // hook triggers only: the unit that was damaged (for OnTake* / OnCritTaken / OnHpDropBelowPercent, the holder)
    LowestHpEnemy,     // the living, targetable enemy with the least current HP (ties: lowest UnitId)
    HighestHpEnemy,    // ... with the most current HP
    AllEnemies,        // every living, targetable enemy
    AllAllies,         // every living ally, the caster and summons included
    AreaAroundDensestEnemy,  // units within `radius` hexes of the living, targetable ENEMY that has the most enemies within `radius` of it
                             // ("the largest cluster"; ties: lowest UnitId). Re-resolved every time the effect runs (Baira's waves, Aureon's sun)
    TopDamageEnemies,        // the `count` living, targetable enemies that have dealt the most damage this fight (ties: lowest UnitId). 90 Caliber Nets
};

enum class TargetSide : std::uint8_t { Enemies, Allies, All };

struct TargetSpec {
    TargetMode mode = TargetMode::CurrentTarget;
    int radius = 0;             // area modes: hex distance from the centre, inclusive
    bool includeCenter = true;  // area modes: also hit the unit standing on the centre hex
    TargetSide side = TargetSide::Enemies;
    int count = 0;              // ClosestEnemies: how many. Area modes: at most this many, nearest to the centre first (0 = all): "bounces to 1 adjacent enemy". LowestHpAlly: the N lowest (0 = 1)
    int windowTicks = 0;        // HighestDamageAlly: how far back "damage dealt" looks (0 = 150 ticks)
    // Filters (any mode): only units that carry this trait tag (their own or an emblem's) / that are this champion. Empty / 0 = everyone.
    std::string trait{};
    ChampionId champion = 0;
    StarValue countPerStar{};   // non-zero: replaces `count` by the caster's star (the Stonebark Tree stuns the nearest 2 / 3 / 3)

    static TargetSpec Self() { return {TargetMode::Self, 0, true, TargetSide::All}; }
    static TargetSpec CurrentTarget() { return {}; }
    // "Adjacent hexes" = AroundTarget(1, false); "within N hexes" = AroundTarget(N, true).
    static TargetSpec AroundTarget(int radius, bool includeCenter, TargetSide side = TargetSide::Enemies) {
        return {TargetMode::AreaAroundTarget, radius, includeCenter, side};
    }
    static TargetSpec AroundSelf(int radius, bool includeCenter, TargetSide side = TargetSide::Enemies) {
        return {TargetMode::AreaAroundSelf, radius, includeCenter, side};
    }
    static TargetSpec Closest(int count) { return {TargetMode::ClosestEnemies, 0, true, TargetSide::Enemies, count}; }
    static TargetSpec LineBehind(int length) { return {TargetMode::LineBehindTarget, length, true, TargetSide::Enemies}; }
    static TargetSpec StartLine() { return {TargetMode::AlliesInStartLine, 0, true, TargetSide::Allies}; }
    static TargetSpec HighestDamageAlly(int windowTicks = 0) {
        TargetSpec t{TargetMode::HighestDamageAlly, 0, true, TargetSide::Allies};
        t.windowTicks = windowTicks;
        return t;
    }
    static TargetSpec LowestHpAlly() { return {TargetMode::LowestHpAlly, 0, true, TargetSide::Allies}; }
    static TargetSpec HighestHpEnemyNear(int zoneRadius) { return {TargetMode::HighestHpEnemyNearTarget, zoneRadius, true, TargetSide::Enemies}; }
    static TargetSpec RandomEnemy() { return {TargetMode::RandomEnemy, 0, true, TargetSide::Enemies}; }
    static TargetSpec Cone(int length) { return {TargetMode::ConeTowardTarget, length, true, TargetSide::Enemies}; }
    static TargetSpec TriggerAttacker() { return {TargetMode::TriggerAttacker, 0, true, TargetSide::All}; }
    static TargetSpec TriggerVictim() { return {TargetMode::TriggerVictim, 0, true, TargetSide::All}; }
};

// ---- Effect primitives --------------------------------------------------------------------

struct StatusEffect {
    StatusType status = StatusType::Stun;
    Amount duration;                // ticks (a formula: Faire's stun length scales with damage dealt)
    int multiplierPercent = 100;    // scales the duration, e.g. 60 for "60% of the main stun"
    StarValue percent{};            // magnitude for the modifier statuses; signed (-10 = -10%). Unused for Stun.
    Amount value;                   // BonusAttackDamage only: its flat size, as a formula (e.g. 15% of own armor)
    // Never expires (used by passives). `duration` is ignored; the event stream reports duration 0.
    bool permanent = false;
    // A percent status whose size is a formula: the percent is `value` evaluated when it is applied ("+2% AD per player level").
    bool percentFromValue = false;
    // Statuses of one type normally ADD UP (two +10% become +20%; the Protector synergy relies on it). With `refreshes`,
    // applying this again while the holder already has one of that type does not add a second: it keeps the larger
    // magnitude and extends the end to the later time. Use it for "modes" that can be re-entered (Les's wall).
    bool refreshes = false;
};

struct DamageEffect {
    DamageType type = DamageType::Magic;
    Amount amount;                // raw damage before mitigation
    int multiplierPercent = 100;  // final scale, e.g. 63 for a splash hit
    int armorPenPercent = 0;      // ignores this % of the victim's armor (Physical) or magic resist (Magic)
    bool canCrit = false;         // rolls the caster's crit chance (once per target hit). A caster with the AbilityCrit status crits regardless.
    // Statuses the CASTER gains if this hit is the one that kills its victim (e.g. Lunis drops aggro on a kill).
    // Formulas in them can read the victim as the "cast target" (CastTargetArmor ...).
    std::vector<StatusEffect> onKill;
    // A critical hit also strikes the nearest OTHER targetable enemy of the victim for this % of it (0 = no bounce): Aphel's moonlit arrows.
    int bounceOnCritPercent = 0;
};

struct ShieldEffect {
    Amount amount;                   // how much it absorbs
    Amount duration;                 // ticks
    StarValue damageReductionPercent{};  // while the shield holds, damage taken is also reduced by this %
    bool permanent = false;          // never expires (spawn shields); `duration` is ignored, events report duration 0
    // Refill-style shields ("restore 20 shield per attack"): the holder's TOTAL shield after this is applied may not
    // exceed `cap`; only the part that fits is added.
    bool capped = false;
    Amount cap;
};

struct DotEffect {
    DamageType type = DamageType::Magic;
    Amount amount;                  // raw damage of each hit of each stack -- or, with amountIsTotal, of the whole DoT
    bool amountIsTotal = false;     // amount is the TOTAL over the duration, split evenly (remainders on later hits)
    Amount duration;                // ticks
    // The DoT hits max(1, duration / intervalTicks) times, evenly spread so the LAST hit lands exactly when
    // the duration ends (a 53-tick burn with a 15-tick interval hits at +17, +35, +53).
    int intervalTicks = 30;
    // Applying the effect again while a stack from the SAME ability is still running makes the
    // new stack deal this much % more (Baira: +25/35/45%). Stacks run independently.
    StarValue stackBonusPercent{};
    StatusType visual = StatusType::Burn;  // how the client shows it: Burn, Poison, Bleed or Drain ("visual" in the data). Also tags each of its Damage events (`kind`).
    int healPercent = 0;            // the caster heals this % of the damage each hit actually deals (a drain)
    // Applying this again while a burn from the SAME ability is already running on the victim (from anybody) REPLACES it: the duration restarts and
    // there is only ever one. (Helios: every attack re-lights the burn; it never stacks.) Exclusive with stackBonusPercent.
    bool refreshes = false;
};

struct HealEffect {
    Amount amount;                  // HP restored: flat, or a formula (e.g. TargetMaxHp x 5%). Wound reduces it.
};

enum class TeleportDestination : std::uint8_t {
    BehindFarthestEnemy,  // lands on the free hex next to the FARTHEST enemy that is furthest from where it started (an assassin dive)
    BehindClosestEnemy,
    NextToLowestHpEnemy,   // blink to the enemy with the least HP, land on its far side, and make it the caster's target
    NextToHighestHpEnemy,
    BehindCurrentTarget,   // "dash through": land on the far side of the cast target and keep fighting it
};

struct TeleportEffect {
    TeleportDestination destination = TeleportDestination::BehindFarthestEnemy;  // the target must be Self
};

// Grants mana (whole mana, the same scale as maxMana) to the targets. Does nothing to a unit without a mana bar.
struct ManaEffect {
    Amount amount;
};

// Spawns units for the caster's team in the middle of a fight, on free hexes next to the caster (nearest first).
// A summon is a champion definition marked `summon` (never sold, never in the pool). It appears instantly, fights for the caster's
// team, has no traits and no items, does NOT count as a unit of the team (it cannot keep a fight going, is not a "survivor" and does not
// count for a player's board) and vanishes when the fight ends. The target must be Self.
struct SummonEffect {
    ChampionId champion = kInvalidChampionId;
    Amount count = FlatAmount(Same(1));   // how many spawn (1..8); may scale (e.g. per star)
    int starLevel = 0;                    // 0 = the summoner's star level
    // Stat overrides: when the formula gives more than 0 it REPLACES the summon's own stat. The formulas may read
    // HighestAllyMaxHp: "Souls have 25% of your tankiest ally's max HP" is {source HighestAllyMaxHp, percent 25}.
    Amount maxHp;
    Amount attackDamage;
};

// Moves the targets up to `hexes` hexes along the straight line from / to the caster, stopping at the first blocked or off-board hex.
// (A pull stops next to the caster.) The event stream reports it as a Teleport event with subtype 1.
// TowardAreaCenter: toward the centre the effect's own AreaAroundDensestEnemy / area target picked (Morrah's rift pulls enemies inward).
enum class DisplaceDirection : std::uint8_t { TowardCaster, AwayFromCaster, TowardAreaCenter };
struct DisplaceEffect {
    DisplaceDirection direction = DisplaceDirection::AwayFromCaster;
    int hexes = 1;
};

// An ally joins in: the living ally of `champion` on the caster's team (the highest star, then the lowest UnitId) blinks to each target and strikes it for
// `percentOfAllyAttackDamage` of ITS OWN attack damage (it is the attacker: its crit chance, its "damage dealt"). The blink is presentation only (a Teleport event
// with subtype 2: the ally stays on its hex). No such ally alive = nothing happens. Sola's "Blade Brothers" with Lunis.
struct AllyStrikeEffect {
    ChampionId ally = 0;
    DamageType type = DamageType::Physical;
    StarValue percentOfAllyAttackDamage{};   // indexed by the ALLY's star level
    bool canCrit = true;
};

// Copies of the caster team's strongest units join the fight: the `count` highest-cost units carrying `trait` (then star, then lowest UnitId), each
// cloned as a summon at `statPercent`% of its max HP and attack damage, next to the original. Superior Lifeform.
struct CloneEffect {
    std::string trait{};
    int count = 1;
    int statPercent = 60;
};

using EffectPayload = std::variant<DamageEffect, ShieldEffect, StatusEffect, DotEffect, HealEffect, TeleportEffect, ManaEffect, SummonEffect, DisplaceEffect, AllyStrikeEffect,
                                   CloneEffect>;

// A condition an effect checks when it RUNS (so a delayed effect looks back over the delay): "if the user took no damage during this time".
enum class EffectCondition : std::uint8_t {
    None,
    NoDamageTakenSinceCast,  // the caster has taken no damage (any hit, even one a shield absorbed) since the cast that made this effect began. Needs a delay
};

struct AbilityEffect {
    TargetSpec target;
    EffectCondition condition = EffectCondition::None;
    // Effects with the same delay happen simultaneously; a larger delay makes an effect happen
    // that many ticks after the cast (sequential effects). Targets are re-resolved when it runs.
    int delayTicks = 0;
    // Repeats: the effect runs `repeatCount` times, the first at `delayTicks` and then every `repeatIntervalTicks`
    // (Vega's channel: 8 pulses, one every 0.5 s). Each run re-resolves its targets.
    int repeatCount = 1;
    int repeatIntervalTicks = 0;
    EffectPayload payload;
};

// ---- The ability --------------------------------------------------------------------------

// The unified event-trigger set: WHEN an ability (a champion's spell, an item's effect, a synergy's hook) fires.
//
//  Cast triggers (a champion's own `ability`): Mana, EveryNthAttack -- they cast (SpellCast event, lock, mana drain).
//  Passive: StartOfCombat.
//  Hook triggers: everything else. They are SILENT (no SpellCast, no mana use, no lock): the effects just happen, on the tick the event
//  happens, and may use the event through the TriggerAttacker / TriggerVictim targets and the TriggerDamage formula source.
//  Hooks live in an item's `abilities`, a champion's `triggers`, or a synergy's `triggers`.
//
//    OnBasicAttack            the holder launches a basic attack (victim = its target)
//    EveryNthAttack           (as a hook) the holder's Nth basic attack
//    OnCast                   the holder casts its ability
//    OnTakeBasicAttackDamage  the holder is hit by a basic attack
//    OnTakeAbilityDamage      the holder is hit by an ability's damage
//    OnDealDamage             the holder deals damage (`damageFilter` picks basic / ability / any)
//    OnCritTaken              the holder is hit by a critical strike
//    OnHpDropBelowPercent     the holder's HP falls below `thresholdPercent` of its max (re-arms if it heals back above)
//    OnAllyDealDamage         an ally OF the holder (not the holder itself) deals damage
//    OnAnyUnitDeath           any unit on the board dies (either team; the holder must still be alive). Fires once per death
//    OnShieldBreak            one of the holder's shields is used up by damage (not by expiring); TriggerAttacker = who broke it;
//                             TriggerDamage = the STORED damage: everything that shield absorbed over its life ("detonates for the damage it
//                             stored"). `shieldFromAbility` limits it to shields made by one ability (Mage Shield's own shield)
//    EveryInterval            every `intervalTicks` of the fight (at tick N, 2N ...) while the holder is alive, not disabled, and has a living
//                             enemy target within its attack range: `CurrentTarget` is that target. Twin Snipers' "3% max HP every second"
//
// For the counted triggers `attackCount` = N means "every Nth occurrence" (0 or 1 = every one) and `maxTriggers` caps the number of
// firings per fight (0 = no cap; 1 = "once"). Damage that comes from a hook's own effects never fires hooks again: no chain reactions.
enum class EventTrigger : std::uint8_t {
    None,            // no ability
    Mana,            // cast when mana reaches the champion's max mana
    EveryNthAttack,  // cast right after every Nth basic attack (uses no mana)
    StartOfCombat,   // a passive: its effects are applied once, when the fight starts (before tick 0)
    OnBasicAttack,   // a rider on every basic attack (its effects apply as the attack is made; no SpellCast, no mana use)
    OnCast,
    OnTakeBasicAttackDamage,
    OnTakeAbilityDamage,
    OnDealDamage,
    OnCritTaken,
    OnHpDropBelowPercent,
    OnAllyDealDamage,
    OnAnyUnitDeath,
    OnShieldBreak,
    EveryInterval,
    OnEnemyDeath,    // an ENEMY of the holder dies (the holder must still be alive). Fires once per death. Nihila's hunger
    OnTeamHpLoss,    // the holder's team has lost another `thresholdPercent`% of its total max HP (summons excluded): at 25, it fires as the team drops
                     // below 75%, 50% and 25%. Helios's Rally
    OnDeath,         // the holder itself dies (its effects still run; the Stonebark Tree's stun)
    OnAllyDeath,     // an ally of the holder dies (`triggerTrait`: only allies carrying that trait). The Omnilium Protector
};
using CastTrigger = EventTrigger;   // the original name, kept so existing code and data keep working

// Hook triggers carry a trigger context (attacker, victim, damage) and are counted / capped.
constexpr bool IsDamageHook(EventTrigger t) {
    return t == EventTrigger::OnTakeBasicAttackDamage || t == EventTrigger::OnTakeAbilityDamage || t == EventTrigger::OnDealDamage ||
           t == EventTrigger::OnCritTaken || t == EventTrigger::OnHpDropBelowPercent || t == EventTrigger::OnAllyDealDamage ||
           t == EventTrigger::OnShieldBreak;
}
// Everything that can live in a unit's trigger list (as opposed to a champion's cast slots): riders and hooks.
constexpr bool IsHook(EventTrigger t) {
    return IsDamageHook(t) || t == EventTrigger::OnBasicAttack || t == EventTrigger::EveryNthAttack || t == EventTrigger::OnCast ||
           t == EventTrigger::OnAnyUnitDeath || t == EventTrigger::EveryInterval || t == EventTrigger::OnEnemyDeath || t == EventTrigger::OnTeamHpLoss ||
           t == EventTrigger::OnDeath || t == EventTrigger::OnAllyDeath;
}

enum class DamageFilter : std::uint8_t { Any, Basic, Ability };   // OnDealDamage / OnAllyDealDamage: which damage counts

// A standing effect on the units around the holder, for as long as the holder lives: "enemies within 2 hexes have their magic
// resist reduced by 30%". Units entering the radius gain the status (StatusApplied, permanent), units leaving or the holder's death end it
// (StatusEnded). Recomputed at the start of every tick.
struct AuraDefinition {
    TargetSide side = TargetSide::Enemies;
    int radius = 1;               // hexes from the holder, inclusive
    StatusType status = StatusType::MagicResist;
    int amount = 0;               // signed: a percent for percent statuses (-30), flat points for the flat family (BonusArmor ...)
    bool includeSelf = false;     // Allies only: does the holder get its own aura?
};

struct AbilityDefinition {
    AbilityId id = kNoAbility;  // kNoAbility = this champion has no active ability
    std::string name;
    CastTrigger trigger = CastTrigger::None;
    int attackCount = 0;        // EveryNthAttack: N. Counted hooks: every Nth occurrence (0 / 1 = every one)
    int thresholdPercent = 0;   // OnHpDropBelowPercent: 1..99
    DamageFilter damageFilter = DamageFilter::Any;   // OnDealDamage / OnAllyDealDamage
    int maxTriggers = 0;        // hooks: at most this many firings per fight (0 = unlimited, 1 = once)
    // Attack riders / attack hooks only: fires only while the holder has an EmpoweredAttack status, and spends one charge each time
    // ("your next 3 basic attacks deal bonus damage").
    bool requiresCharge = false;
    int intervalTicks = 0;        // EveryInterval: the period (1..3600 ticks); 0 for every other trigger
    AbilityId shieldFromAbility = kNoAbility;   // OnShieldBreak: only shields created by this ability (0 = any shield)
    // EveryNthAttack: forget the attack count whenever the caster switches target (or its target dies).
    bool resetCountOnTargetChange = false;
    // After casting, the caster is locked (cannot attack or move) for this many ticks and its
    // attack timer is pushed back by the same amount. 0 = it keeps attacking (Cyla's rocket).
    // Presentation (never changes the fight): how long BEFORE the cast lands (the SpellCast event's tick) its animation should start, in ticks.
    // -1 = not set in the data: CombatConfig::defaultCastWindupTicks applies.
    int windupTicks = -1;
    int castLockTicks = 0;
    // A CHANNEL: the caster is locked this long, and the ability's delayed effects only happen if the channel is not
    // interrupted (the caster is stunned / knocked up / dies). Effects placed at the channel's end are its "if it
    // completes" finale. A channel is its own lock, so castLockTicks must be 0.
    int channelTicks = 0;
    // If true, the caster also casts once when it dies (if it has a target), regardless of mana.
    bool castOnDeath = false;
    // OnBasicAttack / EveryNthAttack hooks: only from the holder's Nth basic attack of the fight on (per star; 0 = from the first). Cyla's Fishbones.
    StarValue afterAttacks{};
    // OnAllyDeath: only allies carrying this trait tag count (empty = any ally).
    std::string triggerTrait;
    // The ability's delayed / repeated effects stop when its caster dies (a plant's timed blessing ends with the plant).
    bool stopsOnDeath = false;
    std::vector<AbilityEffect> effects;

    bool HasAbility() const { return id != kNoAbility; }
};

// Passives are an AbilityDefinition with trigger StartOfCombat. They may only use targets that do not need a
// "current target" (Self, AreaAroundSelf, ClosestEnemies, AlliesInStartLine, LowestHpAlly, HighestDamageAlly, RandomEnemy),
// and cannot have a cast lock or cast on death. An OnBasicAttack rider CAN use CurrentTarget: the unit it just hit.

// Feeds an ability's shape into a hash (ids, triggers and their parameters, what effects it has and how they are aimed). Used for the
// data hashes a snapshot records. It does not cover the numbers inside an effect's formulas.
struct Fnv1a;
void HashAbility(Fnv1a& hash, const AbilityDefinition& ability);

// Checks the definition is internally consistent. `maxMana` is the owning champion's max mana.
bool ValidateAbility(const AbilityDefinition& ability, int maxMana, std::string* error = nullptr);

// The area a spell covers, for the viewer's VFX (derived from the ability's effects; presentation only).
enum class AreaShape : std::uint8_t {
    None,        // only the caster (a buff, a shield on self)
    Single,      // one unit: the cast target (or the unit a targeting rule picks)
    Circle,      // `size` hexes around the cast target's hex (the event's `to`)
    CircleSelf,  // `size` hexes around the caster
    Line,        // a straight line of `size` hexes behind the cast target, seen from the caster
    Cone,        // a 120-degree cone `size` hexes long from the caster toward the cast target
    Row,         // the caster's team's busiest row (allies)
    All,         // every enemy / every ally on the field
};
struct AreaDescription {
    AreaShape shape = AreaShape::None;
    int size = 0;   // radius / length in hexes (0 where it does not apply)
};
// The widest area any effect of `ability` reaches (order: All > Cone > Line > Circle > CircleSelf > Row > Single > None; the bigger radius wins a tie).
AreaDescription DescribeArea(const AbilityDefinition& ability);

}  // namespace w2f
