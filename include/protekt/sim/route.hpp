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
#include "protekt/sim/steering.hpp"

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

/// A unit that has not left a `kStallRadiusTiles` circle for this many ticks (ten seconds) is
/// making no headway, whatever it is doing inside the circle -- standing, pacing, jittering. A
/// turret spends 90 of them unmounting before it moves at all, so this is well clear of that.
inline constexpr std::uint16_t kStallTicks = 300;
inline constexpr int kStallRadiusTiles = 1;
/// How far (Chebyshev, tiles) from a goal tile that cannot be entered the planner will look for
/// ground that can, before it says there is no route.
inline constexpr int kGoalSearchTiles = 24;
/// How many times one order's route may stop serving its unit (it was thrown off, or drifted onto
/// a line the route never vouched for) before the order is given up as `NoProgress`: a unit that
/// keeps being put back where it began is going round in a loop no stall circle can see.
inline constexpr std::uint8_t kReplanBudget = 8;

/// WHY a unit has no route to follow -- the NAMED failure, readable by anything that holds the
/// unit. `None` while the unit is following a plan or has not planned; a unit that has a failure
/// is not driving: `steer` ends its order (it is Idle) rather than push at the goal for ever.
enum class RouteFailure : std::uint8_t {
    None = 0,
    NoRoute = 1,    ///< the planner searched the whole grid: nothing reaches the goal
    NoProgress = 2, ///< it had a route and was driving, but held to one spot for `kStallTicks`
};

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
    RouteFailure failure{RouteFailure::None};
    /// The goal tile can be entered but not held (a rim the slope throws a unit off): the route
    /// ends beside it, and a unit that has come to the end of it has arrived.
    bool hold_goal{false};
    /// The goal tile cannot be entered at all (a crater, a building's middle): the route ends
    /// beside it. A unit going to a fixed spot has arrived at the end of it; one doing work at the
    /// goal (building, attacking) goes on steering at the goal itself from there.
    bool ring_goal{false};
    /// The no-progress rule: where the unit was when it last left its circle (raw, 1/65536 tile)
    /// and how many ticks it has been inside it. `stall_ticks == 0` means no anchor is set.
    std::int32_t anchor_x{};
    std::int32_t anchor_y{};
    std::uint16_t stall_ticks{};
    std::uint8_t strays{}; ///< times this order's route stopped serving the unit (`kReplanBudget`)

    [[nodiscard]] constexpr bool following() const noexcept { return next < count; }
};

namespace route_detail {

[[nodiscard]] constexpr bool in_map(int x, int y) noexcept {
    return x >= 0 && y >= 0 && x < kRouteTiles && y < kRouteTiles;
}

/// The way a tile's slope runs when it THROWS a unit: the press there beats a unit's whole thrust
/// by more than half its step (`press_past_thrust`), so no engine holds any line on it but the one
/// the slope runs -- the old slide's criterion, a drift past half a step, read off the press. As
/// the nearest of the eight unit steps; (0, 0) for a tile that does not throw. The same for every
/// unit (one thrust-to-weight: see steering.hpp), so it takes no speed. A ground without heights
/// has none. A tile is two drawn triangles (`face_triangles`, G7); the one that throws harder is
/// the tile's answer, as the wall is either triangle's.
template <class Ground>
[[nodiscard]] std::array<int, 2> slide_way_of(const Ground& ground, int x, int y) {
    if constexpr (HasHeights<Ground>) {
        const auto height = [&](int tx, int ty) { return ground.height(tx, ty); };
        const std::array<Gradient, 2> faces = face_triangles(height, x, y);
        const Step upper = press_past_thrust(faces[0], Fixed::kOne);
        const Step lower = press_past_thrust(faces[1], Fixed::kOne);
        const auto size = [](Step s) { return (s.dx * s.dx) + (s.dy * s.dy); };
        const Step drift = size(lower) > size(upper) ? lower : upper;
        if (size(drift) * 4 <= std::int64_t{Fixed::kOne} * Fixed::kOne) {
            return {0, 0};
        }
        // Down the slope, to the nearest octant: a component counts when it is more than
        // tan(22.5 degrees) (5/12) of the other, so a drift a few degrees off an axis is that
        // axis, not a diagonal.
        const std::int64_t ax = std::abs(drift.dx);
        const std::int64_t ay = std::abs(drift.dy);
        const int sx = ax * 12 > ay * 5 ? (drift.dx > 0 ? 1 : -1) : 0;
        const int sy = ay * 12 > ax * 5 ? (drift.dy > 0 ? 1 : -1) : 0;
        return {sx, sy};
    } else {
        return {0, 0};
    }
}

/// True when the edge (x, y) -> (nx, ny) is NOT open: the face it drives into is a wall that way
/// (`drivable` -- THE rule, the very question the mover asks, so the planner never opens an edge
/// the mover will not drive), or the face it leaves beats a unit heading that way so it never gets
/// there (the same question, of the tile it stands on), or -- a diagonal -- either face beside the
/// corner it cuts is a wall along it (the same question again), or the tile it leaves holds the
/// unit and the edge is not the way the slope runs (unless `free_exit`: the unit's own tile).
/// `height(x, y)` and `way(x, y)` are the ground's heights and `slide_way_of`, handed in so the
/// planner can cache them and a one-off check need not.
template <class Height, class Way>
[[nodiscard]] bool edge_blocked(const Height& height, const Way& way, int x, int y, int nx, int ny,
                                bool free_exit = false) {
    if (!drivable(height, nx, ny, nx - x, ny - y)) {
        return true; // a wall: the face beyond beats any engine heading that way
    }
    if (!drivable(height, x, y, nx - x, ny - y)) {
        return true; // uphill too steep: the face it stands on beats any engine heading that way
    }
    // NO CORNER CUTTING past a wall (G6): a diagonal passes through the corner it cuts, and a unit
    // following it to within half a tile crosses the two faces beside that corner -- so they must
    // not beat an engine heading along the diagonal either, as the two tiles beside it must be
    // enterable (`plan_route`). Without this the turret of GoAround's reported case was led
    // across the corner of the ramp's flank, (17, 27): a face rising 50 a tile south, 42 along
    // its diagonal, that held it there until its order gave up.
    if (nx != x && ny != y
        && (!drivable(height, nx, y, nx - x, ny - y) || !drivable(height, x, ny, nx - x, ny - y))) {
        return true;
    }
    if (free_exit) {
        return false; // the tile the unit already stands on: it sets off whichever way it can
    }
    const std::array<int, 2> run = way(x, y);
    return (run[0] != 0 || run[1] != 0) && (nx - x != run[0] || ny - y != run[1]);
}

/// Does the straight line between two tile centers stay on enterable ground? Sampled at half-tile
/// steps in integers; the local steering layer handles anything grazing a rim. `cliff(from, to)` is
/// asked for each edge the line crosses from one tile into the next (a diagonal step included), so
/// a line does not cut across a cliff the mover would refuse. `avoid(x, y)` names tiles the line
/// may not pass through at all (other than its own two ends): the clearance the planner keeps from
/// ground that throws a unit off.
template <class Ground, class Cliff, class Avoid>
[[nodiscard]] bool line_clear(const Ground& ground, TileXY a, TileXY b, const Cliff& cliff,
                              const Avoid& avoid) noexcept {
    const int dx = static_cast<int>(b.x) - a.x;
    const int dy = static_cast<int>(b.y) - a.y;
    const int n = 2 * std::max(std::abs(dx), std::abs(dy));
    if (n == 0) {
        return true;
    }
    int last_x = a.x;
    int last_y = a.y;
    for (int i = 0; i <= n; ++i) {
        const int cx = (2 * a.x + 1) * n + 2 * dx * i; // in units of 1 / (2n) tile
        const int cy = (2 * a.y + 1) * n + 2 * dy * i;
        const int tile_x = cx / (2 * n);
        const int tile_y = cy / (2 * n);
        if (ground.factor(tile_x, tile_y) == 0) {
            return false;
        }
        if ((tile_x != last_x || tile_y != last_y) && cliff(last_x, last_y, tile_x, tile_y)) {
            return false;
        }
        const bool end = (tile_x == a.x && tile_y == a.y) || (tile_x == b.x && tile_y == b.y);
        if (!end && avoid(tile_x, tile_y)) {
            return false;
        }
        last_x = tile_x;
        last_y = tile_y;
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
/// THE MOVER'S OWN RULE for an edge (a ground that knows heights): an edge is open when neither the
/// face it leaves nor the face it enters beats an engine heading along it (`drivable`, the one rule
/// the mover asks too: the wall is the press's), and a tile the slope will not let a unit hold (a
/// parked drift past half its step: `slide_way_of`) is left only the way the slope runs. A route
/// that ignores the second leads a unit onto a ramp's edge row, which throws it off: the stall
/// this answers.
///
/// A GOAL THAT CANNOT BE ENTERED (a building's centre, a unit standing on solid ground) is not a
/// failure: the plan ends at ground that can be, within one tile of the nearest such ground to the
/// goal on any side, and the unit's own stopping rule (weapon range, build reach) takes it from
/// there. Only a goal with NO enterable ground within `kGoalSearchTiles`, or one the grid cannot
/// reach, is `NoRoute`.
///
/// On success `route` holds the corners of the line-of-sight-smoothed path (the start excluded, the
/// goal last) and true is returned. On failure `route` is left with no waypoints, `failure` is
/// `NoRoute`, and false is returned. The plan takes no speed: which edge is a wall (`drivable`)
/// and which tile the press holds a unit on (`slide_way_of`) are the same for every unit.
///
/// `max_expanded` bounds the search: a plan that has closed that many tiles without reaching the
/// goal set gives up as `NoRoute`. Unbounded by default -- every plan a unit makes for an order
/// searches the whole grid, as it always has; the bound is for a plan made in passing (G13's way
/// round a friend, `kFriendPlanBudget`), whose failure only means "wait", and which must not cost
/// a search of the whole map each time a crowd hems a unit in.
template <class Ground>
[[nodiscard]] bool plan_route(const Ground& ground, TileXY start, TileXY goal, Route& route,
                              int max_expanded = 0x7FFFFFFF) {
    using route_detail::in_map;
    constexpr int kCells = kRouteTiles * kRouteTiles;
    constexpr int kStraight = 10;
    constexpr int kDiagonal = 14;
    constexpr int kInf = 0x7FFFFFFF;
    constexpr int kClearance = 12; ///< what a step beside ground that throws a unit off costs

    route.count = 0;
    route.next = 0;
    route.partial = false;
    route.goal = goal;
    route.from = start;
    route.has_goal = true;
    route.age = 0;
    route.failure = RouteFailure::None;
    route.stall_ticks = 0;

    const auto index = [](int x, int y) { return y * kRouteTiles + x; };

    // THE WALL (M1, G6) AND THE SLOPE: where the ground knows heights, a face the press beats an
    // engine on is a wall to the mover (`plan_step` asks `drivable`), so it is a wall here too, by
    // the same call -- or a route
    // would lead a unit up to a face it will not climb and leave it there. Each tile's height and
    // slide way are worked out once. The press does not weigh the route (a declared
    // simplification -- the planner takes the straightest way through ground that can be driven,
    // not the gentlest), but a tile the press will not let the unit hold is left only the way the
    // press runs: the edge rows of a ramp are not part of the ramp. And a route keeps a tile's
    // clearance from such ground (`near_hold`): a unit follows a line to within half a tile, and
    // one that clips a rim is thrown off it.
    std::vector<std::int16_t> heights;
    std::vector<std::uint8_t> height_known;
    std::vector<std::uint8_t> slide_known; // 0 unknown, else 1 + (way_x + 1) * 3 + (way_y + 1)
    std::vector<std::uint8_t> near_known;  // 0 unknown, 1 clear, 2 beside a holding tile
    if constexpr (HasHeights<Ground>) {
        heights.assign(kCells, 0);
        height_known.assign(kCells, 0);
        slide_known.assign(kCells, 0);
        near_known.assign(kCells, 0);
    }
    const auto height_at = [&](int x, int y) -> int {
        if constexpr (HasHeights<Ground>) {
            if (!in_map(x, y)) {
                return ground.height(x, y); // a face's far corners, past the last row: not cached
            }
            const int at = index(x, y);
            if (height_known[at] == 0) {
                heights[at] = static_cast<std::int16_t>(ground.height(x, y));
                height_known[at] = 1;
            }
            return heights[at];
        } else {
            return 0;
        }
    };
    const auto slide_way = [&](int x, int y) -> std::array<int, 2> {
        if constexpr (HasHeights<Ground>) {
            if (!in_map(x, y)) {
                return {0, 0};
            }
            const int at = index(x, y);
            if (slide_known[at] == 0) {
                const std::array<int, 2> way = route_detail::slide_way_of(ground, x, y);
                slide_known[at] = static_cast<std::uint8_t>(1 + (way[0] + 1) * 3 + (way[1] + 1));
            }
            const int code = slide_known[at] - 1;
            return {code / 3 - 1, code % 3 - 1};
        } else {
            return {0, 0};
        }
    };
    const auto near_hold = [&](int x, int y) -> bool {
        if constexpr (HasHeights<Ground>) {
            const int at = index(x, y);
            if (near_known[at] == 0) {
                bool beside = false;
                for (int oy = -1; oy <= 1 && !beside; ++oy) {
                    for (int ox = -1; ox <= 1; ++ox) {
                        const std::array<int, 2> way = slide_way(x + ox, y + oy);
                        if ((ox != 0 || oy != 0) && (way[0] != 0 || way[1] != 0)) {
                            beside = true;
                            break;
                        }
                    }
                }
                near_known[at] = beside ? 2 : 1;
            }
            return near_known[at] == 2;
        } else {
            return false;
        }
    };
    // True when the edge (x, y) -> (nx, ny) is NOT open to this unit.
    const auto cliff = [&](int x, int y, int nx, int ny) -> bool {
        if constexpr (HasHeights<Ground>) {
            return route_detail::edge_blocked(height_at, slide_way, x, y, nx, ny,
                                              x == start.x && y == start.y);
        } else {
            return false;
        }
    };
    // The goal set: the goal tile itself when the unit can enter AND HOLD it, else every such tile
    // within one ring of the nearest (the near side of a building, not only the far one). A tile
    // the slope will not let the unit hold (a plateau's rim) is no place to end a drive: it would
    // arrive, be thrown off, and plan the long way back for ever.
    const auto standable = [&](int x, int y) {
        const std::array<int, 2> way = slide_way(x, y);
        return ground.factor(x, y) != 0 && way[0] == 0 && way[1] == 0;
    };
    // ...and on the goal's own level: ground a cliff's height away is no nearer in any way that
    // matters, whatever its distance on the grid.
    // ("A cliff's height" is what the wall comes to on one axis face: `steepest_drivable_rise`,
    // read off the press, not a limit of its own.)
    const auto level_with_goal = [&](int x, int y) {
        return std::abs(height_at(x, y) - height_at(goal.x, goal.y)) <= steepest_drivable_rise();
    };
    int reach = 0;
    bool want_level = true; // dropped when there is no ground on the goal's level to be had
    route.hold_goal = false;
    route.ring_goal = false;
    if (!standable(goal.x, goal.y)) {
        route.hold_goal = ground.factor(goal.x, goal.y) != 0;
        route.ring_goal = !route.hold_goal;
        int nearest = 0;
        for (int pass = 0; pass < 2 && nearest == 0; ++pass) {
            want_level = pass == 0;
            for (int ring = 1; ring <= kGoalSearchTiles && nearest == 0; ++ring) {
                for (int y = goal.y - ring; y <= goal.y + ring && nearest == 0; ++y) {
                    for (int x = goal.x - ring; x <= goal.x + ring; ++x) {
                        if (std::max(std::abs(x - goal.x), std::abs(y - goal.y)) == ring
                            && route_detail::in_map(x, y) && standable(x, y)
                            && (!want_level || level_with_goal(x, y))) {
                            nearest = ring;
                            break;
                        }
                    }
                }
            }
        }
        if (nearest == 0) {
            route.failure = RouteFailure::NoRoute;
            return false;
        }
        reach = nearest + 1;
    }
    const auto in_goal_set = [&](int x, int y) {
        return std::max(std::abs(x - goal.x), std::abs(y - goal.y)) <= reach
               && (reach == 0 || (standable(x, y) && (!want_level || level_with_goal(x, y))));
    };
    if (in_goal_set(start.x, start.y)) {
        return true; // already there: an empty route that did not fail
    }
    const auto heuristic = [&](int x, int y) {
        const int dx = std::max(0, std::abs(x - goal.x) - reach);
        const int dy = std::max(0, std::abs(y - goal.y) - reach);
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
    TileXY goal_reached = goal;
    int expanded = 0;
    while (!open.empty() && expanded < max_expanded) {
        const auto [f, h, x, y] = open.top();
        open.pop();
        (void)f;
        (void)h;
        const int here = index(x, y);
        if (closed[here] != 0) {
            continue;
        }
        closed[here] = 1;
        ++expanded;
        if (in_goal_set(x, y)) {
            found = true;
            goal_reached = TileXY{static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y)};
            break;
        }
        for (const auto& d : kRing) {
            const int nx = x + d[0];
            const int ny = y + d[1];
            if (!in_map(nx, ny) || ground.factor(nx, ny) == 0 || cliff(x, y, nx, ny)) {
                continue;
            }
            const bool diagonal = d[0] != 0 && d[1] != 0;
            if (diagonal && (ground.factor(x + d[0], y) == 0 || ground.factor(x, y + d[1]) == 0)) {
                continue;
            }
            // Nor may a diagonal graze a tile the slope throws a unit off: it is followed to within
            // half a tile, and the corner of that tile is where it is clipped.
            if (diagonal) {
                const std::array<int, 2> side_a = slide_way(x + d[0], y);
                const std::array<int, 2> side_b = slide_way(x, y + d[1]);
                if (side_a[0] != 0 || side_a[1] != 0 || side_b[0] != 0 || side_b[1] != 0) {
                    continue;
                }
            }
            const int there = index(nx, ny);
            if (closed[there] != 0) {
                continue;
            }
            const int cost = best[here] + (diagonal ? kDiagonal : kStraight)
                             + (near_hold(nx, ny) ? kClearance : 0);
            if (cost < best[there]) {
                best[there] = cost;
                parent[there] = here;
                const int hh = heuristic(nx, ny);
                open.emplace(cost + hh, hh, nx, ny);
            }
        }
    }
    if (!found) {
        route.failure = RouteFailure::NoRoute;
        return false;
    }

    // Walk back to the start, then reverse into start -> goal order.
    std::vector<TileXY> path;
    for (int at = index(goal_reached.x, goal_reached.y); at != -1; at = parent[at]) {
        path.push_back(TileXY{static_cast<std::uint8_t>(at % kRouteTiles),
                              static_cast<std::uint8_t>(at / kRouteTiles)});
    }
    std::reverse(path.begin(), path.end());

    // Smooth: from each anchor, go as far along the path as the straight line stays clear.
    std::vector<TileXY> corners;
    std::size_t anchor = 0;
    while (anchor + 1 < path.size()) {
        std::size_t reach = anchor + 1;
        while (
            reach + 1 < path.size()
            && route_detail::line_clear(ground, path[anchor], path[reach + 1], cliff, near_hold)) {
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

/// Is the straight line from tile `a` to tile `b` one this unit can drive: enterable ground, and
/// no edge the planner would call blocked? The unit steers along exactly this line to its next
/// waypoint, so when it is false the unit has drifted off its route onto a line the route never
/// vouched for, and it should plan again from where it stands.
template <class Ground>
[[nodiscard]] bool route_leg_open(const Ground& ground, TileXY a, TileXY b) {
    const auto height = [&](int x, int y) -> int {
        if constexpr (HasHeights<Ground>) {
            return ground.height(x, y);
        } else {
            return 0;
        }
    };
    const auto way = [&](int x, int y) { return route_detail::slide_way_of(ground, x, y); };
    const auto cliff = [&](int x, int y, int nx, int ny) {
        if constexpr (HasHeights<Ground>) {
            return route_detail::edge_blocked(height, way, x, y, nx, ny, x == a.x && y == a.y);
        } else {
            return false;
        }
    };
    return route_detail::line_clear(ground, a, b, cliff, [](int, int) { return false; });
}

/// THE NO-PROGRESS RULE, for a unit that is driving: one tick of it. The unit's position (raw) is
/// compared with the anchor it last left; inside a circle of `kStallRadiusTiles` for `kStallTicks`
/// ticks it is stuck -- however it is moving inside the circle, standing, pacing or jittering --
/// and the failure is named (`NoProgress`) and returned true. Leaving the circle moves the anchor.
[[nodiscard]] inline bool route_stalled(Route& route, std::int32_t x_raw,
                                        std::int32_t y_raw) noexcept {
    const std::int64_t radius = static_cast<std::int64_t>(kStallRadiusTiles) * Fixed::kOne;
    const std::int64_t dx = static_cast<std::int64_t>(x_raw) - route.anchor_x;
    const std::int64_t dy = static_cast<std::int64_t>(y_raw) - route.anchor_y;
    if (route.stall_ticks == 0 || dx * dx + dy * dy >= radius * radius) {
        route.anchor_x = x_raw;
        route.anchor_y = y_raw;
        route.stall_ticks = 1;
        return false;
    }
    if (++route.stall_ticks < kStallTicks) {
        return false;
    }
    route.failure = RouteFailure::NoProgress;
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
