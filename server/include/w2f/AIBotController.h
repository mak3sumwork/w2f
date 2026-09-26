#pragma once

// A simple planning-phase player, so full matches can be simulated headless (and so a
// lobby can be topped up with bots later). It uses ONLY the public MatchManager action API,
// exactly like a networked human would, so it can't bypass any rule.
//
// Once per round, the first time it is ticked during Planning, it plays roughly the way a TFT player does:
//   1. Economy: it banks gold for interest ((stage - 1) x 10, at most 50; nothing once it is at low health), buys XP with everything
//      above 50, and buys XP up to the level it wants for the stage (the board holds as many units as its level).
//   2. Buys units from its shop, best first: another copy of a champion it owns (a star-up) or a champion that brings a synergy closer
//      (the traits' breakpoints) beat plain strength; while it owns fewer units than the board size plus two, it buys anything. It spends
//      into its savings only to complete a star-up. When its roster is full it SELLS its weakest lone unit to make room for a purchase
//      that is worth more (see MakeRoomFor).
//   3. Rerolls the shop with the gold above its savings (from stage 2 on) and buys again.
//   4. Fields the best `level` units: strength (cost x star) plus the synergy each unit adds to the team. Tanks take the front rows,
//      everyone else the back; a strong bench unit replaces the weakest fielded one.
//   5. Equips every item in its bag on a unit that is fielded: defensive items on tanks, damage items on the damage dealers,
//      preferring the strongest units and pairs of components that combine into a finished item.
//
// Its only randomness is the shop-slot order (the tie-break between equally good offers), from its own seeded stream, so a match with
// bots is exactly as reproducible as one without.

#include <array>
#include <cstdint>

#include "w2f/MatchManager.h"
#include "w2f/Rng.h"
#include "w2f/Trait.h"
#include "w2f/Types.h"

namespace w2f {

struct BotProfile {
    int levelUpAboveGold = 50;  // buy XP whenever gold exceeds this
    int reserveGold = 0;        // never spend units-money below this (on top of the interest savings)
    // Savings: (stage - 1) x interestStepGold, at most maxInterestGold (TFT's interest stops at 50); none once health <= lowHealth.
    int interestStepGold = 10;
    int maxInterestGold = 50;
    int lowHealth = 50;
    int maxRerollsPerRound = 8;
    int benchSlack = 2;         // it buys any unit while it owns fewer than (board size + benchSlack)
    // The level it wants to have reached, by stage (stage 1 first; later stages use the last entry).
    std::array<int, 7> targetLevelByStage = {{3, 5, 6, 7, 8, 9, 10}};
};

class AIBotController {
public:
    // `traits` (optional, must outlive the bot) lets it see synergy breakpoints; without it it judges units by strength alone.
    AIBotController(PlayerId player, std::uint64_t matchSeed, const BotProfile& profile = BotProfile{}, const TraitDatabase* traits = nullptr);

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
    void AnswerTraitChoice(MatchManager& match);
    void BuyExperience(MatchManager& match);
    void BuyUnits(MatchManager& match);
    void BuyWantedUnits(MatchManager& match, int keepGold);   // buys from the current shop until nothing worth it is left
    void LevelToTarget(MatchManager& match, int keepGold);
    int KeepGold(const MatchManager& match) const;             // the savings it will not spend on anything but a star-up
    void ArrangeBoard(MatchManager& match);
    void EquipItems(MatchManager& match);
    // The unit the bot would part with first, or nullptr when it has nothing it is willing to sell (it never sells a star-2+ unit or
    // one of a pair of copies about to merge; units that carry items go last, bench units before board units, cheapest first).
    const UnitInstance* WeakestSellable(const UnitRoster& roster) const;
    // The unit to sell so that `offered` fits, or nullptr when it should not be sold for (`offered` is worth no more than the weakest
    // unit, unless it is one more copy of a champion the bot already has).
    const UnitInstance* SellCandidateFor(const UnitRoster& roster, const ChampionDefinition& offered) const;
    // Sells that unit. True once the roster can receive `offered`.
    bool MakeRoomFor(MatchManager& match, const ChampionDefinition& offered);

    PlayerId player_;
    Rng rng_;
    BotProfile profile_;
    const TraitDatabase* traits_;
    int lastActedRound_ = 0;
};

}  // namespace w2f
