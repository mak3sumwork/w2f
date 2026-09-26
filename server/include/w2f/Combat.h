#pragma once

// The seam between the match flow and the combat simulation.
// MatchManager decides WHO fights and applies the RESULTS; an ICombatSimulator decides what
// happens in between and reports it as a CombatLog -- a timestamped stream of events.
//
// The UE5 client is a "dumb viewer": it never simulates or predicts anything. It receives the
// finished log when the Combat phase starts and just plays the events back at their ticks.

#include <cstdint>
#include <vector>

#include "w2f/Ability.h"
#include "w2f/Hex.h"
#include "w2f/Types.h"
#include "w2f/Unit.h"

namespace w2f {

class PlayerManager;
class EncounterDatabase;
class ChampionDatabase;

struct Matchup {
    PlayerId home = kInvalidPlayerId;
    PlayerId away = kInvalidPlayerId;
    // Odd player count: the leftover player fights a copy ("ghost") of `away`'s board.
    // Only `home` is affected by the result; `away` is untouched.
    bool awayIsGhost = false;
    // PvE round: `home` fights a preset monster board (`encounter` in the EncounterDatabase; 0 = none was available);
    // `away` is kInvalidPlayerId. Only `home` is affected, and only by the drop for winning.
    bool awayIsMonsters = false;
    std::uint32_t encounter = 0;
};

enum class CombatWinner : std::uint8_t { Home, Away, Draw };

// One thing that happened. `tick` is combat-local: tick 0 is the first tick of the Combat phase.
// Within a tick, events appear in the order they must be played.
//
//   type           fields used
//   -------------  -----------------------------------------------------------------------------
//   Spawn          unit, team (0 home / 1 away), champion, star, to = arena hex, amount = base max HP,
//                  maxMana / manaRegen (thousandths of a mana; 0 = the unit has no mana bar).
//                  A SUMMON (flags kFlagSummon, other = the summoner) spawns instantly on a free hex at any tick of the fight; at the end
//                  of the fight every summon still alive gets a Death event (flags kFlagSummon) and vanishes. Summons do not count as
//                  "survivors" and cannot keep a fight going.
//   Move           unit, from, to (adjacent hexes), amount = ticks the step takes (for tweening)
//   Attack         unit (attacker), other (target), from = attacker's hex, to = target's hex,
//                  kind = 0 melee / instant, 1 projectile,
//                  windup = ticks BEFORE this event's tick at which the swing animation should start,
//                  flight = ticks the projectile is in the air (0 for melee). THE EVENT'S TICK IS THE MOMENT OF IMPACT (the Damage event is on the same
//                  tick): the swing starts at tick - flight - windup, the projectile leaves at tick - flight and lands at tick. (The whole log is known in
//                  advance, so a viewer just schedules the animation early; where that would start before tick 0 or overlap the unit's previous
//                  action, shorten it.)
//                  A Tether splits one hit into two Damage events: the holder's (already reduced) and the
//                  source's, flagged kFlagRedirected, both crediting the original attacker.
//   Damage         (kind = the StatusType of a typed damage-over-time tick -- Burn, Poison, Bleed or Drain --, 0 for everything else)
//                  unit (victim), other (attacker), amount = damage after armor/resist/shield
//                  reduction (before shield absorption), absorbed = part soaked up by shields,
//                  hpAfter, subtype = DamageType, flags = kFlag*    -- hp lost = amount - absorbed
//   Death          unit
//   SpellCast      unit (caster), other (main target, 0 if none), ability = AbilityId,
//                  duration = ticks the caster is locked in the cast animation AFTER the tick, flags (kFlagOnDeath),
//                  windup = ticks BEFORE this event's tick at which the cast animation should start (the effects land ON the tick),
//                  shape = AreaShape, size = its radius / length in hexes, from = caster's hex, to = the centre of the area (the target's hex for
//                  Circle / Line / Cone, the caster's own hex otherwise)
//   ShieldApplied  unit (holder), other (caster), amount = absorb capacity, duration = ticks
//   ShieldEnded    unit, amount = capacity that was left unused (0 = fully broken)
//   StatusApplied  unit (holder), other (caster), subtype = StatusType, duration = ticks (0 = permanent,
//                  e.g. a passive), amount = magnitude % for modifier statuses (0 for Stun / Burn),
//                  hpAfter = holder's current HP afterwards (it changes for MaxHp statuses)
//   StatusEnded    unit, subtype = StatusType, hpAfter = holder's current HP afterwards
//                  (a unit's death clears everything without events)
//   Heal           unit (healed), other (healer; the unit itself for self-regeneration), amount = HP actually
//                  restored (after Wound and the max-HP cap), reduced = healing removed by Wound, hpAfter.
//                  Nothing is sent when no HP was restored.
//   Teleport       unit, from, to (an assassin jump: the unit is at `to` from this tick on)
//   SpellInterrupted  unit, ability -- its channel was broken (stunned / knocked up); the pulses and finale that
//                  were still to come will not happen. (A channeller that dies just dies: see Death.)
//   TraitActivated team, traitId, amount = how many different champions on that team have the trait,
//                  subtype = which breakpoint (1 = the lowest) is active. Sent on tick 0, before the
//                  synergy's own StatusApplied events and before any passive.
//   Overtime       (no unit) sent once, at tick = the end of regulation, when the fight is still undecided: from this tick on every unit's attack
//                  speed, movement and mana regeneration run `amount` times faster (so Move.amount and the gaps between Attack events shrink by
//                  that factor). Overtime has no end: it lasts until one team is wiped out (`duration` is 0).
//   ManaChanged    unit, amount = its mana now, in thousandths. Sent when mana changes for a discrete
//                  reason (an attack, damage taken, a cast draining it to 0) -- NOT for passive regen,
//                  which would be one event per unit per tick. Between events a client shows the bar
//                  rising at Spawn.manaRegen per second, except while the caster is locked in a cast
//                  animation (SpellCast.duration); each event re-syncs it to the exact value.
//
// Passives (trigger StartOfCombat) produce only their StatusApplied / ShieldApplied events, on tick 0
// right after the Spawn events: no SpellCast.
//
// Coordinates are arena hexes (see BoardToArena); the same log is shown to both players.
enum class CombatEventType : std::uint8_t {
    Spawn, Move, Attack, Damage, Death, SpellCast, ShieldApplied, ShieldEnded, StatusApplied, StatusEnded, ManaChanged,
    Heal, TraitActivated, Teleport, SpellInterrupted, Overtime
};

// CombatEvent::flags
constexpr std::uint8_t kFlagCrit = 1;     // Damage: was a critical hit
constexpr std::uint8_t kFlagDot = 2;      // Damage: one tick of a damage-over-time effect
constexpr std::uint8_t kFlagAbility = 4;  // Damage: dealt by an ability effect
constexpr std::uint8_t kFlagBasic = 8;    // Damage: from a basic attack (possibly spread over time)
constexpr std::uint8_t kFlagOnDeath = 16; // SpellCast: the caster's last cast as it died
constexpr std::uint8_t kFlagRedirected = 32;  // Damage: the tethered share of a hit, taken by the tether's source
constexpr std::uint8_t kFlagTriggered = 64;   // Damage: caused by a hook trigger's effect (an item / synergy reaction), not by an attack or a spell
constexpr std::uint8_t kFlagSummon = 128;     // Spawn / Death: the unit is a summon (Spawn.other = its summoner); it can appear mid-fight

// Unit ids of summons: kSummonUnitBase + 1, + 2, ... (players' ids have a high byte 1..8, monsters' are above kMonsterUnitBase).
constexpr UnitId kSummonUnitBase = 0xE0000000u;

struct CombatEvent {
    int tick = 0;
    CombatEventType type = CombatEventType::Spawn;
    std::uint8_t team = 0;
    UnitId unit = kInvalidUnitId;
    UnitId other = kInvalidUnitId;
    HexCoord from;
    HexCoord to;
    int amount = 0;
    int hpAfter = 0;
    ChampionId champion = kInvalidChampionId;  // Spawn only
    std::uint8_t star = 0;                     // Spawn only
    int absorbed = 0;                          // Damage only
    std::uint8_t subtype = 0;                  // Damage: DamageType. StatusApplied / StatusEnded: StatusType
    std::uint8_t flags = 0;
    AbilityId ability = kNoAbility;            // SpellCast only
    int duration = 0;                          // SpellCast / ShieldApplied / StatusApplied
    int manaMax = 0;                           // Spawn only (thousandths)
    int manaRegen = 0;                         // Spawn only (thousandths of a mana per second)
    int reduced = 0;                           // Heal only: healing removed by Wound
    std::uint32_t traitId = 0;                 // TraitActivated only
    // Presentation contract (Phase B) -- nothing here changes the fight; see the per-type notes above.
    int windup = 0;                            // Attack / SpellCast: ticks BEFORE the event's tick that the animation should start
    int flight = 0;                            // Attack: ticks the projectile travels (0 = melee / instant)
    std::uint8_t kind = 0;                     // Attack: 0 melee, 1 projectile. Damage: the DoT's StatusType (0 = not a typed DoT)
    std::uint8_t shape = 0;                    // SpellCast: AreaShape
    std::uint8_t size = 0;                     // SpellCast: the area's radius / length in hexes
};

struct CombatLog {
    std::vector<CombatEvent> events;
    int endTick = 0;                 // tick of the last death, or the time limit if nobody won outright
    int survivors[2] = {0, 0};       // living units at the end: [home, away]
    int pathSearches = 0;            // how many path searches the fight needed (perf diagnostic)
    std::uint64_t checksum = 0;      // hash of `events`; lets a client verify it received the stream intact
    // Takedowns per unit (a kill, or damage to the victim in the 3 s before it died), for the traits that pay for them (Selini's Prosperity,
    // Najmi's star dust). Only units that scored at least one, summons excluded; ascending UnitId. Not part of the checksum.
    std::vector<std::pair<UnitId, int>> takedowns;

    std::uint64_t ComputeChecksum() const;
};

// What a PvE win awarded (one drop per win). Filled in by the MatchManager when the round resolves.
enum class PveDropType : std::uint8_t { None, Gold, Champion, Item };

struct PveDrop {
    PveDropType type = PveDropType::None;
    int gold = 0;                              // Gold: the amount (also a Champion drop that could not be placed pays its cost)
    ChampionId champion = kInvalidChampionId;  // Champion: which one
    ItemId item = 0;                           // Item: which one
    bool operator==(const PveDrop& o) const { return type == o.type && gold == o.gold && champion == o.champion && item == o.item; }
};

struct CombatOutcome {
    Matchup matchup;
    CombatWinner winner = CombatWinner::Draw;
    // Set by the SIMULATOR: how many units the winning side had left alive (0 for a draw). The rules that turn this into
    // player damage live in the MatchManager (GameConfig::PlayerDamage), not in the simulator.
    int winnerSurvivors = 0;
    // Set by the MatchManager when the round resolves (0 before that, for draws, ghosts' owners' wins and PvE).
    int damageToLoser = 0;
    PveDrop drop;           // PvE win only: what the player was given
    std::vector<PveDrop> loot;   // PvE: the encounter's guaranteed drops, paid win or lose (demo 1.1)
    CombatLog log;          // Empty for simulators that don't produce one.
};

struct CombatContext {
    int round = 0;
    std::uint64_t seed = 0;  // Unique per round, derived from the match seed. Seed all combat randomness from this.
    const PlayerManager& players;  // Read-only view: boards live in players' rosters
    const std::vector<Matchup>& matchups;
    int maxTicks = 0;        // Length of the Combat phase; a fight must finish (or time out) within this
    const EncounterDatabase* encounters = nullptr;   // monster boards for PvE matchups (may be null when there are none)
    const ChampionDatabase* champions = nullptr;     // where SummonEffects find their (summon-only) champion definitions
    std::vector<std::pair<std::uint32_t, int>> traitPaths{};   // trait system v2: the path the match picked for each trait that has paths
};

class ICombatSimulator {
public:
    virtual ~ICombatSimulator() = default;
    // Must be a pure function of `context` (deterministic). Return one outcome per matchup.
    virtual std::vector<CombatOutcome> Simulate(const CombatContext& context) = 0;
};

}  // namespace w2f
