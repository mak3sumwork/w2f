#pragma once

// Deterministic auto-battler combat: units find the closest enemy, walk to it, trade basic
// attacks, generate mana, and cast the data-defined abilities of their champions (see Ability.h).
// The fight is resolved instantly and returned as a CombatLog; nothing runs in real time.
//
// Determinism: integer math only. The single source of randomness is crit rolls, drawn from an
// Rng seeded per fight, always in a fixed order (acting order = ascending UnitId, then effect
// order), and only by units that actually have crit chance.
//
// Per tick:
//   0. START    once, before tick 0: synergies (traits) are applied per team, then every unit's passive.
//   1. EXPIRE   shields / statuses whose time is up end (events at their exact end tick).
//   2. ACT      each living unit, in ascending UnitId order, only *declares* what it does: pick a
//               target, cast (mana full / Nth attack), attack, or step. Steps happen immediately
//               (so two units never claim one hex); attacks and casts are only queued.
//   3. RESOLVE  in a fixed order, everything queued takes effect:
//                 a. passive mana regeneration
//                 b. delayed effects that are due, then the effects of this tick's casts (effects that grant CC
//                    immunity land first, so immunity beats same-tick crowd control whatever the UnitIds)
//                    (these create shields, statuses, damage-over-time stacks and queued damage)
//                 c. damage-over-time ticks that are due
//                 d. ALL queued damage lands (armor / resist -> shield reduction -> shield absorb -> HP)
//                 e. mana from damage taken
//                 f. units at 0 HP die; a dying unit with a cast-on-death ability casts once,
//                    and that cast resolves the same way (repeat until nothing new dies).
//   Because Act never changes another unit, and Resolve treats everything queued in a tick as
//   simultaneous, neither a lower UnitId nor the order units are listed gives an advantage.
//
// Cost control: a unit keeps its target until it dies (or it is out of range while another
// enemy is in range), and keeps its path until the target moved / the next hex got taken. See
// CombatLog::pathSearches.

#include <vector>

#include "w2f/ChampionDatabase.h"
#include "w2f/Combat.h"
#include "w2f/Config.h"
#include "w2f/Item.h"

namespace w2f {

// One unit entering a fight, already positioned in ARENA coordinates.
struct FightUnitSpec {
    UnitId id = kInvalidUnitId;
    const ChampionDefinition* champion = nullptr;
    int starLevel = 1;
    int team = 0;  // 0 = home, 1 = away
    HexCoord position;
    std::vector<const ItemDefinition*> items = {};   // equipment (applied as permanent statuses on tick 0)
};

struct FightResult {
    CombatWinner winner = CombatWinner::Draw;
    CombatLog log;
};

// Damage after armor / magic resist: floor(raw * 100 / (100 + resist)), never below `minDamage`.
constexpr int MitigatedDamage(int raw, int resist, int minDamage) {
    const int reduced = static_cast<int>(static_cast<long long>(raw) * 100 / (100 + resist));
    return reduced < minDamage ? minDamage : reduced;
}

class TraitDatabase;

class CombatSimulator : public ICombatSimulator {
public:
    // `traits` (optional, non-owning, must outlive the simulator) enables synergies: without it, trait tags do nothing.
    // `items` (optional, same lifetime rule) lets units carry equipment: without it, unit items are ignored.
    // `summons` (optional) is where RunFight looks up champions that SummonEffects create; Simulate() uses the match's champion database
    // (CombatContext::champions) and, for monsters, the encounter data.
    explicit CombatSimulator(const CombatConfig& config, const TraitDatabase* traits = nullptr, const ItemDatabase* items = nullptr,
                             const ChampionDatabase* summons = nullptr)
        : config_(config), traits_(traits), items_(items), summons_(summons) {}

    // Builds each matchup's teams from the players' boards (or, for a PvE matchup, the encounter's monsters) and fights them.
    // Reports who won and with how many survivors; what that costs a player is the MatchManager's business.
    std::vector<CombatOutcome> Simulate(const CombatContext& context) override;

    // Fights explicit teams. Units without combat stats are ignored. Positions must be unique,
    // in-bounds arena hexes and ids unique. `maxTicks` is the time limit; on timeout the team
    // with more survivors wins, then the one with more total HP, otherwise it's a draw.
    // `seed` drives crit rolls only.
    FightResult RunFight(const std::vector<FightUnitSpec>& units, int maxTicks, std::uint64_t seed = 0) const;

private:
    CombatConfig config_;
    const TraitDatabase* traits_;
    const ItemDatabase* items_;
    const ChampionDatabase* summons_;
};

}  // namespace w2f
