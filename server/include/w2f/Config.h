#pragma once

// All tunable numbers live here (never hard-coded in logic) so they can later be
// loaded from data files. Defaults follow TFT-style values and are placeholders for balance.
//
// All math is integer-only: no floats, so results are identical on every platform.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "w2f/Types.h"

namespace w2f {

struct StreakBonus {
    int minStreak;  // applies when |streak| >= minStreak (list must be ascending)
    int bonusGold;
};

struct PlayerConfig {
    int startingHealth = 100;
    int startingGold = 0;
    // TFT rule (the designer's): you may field at most `level` units on the board (level 1 = 1 unit ... level 10 = 10). Off = any of the
    // 28 cells may be used (only tests and experiments turn it off).
    bool limitBoardToLevel = true;

    // Leveling: spend gold for XP.
    int buyXpCost = 4;
    int buyXpAmount = 4;
    // xpToNextLevel[L-1] = XP needed to go from level L to L+1.
    std::array<int, kMaxPlayerLevel - 1> xpToNextLevel = {{2, 2, 6, 10, 20, 36, 48, 76, 84}};
    int firstPassiveXpRound = 2;
    int passiveXpPerRound = 2;

    // Passive income. baseIncomeByRound[i] applies to round i+1; later rounds use baseIncomeAfterTable.
    std::vector<int> baseIncomeByRound = {2, 2, 3, 4};
    int baseIncomeAfterTable = 5;
    int interestStepGold = 10;  // +interestPerStep for each interestStepGold banked...
    int interestPerStep = 1;
    int interestCap = 5;        // ...up to this much.
    std::vector<StreakBonus> streakBonuses = {{2, 1}, {4, 2}, {5, 3}};
};

struct ShopConfig {
    int slotCount = 5;
    int rerollCost = 2;
    // dropRatesByLevel[level-1][tier-1] = relative weight (percent by convention; any positive
    // total works). Tiers that are sold out in the pool are excluded and the rest re-normalised.
    std::array<std::array<int, kMaxCostTier>, kMaxPlayerLevel> dropRatesByLevel = {{
        {{100, 0, 0, 0, 0}},  // L1
        {{100, 0, 0, 0, 0}},  // L2
        {{75, 25, 0, 0, 0}},  // L3
        {{55, 30, 15, 0, 0}},  // L4
        {{45, 33, 20, 2, 0}},  // L5
        {{30, 40, 25, 5, 0}},  // L6
        {{19, 30, 35, 15, 1}},  // L7
        {{16, 20, 35, 25, 4}},  // L8
        {{9, 15, 30, 30, 16}},  // L9
        {{5, 10, 20, 40, 25}},  // L10
    }};
};

struct PoolConfig {
    // Copies of *each* champion of that cost. copiesPerTier[cost-1].
    std::array<int, kMaxCostTier> copiesPerTier = {{29, 22, 18, 12, 10}};
};

// "Stage-round" naming, as players see it: round 1 is 1-1, round 4 is 2-1, round 11 is 3-1 ...
struct StageRound {
    int stage = 1;         // 1-based
    int roundInStage = 1;  // 1-based
};

struct MatchConfig {
    int playerCount = kMaxPlayers;
    int motherNatureTicks = Seconds(20);   // how long players have to pick a gift (the phase ends sooner once everybody has)
    int planningTicks = Seconds(30);
    // The LONGEST a Combat phase can last: it must cover CombatConfig::hardLimitTicks plus the linger (normal fights end far earlier). With combatEndsWithFights (the default) the phase
    // ends `combatLingerTicks` after the last fight of the round has ended (never shorter than combatMinTicks), like a normal auto-battler; without
    // it every Combat phase lasts combatTicks.
    int combatTicks = Seconds(125);
    bool combatEndsWithFights = true;
    int combatLingerTicks = Seconds(2);
    int combatMinTicks = Seconds(3);
    int resolutionTicks = Seconds(3);      // the beat after a fight: damage, gold and streaks are shown, then the next round starts
    // Mother Nature (replaces the carousel and augments): rounds 3, 6, 9 ... open with a gift phase instead of a shop. Needs
    // data/mother_nature.json loaded: without it these rounds are ordinary rounds.
    int motherNatureEveryRounds = 3;

    // Stages. Stage 1 is short (1-1, 1-2, 1-3), every later stage has roundsPerStage rounds (2-1 ... 2-7).
    int firstStageRounds = 3;
    int roundsPerStage = 7;
    // PvE rounds: everyone fights the same preset monster board instead of another player. In stage 1 the first
    // firstStagePveRounds rounds are PvE (all three by default); in every later stage the round numbered
    // pveRoundInLaterStages is (X-7 by default; 0 = no PvE after stage 1).
    int firstStagePveRounds = 3;
    int pveRoundInLaterStages = 7;

    // The opening (TFT's first PvE round): before round 1 every player is dealt one random unit of each cost in openingUnitCosts (from the shared
    // pool; free). The shop can also be kept closed for rounds 1..shopClosedOpeningRounds; FEEDBACK V1 (demo 1.1) opens it in every Planning phase: 0.
    int shopClosedOpeningRounds = 0;
    std::vector<int> openingUnitCosts = {1};   // ASSUMED: one 1-cost unit (the designer said "a random unit")

    bool IsMotherNatureRound(int round) const { return motherNatureEveryRounds > 0 && round >= 1 && round % motherNatureEveryRounds == 0; }
    bool IsOpeningRound(int round) const { return round >= 1 && round <= shopClosedOpeningRounds; }

    StageRound StageOf(int round) const {
        const int r = round < 1 ? 1 : round;
        if (r <= firstStageRounds) return {1, r};
        const int after = r - firstStageRounds - 1;
        return {2 + after / roundsPerStage, 1 + after % roundsPerStage};
    }
    bool IsPveRound(int round) const {
        const StageRound sr = StageOf(round);
        if (sr.stage == 1) return sr.roundInStage <= firstStagePveRounds;
        return pveRoundInLaterStages > 0 && sr.roundInStage == pveRoundInLaterStages;
    }
};

// What a player loses when they lose a PvP round: the stage's base damage plus a bit for every unit the winner kept alive.
// (PvE rounds never hurt the player.)
struct DamageConfig {
    // baseDamageByStage[stage-1]; stages beyond the table use the last entry. Stage 1 is all PvE by default, so its entry is unused.
    // FEEDBACK V1 (demo 1.1): losing hurts more as the game goes on, closer to TFT (was {0, 2, 3, 5, 8, 12}).
    std::vector<int> baseDamageByStage = {0, 2, 5, 8, 11, 14, 18};
    int perSurvivingUnit = 1;
};

// Crash safety. The server keeps a snapshot of the whole match taken at the start of every Planning phase, so a crashed
// process can be restored to the beginning of the round (see docs/snapshots.md).
struct SnapshotConfig {
    bool atPlanningStart = true;   // not part of the game's rules: it is left out of GameConfig::ContentHash
};

struct CombatConfig {
    int ticksPerHexMove = 15;     // a unit steps one hex every 15 ticks (0.5 s)
    int repathBackoffTicks = 8;   // if no route exists, wait this long before searching again
    int minDamage = 1;            // damage floor after armor / magic resist

    // Mana generation, on the same scale as the champions' maxMana. The designer's scale is the 100-mana one
    // ("0/60", "0/100"): +10 per basic attack and 1 per 10 damage taken.
    int manaPerAttackMilli = 10000;         // +10 mana per basic attack launched
    int rawDamagePerMana = 10;              // +1 mana per this much PRE-mitigation damage taken
    int manaFromDamagePerTickCapMilli = 20000;  // a unit gains at most this much mana from damage in one tick
    // Mana is not gained while a unit is locked in a cast animation.

    // OVERTIME (all in ticks). A fight runs `regulationTicks` at normal speed; if it is still undecided it goes into overtime, which lasts UNTIL ONE TEAM IS
    // WIPED OUT: every unit's attack speed, movement and mana regeneration run `overtimeSpeed` times faster (cast animations, damage-over-time and status
    // durations keep their normal length). There are no timeouts and no draws by time. `hardLimitTicks` is only a safety net against a fight that can
    // never end (two teams that cannot hurt each other): when it is reached the fight is decided deterministically -- more surviving units, then more
    // total HP, then a coin from the fight's seed -- so a fight ALWAYS has a winner. (Only a mutual wipe-out on the same tick is a draw.)
    int regulationTicks = Seconds(30);
    int overtimeSpeed = 4;
    int hardLimitTicks = Seconds(120);

    // Presentation defaults (see CombatEvent: windup / flight). Per champion `stats.attackWindup` / `stats.projectileSpeed` and per ability `windup` override them.
    // They never change what happens in a fight, only the timings a viewer is told to animate with.
    int defaultAttackWindupTicks = 6;              // 0.2 s
    int defaultRangedProjectileSpeedMilli = 12000; // 12 hexes per second, for champions with range >= 2
    int defaultCastWindupTicks = 9;                // 0.3 s

    int critBonusPercent = 21;    // a crit deals base damage + 21%
    int dotSpreadIntervalTicks = 10;  // spread basic attacks (attackSpreadTicks) hit once per this many ticks
};

struct GameConfig {
    PlayerConfig player;
    ShopConfig shop;
    PoolConfig pool;
    MatchConfig match;
    CombatConfig combat;
    DamageConfig damage;
    SnapshotConfig snapshot;

    // Returns false and fills *error (if provided) when a value would break an invariant.
    bool Validate(std::string* error = nullptr) const;

    // Damage the loser of a PvP round takes in `round`, given how many units the winner has left alive.
    int PlayerDamage(int round, int winnerSurvivors) const;

    // Hash of every value. A snapshot records it: restoring under a different configuration would silently change
    // the rules mid-match.
    std::uint64_t ContentHash() const;
};

}  // namespace w2f
