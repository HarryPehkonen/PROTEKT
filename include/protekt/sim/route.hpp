#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <queue>
#include <tuple>
#include <vector>

#include "protekt/sim/fixed.hpp"

namespace protekt::sim {

/// A tile on the map. The map is 256 tiles square, so a tile coordinate is one byte.
struct TileXY {
    std::uint8_t x{};
    std::uint8_t y{};
};
[[nodiscard]] constexpr bool operator==(TileXY a, TileXY b) noexcept {
    return a.x == b.x && a.y == b.y;
}

/// The longest route a unit remembers. A longer one is kept as its first kMaxWaypoints corners and
/// marked `partial`: the unit plans again from where it stands when it runs out.
inline constexpr int kMaxWaypoints = 24;
inline constexpr int kRouteTiles = 256; ///< the map's side; units.hpp asserts it equals kMapTiles

/// A re-plan for a moved goal or a wandered unit waits at least this long after the last plan.
inline constexpr std::uint16_t kReplanMinTicks = 30;
/// The goal tile may drift this far (Chebyshev, tiles) before the route is considered stale.
inline constexpr int kReplanGoalDriftTiles = 2;
/// A unit further than this from the leg it should be on has wedged despite the route.
inline constexpr int kReplanOffRouteTiles = 3;
/// The age a route is given when a new order lands, so a changed goal re-plans at once.
inline constexpr std::uint16_t kRouteAgeFresh = 0xFFFF;

/// The unit's planned route: tile-center waypoints and the goal tile they serve. State, not a
/// decision -- it lives on the unit and in `state_hash()`, because it decides every step.
struct Route {
    std::array<TileXY, kMaxWaypoints> points{};
    std::uint8_t count{}; ///< waypoints stored
    std::uint8_t next{};  ///< the waypoint being followed; `next == count` means the route is spent
    bool has_goal{false}; ///< a plan was attempted for `goal` (a failed one leaves count == 0)
    bool partial{false};  ///< the real route continues beyond points[count - 1]
    TileXY goal{};
    TileXY from{}; ///< where the current leg began: the plan's start, then each waypoint reached
    std::uint16_t age{kRouteAgeFresh}; ///< ticks since the last plan, saturating

    [[nodiscard]] constexpr bool following() const noexcept { return next < count; }
};

namespace route_detail {

[[nodiscard]] constexpr bool in_map(int x, int y) noexcept {
    return x >= 0 && y >= 0 && x < kRouteTiles && y < kRouteTiles;
}

/// Does the straight line between two tile centers stay on enterable ground? Sampled at half-tile
/// steps in integers; the local steering layer handles anything grazing a rim.
template <class Ground>
[[nodiscard]] bool line_clear(const Ground& ground, TileXY a, TileXY b) noexcept {
    const int dx = static_cast<int>(b.x) - a.x;
    const int dy = static_cast<int>(b.y) - a.y;
    const int n = 2 * std::max(std::abs(dx), std::abs(dy));
    if (n == 0) {
        return true;
    }
    for (int i = 0; i <= n; ++i) {
        const int cx = (2 * a.x + 1) * n + 2 * dx * i; // in units of 1 / (2n) tile
        const int cy = (2 * a.y + 1) * n + 2 * dy * i;
        if (ground.factor(cx / (2 * n), cy / (2 * n)) == 0) {
            return false;
        }
    }
    return true;
}

} // namespace route_detail

/// Deterministic integer A* over the tiles, using THE mover's passability: `ground.factor(x, y)`
/// is zero for a tile that cannot be entered. Eight-connected, no corner cutting (a diagonal needs
/// both orthogonal neighbours open), cost 10 per straight step and 14 per diagonal, and the open
/// set is ordered by lowest f, then lowest h, then lowest x, then lowest y -- no tie is left to
/// chance, so every platform finds the same route.
///
/// On success `route` holds the corners of the line-of-sight-smoothed path (the start excluded, the
/// goal last) and true is returned. On failure (the goal is unreachable or not enterable) `route`
/// is left with no waypoints and false is returned.
template <class Ground>
[[nodiscard]] bool plan_route(const Ground& ground, TileXY start, TileXY goal, Route& route) {
    using route_detail::in_map;
    constexpr int kCells = kRouteTiles * kRouteTiles;
    constexpr int kStraight = 10;
    constexpr int kDiagonal = 14;
    constexpr int kInf = 0x7FFFFFFF;

    route.count = 0;
    route.next = 0;
    route.partial = false;
    route.goal = goal;
    route.from = start;
    route.has_goal = true;
    route.age = 0;

    if (ground.factor(goal.x, goal.y) == 0) {
        return false;
    }
    if (start == goal) {
        return true; // already there: an empty route that did not fail
    }

    const auto index = [](int x, int y) { return y * kRouteTiles + x; };
    const auto heuristic = [&](int x, int y) {
        const int dx = std::abs(x - goal.x);
        const int dy = std::abs(y - goal.y);
        return kStraight * (dx + dy) + (kDiagonal - 2 * kStraight) * std::min(dx, dy);
    };

    std::vector<int> best(kCells, kInf);
    std::vector<int> parent(kCells, -1);
    std::vector<std::uint8_t> closed(kCells, 0);
    // (f, h, x, y): std::tuple orders lexicographically, so the greater-comparator pops the lowest.
    using Node = std::tuple<int, int, int, int>;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;

    best[index(start.x, start.y)] = 0;
    open.emplace(heuristic(start.x, start.y), heuristic(start.x, start.y), start.x, start.y);

    constexpr std::array<std::array<int, 2>, 8> kRing{{
        {0, -1},
        {-1, 0},
        {1, 0},
        {0, 1},
        {-1, -1},
        {1, -1},
        {-1, 1},
        {1, 1},
    }};

    bool found = false;
    while (!open.empty()) {
        const auto [f, h, x, y] = open.top();
        open.pop();
        (void)f;
        (void)h;
        const int here = index(x, y);
        if (closed[here] != 0) {
            continue;
        }
        closed[here] = 1;
        if (x == goal.x && y == goal.y) {
            found = true;
            break;
        }
        for (const auto& d : kRing) {
            const int nx = x + d[0];
            const int ny = y + d[1];
            if (!in_map(nx, ny) || ground.factor(nx, ny) == 0) {
                continue;
            }
            const bool diagonal = d[0] != 0 && d[1] != 0;
            if (diagonal && (ground.factor(x + d[0], y) == 0 || ground.factor(x, y + d[1]) == 0)) {
                continue;
            }
            const int there = index(nx, ny);
            if (closed[there] != 0) {
                continue;
            }
            const int cost = best[here] + (diagonal ? kDiagonal : kStraight);
            if (cost < best[there]) {
                best[there] = cost;
                parent[there] = here;
                const int hh = heuristic(nx, ny);
                open.emplace(cost + hh, hh, nx, ny);
            }
        }
    }
    if (!found) {
        return false;
    }

    // Walk back to the start, then reverse into start -> goal order.
    std::vector<TileXY> path;
    for (int at = index(goal.x, goal.y); at != -1; at = parent[at]) {
        path.push_back(TileXY{static_cast<std::uint8_t>(at % kRouteTiles),
                              static_cast<std::uint8_t>(at / kRouteTiles)});
    }
    std::reverse(path.begin(), path.end());

    // Smooth: from each anchor, go as far along the path as the straight line stays clear.
    std::vector<TileXY> corners;
    std::size_t anchor = 0;
    while (anchor + 1 < path.size()) {
        std::size_t reach = anchor + 1;
        while (reach + 1 < path.size()
               && route_detail::line_clear(ground, path[anchor], path[reach + 1])) {
            ++reach;
        }
        corners.push_back(path[reach]);
        anchor = reach;
    }
    const std::size_t keep = std::min<std::size_t>(corners.size(), kMaxWaypoints);
    for (std::size_t i = 0; i < keep; ++i) {
        route.points.at(i) = corners[i];
    }
    route.count = static_cast<std::uint8_t>(keep);
    route.partial = corners.size() > keep;
    return true;
}

/// Squared distance from a point to the segment a-b, all in 1/256 tiles (so nothing overflows).
[[nodiscard]] constexpr std::int64_t point_segment_dist2(std::int64_t px, std::int64_t py,
                                                         std::int64_t ax, std::int64_t ay,
                                                         std::int64_t bx,
                                                         std::int64_t by) noexcept {
    const std::int64_t abx = bx - ax;
    const std::int64_t aby = by - ay;
    const std::int64_t len2 = abx * abx + aby * aby;
    std::int64_t cx = ax;
    std::int64_t cy = ay;
    if (len2 != 0) {
        const std::int64_t dot = (px - ax) * abx + (py - ay) * aby;
        if (dot >= len2) {
            cx = bx;
            cy = by;
        } else if (dot > 0) {
            cx = ax + abx * dot / len2;
            cy = ay + aby * dot / len2;
        }
    }
    return (px - cx) * (px - cx) + (py - cy) * (py - cy);
}

} // namespace protekt::sim
