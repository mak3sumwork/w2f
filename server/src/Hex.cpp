#include "w2f/Hex.h"

#include <algorithm>
#include <cassert>

namespace w2f {

// ---- HexGrid -----------------------------------------------------------------------------

HexGrid::HexGrid(int width, int height)
    : width_(width), height_(height), blocked_(static_cast<std::size_t>(width * height), 0) {}

void HexGrid::SetBlocked(HexCoord c, bool blocked) {
    assert(InBounds(c));
    if (InBounds(c)) blocked_[Index(c)] = blocked ? 1 : 0;
}

void HexGrid::Clear() { std::fill(blocked_.begin(), blocked_.end(), std::uint8_t{0}); }

// ---- HexPathfinder -----------------------------------------------------------------------

HexPathfinder::HexPathfinder(int width, int height)
    : width_(width),
      height_(height),
      visitedStamp_(static_cast<std::size_t>(width * height), 0),
      parent_(static_cast<std::size_t>(width * height), -1) {
    queue_.reserve(static_cast<std::size_t>(width * height));
}

template <typename IsGoal>
bool HexPathfinder::Search(const HexGrid& grid, HexCoord start, IsGoal isGoal, std::vector<HexCoord>& outPath) {
    outPath.clear();
    if (!grid.InBounds(start)) return false;

    // A generation stamp avoids clearing the visited table on every search.
    if (++stamp_ == 0) {  // wrapped: reset once
        std::fill(visitedStamp_.begin(), visitedStamp_.end(), 0u);
        stamp_ = 1;
    }

    auto indexOf = [this](HexCoord c) { return static_cast<int>(c.y * width_ + c.x); };
    auto coordOf = [this](int index) { return HexCoord{index % width_, index / width_}; };

    queue_.clear();
    const int startIndex = indexOf(start);
    visitedStamp_[static_cast<std::size_t>(startIndex)] = stamp_;
    parent_[static_cast<std::size_t>(startIndex)] = -1;
    queue_.push_back(startIndex);

    std::array<HexCoord, hex::kDirections> neighbors;
    for (std::size_t head = 0; head < queue_.size(); ++head) {
        const int current = queue_[head];
        const HexCoord here = coordOf(current);

        if (isGoal(here)) {
            // Walk parents back to (but excluding) the start, then flip.
            for (int at = current; at != startIndex; at = parent_[static_cast<std::size_t>(at)]) {
                outPath.push_back(coordOf(at));
            }
            std::reverse(outPath.begin(), outPath.end());
            return true;
        }

        const int count = hex::Neighbors(here, width_, height_, neighbors);
        for (int i = 0; i < count; ++i) {
            const HexCoord next = neighbors[static_cast<std::size_t>(i)];
            const std::size_t nextIndex = static_cast<std::size_t>(indexOf(next));
            if (visitedStamp_[nextIndex] == stamp_ || grid.IsBlocked(next)) continue;
            visitedStamp_[nextIndex] = stamp_;
            parent_[nextIndex] = current;
            queue_.push_back(static_cast<int>(nextIndex));
        }
    }
    return false;
}

bool HexPathfinder::FindPath(const HexGrid& grid, HexCoord start, HexCoord goal, std::vector<HexCoord>& outPath) {
    assert(grid.Width() == width_ && grid.Height() == height_);
    if (!grid.InBounds(goal)) {
        outPath.clear();
        return false;
    }
    // A blocked goal is unreachable (Search never enters blocked cells), except start == goal.
    return Search(grid, start, [goal](HexCoord c) { return c == goal; }, outPath);
}

bool HexPathfinder::FindPathToRange(const HexGrid& grid, HexCoord start, HexCoord target, int range,
                                    std::vector<HexCoord>& outPath) {
    assert(grid.Width() == width_ && grid.Height() == height_);
    // Blocked hexes are never enqueued, so the target's own hex (occupied by the target) can't be a goal.
    return Search(grid, start, [target, range](HexCoord c) { return hex::Distance(c, target) <= range; }, outPath);
}

}  // namespace w2f
