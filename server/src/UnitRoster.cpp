#include "w2f/UnitRoster.h"

#include <algorithm>
#include <cassert>
#include <set>

namespace w2f {

namespace {

constexpr std::uint32_t kSerialMask = 0x00FFFFFFu;

// Which copy survives / is consumed when three merge: prefer units on the board (so a merge
// never pulls a fielded unit off the board), then lower row, then lower column; unplaced last.
struct MergeRank {
    int group;  // 0 board, 1 bench, 2 unplaced
    int y;
    int x;
    bool operator<(const MergeRank& o) const {
        if (group != o.group) return group < o.group;
        if (y != o.y) return y < o.y;
        return x < o.x;
    }
};

MergeRank RankOf(const UnitInstance& u) {
    switch (u.location) {
        case LocationType::Board: return {0, u.y, u.x};
        case LocationType::Bench: return {1, 0, u.x};
        case LocationType::None: break;
    }
    return {2, 0, 0};
}

}  // namespace

// ---- Queries -----------------------------------------------------------------------------

int UnitRoster::BenchCount() const {
    int n = 0;
    for (const UnitInstance& u : units_) n += u.location == LocationType::Bench ? 1 : 0;
    return n;
}

// Board SLOTS in use: a plant takes none, the Phaisa Queen two (ChampionDefinition::teamSlots).
int UnitRoster::BoardCount() const {
    int n = 0;
    for (const UnitInstance& u : units_) n += u.location == LocationType::Board ? u.champion->teamSlots : 0;
    return n;
}

const UnitInstance* UnitRoster::Find(UnitId id) const {
    for (const UnitInstance& u : units_) {
        if (u.id == id) return &u;
    }
    return nullptr;
}

UnitInstance* UnitRoster::FindMutable(UnitId id) {
    return const_cast<UnitInstance*>(static_cast<const UnitRoster*>(this)->Find(id));
}

const UnitInstance* UnitRoster::BenchAt(int slot) const {
    if (!InRange(LocationType::Bench, slot, 0)) return nullptr;
    return Find(bench_[static_cast<std::size_t>(slot)]);
}

const UnitInstance* UnitRoster::BoardAt(int x, int y) const {
    if (!InRange(LocationType::Board, x, y)) return nullptr;
    return Find(board_[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)]);
}

int UnitRoster::CountOf(const ChampionDefinition* champion, int starLevel) const {
    int n = 0;
    for (const UnitInstance& u : units_) {
        n += (u.champion == champion && u.starLevel == starLevel) ? 1 : 0;
    }
    return n;
}

bool UnitRoster::CanAdd(const ChampionDefinition* champion, int starLevel) const {
    if (champion == nullptr || starLevel < 1 || starLevel > kMaxStarLevel) return false;
    if (HasFreeSlot()) return true;
    return starLevel < kMaxStarLevel && CountOf(champion, starLevel) >= 2;  // would merge on arrival
}

// ---- Low-level helpers -------------------------------------------------------------------

UnitId UnitRoster::MakeId() {
    assert(nextSerial_ <= kSerialMask && "per-player unit id serial exhausted");
    return ((static_cast<UnitId>(owner_) + 1u) << 24) | (nextSerial_++ & kSerialMask);
}

bool UnitRoster::InRange(LocationType location, int x, int y) {
    switch (location) {
        case LocationType::Bench: return y == 0 && x >= 0 && x < kBenchSlots;
        case LocationType::Board: return x >= 0 && x < kBoardColumns && y >= 0 && y < kBoardRows;
        case LocationType::None: break;
    }
    return false;
}

UnitId& UnitRoster::Cell(LocationType location, int x, int y) {
    assert(InRange(location, x, y));
    if (location == LocationType::Bench) return bench_[static_cast<std::size_t>(x)];
    return board_[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)];
}

UnitId UnitRoster::CellValue(LocationType location, int x, int y) const {
    return const_cast<UnitRoster*>(this)->Cell(location, x, y);
}

bool UnitRoster::HasFreeSlot() const {
    if (BenchCount() < kBenchSlots) return true;
    if (BoardCount() >= std::min(boardCapacity_, kBoardRows * kBoardColumns)) return false;
    // A slot also needs an empty hex: plants take no board slot but do stand on a hex, so a crowded board can be "not full" and still have no room.
    for (const auto& row : board_) {
        for (UnitId cell : row) {
            if (cell == kInvalidUnitId) return true;
        }
    }
    return false;
}

bool UnitRoster::PlaceInFirstFree(UnitInstance& unit) {
    for (int slot = 0; slot < kBenchSlots; ++slot) {
        if (bench_[static_cast<std::size_t>(slot)] == kInvalidUnitId) {
            Place(unit, LocationType::Bench, slot, 0);
            return true;
        }
    }
    if (BoardCount() >= boardCapacity_) return false;
    for (int y = 0; y < kBoardRows; ++y) {
        for (int x = 0; x < kBoardColumns; ++x) {
            if (board_[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] == kInvalidUnitId) {
                Place(unit, LocationType::Board, x, y);
                return true;
            }
        }
    }
    return false;
}

void UnitRoster::Place(UnitInstance& unit, LocationType location, int x, int y) {
    Cell(location, x, y) = unit.id;
    unit.location = location;
    unit.x = x;
    unit.y = y;
}

void UnitRoster::Unplace(UnitInstance& unit) {
    if (unit.location != LocationType::None) Cell(unit.location, unit.x, unit.y) = kInvalidUnitId;
    unit.location = LocationType::None;
    unit.x = -1;
    unit.y = -1;
}

// ---- Add + merge -------------------------------------------------------------------------

UnitRoster::AddResult UnitRoster::Add(const ChampionDefinition* champion, int starLevel) {
    AddResult result;
    if (!CanAdd(champion, starLevel)) return result;

    UnitInstance unit;
    unit.id = MakeId();
    unit.champion = champion;
    unit.starLevel = starLevel;
    PlaceInFirstFree(unit);  // may leave it unplaced; CanAdd guaranteed that implies a merge below
    units_.push_back(unit);
    result.added = true;
    result.unit = unit;

    UnitMerge merge;
    while (MergeOnce(champion, merge)) result.merges.push_back(merge);

    assert(std::none_of(units_.begin(), units_.end(),
                        [](const UnitInstance& u) { return u.location == LocationType::None; }) &&
           "an unplaced unit survived Add()");
    return result;
}

bool UnitRoster::MergeOnce(const ChampionDefinition* champion, UnitMerge& out) {
    // Lowest star first; each successful merge restarts the scan (the caller loops), so an
    // upgrade that completes the next tier cascades: nine 1-stars end as one 3-star.
    for (int star = 1; star < kMaxStarLevel; ++star) {
        std::vector<UnitId> candidates;
        for (const UnitInstance& u : units_) {
            if (u.champion == champion && u.starLevel == star) candidates.push_back(u.id);
        }
        if (candidates.size() < 3) continue;

        std::sort(candidates.begin(), candidates.end(), [this](UnitId a, UnitId b) {
            const MergeRank ra = RankOf(*Find(a));
            const MergeRank rb = RankOf(*Find(b));
            return ra < rb;  // Ranks are unique for placed units; unplaced ties are impossible (<= 1 exists).
        });

        const UnitId survivorId = candidates[0];
        out.consumed = {candidates[1], candidates[2]};
        out.previousStarLevel = star;
        out.overflowItems.clear();
        std::vector<ItemId> carried;   // the consumed copies' items go to the survivor
        for (UnitId consumedId : out.consumed) {
            for (ItemId item : Find(consumedId)->items) {
                if (item != 0) carried.push_back(item);
            }
        }
        for (UnitId consumedId : out.consumed) Remove(consumedId);

        UnitInstance* survivor = FindMutable(survivorId);
        assert(survivor != nullptr);
        survivor->starLevel = star + 1;
        out.combinations.clear();
        for (ItemId item : carried) {
            ItemCombination combo;
            if (!PlaceItem(*survivor, item, &combo)) {
                out.overflowItems.push_back(item);   // the owner's bag takes what does not fit
            } else if (combo.result != 0) {
                out.combinations.push_back(combo);
            }
        }
        out.upgraded = *survivor;
        return true;
    }
    return false;
}

// ---- Move / remove -----------------------------------------------------------------------

ActionResult UnitRoster::Move(UnitId id, LocationType location, int x, int y, std::vector<UnitMove>& outMoves) {
    outMoves.clear();
    if (!InRange(location, x, y)) return ActionResult::InvalidSlot;
    UnitInstance* unit = FindMutable(id);
    if (unit == nullptr) return ActionResult::InvalidUnit;
    if (unit->location == location && unit->x == x && unit->y == y) return ActionResult::Ok;  // no-op

    const UnitId occupantId = CellValue(location, x, y);
    UnitMove first;
    first.fromLocation = unit->location;
    first.fromX = unit->x;
    first.fromY = unit->y;

    const auto slots = [](const UnitInstance* u) { return u != nullptr ? u->champion->teamSlots : 0; };
    if (unit->champion->plant && location != LocationType::Board) return ActionResult::InvalidSlot;   // a plant never leaves the board
    if (occupantId == kInvalidUnitId) {
        if (location == LocationType::Board && unit->location != LocationType::Board &&
            BoardCount() + slots(unit) > boardCapacity_) {
            return ActionResult::BoardFull;
        }
        Unplace(*unit);
        Place(*unit, location, x, y);
        first.unit = *unit;
        outMoves.push_back(first);
        return ActionResult::Ok;
    }

    // Swap. Clear both cells first so neither placement can overwrite the other.
    UnitInstance* other = FindMutable(occupantId);
    assert(other != nullptr);
    if (other->champion->plant && unit->location != LocationType::Board) return ActionResult::InvalidSlot;
    if (unit->location != location) {   // a bench <-> board swap may change the slots in use (a plant or the Queen)
        const int delta = location == LocationType::Board ? slots(unit) - slots(other) : slots(other) - slots(unit);
        if (delta > 0 && BoardCount() + delta > boardCapacity_) return ActionResult::BoardFull;
    }
    UnitMove second;
    second.fromLocation = other->location;
    second.fromX = other->x;
    second.fromY = other->y;

    Unplace(*unit);
    Unplace(*other);
    Place(*unit, location, x, y);
    Place(*other, first.fromLocation, first.fromX, first.fromY);
    first.unit = *unit;
    second.unit = *other;
    outMoves.push_back(first);
    outMoves.push_back(second);
    return ActionResult::Ok;
}

bool UnitRoster::PlaceItem(UnitInstance& unit, ItemId item, ItemCombination* combined) {
    if (combined) *combined = ItemCombination{};
    if (items_ != nullptr) {
        for (ItemId& slot : unit.items) {   // a partner the item combines with: both are consumed, the result takes the partner's slot
            if (slot == 0) continue;
            if (const ItemDefinition* made = items_->FindCombination(slot, item)) {
                if (combined) *combined = ItemCombination{slot, item, made->id};
                slot = made->id;
                return true;
            }
        }
    }
    for (ItemId& slot : unit.items) {
        if (slot == 0) {
            slot = item;
            return true;
        }
    }
    return false;
}

ActionResult UnitRoster::EquipItem(UnitId id, ItemId item, ItemCombination* combined) {
    if (combined) *combined = ItemCombination{};
    UnitInstance* unit = FindMutable(id);
    if (unit == nullptr) return ActionResult::InvalidUnit;
    if (item == 0) return ActionResult::InvalidItem;
    return PlaceItem(*unit, item, combined) ? ActionResult::Ok : ActionResult::ItemsFull;
}

ActionResult UnitRoster::UnequipItem(UnitId id, int slot, ItemId* outItem) {
    UnitInstance* unit = FindMutable(id);
    if (unit == nullptr) return ActionResult::InvalidUnit;
    if (slot < 0 || slot >= kMaxItemsPerUnit || unit->items[static_cast<std::size_t>(slot)] == 0) return ActionResult::InvalidSlot;
    if (outItem) *outItem = unit->items[static_cast<std::size_t>(slot)];
    unit->items[static_cast<std::size_t>(slot)] = 0;
    return ActionResult::Ok;
}

UnitInstance UnitRoster::AddPlaced(const ChampionDefinition* champion, int starLevel, int x, int y) {
    UnitInstance unit;
    if (champion == nullptr || starLevel < 1 || starLevel > kMaxStarLevel || !InRange(LocationType::Board, x, y) ||
        CellValue(LocationType::Board, x, y) != kInvalidUnitId) {
        return unit;   // id 0: nothing was added
    }
    unit.id = MakeId();
    unit.champion = champion;
    unit.starLevel = starLevel;
    Place(unit, LocationType::Board, x, y);
    units_.push_back(unit);
    return unit;
}

bool UnitRoster::Remove(UnitId id, UnitInstance* outRemoved) {
    auto it = std::find_if(units_.begin(), units_.end(), [id](const UnitInstance& u) { return u.id == id; });
    if (it == units_.end()) return false;
    if (outRemoved) *outRemoved = *it;
    Unplace(*it);
    units_.erase(it);  // order-preserving
    return true;
}

bool UnitRoster::Restore(const std::vector<UnitInstance>& units, std::uint32_t nextSerial) {
    Clear();
    nextSerial_ = 1;
    if (nextSerial < 1 || nextSerial > kSerialMask + 1u) return false;
    for (const UnitInstance& u : units) {
        if (u.id == kInvalidUnitId || u.champion == nullptr) return false;
        if ((u.id >> 24) != static_cast<UnitId>(owner_) + 1u || (u.id & kSerialMask) >= nextSerial) return false;
        if (!InRange(u.location, u.x, u.y) || CellValue(u.location, u.x, u.y) != kInvalidUnitId) {
            Clear();
            return false;
        }
        Cell(u.location, u.x, u.y) = u.id;
        units_.push_back(u);
    }
    nextSerial_ = nextSerial;
    if (!CheckInvariants()) {
        Clear();
        return false;
    }
    return true;
}

void UnitRoster::Clear() {
    units_.clear();
    bench_.fill(kInvalidUnitId);
    for (auto& row : board_) row.fill(kInvalidUnitId);
}

// ---- Diagnostics -------------------------------------------------------------------------

bool UnitRoster::CheckInvariants() const {
    std::set<UnitId> seen;
    for (const UnitInstance& u : units_) {
        if (u.id == kInvalidUnitId || u.champion == nullptr) return false;
        if (u.starLevel < 1 || u.starLevel > kMaxStarLevel) return false;
        if (!seen.insert(u.id).second) return false;
        if (!InRange(u.location, u.x, u.y)) return false;  // also rejects LocationType::None
        if (CellValue(u.location, u.x, u.y) != u.id) return false;
        if ((u.id >> 24) != static_cast<UnitId>(owner_) + 1u) return false;
    }
    int occupied = 0;
    for (UnitId id : bench_) occupied += id != kInvalidUnitId ? 1 : 0;
    for (const auto& row : board_) {
        for (UnitId id : row) occupied += id != kInvalidUnitId ? 1 : 0;
    }
    if (occupied != static_cast<int>(units_.size())) return false;
    if (BoardCount() > kBoardRows * kBoardColumns) return false;

    for (const UnitInstance& u : units_) {
        if (u.starLevel < kMaxStarLevel && CountOf(u.champion, u.starLevel) > 2 && !u.champion->plant) return false;
        if (u.champion->plant && u.location != LocationType::Board) return false;
    }
    return true;
}

}  // namespace w2f
