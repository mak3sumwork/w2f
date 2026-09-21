#pragma once

// A simple planning-phase player, so full matches can be simulated headless (and so a
// lobby can be topped up with bots later). It uses ONLY the public MatchManager action API,
// exactly like a networked human would, so it can't bypass any rule.
//
// Once per round, the first time it is ticked during Planning, it:
//   1. Buys XP while it holds more than `levelUpAboveGold` gold.
//   2. Buys units from its shop while it can afford them (keeping `reserveGold`): copies of champions it already owns
//      first (they lead to a star-up), the rest in random order. When its roster is full it SELLS its weakest unit to make room
//      for a purchase that is worth more (see MakeRoomFor).
//   3. Moves benched units onto the board while the board has room: tanks take the front
//      row (falling back toward the back), everyone else takes the back row (falling forward).
//   4. Equips every item in its bag on a unit that is fielded: defensive items on tanks, damage items on the damage dealers,
//      preferring the strongest units and pairs of components that combine into a finished item.
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
    void PickGift(MatchManager& match);
    void BuyExperience(MatchManager& match);
    void BuyUnits(MatchManager& match);
    void PlaceUnits(MatchManager& match);
    void EquipItems(MatchManager& match);
    // The unit the bot would part with first, or nullptr when it has nothing it is willing to sell (it never sells a star-2+ unit or
    // one of a pair of copies about to merge; units that carry items go last, bench units before board units, cheapest first).
    const UnitInstance* WeakestSellable(const UnitRoster& roster) const;
    // A purchase of `offered` does not fit: sells the weakest unit if `offered` is worth more (or is one more copy of a champion the
    // bot already has). True once the roster can receive `offered`.
    bool MakeRoomFor(MatchManager& match, const ChampionDefinition& offered);

    PlayerId player_;
    Rng rng_;
    BotProfile profile_;
    int lastActedRound_ = 0;
};

}  // namespace w2f
