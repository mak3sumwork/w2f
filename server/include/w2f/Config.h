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
    // TFT rule: you may field at most `level` units on the board. Off by default (any of the
    // 28 cells may be used) until the design decision is made.
    bool limitBoardToLevel = false;

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
    int combatTicks = Seconds(40);
    int resolutionTicks = Seconds(5);
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

    bool IsMotherNatureRound(int round) const { return motherNatureEveryRounds > 0 && round >= 1 && round % motherNatureEveryRounds == 0; }

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
    std::vector<int> baseDamageByStage = {0, 2, 3, 5, 8, 12};
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
