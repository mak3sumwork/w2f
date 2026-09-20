#include "w2f/Config.h"

#include "w2f/Hash.h"

namespace w2f {

namespace {
bool Fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}
}  // namespace

bool GameConfig::Validate(std::string* error) const {
    // Player
    if (player.startingHealth <= 0) return Fail(error, "startingHealth must be > 0");
    if (player.startingGold < 0) return Fail(error, "startingGold must be >= 0");
    if (player.buyXpCost < 0 || player.buyXpAmount <= 0) return Fail(error, "invalid buy-XP values");
    if (player.passiveXpPerRound < 0 || player.firstPassiveXpRound < 1) return Fail(error, "invalid passive XP values");
    for (int need : player.xpToNextLevel) {
        if (need <= 0) return Fail(error, "xpToNextLevel entries must be > 0");
    }
    for (int base : player.baseIncomeByRound) {
        if (base < 0) return Fail(error, "baseIncomeByRound entries must be >= 0");
    }
    if (player.baseIncomeAfterTable < 0) return Fail(error, "baseIncomeAfterTable must be >= 0");
    if (player.interestStepGold <= 0 || player.interestPerStep < 0 || player.interestCap < 0) {
        return Fail(error, "invalid interest values");
    }
    int lastStreak = 0;
    for (const StreakBonus& b : player.streakBonuses) {
        if (b.minStreak <= lastStreak) return Fail(error, "streakBonuses must be strictly ascending and > 0");
        if (b.bonusGold < 0) return Fail(error, "streak bonus gold must be >= 0");
        lastStreak = b.minStreak;
    }

    // Shop
    if (shop.slotCount <= 0) return Fail(error, "slotCount must be > 0");
    if (shop.rerollCost < 0) return Fail(error, "rerollCost must be >= 0");
    for (const auto& row : shop.dropRatesByLevel) {
        int total = 0;
        for (int weight : row) {
            if (weight < 0) return Fail(error, "drop rate weights must be >= 0");
            total += weight;
        }
        if (total <= 0) return Fail(error, "each level's drop rates must sum to > 0");
    }

    // Pool
    for (int copies : pool.copiesPerTier) {
        if (copies <= 0) return Fail(error, "copiesPerTier entries must be > 0");
    }

    // Match
    if (match.playerCount < 2 || match.playerCount > kMaxPlayers) return Fail(error, "playerCount must be in [2, kMaxPlayers]");
    if (match.draftTicks <= 0 || match.planningTicks <= 0 || match.combatTicks <= 0 || match.resolutionTicks <= 0) {
        return Fail(error, "phase durations must be > 0 ticks");
    }
    if (match.draftRoundInterval <= 0) return Fail(error, "draftRoundInterval must be > 0");
    if (match.firstStageRounds < 1 || match.roundsPerStage < 1) return Fail(error, "stages need at least one round");
    if (match.firstStagePveRounds < 0 || match.firstStagePveRounds > match.firstStageRounds) {
        return Fail(error, "firstStagePveRounds must be between 0 and firstStageRounds");
    }
    if (match.pveRoundInLaterStages < 0 || match.pveRoundInLaterStages > match.roundsPerStage) {
        return Fail(error, "pveRoundInLaterStages must be between 0 (none) and roundsPerStage");
    }

    // Combat
    if (combat.ticksPerHexMove < 1) return Fail(error, "ticksPerHexMove must be >= 1");
    if (combat.repathBackoffTicks < 1) return Fail(error, "repathBackoffTicks must be >= 1");
    if (combat.minDamage < 0) return Fail(error, "minDamage must be >= 0");
    if (combat.manaPerAttackMilli < 0 || combat.manaFromDamagePerTickCapMilli < 0) return Fail(error, "mana gain values must be >= 0");
    if (combat.rawDamagePerMana < 1) return Fail(error, "rawDamagePerMana must be >= 1");
    if (combat.critBonusPercent < 0) return Fail(error, "critBonusPercent must be >= 0");
    if (combat.dotSpreadIntervalTicks < 1) return Fail(error, "dotSpreadIntervalTicks must be >= 1");

    // Player damage
    if (damage.baseDamageByStage.empty()) return Fail(error, "baseDamageByStage needs at least one entry");
    for (int base : damage.baseDamageByStage) {
        if (base < 0) return Fail(error, "baseDamageByStage entries must be >= 0");
    }
    if (damage.perSurvivingUnit < 0) return Fail(error, "perSurvivingUnit must be >= 0");

    return true;
}

int GameConfig::PlayerDamage(int round, int winnerSurvivors) const {
    const std::size_t stageIndex = static_cast<std::size_t>(match.StageOf(round).stage - 1);
    const std::size_t index = stageIndex < damage.baseDamageByStage.size() ? stageIndex : damage.baseDamageByStage.size() - 1;
    return damage.baseDamageByStage[index] + (winnerSurvivors < 0 ? 0 : winnerSurvivors) * damage.perSurvivingUnit;
}

std::uint64_t GameConfig::ContentHash() const {
    Fnv1a h;
    h.AddInt(player.startingHealth);
    h.AddInt(player.startingGold);
    h.Add(player.limitBoardToLevel ? 1 : 0);
    h.AddInt(player.buyXpCost);
    h.AddInt(player.buyXpAmount);
    for (int v : player.xpToNextLevel) h.AddInt(v);
    h.AddInt(player.firstPassiveXpRound);
    h.AddInt(player.passiveXpPerRound);
    h.AddInt(static_cast<std::int64_t>(player.baseIncomeByRound.size()));
    for (int v : player.baseIncomeByRound) h.AddInt(v);
    h.AddInt(player.baseIncomeAfterTable);
    h.AddInt(player.interestStepGold);
    h.AddInt(player.interestPerStep);
    h.AddInt(player.interestCap);
    h.AddInt(static_cast<std::int64_t>(player.streakBonuses.size()));
    for (const StreakBonus& b : player.streakBonuses) {
        h.AddInt(b.minStreak);
        h.AddInt(b.bonusGold);
    }

    h.AddInt(shop.slotCount);
    h.AddInt(shop.rerollCost);
    for (const auto& row : shop.dropRatesByLevel) {
        for (int v : row) h.AddInt(v);
    }

    for (int v : pool.copiesPerTier) h.AddInt(v);

    h.AddInt(match.playerCount);
    h.AddInt(match.draftTicks);
    h.AddInt(match.planningTicks);
    h.AddInt(match.combatTicks);
    h.AddInt(match.resolutionTicks);
    h.AddInt(match.draftRoundInterval);
    h.AddInt(match.firstStageRounds);
    h.AddInt(match.roundsPerStage);
    h.AddInt(match.firstStagePveRounds);
    h.AddInt(match.pveRoundInLaterStages);

    for (int v : {combat.ticksPerHexMove, combat.repathBackoffTicks, combat.minDamage, combat.manaPerAttackMilli,
                  combat.rawDamagePerMana, combat.manaFromDamagePerTickCapMilli, combat.critBonusPercent,
                  combat.dotSpreadIntervalTicks}) {
        h.AddInt(v);
    }

    h.AddInt(static_cast<std::int64_t>(damage.baseDamageByStage.size()));
    for (int v : damage.baseDamageByStage) h.AddInt(v);
    h.AddInt(damage.perSurvivingUnit);
    return h.value;
}

}  // namespace w2f
