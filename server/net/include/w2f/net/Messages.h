#pragma once

// Builders for every server -> client message (each returns one JSON text). They only READ engine state through its public,
// const interface; nothing here can change a match. What each message contains -- and therefore who may be sent it -- is
// documented in docs/network-protocol.md. The privacy rule is enforced by construction:
//   * PrivateState / Income / PveDrop / UnitEvent messages carry one player's gold, shop, bench and bag: the server sends them
//     to that player's connection ONLY;
//   * everything else (public state, phase, damage, eliminations, combat logs) is information every player may see.

#include <string>
#include <string_view>
#include <vector>

#include "w2f/ChampionDatabase.h"
#include "w2f/Combat.h"
#include "w2f/Config.h"
#include "w2f/Item.h"
#include "w2f/MatchManager.h"
#include "w2f/Pve.h"
#include "w2f/Text.h"
#include "w2f/Trait.h"
#include "w2f/net/Protocol.h"

namespace w2f::net::msg {

// `seats` counts every seat; `bots` of them (the last ones) are AI, so `connected` humans fill `seats - bots` of them.
std::string Welcome(PlayerId seat, std::string_view token, bool reconnected, int connected, int seats, int bots, bool matchRunning);
std::string Lobby(const std::vector<PlayerId>& seatsTaken, int seats, int bots);
// What the ids in every other message mean: champions (the PvE monsters too, flagged `monster`), items and traits, straight from the loaded
// data. Public: the same for everyone, answered to `get_catalog`. Any of the optional databases may be null.
// `combat` supplies the presentation defaults (windup / projectile speed), so every champion is given its EFFECTIVE timings.
std::string Catalog(const ChampionDatabase& champions, const ItemDatabase* items, const TraitDatabase* traits, const EncounterDatabase* encounters, const CombatConfig& combat,
                    const TextTable* text = nullptr);
std::string Error(std::string_view code, std::string_view detail, bool hasId = false, long long id = 0);
std::string Result(const Command& command, ActionResult result);
std::string Pong(bool hasId, long long id);
// Matchmaking (revision 7, QueueServer only). `state` "idle" | "searching"; `reason` (idle only, may be empty): "match_over" | "left_match" | "cancelled".
struct QueueInfo {
    bool searching = false;
    QueueMode mode = QueueMode::Bots;
    int inQueue = 0;        // players searching in this mode, you included
    long long waitedMs = 0;
    long long fillMs = 0;   // after this long a normal queue starts with bots in the empty seats (0: never waits)
    int seats = 0;
    int online = 0;         // connections on the server
    int matches = 0;        // matches running
    std::string reason;
};
std::string QueueStatus(const QueueInfo& info);
std::string MatchFound(QueueMode mode, int humans, int bots);

// `motherNatureEvery` = every how many rounds Mother Nature comes (0 = no Mother Nature data loaded: never). `botSeats` = the AI players' seats.
std::string MatchStarted(const GameConfig& config, int seats, PlayerId you, int motherNatureEvery, const std::vector<PlayerId>& botSeats = {});
// `motherNatureRound`: this round is one of Mother Nature's: a gift phase first, and no shop for the whole round.
// `shopClosed`: no shop this round (Mother Nature's, or the opening round: the free unit was dealt at the start).
std::string Phase(const GameConfig& config, MatchPhase phase, int round, int ticksElapsed, int durationTicks, std::uint64_t serverTick, bool motherNatureRound, bool shopClosed);

// One player's full private state (gold, xp, shop, bench, board, bag ...).
std::string PrivateState(const MatchManager& match, PlayerId player);
// Everyone's public state: health, level, streak, alive/placement and the BOARD (the bench and gold stay private).
std::string PublicState(const MatchManager& match);

struct FightSummary {
    int index = 0;
    PlayerId home = kInvalidPlayerId;
    PlayerId away = kInvalidPlayerId;
    bool ghost = false;
    bool monsters = false;
    std::uint32_t encounter = 0;
    int events = 0;
};
FightSummary Summarize(int index, const CombatOutcome& outcome);
std::string CombatSummary(int round, const std::vector<FightSummary>& fights);
// The whole fight as a timestamped event stream; see docs/network-protocol.md for the column layout.
// `match` (optional) lets the message name the items the fighters carry (`unit_items`: unit id -> item ids), read from the boards of the two seats: the log's Spawn rows carry none.
std::string Combat(int round, int index, const CombatOutcome& outcome, const MatchManager* match = nullptr);

std::string PlayerDamaged(PlayerId player, int damage, int healthAfter);
std::string PlayerEliminated(PlayerId player, int placement);
std::string MatchOver(const MatchManager& match);
std::string Income(PlayerId player, int round, const IncomeBreakdown& income);
std::string PveDropMsg(const PveDrop& drop);
// Mother Nature (private to the player): what is on offer, and what they took.
std::string GiftsOffered(const std::vector<GiftOffer>& offers, const MotherNatureDatabase* data);
std::string GiftPicked(int index, const GiftOffer& gift, const MotherNatureDatabase* data, bool automatic, int goldConverted);
std::string UnitBought(const UnitInstance& unit, int goldSpent);
std::string UnitSold(const UnitInstance& unit, int goldGained);
std::string UnitMoved(const UnitMove& move);
std::string UnitMerged(const UnitMerge& merge);
std::string ItemEquipped(const UnitInstance& unit, ItemId item);
std::string ItemUnequipped(const UnitInstance& unit, ItemId item);
// Two components of the item bag were combined (the bag itself is in the next `state`).
std::string BagItemsCombined(ItemId first, ItemId second, ItemId result);
std::string ItemsCombined(const UnitInstance& unit, const ItemCombination& combination);
std::string ItemConsumed(const UnitInstance& unit, ItemId consumable, const std::vector<ItemId>& returned);
// (revision 5) Trait system v2, private to the player: a trait asks for a choice / it was made; what the traits paid after a combat.
std::string TraitChoiceOffered(const TraitChoice& choice);
std::string TraitChoiceResolved(const TraitChoice& choice, int index, bool automatic);
std::string TraitRewardsMsg(const TraitRewards& rewards, int starDustTotal);
std::string ChampionUnlocked(ChampionId champion);

}  // namespace w2f::net::msg
