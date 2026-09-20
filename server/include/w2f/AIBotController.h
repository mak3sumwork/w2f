#pragma once

// A simple planning-phase player, so full matches can be simulated headless (and so a
// lobby can be topped up with bots later). It uses ONLY the public MatchManager action API,
// exactly like a networked human would, so it can't bypass any rule.
//
// Once per round, the first time it is ticked during Planning, it:
//   1. Buys XP while it holds more than `levelUpAboveGold` gold.
//   2. Buys random units from its shop while it can afford them (keeping `reserveGold`).
//   3. Moves benched units onto the board while the board has room: tanks take the front
//      row (falling back toward the back), everyone else takes the back row (falling forward).
//
// Its only randomness is the shop-slot order, from its own seeded stream, so a match with
// bots is exactly as reproducible as one without.

#include <cstdint>

#include "w2f/MatchManager.h"
#include "w2f/Rng.h"
#include "w2f/Types.h"

namespace w2f {

struct BotProfile {
    int levelUpAboveGold = 50;  // buy XP whenever gold exceeds this
    int reserveGold = 0;        // never spend units-money below this
};

class AIBotController {
public:
    AIBotController(PlayerId player, std::uint64_t matchSeed, const BotProfile& profile = BotProfile{});

    // Call every match tick (or at least once per Planning phase). Cheap when there's nothing to do.
    void Tick(MatchManager& match);

    PlayerId Player() const { return player_; }

    // A bot lives OUTSIDE the match (it is just another client), so a match snapshot does not contain it; whoever
    // snapshots the match saves and restores its bots with these.
    struct State {
        RngState rng;
        int lastActedRound = 0;
    };
    State GetState() const { return State{rng_.GetState(), lastActedRound_}; }
    void SetState(const State& state) {
        rng_.SetState(state.rng);
        lastActedRound_ = state.lastActedRound;
    }

private:
    void BuyExperience(MatchManager& match);
    void BuyUnits(MatchManager& match);
    void PlaceUnits(MatchManager& match);

    PlayerId player_;
    Rng rng_;
    BotProfile profile_;
    int lastActedRound_ = 0;
};

}  // namespace w2f
