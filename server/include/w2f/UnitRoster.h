#pragma once

// A player's physical units: the 9-slot bench and the 4x7 board, plus 3-star merging.
//
// Pure data structure: it knows nothing about gold, the shared pool or listeners. PlayerState
// wraps it and does the bookkeeping that needs those, so all of the placement / merge rules
// can be reasoned about (and tested) in one place.
//
// Storage: `units_` owns the UnitInstance data (insertion order, so iteration is
// deterministic). `bench_` / `board_` are occupancy tables holding UnitIds (0 = empty) and
// must always agree with each unit's own location/x/y; CheckInvariants() verifies that.

#include <array>
#include <cstddef>
#include <vector>

#include "w2f/Item.h"
#include "w2f/Types.h"
#include "w2f/Unit.h"

namespace w2f {

class UnitRoster {
public:
    struct AddResult {
        bool added = false;
        UnitInstance unit;             // As first placed, before merging (location None if it merged on arrival).
        std::vector<UnitMerge> merges; // In the order they happened (a cascade can produce several).
    };

    // `items` (optional, must outlive the roster) enables item combination: without it items only ever occupy slots.
    explicit UnitRoster(PlayerId owner, const ItemDatabase* items = nullptr) : owner_(owner), items_(items) {}

    // ---- Queries ----
    const std::vector<UnitInstance>& Units() const { return units_; }
    std::size_t Count() const { return units_.size(); }
    int BenchCount() const;
    int BoardCount() const;   // board SLOTS in use (plants take none, the Queen two)
    const UnitInstance* Find(UnitId id) const;
    const UnitInstance* BenchAt(int slot) const;
    const UnitInstance* BoardAt(int x, int y) const;
    // How many units of this champion at exactly this star level.
    int CountOf(const ChampionDefinition* champion, int starLevel) const;
    // True if Add() would succeed: there is a free spot, or the unit completes a merge.
    bool CanAdd(const ChampionDefinition* champion, int starLevel = 1) const;

    // Snapshot support. The id serial the next new unit will get (part of the state: ids are never reused).
    std::uint32_t NextSerial() const { return nextSerial_; }
    // Replaces the whole roster with `units` (kept in this order) and the id counter. Every unit must be placed at a
    // valid, free cell, carry an id owned by this seat and below `nextSerial`, and satisfy CheckInvariants().
    // On failure the roster is left EMPTY and false is returned.
    bool Restore(const std::vector<UnitInstance>& units, std::uint32_t nextSerial);

    // Max units allowed on the board at once (PlayerState ties this to level when configured).
    void SetBoardCapacity(int capacity) { boardCapacity_ = capacity; }
    int BoardCapacity() const { return boardCapacity_; }

    // ---- Mutations ----
    // Places a new unit in the first free bench slot; if the bench is full, in the first free
    // board cell (row-major from the back row) while under the board capacity. Then merges
    // as far as it can. If nothing is free but the unit completes a set of three, it merges
    // on arrival without ever occupying a slot (matches TFT). Fails only if CanAdd() is false.
    AddResult Add(const ChampionDefinition* champion, int starLevel = 1);

    // Move to (location, x, y). Empty target: plain move. Occupied target: the two swap.
    // A bench->board move onto an empty cell fails with BoardFull at capacity; swaps never
    // change the board population so they are always allowed. Moving onto its own cell is Ok
    // and reports nothing. `outMoves` receives one entry per unit that moved (0, 1 or 2).
    ActionResult Move(UnitId id, LocationType location, int x, int y, std::vector<UnitMove>& outMoves);

    // ---- Items ----
    // If the unit already holds an item that combines with `item`, both are consumed and the combined item takes the first one's slot
    // (`*combined` reports it); this works even when the unit carries the maximum. Otherwise the first free slot. InvalidUnit / ItemsFull.
    ActionResult EquipItem(UnitId id, ItemId item, ItemCombination* combined = nullptr);
    // Empties `slot` (0..kMaxItemsPerUnit-1) and reports what was in it. InvalidUnit / InvalidSlot (out of range or empty).
    ActionResult UnequipItem(UnitId id, int slot, ItemId* outItem = nullptr);

    // Puts a new unit straight onto the free board cell (x, y), never merging (the Nature trait's plants). Returns it (id 0 = the cell was not free).
    UnitInstance AddPlaced(const ChampionDefinition* champion, int starLevel, int x, int y);
    // Removes a unit and frees its cell. Returns false if the id is unknown.
    bool Remove(UnitId id, UnitInstance* outRemoved = nullptr);
    void Clear();

    // ---- Diagnostics ----
    // Occupancy tables and unit fields agree; no dangling ids; no unplaced units; unique ids;
    // no more than two loose copies of any (champion, star<max) pair.
    bool CheckInvariants() const;

private:
    UnitInstance* FindMutable(UnitId id);
    UnitId MakeId();
    static bool InRange(LocationType location, int x, int y);
    UnitId& Cell(LocationType location, int x, int y);
    UnitId CellValue(LocationType location, int x, int y) const;
    bool HasFreeSlot() const;
    bool PlaceInFirstFree(UnitInstance& unit);
    void Place(UnitInstance& unit, LocationType location, int x, int y);
    void Unplace(UnitInstance& unit);
    bool MergeOnce(const ChampionDefinition* champion, UnitMerge& out);
    // Combines `item` with something the unit holds, or puts it in a free slot. False if neither is possible.
    bool PlaceItem(UnitInstance& unit, ItemId item, ItemCombination* combined);

    PlayerId owner_;
    const ItemDatabase* items_;
    std::uint32_t nextSerial_ = 1;
    int boardCapacity_ = kBoardRows * kBoardColumns;
    std::vector<UnitInstance> units_;
    std::array<UnitId, kBenchSlots> bench_{};
    std::array<std::array<UnitId, kBoardColumns>, kBoardRows> board_{};  // [row][column]
};

}  // namespace w2f
