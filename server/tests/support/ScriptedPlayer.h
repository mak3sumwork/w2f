#pragma once

// Stand-in for a human seat in headless matches. Deliberately different from AIBotController
// (it rerolls, buys everything it can afford, ignores unit roles when placing) so a match mixes
// behaviours. Uses only the public MatchManager action API.

#include "w2f/MatchManager.h"

namespace sample {

class ScriptedPlayer {
public:
    explicit ScriptedPlayer(w2f::PlayerId id) : id_(id) {}

    void Tick(w2f::MatchManager& match) {
        using namespace w2f;
        if (match.Phase() != MatchPhase::Planning || match.Round() == lastRound_) return;
        const PlayerState* self = match.Players().Get(id_);
        if (self == nullptr || !self->IsAlive()) return;
        lastRound_ = match.Round();

        if (self->Gold() >= 30) match.TryBuyXp(id_);
        BuyAll(match, *self);
        if (self->Gold() >= 12) {
            match.TryRerollShop(id_);
            BuyAll(match, *self);
        }
        // Keep the roster from clogging: sell the oldest unit once there are many.
        if (self->Roster().Count() >= 14) match.TrySellUnit(id_, self->Roster().Units().front().id);
        Place(match, *self);
    }

private:
    void BuyAll(w2f::MatchManager& match, const w2f::PlayerState& self) {
        for (std::size_t slot = 0; slot < self.Shop().Slots().size(); ++slot) match.TryBuyShopUnit(id_, slot);
    }

    // First free board cell, row-major, until the board is at its limit.
    void Place(w2f::MatchManager& match, const w2f::PlayerState& self) {
        using namespace w2f;
        for (int slot = 0; slot < kBenchSlots; ++slot) {
            const UnitRoster& roster = self.Roster();
            if (roster.BoardCount() >= roster.BoardCapacity()) return;
            const UnitInstance* unit = roster.BenchAt(slot);
            if (unit == nullptr) continue;
            const UnitId id = unit->id;
            for (int y = 0; y < kBoardRows; ++y) {
                for (int x = 0; x < kBoardColumns; ++x) {
                    if (roster.BoardAt(x, y) == nullptr &&
                        match.TryMoveUnit(id_, id, LocationType::Board, x, y) == ActionResult::Ok) {
                        y = kBoardRows;  // placed: leave both loops
                        break;
                    }
                }
            }
        }
    }

    w2f::PlayerId id_;
    int lastRound_ = 0;
};

}  // namespace sample
