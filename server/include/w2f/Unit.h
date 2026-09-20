#pragma once

// A single owned unit ("champion copy") and where it sits.

#include <array>
#include <cstdint>
#include <vector>

#include "w2f/ChampionDatabase.h"
#include "w2f/Types.h"

namespace w2f {

// Unique for the whole match, never reused, never 0. Layout: (ownerSeat + 1) << 24 | serial,
// where the serial is a per-player counter. Deriving it from the owner (instead of one global
// counter) keeps ids independent of what *other* players did, which makes replays and
// per-player network streams simpler. A merged unit keeps the id of the copy that survives.
using UnitId = std::uint32_t;
constexpr UnitId kInvalidUnitId = 0;

// None only ever appears on a freshly bought unit that is consumed by a merge before it
// could be placed (bench and board full, but the purchase completes a set of three).
enum class LocationType : std::uint8_t { None, Bench, Board };

// Coordinates:
//   Bench: x = slot 0..kBenchSlots-1, y = 0.
//   Board: x = column 0..kBoardColumns-1, y = row 0..kBoardRows-1.
//          y = 0 is the back row (furthest from the enemy), y = kBoardRows-1 the front row.
//          Hex layout is "odd-r" offset: odd rows are shifted half a cell to the right.
//          Nothing in Phase 2 depends on hex adjacency; combat will.
struct UnitInstance {
    UnitId id = kInvalidUnitId;
    const ChampionDefinition* champion = nullptr;
    int starLevel = 1;  // 1..kMaxStarLevel
    LocationType location = LocationType::None;
    int x = -1;
    int y = -1;
    // Equipment: up to kMaxItemsPerUnit items; 0 = empty slot. Items travel with the unit (a merge keeps them on the
    // survivor, or moves the overflow to the owner's item bag; selling a unit returns them to the bag).
    std::array<ItemId, kMaxItemsPerUnit> items{};

    int ItemCount() const {
        int n = 0;
        for (ItemId item : items) n += item != 0 ? 1 : 0;
        return n;
    }
};

// Two items that turned into one when a unit was given the second (see ItemDatabase::FindCombination). `first` was already on the unit.
struct ItemCombination {
    ItemId first = 0;
    ItemId second = 0;
    ItemId result = 0;
};

// A unit that changed position. `unit` holds the NEW position; `from*` the old one.
struct UnitMove {
    UnitInstance unit;
    LocationType fromLocation = LocationType::None;
    int fromX = -1;
    int fromY = -1;
};

// Three copies of the same star level became one unit of the next star level.
struct UnitMerge {
    UnitInstance upgraded;                       // The survivor, after upgrading and at its final position.
    std::array<UnitId, 2> consumed{};            // The two copies that were absorbed (ids no longer exist).
    int previousStarLevel = 1;
    std::vector<ItemId> overflowItems;           // items from the consumed copies that did not fit on the survivor
    std::vector<ItemCombination> combinations;   // carried items that combined with items the survivor already held
};

}  // namespace w2f
