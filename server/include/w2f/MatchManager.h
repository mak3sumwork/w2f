#pragma once

// Top-level authority for one match: owns the pool and all players, and drives the phase
// state machine. Fully headless -- advanced only by Tick() at kTicksPerSecond.
//
//   Start (deals the opening units) -> [MotherNature] -> Planning -> Combat -> Resolution -+-> next round (income, back to top)
//            (every 3rd round: a free gift        (no shop in      +-> MatchOver (<= 1 player alive)
//             instead of a shop)                   those rounds)
//
// All player actions go through the Try* methods below, which enforce phase rules and
// return an ActionResult. Nothing here allocates per tick.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "w2f/ChampionDatabase.h"
#include "w2f/Combat.h"
#include "w2f/Config.h"
#include "w2f/MotherNature.h"
#include "w2f/PlayerEvents.h"
#include "w2f/PlayerManager.h"
#include "w2f/Pve.h"
#include "w2f/Rng.h"
#include "w2f/SharedChampionPool.h"
#include "w2f/Snapshot.h"

namespace w2f {

class TraitDatabase;
struct TraitDefinition;
struct TraitCountUnit;

// ---- Trait system v2: the decisions and rewards the traits hand a player (see Trait.h) ----

// A choice waiting for a player: Hexagon offers modules (pick one), Najmi offers a star-dust prototype (take it, or decline and bank the dust).
enum class TraitChoiceKind : std::uint8_t { None, Module, Prototype };
struct TraitChoice {
    TraitChoiceKind kind = TraitChoiceKind::None;
    std::uint32_t trait = 0;              // the trait asking
    int tier = 0;                         // Module: the module tier (1..3). Prototype: the cash-out tier = hundreds of star dust banked (1..6)
    std::vector<std::uint32_t> options;   // Module: module ids. Prototype: item ids (pick one)
    int bonusGold = 0;                    // Prototype cash-out: gold paid along with the picked item
    std::vector<std::uint32_t> bonusItems;   // Prototype cash-out: items given along with the picked one (the bigger cash-outs)
    bool Pending() const { return kind != TraitChoiceKind::None; }
    bool operator==(const TraitChoice& o) const {
        return kind == o.kind && trait == o.trait && tier == o.tier && options == o.options && bonusGold == o.bonusGold && bonusItems == o.bonusItems;
    }
};

// What the traits paid a player after a combat (Resolution): Selini's XP and gold, Najmi's star dust, Hexagon's Mining Drill, Phaisa's Rift Herald.
struct TraitRewards {
    int xp = 0;
    int gold = 0;
    int starDust = 0;                       // gained this combat (the bank is in PlayerState::Traits())
    ChampionId unit = kInvalidChampionId;   // a unit given to the bench
    bool Any() const { return xp != 0 || gold != 0 || starDust != 0 || unit != kInvalidChampionId; }
};

constexpr const char* ToString(MatchPhase p) {
    switch (p) {
        case MatchPhase::NotStarted: return "NotStarted";
        case MatchPhase::MotherNature: return "MotherNature";
        case MatchPhase::Planning: return "Planning";
        case MatchPhase::Combat: return "Combat";
        case MatchPhase::Resolution: return "Resolution";
        case MatchPhase::MatchOver: return "MatchOver";
    }
    return "Unknown";
}

// Observer hook for the eventual UE5 wrapper / network layer. All methods optional.
// Also carries every per-player unit event (OnUnitBought / Sold / Moved / Merged) inherited
// from IPlayerListener, so one registered listener sees the whole match.
// Callbacks run synchronously inside Tick() or the Try* action that caused them; do not call
// back into the MatchManager from them.
class IMatchListener : public IPlayerListener {
public:
    ~IMatchListener() override = default;
    // Fired after the phase's entry logic has run (shops refreshed, combat resolved, ...).
    virtual void OnPhaseChanged(MatchPhase /*from*/, MatchPhase /*to*/, int /*round*/) {}
    // Fired during Resolution entry, before OnPhaseChanged(Resolution).
    virtual void OnPlayerEliminated(PlayerId /*player*/, int /*placement*/) {}
    virtual void OnMatchEnded(PlayerId /*winner*/) {}
    // Fired once per fight when the Combat phase begins, before OnPhaseChanged(Combat). The log
    // holds the ENTIRE fight (combat-local ticks, tick 0 = start of the phase). A viewer just
    // queues and plays it; it must not simulate or predict anything itself.
    virtual void OnCombatSimulated(int /*round*/, const CombatOutcome& /*outcome*/) {}
    // Fired during Resolution entry: a player lost a PvP round and took `damage` (health is what is left; <= 0 means
    // they are about to be eliminated, and OnPlayerEliminated follows).
    virtual void OnPlayerDamaged(PlayerId /*player*/, int /*damage*/, int /*healthAfter*/) {}
    // Fired during Resolution entry: a player beat the monsters and was given `drop`. (A Champion drop is also announced
    // through OnUnitBought with 0 gold spent, like any unit that enters a roster.)
    virtual void OnPveDrop(PlayerId /*player*/, const PveDrop& /*drop*/) {}
    // Mother Nature opened her phase: `offers` are what this player may choose from (private information; the unit copies on offer are
    // checked out of the shared pool until the pick). Fires for every living player, in seat order, before OnPhaseChanged(MotherNature). An empty
    // list means nothing could be offered.
    virtual void OnGiftsOffered(PlayerId /*player*/, const std::vector<GiftOffer>& /*offers*/) {}
    // A player took offer number `index` (`automatic` = the phase ran out and the first offer was taken for them). `goldConverted` > 0: a Unit gift
    // could not be received (no room, no merge) and was paid as gold equal to its cost instead. A Unit gift is also announced through OnUnitBought
    // with 0 gold spent, like any unit that enters a roster.
    virtual void OnGiftPicked(PlayerId /*player*/, int /*index*/, const GiftOffer& /*gift*/, bool /*automatic*/, int /*goldConverted*/) {}
    // Fired when Planning begins (shops already refreshed, before OnPhaseChanged): the crash-recovery snapshot of the
    // whole match as of the start of the round. Persist it; MatchManager::Restore turns it back into this exact moment.
    virtual void OnAutoSnapshot(int /*round*/, const std::vector<std::uint8_t>& /*snapshot*/) {}
    // Trait system v2. A trait asks the player to choose (private information).
    virtual void OnTraitChoiceOffered(PlayerId /*player*/, const TraitChoice& /*choice*/) {}
    // The choice was made: `index` into its options, -1 = declined (a prototype: the star dust stays banked). `automatic` = the time ran out.
    virtual void OnTraitChoiceResolved(PlayerId /*player*/, const TraitChoice& /*choice*/, int /*index*/, bool /*automatic*/) {}
    // Fired during Resolution entry, after the round's results: what the player's traits paid out.
    virtual void OnTraitRewards(PlayerId /*player*/, const TraitRewards& /*rewards*/) {}
    // An unlockable champion (Hexa) can now appear in the player's shop.
    virtual void OnChampionUnlocked(PlayerId /*player*/, ChampionId /*champion*/) {}
};

class MatchManager : private IPlayerListener {  // private: it only re-broadcasts player events
public:
    // Returns nullptr (and fills *error) if the config is invalid.
    // `database` must outlive the match. `simulator` may be null: every fight is then a
    // scoreless draw with an empty log (use CombatSimulator for real fights). Same (config, database, seed, simulator behaviour, inputs) => same match.
    // `items` (optional, must outlive the match) validates item ids and gives units their equipment; the combat simulator
    // needs the same database to apply it (see CombatSimulator's constructor).
    // `encounters` (optional, must outlive the match) holds the PvE monster boards and drop tables. Without it a PvE round
    // is a round nobody fights: no result, no drop.
    // `motherNature` (optional, must outlive the match) holds the gifts. Without it the rounds `motherNatureEveryRounds` would make Mother Nature's
    // are ordinary rounds (shop open, no gift phase).
    static std::unique_ptr<MatchManager> Create(const GameConfig& config, const ChampionDatabase& database,
                                                std::uint64_t seed, std::unique_ptr<ICombatSimulator> simulator,
                                                std::string* error = nullptr, const ItemDatabase* items = nullptr,
                                                const EncounterDatabase* encounters = nullptr,
                                                const MotherNatureDatabase* motherNature = nullptr, const TraitDatabase* traits = nullptr);

    // Owns objects that reference each other (players hold a pointer back to it), so it is pinned in memory.
    MatchManager(const MatchManager&) = delete;
    MatchManager& operator=(const MatchManager&) = delete;

    void Start();  // NotStarted -> round 1
    void Tick();   // Advance one fixed simulation step.

    // ---- Observers (non-owning; must outlive the match or be removed) ----
    void AddListener(IMatchListener* listener);
    void RemoveListener(IMatchListener* listener);

    // ---- State ----
    MatchPhase Phase() const { return phase_; }
    int Round() const { return round_; }
    int TicksInPhase() const { return ticksInPhase_; }
    // How long the current phase lasts in total. Fixed by the config, except Combat: with `combatEndsWithFights` it is as long as the round's longest
    // fight plus a short linger (never more than `combatTicks`), and is known from the moment the phase begins.
    int PhaseTicks() const { return PhaseDuration(phase_); }
    int TicksRemainingInPhase() const;
    bool IsFinished() const { return phase_ == MatchPhase::MatchOver; }
    PlayerId Winner() const { return winner_; }  // kInvalidPlayerId until finished
    const std::vector<Matchup>& CurrentMatchups() const { return matchups_; }
    // This round's fights, logs included (valid from the start of Combat until the next Combat).
    // Lets a reconnecting client fetch the stream it missed.
    const std::vector<CombatOutcome>& CurrentCombatOutcomes() const { return outcomes_; }
    bool IsPveRound() const { return config_.match.IsPveRound(round_); }
    StageRound CurrentStageRound() const { return config_.match.StageOf(round_); }
    // Is this round one of Mother Nature's (a gift phase, and no shop)? False when no Mother Nature data is loaded.
    bool IsMotherNatureRound() const { return IsMotherNatureRound(round_); }
    bool IsMotherNatureRound(int round) const { return motherNature_ != nullptr && config_.match.IsMotherNatureRound(round); }
    // Is the shop closed this round? True in the opening round(s) (the free unit is the reward) and in Mother Nature's rounds (the gift is).
    // The shop is open in every Planning phase (FEEDBACK V1, demo 1.1); only Mother Nature's gift phase closes it (and the opening rounds, if configured).
    bool IsShopClosed() const { return phase_ == MatchPhase::MotherNature || config_.match.IsOpeningRound(round_); }
    const MotherNatureDatabase* MotherNature() const { return motherNature_; }
    // Trait system v2 (all empty / none when the match runs without trait data -- then the traits only act inside fights).
    // `traits` (optional, must outlive the match) turns on the match-level half of the traits: the path each trait with paths takes this match,
    // after-combat rewards, choices, plants, granted and offered units.
    const TraitDatabase* Traits() const { return traits_; }
    // The path the match picked for each trait that has paths (trait id, path index), chosen from the match seed when it was created.
    const std::vector<std::pair<std::uint32_t, int>>& TraitPaths() const { return traitPaths_; }
    int TraitPath(std::uint32_t trait) const;
    // The choice waiting for `player` (kind None when there is none).
    const TraitChoice& PendingTraitChoice(PlayerId player) const;
    // How many different champions on `player`'s board carry `trait`, and the tier that reaches (0 = none).
    int TraitTier(PlayerId player, const TraitDefinition& trait, int* countOut = nullptr) const;
    // The item data this match validates against (nullptr when the match runs without items). Read-only: lets a client of the match (a bot,
    // a UI) look up what an item is.
    const ItemDatabase* Items() const { return items_; }
    // What `player` may still choose from (empty once they picked, and outside the MotherNature phase), and whether their choice is settled
    // (picked, or nothing was on offer).
    const std::vector<GiftOffer>& GiftOffers(PlayerId player) const;
    bool GiftSettled(PlayerId player) const;

    // The crash-recovery snapshot taken when the current (or latest) Planning phase began -- empty until the first one, and
    // always empty when GameConfig::snapshot.atPlanningStart is off. A match restored from such a snapshot starts with it.
    const std::vector<std::uint8_t>& LastPlanningSnapshot() const { return autoSnapshot_; }
    int LastPlanningSnapshotRound() const { return autoSnapshotRound_; }

    const GameConfig& Config() const { return config_; }
    const PlayerManager& Players() const { return players_; }
    const SharedChampionPool& Pool() const { return pool_; }
    // Bypasses phase rules. For server-internal systems, admin tools and tests only.
    PlayerManager& PlayersMutable() { return players_; }
    SharedChampionPool& PoolMutable() { return pool_; }

    // ---- Player actions ----
    // Planning: everything. Combat and Resolution: the shop (TryBuyShopUnit -- the champion goes to the BENCH and may only merge bench units --,
    // TryRerollShop, TryBuyXp) and anything that touches only bench units (sell, move between bench slots, equip / unequip). The board is locked
    // while its units fight: those actions answer UnitInCombat for a unit on the board. The shop is closed in round 1 and in Mother Nature's rounds.
    ActionResult TryRerollShop(PlayerId player);
    ActionResult TryBuyShopUnit(PlayerId player, std::size_t shopSlot);
    ActionResult TryBuyXp(PlayerId player);
    // Lock / unlock the shop (the offer then survives the next round's automatic refresh). Allowed whenever the player may use the shop; never refused because the shop is closed.
    ActionResult TrySetShopLock(PlayerId player, bool locked);
    // MotherNature phase only: take offer `index` for free. The other offers are gone (their unit copies return to the pool).
    ActionResult TryPickGift(PlayerId player, std::size_t index);
    ActionResult TrySellUnit(PlayerId player, UnitId unit);
    // Move to bench slot (x, 0) or board cell (x, y); swaps with whatever is there.
    ActionResult TryMoveUnit(PlayerId player, UnitId unit, LocationType location, int x, int y);
    // Put an item from the player's bag on a unit / take it off again.
    ActionResult TryEquipItem(PlayerId player, UnitId unit, ItemId item);
    ActionResult TryUnequipItem(PlayerId player, UnitId unit, int slot);
    // Combine two components of the player's item bag into a finished item (any phase in which items may be handled, like equipping to a bench unit).
    ActionResult TryCombineItems(PlayerId player, ItemId first, ItemId second);
    // Answer the pending trait choice: an option index, or -1 to decline a prototype (a module must be picked). Any phase in which the shop is usable.
    ActionResult TryPickTraitChoice(PlayerId player, int index);

    // ---- Snapshot / restore (see Snapshot.h) ----
    // The whole match as bytes. Call between ticks / actions (which is the only time anything can call it).
    std::vector<std::uint8_t> Snapshot() const;
    // Rebuilds a match from Snapshot() output. `config`, `database`, `items` and the simulator must be equivalent to
    // the ones the original ran with (their hashes are checked unless options say otherwise). No listeners are attached:
    // add them after restoring. nullptr + *error when the buffer is not a valid snapshot of a consistent match.
    static std::unique_ptr<MatchManager> Restore(const std::vector<std::uint8_t>& snapshot, const GameConfig& config,
                                                 const ChampionDatabase& database, std::unique_ptr<ICombatSimulator> simulator,
                                                 std::string* error = nullptr, const ItemDatabase* items = nullptr,
                                                 const EncounterDatabase* encounters = nullptr, const MotherNatureDatabase* motherNature = nullptr,
                                                 const RestoreOptions& options = RestoreOptions{}, const TraitDatabase* traits = nullptr);

    // ---- Diagnostics ----
    // Every champion copy must be in exactly one of: pool, a shop slot, a unit (a star-N unit
    // stands for 3^(N-1) copies, so merging never changes the count).
    bool VerifyPoolIntegrity() const;
    // Every player's bench/board layout is internally consistent (see UnitRoster::CheckInvariants).
    bool VerifyRosterLayouts() const;
    // Hash of all authoritative state. Use to detect desyncs and to test determinism.
    std::uint64_t StateHash() const;

private:
    MatchManager(const GameConfig& config, const ChampionDatabase& database, std::uint64_t seed,
                 std::unique_ptr<ICombatSimulator> simulator, const ItemDatabase* items, const EncounterDatabase* encounters,
                 const MotherNatureDatabase* motherNature, const TraitDatabase* traits);

    // ---- trait system v2 (MatchTraits.cpp) ----
    void ChooseTraitPaths();
    std::vector<TraitCountUnit> BoardUnits(const PlayerState& player) const;
    int TierFor(const PlayerState& player, const TraitDefinition& trait, int* countOut) const;
    void AfterRosterChange(PlayerId player);   // plants, module offers, the Queen
    void SyncPlants(PlayerState& player);
    void OfferQueen(PlayerState& player);
    void OfferModules(PlayerState& player);
    void OfferPrototype(PlayerState& player);
    void GrantTraitRewards(PlayerState& player, const CombatOutcome& outcome, bool home, bool lost);
    ActionResult ResolveTraitChoice(PlayerId player, int index, bool automatic);
    void SettleTraitChoices();   // a Combat begins: every open choice is settled (a module: the first; a prototype: declined)

    int PhaseDuration(MatchPhase phase) const;
    void BeginRound();
    void AdvancePhase();
    void EnterPhase(MatchPhase next);
    void BuildMatchups();
    void RunCombat();
    int ComputeCombatTicks() const;   // the Combat phase's length for the fights in outcomes_
    void ApplyCombatOutcomes();
    PveDrop GrantPveDrop(PlayerState& player, std::uint32_t encounterId, Rng& rng);
    PveDrop PayDropLine(PlayerState& player, const PveDropEntry& entry, Rng& rng);   // one payout of one drop-table line
    void GrantGuaranteedDrops(PlayerState& player, std::uint32_t encounterId, Rng& rng, std::vector<PveDrop>& paid);
    void CheckUnlocks(PlayerState& player);   // unlockable champions whose rule the player now meets
    void TakeAutoSnapshot();
    // Mother Nature
    void GenerateGifts();
    bool GiftUsable(const GiftDefinition& gift) const;
    bool RealizeGift(const GiftDefinition& gift, Rng& rng, GiftOffer& out);
    ActionResult PickGift(PlayerId player, std::size_t index, bool automatic);
    void FinishGiftPhase();
    bool AllGiftsSettled() const;
    void ReleaseOffers(PlayerId player);
    void DealOpeningUnits();
    void EndMatch();
    enum class ActorRule { PlanningOnly, Shop, Bench };   // when an action may run: see ResolveActor
    ActionResult ResolveActor(PlayerId id, PlayerState*& outPlayer, ActorRule rule);
    bool BoardUnitLocked(const PlayerState& player, UnitId unit) const;

    // IPlayerListener: forward each player event to every registered IMatchListener.
    void OnUnitBought(PlayerId player, const UnitInstance& unit, int goldSpent) override;
    void OnUnitSold(PlayerId player, const UnitInstance& unit, int goldGained) override;
    void OnUnitMoved(PlayerId player, const UnitMove& move) override;
    void OnUnitMerged(PlayerId player, const UnitMerge& merge) override;
    void OnItemEquipped(PlayerId player, const UnitInstance& unit, ItemId item) override;
    void OnItemUnequipped(PlayerId player, const UnitInstance& unit, ItemId item) override;
    void OnItemsCombined(PlayerId player, const UnitInstance& unit, const ItemCombination& combination) override;
    void OnBagItemsCombined(PlayerId player, ItemId first, ItemId second, ItemId result) override;
    void OnItemConsumed(PlayerId player, const UnitInstance& unit, ItemId consumable, const std::vector<ItemId>& returned) override;
    void OnIncomeGranted(PlayerId player, int round, const IncomeBreakdown& income) override;

    // Declaration order matters: pool_ before players_ (players hold a reference to it);
    // listeners_ before players_ (players call back into this object).
    GameConfig config_;
    const ChampionDatabase& database_;
    const ItemDatabase* items_;
    const EncounterDatabase* encounters_;
    const MotherNatureDatabase* motherNature_;
    const TraitDatabase* traits_;
    std::uint64_t seed_;
    Rng rng_;
    std::vector<IMatchListener*> listeners_;
    SharedChampionPool pool_;
    PlayerManager players_;
    std::unique_ptr<ICombatSimulator> simulator_;

    MatchPhase phase_ = MatchPhase::NotStarted;
    int round_ = 0;
    int ticksInPhase_ = 0;
    PlayerId winner_ = kInvalidPlayerId;
    std::vector<Matchup> matchups_;
    std::vector<CombatOutcome> outcomes_;
    int combatPhaseTicks_ = 0;   // derived from outcomes_ (see PhaseTicks); never part of a snapshot or of StateHash()

    // Mother Nature: per player, the gifts on offer this round and whether the choice is settled. Empty / false outside the phase.
    struct PlayerGifts {
        std::vector<GiftOffer> offers;
        bool settled = false;
    };
    std::vector<PlayerGifts> gifts_;
    std::vector<std::pair<std::uint32_t, int>> traitPaths_;   // derived from the seed at creation (so not stored in a snapshot)
    std::vector<TraitChoice> choices_;                         // per seat

    // Derived, not authoritative: never part of a snapshot or of StateHash().
    std::vector<std::uint8_t> autoSnapshot_;
    int autoSnapshotRound_ = 0;
};

}  // namespace w2f
