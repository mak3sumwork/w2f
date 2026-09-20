#pragma once

// Hex-grid math and pathfinding. Integer-only.
//
// Layout: "odd-r" offset coordinates, exactly as documented in Unit.h -- x is the column,
// y is the row, and odd rows are shifted half a cell to the right.
//
//   row 0:   (0,0) (1,0) (2,0) ...
//   row 1:      (0,1) (1,1) (2,1) ...      <- shifted right
//   row 2:   (0,2) (1,2) (2,2) ...
//
// Every function is a pure function of its inputs; iteration orders are fixed, so results
// (including which of several equally short paths is returned) are identical everywhere.

#include <array>
#include <cstdint>
#include <vector>

#include "w2f/Types.h"

namespace w2f {

// The arena is both players' boards joined: kBoardColumns wide, 2*kBoardRows tall.
constexpr int kArenaColumns = kBoardColumns;
constexpr int kArenaRows = kBoardRows * 2;

struct HexCoord {
    int x = 0;
    int y = 0;
};
constexpr bool operator==(HexCoord a, HexCoord b) { return a.x == b.x && a.y == b.y; }
constexpr bool operator!=(HexCoord a, HexCoord b) { return !(a == b); }

namespace hex {

constexpr int kDirections = 6;

// Axial coordinates: the representation in which hex distance is a simple formula.
struct Axial {
    int q = 0;
    int r = 0;
};

// (y & 1) is exact for negative y too on two's complement, and (y - (y & 1)) is always even.
constexpr Axial ToAxial(HexCoord c) { return Axial{c.x - (c.y - (c.y & 1)) / 2, c.y}; }
constexpr HexCoord FromAxial(Axial a) { return HexCoord{a.q + (a.r - (a.r & 1)) / 2, a.r}; }

constexpr int Abs(int v) { return v < 0 ? -v : v; }

// Minimum number of steps between two hexes on an unobstructed grid.
constexpr int Distance(HexCoord a, HexCoord b) {
    const Axial pa = ToAxial(a);
    const Axial pb = ToAxial(b);
    const int dq = pa.q - pb.q;
    const int dr = pa.r - pb.r;
    return (Abs(dq) + Abs(dr) + Abs(dq + dr)) / 2;
}

// Direction order is fixed: 0=E, 1=NE, 2=NW, 3=W, 4=SW, 5=SE. (North = smaller y.)
constexpr HexCoord Neighbor(HexCoord c, int direction) {
    constexpr int kEven[kDirections][2] = {{+1, 0}, {0, -1}, {-1, -1}, {-1, 0}, {-1, +1}, {0, +1}};
    constexpr int kOdd[kDirections][2] = {{+1, 0}, {+1, -1}, {0, -1}, {-1, 0}, {0, +1}, {+1, +1}};
    return (c.y & 1) ? HexCoord{c.x + kOdd[direction][0], c.y + kOdd[direction][1]}
                     : HexCoord{c.x + kEven[direction][0], c.y + kEven[direction][1]};
}

// The neighbor direction (0..5) of `from` that points most nearly at `to` (the one whose neighbor ends up
// closest to it; ties -> lowest direction). For an adjacent `to` this is exactly its direction. Stepping
// repeatedly in one direction from any hex walks a straight line, because Neighbor() accounts for the row parity.
constexpr int DirectionToward(HexCoord from, HexCoord to) {
    int best = 0;
    int bestDistance = Distance(Neighbor(from, 0), to);
    for (int d = 1; d < kDirections; ++d) {
        const int dist = Distance(Neighbor(from, d), to);
        if (dist < bestDistance) {
            bestDistance = dist;
            best = d;
        }
    }
    return best;
}

// Is `h` inside the 120-degree cone that starts at `origin`, points along neighbor direction `direction`, and is
// `length` hexes long? The cone is the two 60-degree wedges on either side of the axis (so it is 1 hex wide at range 1,
// 3 at range 2 with the axis in the middle, ... 2k+1 at range k). Exact integer test in axial coordinates.
constexpr bool InCone(HexCoord origin, int direction, HexCoord h, int length) {
    const int d = Distance(origin, h);
    if (d < 1 || d > length) return false;
    constexpr int kAxis[kDirections][2] = {{1, 0}, {1, -1}, {0, -1}, {-1, 0}, {-1, 1}, {0, 1}};  // axial (q, r) per direction
    const Axial a = ToAxial(origin);
    const Axial b = ToAxial(h);
    const int dq = b.q - a.q;
    const int dr = b.r - a.r;
    for (int wedge = 0; wedge < 2; ++wedge) {
        // wedge 0 spans axes (direction, direction+1); wedge 1 spans (direction-1, direction)
        const int i = wedge == 0 ? direction : (direction + kDirections - 1) % kDirections;
        const int j = wedge == 0 ? (direction + 1) % kDirections : direction;
        const int det = kAxis[i][0] * kAxis[j][1] - kAxis[i][1] * kAxis[j][0];  // +-1 for neighbouring axes
        const int coeffI = (dq * kAxis[j][1] - dr * kAxis[j][0]) / det;
        const int coeffJ = (kAxis[i][0] * dr - kAxis[i][1] * dq) / det;
        if (coeffI >= 0 && coeffJ >= 0) return true;
    }
    return false;
}

constexpr bool InBounds(HexCoord c, int width, int height) {
    return c.x >= 0 && c.x < width && c.y >= 0 && c.y < height;
}

// Writes the in-bounds neighbors of `c` into `out` in direction order; returns how many (0..6).
inline int Neighbors(HexCoord c, int width, int height, std::array<HexCoord, kDirections>& out) {
    int n = 0;
    for (int d = 0; d < kDirections; ++d) {
        const HexCoord next = Neighbor(c, d);
        if (InBounds(next, width, height)) out[static_cast<std::size_t>(n++)] = next;
    }
    return n;
}

}  // namespace hex

// ---- Board <-> arena ---------------------------------------------------------------------

// The two sides of a fight. Home is drawn at the top of the arena (rows 0..3), Away at the
// bottom (rows 4..7). A player's board frame has y = 0 as the BACK row and y = kBoardRows-1
// as the FRONT row; in the arena the two front rows meet in the middle.
enum class ArenaSide : std::uint8_t { Home, Away };

// Home keeps its coordinates. Away is the same board rotated 180 degrees about the arena
// centre: (x, y) -> (kArenaColumns-1-x, kArenaRows-1-y). Because the arena has an even number
// of rows and the layout is odd-r, this rotation maps adjacent hexes to adjacent hexes, so a
// formation looks the same to both players and distances inside a board are preserved.
constexpr HexCoord BoardToArena(int boardX, int boardY, ArenaSide side) {
    return side == ArenaSide::Home ? HexCoord{boardX, boardY}
                                   : HexCoord{kArenaColumns - 1 - boardX, kArenaRows - 1 - boardY};
}

// ---- Grid + pathfinding ------------------------------------------------------------------

// Occupancy of a rectangular hex grid. Out-of-bounds counts as blocked.
class HexGrid {
public:
    HexGrid(int width, int height);

    int Width() const { return width_; }
    int Height() const { return height_; }
    bool InBounds(HexCoord c) const { return hex::InBounds(c, width_, height_); }
    bool IsBlocked(HexCoord c) const { return !InBounds(c) || blocked_[Index(c)] != 0; }
    void SetBlocked(HexCoord c, bool blocked);
    void Clear();
    std::size_t Index(HexCoord c) const { return static_cast<std::size_t>(c.y * width_ + c.x); }

private:
    int width_;
    int height_;
    std::vector<std::uint8_t> blocked_;
};

// Breadth-first shortest paths. On a uniform-cost grid BFS returns the same shortest paths as
// A*, without a heuristic, and for boards this small (56 cells) it is the cheaper of the two.
// Neighbors are expanded in fixed direction order, so ties between equal-length paths always
// resolve the same way. The object keeps its scratch buffers between calls; reuse one per fight.
class HexPathfinder {
public:
    HexPathfinder(int width, int height);

    // Shortest path start -> goal. `outPath` receives every step AFTER start, ending at goal
    // (empty if start == goal). Blocked hexes are never entered, and the goal itself must be
    // free. The start hex is exempt from the blocked check (a unit standing on its own hex).
    // Returns false (outPath empty) if the goal is unreachable.
    bool FindPath(const HexGrid& grid, HexCoord start, HexCoord goal, std::vector<HexCoord>& outPath);

    // Shortest path to the NEAREST free hex from which `target` is within `range` hexes
    // (target's own, usually blocked, hex is never entered). Empty path + true if `start` is
    // already in range. This is what a melee unit wants: "get next to that enemy".
    bool FindPathToRange(const HexGrid& grid, HexCoord start, HexCoord target, int range,
                         std::vector<HexCoord>& outPath);

private:
    template <typename IsGoal>
    bool Search(const HexGrid& grid, HexCoord start, IsGoal isGoal, std::vector<HexCoord>& outPath);

    int width_;
    int height_;
    std::uint32_t stamp_ = 0;
    std::vector<std::uint32_t> visitedStamp_;
    std::vector<int> parent_;
    std::vector<int> queue_;
};

}  // namespace w2f
