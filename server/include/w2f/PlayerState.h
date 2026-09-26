#pragma once

#include <algorithm>

// Everything the server tracks for one player: health, level/XP, gold, streak, their units
// (bench + board, via UnitRoster), and their attached ShopManager.
//
// PlayerState enforces the *rules* of the economy and unit management (can you afford it?
// is there room? does it merge?) and keeps the shared pool balanced, but not *when* an
// action is allowed -- phase gating is MatchManager's job.

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "w2f/ChampionDatabase.h"
#include "w2f/Config.h"
#include "w2f/Item.h"
#include "w2f/PlayerEvents.h"
#include "w2f/ShopManager.h"
#include "w2f/SharedChampionPool.h"
#include "w2f/Types.h"
#include "w2f/Unit.h"
#include "w2f/UnitRoster.h"

namespace w2f {

// What the v2 traits remember about one player across rounds (see Trait.h). Everything here is part of the snapshot and the state hash.
struct TraitProgress {
    int traitGold = 0;           // Selini (Prosperity): gold earned through the path so far (its bonus grows with it)
    int takedownCounter = 0;     // Selini (Prosperity): takedowns toward the next gold
    int starDust = 0;            // Najmi: banked star dust
    int grantCombats = 0;        // Phaisa: player combats fought with the Rift Herald breakpoint active
    bool unitGranted = false;    // Phaisa: the Rift Herald was given (once per match)
    int moduleTiersOffered = 0;  // Hexagon: the highest module tier already offered
    std::vector<std::uint32_t> modules;   // Hexagon: chosen module ids, in the order they were chosen
    std::vector<ChampionId> unlocked;     // unlockable champions this player has unlocked (Hexa), in the order they were unlocked
    bool operator==(const TraitProgress& o) const {
        return traitGold == o.traitGold && takedownCounter == o.takedownCounter && starDust == o.starDust && grantCombats == o.grantCombats &&
               unitGranted == o.unitGranted && moduleTiersOffered == o.moduleTiersOffered && modules == o.modules && unlocked == o.unlocked;
    }
};

// Everything a snapshot stores about one player (champions already resolved to definitions).
struct PlayerRestoreData {
    int health = 0;
    int level = 1;
    int xp = 0;
    int gold = 0;
    int streak = 0;
    bool shopLocked = false;
    bool eliminated = false;
    int placement = 0;
    std::vector<UnitInstance> units;   // roster order
    std::uint32_t nextUnitSerial = 1;
    std::vector<ItemId> itemBag;
    std::vector<const ChampionDefinition*> shopSlots;
    RngState shopRng;
    TraitProgress traits;
};

class PlayerState {
public:
    // Creates the attached ShopManager. `pool` must outlive the player.
    // `listener` (optional, non-owning) receives unit events.
    PlayerState(PlayerId id, const PlayerConfig& playerConfig, const ShopConfig& shopConfig,
                SharedChampionPool& pool, std::uint64_t matchSeed, IPlayerListener* listener = nullptr,
                const ItemDatabase* items = nullptr);
    ~PlayerState();

    // Not copyable/movable: ShopManager holds a reference back to this object.
    PlayerState(const PlayerState&) = delete;
    PlayerState& operator=(const PlayerState&) = delete;

    void SetListener(IPlayerListener* listener) { listener_ = listener; }

    // ---- Queries ----
    PlayerId Id() const { return id_; }
    // May be <= 0 for a player that just took a killing blow and has not been processed yet.
    int Health() const { return health_; }
    int Level() const { return level_; }
    int Xp() const { return xp_; }
    int Gold() const { return gold_; }
    bool IsMaxLevel() const { return level_ >= kMaxPlayerLevel; }
    int XpToNextLevel() const;  // 0 at max level
    int Streak() const { return streak_; }  // > 0 win streak, < 0 loss streak
    // A LOCKED shop keeps its offer through the automatic refresh at the start of a round (rerolling and buying still work); it stays locked until unlocked.
    bool ShopLocked() const { return shopLocked_; }
    void SetShopLocked(bool locked) { shopLocked_ = locked; }
    bool IsAlive() const { return !eliminated_; }
    int Placement() const { return placement_; }  // 0 until eliminated / match won; 1 = winner
    const UnitRoster& Roster() const { return roster_; }
    const TraitProgress& Traits() const { return traits_; }
    TraitProgress& TraitsMutable() { return traits_; }
    bool HasUnlocked(ChampionId id) const { return std::find(traits_.unlocked.begin(), traits_.unlocked.end(), id) != traits_.unlocked.end(); }
    ShopManager& Shop() { return *shop_; }
    const ShopManager& Shop() const { return *shop_; }

    // ---- Leveling ----
    // Spend buyXpCost gold for buyXpAmount XP (may cascade through several levels).
    ActionResult TryBuyXp();
    void AddXp(int amount);

    // ---- Gold ----
    void AddGold(int amount);
    bool TrySpendGold(int amount);  // Atomic: deducts only if fully affordable.

    // What this player would earn at the start of `round` (base + interest + streak, plus
    // passive XP), based on their current gold and streak. Does not modify state.
    IncomeBreakdown CalculateIncome(int round) const;
    // Applies CalculateIncome: adds the gold and the passive XP. No-op for eliminated players.
    IncomeBreakdown GrantRoundIncome(int round);

    // ---- Combat aftermath ----
    void ApplyDamage(int amount);
    // Restores health, never above the starting health. Nothing for an eliminated player or a non-positive amount. Returns what was actually restored.
    int Heal(int amount);
    void RecordRoundResult(RoundResult result);  // Draws leave the streak untouched.

    // ---- Units ----
    // Can this champion be received right now? (free bench/board spot, or it completes a merge)
    bool CanAcquire(const ChampionDefinition* champion, int starLevel = 1) const;
    // Why CanAcquire is false: RosterFull, or (only while bench-only purchases are on) UnitInCombat when the copy would have to merge into a unit on the board.
    ActionResult AcquireBlockedReason(const ChampionDefinition* champion, int starLevel = 1) const;
    // Adds a unit: first free bench slot, else first free board cell, then merges immediately.
    // Does NOT charge gold or touch the pool -- the caller (shop / carousel / award) already
    // took the copy out of the pool and paid. `goldSpent` is only reported in the event.
    ActionResult AcquireUnit(const ChampionDefinition* champion, int goldSpent = 0, int starLevel = 1);

    // While set (the MatchManager sets it for a player's own shop actions in Combat / Resolution) a purchase may only land on the BENCH, and may only merge
    // units that are on the bench: it never puts a unit on the board and never touches a unit that is fighting. CanAcquire / AcquireUnit obey it.
    void SetBenchOnlyPurchases(bool on) { benchOnly_ = on; }
    // Bench<->board moves and swaps. See UnitRoster::Move for the exact rules.
    ActionResult TryMoveUnit(UnitId unit, LocationType location, int x, int y);

    // Removes the unit, pays out its sell value and returns its copies to the shared pool.
    ActionResult SellUnit(UnitId unit);
    // Value of a unit: its total cost in gold, minus 1 for merged units above 1 cost (TFT rule).
    static int SellValue(int cost, int starLevel);

    // ---- Items ----
    // Items the player owns but has not put on a unit. (Where items COME from -- carousel, drops, shop -- is not built yet:
    // AddItemToBag is the entry point for whatever does that.)
    const std::vector<ItemId>& ItemBag() const { return itemBag_; }
    // False (and nothing changes) for id 0 or, when an item database is attached, an id it does not know.
    bool AddItemToBag(ItemId item);
    // A CONSUMABLE (the Item Remover) does not go onto the unit: it takes every item off it (they land in the bag, in slot order) and is used up
    // (NoItemsToRemove, and nothing changes, if the unit carries none). Several can be held. Everything below applies to ordinary items.
    // Moves an item from the bag onto a unit (ItemsFull if it carries 3; InvalidItem if not in the bag / unknown). If the unit holds an
    // item this one combines with (a recipe in items.json), both are consumed at once and the finished item takes their place --
    // even on a unit that is already full.
    ActionResult TryEquipItem(UnitId unit, ItemId item);
    // Takes the item in `slot` off the unit and puts it back in the bag.
    ActionResult TryUnequipItem(UnitId unit, int slot);
    // Combines two components that are both IN THE BAG (a recipe in items.json, either order; the same component twice needs two copies) into the finished item, which lands
    // in the bag. InvalidItem, and nothing changes, if either is not in the bag, they are not both base components, or no recipe exists.
    ActionResult TryCombineBagItems(ItemId first, ItemId second);

    // ---- Plants (the Nature trait; the MatchManager decides which) ----
    // Puts a plant on the free board cell (x, y) / takes one away. Reported as a unit bought / sold for 0 gold.
    bool GrantPlant(const ChampionDefinition* plant, int starLevel, int x, int y);
    bool RemovePlant(UnitId unit);

    // ---- Snapshot ----
    // Overwrites this player with saved state. It does NOT touch the shared pool: the pool's counts are restored
    // separately, and the caller then checks the two agree (MatchManager::VerifyPoolIntegrity). Validates ranges and
    // layout; on failure returns false with *error set and leaves the player in an unspecified (discard it) state.
    bool RestoreState(const PlayerRestoreData& data, std::string* error = nullptr);

    // ---- Lifecycle ----
    // Returns the shop and every owned unit to the pool and marks the player out of the match.
    void Eliminate(int placement);
    void SetPlacement(int placement) { placement_ = placement; }

private:
    void SyncBoardCapacity();
    bool BenchOnlyPurchaseFits(const ChampionDefinition* champion, int starLevel) const;

    PlayerId id_;
    PlayerConfig config_;
    SharedChampionPool& pool_;
    IPlayerListener* listener_;

    int health_;
    int level_ = 1;
    int xp_ = 0;
    int gold_;
    int streak_ = 0;
    bool shopLocked_ = false;
    bool eliminated_ = false;
    int placement_ = 0;
    bool benchOnly_ = false;   // not state: only ever true inside one player action
    UnitRoster roster_;
    std::vector<ItemId> itemBag_;
    TraitProgress traits_;
    const ItemDatabase* items_;

    std::unique_ptr<ShopManager> shop_;  // Declared last: constructed after everything above.
};

}  // namespace w2f
