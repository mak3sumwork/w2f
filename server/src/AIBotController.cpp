#include "w2f/AIBotController.h"

#include <numeric>
#include <vector>

namespace w2f {

namespace {
// Fill order for a row: centre outward, so formations are compact rather than hugging one edge.
constexpr int kColumnOrder[kBoardColumns] = {3, 2, 4, 1, 5, 0, 6};
// Row preference (board frame: y = 0 back row, y = kBoardRows-1 front row).
constexpr int kFrontFirst[kBoardRows] = {3, 2, 1, 0};
constexpr int kBackFirst[kBoardRows] = {0, 1, 2, 3};
}  // namespace

AIBotController::AIBotController(PlayerId player, std::uint64_t matchSeed, const BotProfile& profile)
    : player_(player), rng_(matchSeed, kRngStreamBotBase + player), profile_(profile) {}

void AIBotController::Tick(MatchManager& match) {
    if (match.Phase() != MatchPhase::Planning) return;
    if (match.Round() == lastActedRound_) return;  // already played this round's planning
    const PlayerState* self = match.Players().Get(player_);
    if (self == nullptr || !self->IsAlive()) return;
    lastActedRound_ = match.Round();

    BuyExperience(match);
    BuyUnits(match);
    PlaceUnits(match);
}

void AIBotController::BuyExperience(MatchManager& match) {
    const PlayerState* self = match.Players().Get(player_);
    while (self->Gold() > profile_.levelUpAboveGold) {
        if (match.TryBuyXp(player_) != ActionResult::Ok) break;  // max level (or anything unexpected)
    }
}

void AIBotController::BuyUnits(MatchManager& match) {
    const PlayerState* self = match.Players().Get(player_);
    const std::size_t slotCount = self->Shop().Slots().size();

    std::vector<std::size_t> order(slotCount);
    std::iota(order.begin(), order.end(), std::size_t{0});
    rng_.Shuffle(order);  // "random units": the visit order is random

    for (std::size_t slot : order) {
        const ChampionDefinition* offered = self->Shop().Slots()[slot];
        if (offered == nullptr) continue;
        if (self->Gold() - offered->cost < profile_.reserveGold) continue;  // not spare gold
        match.TryBuyShopUnit(player_, slot);  // RosterFull / NotEnoughGold are simply skipped
    }
}

void AIBotController::PlaceUnits(MatchManager& match) {
    const PlayerState* self = match.Players().Get(player_);
    const UnitRoster& roster = self->Roster();

    for (int slot = 0; slot < kBenchSlots; ++slot) {
        if (roster.BoardCount() >= roster.BoardCapacity()) return;  // board is at its unit limit
        const UnitInstance* unit = roster.BenchAt(slot);
        if (unit == nullptr) continue;

        const int* rows = unit->champion->role == ChampionRole::Tank ? kFrontFirst : kBackFirst;
        bool placed = false;
        for (int r = 0; r < kBoardRows && !placed; ++r) {
            for (int c = 0; c < kBoardColumns && !placed; ++c) {
                const int y = rows[r];
                const int x = kColumnOrder[c];
                if (roster.BoardAt(x, y) != nullptr) continue;
                placed = match.TryMoveUnit(player_, unit->id, LocationType::Board, x, y) == ActionResult::Ok;
            }
        }
        if (!placed) return;  // board completely full
    }
}

}  // namespace w2f
