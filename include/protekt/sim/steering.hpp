#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>

#include "protekt/sim/fixed.hpp"

namespace protekt::sim {

/// One step, in the same units as a velocity, already scaled by the terrain it lands on.
struct Step {
    std::int64_t dx{};
    std::int64_t dy{};
};

/// Working around an obstacle is a STATE, not a decision.
///
/// A one-tick escape cannot work: `steer` re-aims straight at the goal on every tick, so a unit
/// that dodges a crater rim for a single tick walks straight back into it and the two moves cancel.
/// The unit has to remember that it is going around something, which is why this lives on the unit
/// and therefore inside `state_hash()`: a replay has to reproduce the same walk.
struct Detour {
    std::int64_t dx{};
    std::int64_t dy{};
    std::uint16_t ticks_left{};
    [[nodiscard]] constexpr bool active() const noexcept { return ticks_left != 0; }
};

/// Everything the decision needs, and nothing it does not.
struct StepRequest {
    Fixed x{};
    Fixed y{};
    /// The step the unit wants this tick, before the terrain has its say.
    std::int64_t want_dx{};
    std::int64_t want_dy{};
    /// The unit's full speed, in the same units, which is how big an escape step is.
    std::int64_t speed{};
    Detour detour{};
};

struct StepPlan {
    /// The move to make, already scaled by the terrain of the tile it lands on. Empty when
    /// `moved` is false.
    Step step{};
    Detour detour{};
    /// False when there is genuinely nowhere to go: the caller stops the unit for this tick.
    bool moved{false};
};

/// How long an escape direction is followed before the straight line is reconsidered: short enough
/// that a unit does not wander, long enough to clear a rim.
inline constexpr std::uint16_t kDetourTicks = 12;

/// cos(45 degrees) as a fixed ratio, so a diagonal escape is not 41% faster than an axis one.
inline constexpr std::int64_t kDiagonalNumerator = 181;
inline constexpr std::int64_t kDiagonalDenominator = 256;

/// THE SLOPE (M1). Everything below is integer arithmetic on the INTEGER tile heights, so the slope
/// is as hashable as the world it is read from: there are no forces and no gravity, only the signed
/// height difference between the tile a unit stands on and the tile it would enter.
///
/// THE ONE FACTOR FOR EVERY UNIT: a declared simplification. Harri's ruling was "one factor for
/// every unit -- a declared simplification that can become per-kind later, once you've played it".
/// A light scout and a heavy walker climb, descend and slide by EXACTLY these numbers; nothing here
/// takes a unit kind, a mass or a speed, so a per-kind factor is a parameter added to these
/// functions later and not a rewrite.
///
/// Heights are in the world's units (kMinHeight..kMaxHeight, +-512); a "rise" is the height of the
/// tile entered minus the height of the tile left, so uphill is positive. Measured over seeds 1, 7
/// and 42, a tile edge rises by at most 8 half the time, 24 for 94% of edges, 40 for 99.8%, and 55
/// at the very worst; the numbers below are chosen against that spread.

/// How much rise takes the whole speed away: speed is (1 - rise / kGradeSpan) of the terrain's,
/// so a rise of 32 halves it and a drop of 32 gives half as much again.
inline constexpr int kGradeSpan = 64;
/// The slowest a climb gets and the fastest a descent gets, as multipliers of the level speed.
/// The floor keeps a climbable slope from freezing a unit solid; the ceiling keeps a long descent
/// from out-running the shot that is aimed at it.
inline constexpr std::int64_t kMinGrade = Fixed::kOne / 4;
inline constexpr std::int64_t kMaxGrade = Fixed::kOne * 3 / 2;
/// THE SLIDE: a unit stands firm while its tile drops away by no more than this to its steepest
/// downhill neighbour. 32 is the drop at which a climb is already at HALF speed (`kGradeSpan` / 2):
/// ground you could not climb faster than that does not hold you. About one tile edge in eighty
/// exceeds it (24, the world's own "Rough" figure, was tried first and set a fifth of all tiles
/// sliding: a hillside, not the map).
inline constexpr int kSlideThreshold = 32;
/// Raw drift per tick for every unit of drop past `kSlideThreshold`: a sixteenth of a tank's
/// 13107-raw step (0.2 tiles). On a slope of 39 that is 7 * 819 = 5733 raw downhill, which beats
/// the 0.39 grade (5119 raw) a tank manages uphill there: the slope wins, and that is where the
/// climb limit comes from (`kClimbLimit`). Nearer the threshold the drift is small and a driven
/// unit can still climb.
inline constexpr std::int64_t kSlideRawPerHeight = 819;
/// The drift is never faster than a tank's step, however sheer the face (the world's worst edge is
/// a drop of 55, which would ask for 1.4 steps): a slide is "slowly", Harri's word.
inline constexpr std::int64_t kSlideMaxRaw = 13107;

/// TWO QUANTITIES, NOT ONE (G5). An edge has a HEIGHT DIFFERENCE and a GRADE, and they answer
/// different questions:
///   * the GRADE is the height difference divided by the RUN -- how steep the ground is along the
///     way a unit crosses it. An axis edge runs one tile and a diagonal one 1.41, so a diagonal's
///     difference is scaled by the same 181/256 the escape step uses. The grade is what SPEED and
///     the SLIDE read (`grade_factor`, `plan_slide`): a 1.41-tile ramp is gentler to drive up and
///     to stand on than a 1-tile one of the same height.
///   * the HEIGHT DIFFERENCE, unscaled, is what the CLIMB LIMIT reads (`climbable`): a cliff is a
///     wall because of how high it is, and a face 41 high is a wall whichever way it is
///     approached. Scaling it let a diagonal of 56 through as "39": every diagonal cliff in the
///     world (the worst edge is 55) was climbable, while the same face on the axis was refused.
/// This returns the GRADE. It must never be compared with `kClimbLimit`.
[[nodiscard]] constexpr std::int64_t edge_grade(int from_height, int to_height,
                                                bool diagonal) noexcept {
    const std::int64_t rise = static_cast<std::int64_t>(to_height) - from_height;
    return diagonal ? rise * kDiagonalNumerator / kDiagonalDenominator : rise;
}

/// The speed multiplier (raw, `Fixed::kOne` = level) for a rise: down is faster, up is slower,
/// bounded at both ends.
[[nodiscard]] constexpr std::int64_t grade_factor(std::int64_t rise) noexcept {
    const std::int64_t raw = Fixed::kOne - rise * Fixed::kOne / kGradeSpan;
    return std::clamp<std::int64_t>(raw, kMinGrade, kMaxGrade);
}

/// Does a unit driving at the full step `thrust` (raw) still make headway straight up a slope that
/// rises `rise` a tile, against the slide that slope sets up? Its climb is the grade's share of its
/// step; the slide is `kSlideRawPerHeight` for each unit of the slope past `kSlideThreshold`.
[[nodiscard]] constexpr bool makes_headway(std::int64_t rise, std::int64_t thrust) noexcept {
    const std::int64_t climb = thrust * grade_factor(rise) / Fixed::kOne;
    const std::int64_t slide = std::min(
        std::max<std::int64_t>(rise - kSlideThreshold, 0) * kSlideRawPerHeight, kSlideMaxRaw);
    return climb > slide;
}

/// The steepest rise a TANK at full thrust (`kSlideMaxRaw`, its step: sim.cpp asserts the two are
/// one) still climbs against the slide.
[[nodiscard]] constexpr int steepest_headway_rise() noexcept {
    int rise = 0;
    while (makes_headway(rise + 1, kSlideMaxRaw)) {
        ++rise;
    }
    return rise;
}

/// THE CLIMB LIMIT (G5): a tile edge whose HEIGHT rises by more than this is a CLIFF and is a wall,
/// the way a crater is -- at every angle (`climbable`). It is not a number chosen on its own: it is
/// where the slope physics above stops a tank (`steepest_headway_rise`), 38 with these constants
/// (5324 raw of climb against 4914 of slide; at 39, 5119 against 5733).
///
/// THE MISTAKEN ASSUMPTION IT REPLACES: the limit was a free 40, written as "the steepest rise a
/// unit may climb". The physics never let a tank climb a slope of 39 or 40 -- the slide beats it --
/// so the planner kept its own estimate (38, `drivable_rise`) beside the mover's 40, and the two
/// disagreed about every rise in between: the planner refused what the mover drove, and where the
/// planner had no say (a driver at the helm) a tank was let up a 40 it could not finish. One number
/// now, and it is the physics' number. Change the grade or the slide, and the wall moves with them.
inline constexpr int kClimbLimit = steepest_headway_rise();
static_assert(kClimbLimit == 38, "the climb limit moved: a decision, so restate it here");

/// THE CLIMB RULE, the ONE place it is decided: may a unit cross from ground of `from_height` onto
/// ground of `to_height`? Yes unless the height RISES by more than `kClimbLimit`, at any angle (the
/// height difference, not the grade: see `edge_grade`). The mover (`plan_step`) and the route
/// planner (`route_detail::edge_blocked`) both ask this and nothing else, so there is no rise one
/// of them drives and the other will not plan.
///
/// A unit weaker than a tank (a turret crawls at a quarter of its step) may still be beaten by a
/// slope under this limit: "slower, and backwards if the slope beats your thrust -- correct, not a
/// bug" (Harri). That is the slide, applied as it happens (`plan_slide`), not a second limit.
[[nodiscard]] constexpr bool climbable(int from_height, int to_height) noexcept {
    return static_cast<std::int64_t>(to_height) - from_height <= kClimbLimit;
}

/// A ground that knows heights: `std::int64_t`-convertible `height(tile_x, tile_y)`. A ground
/// without one (a drawn crater map) is level, and none of the slope rules touch it.
template <class Ground>
concept HasHeights = requires(const Ground& ground, int tile) {
    { ground.height(tile, tile) } -> std::convertible_to<int>;
};

/// THE SEAM. One question -- "which way does this unit go this tick?" -- asked of the ground.
///
/// Today the answer is a short detour around whatever is in the way. The switch this is built for
/// is a planned route (a deterministic A* over the same tiles), which answers the same question
/// from a waypoint list and needs nothing else in the tick loop to change.
///
/// `Ground` is any type with `std::int64_t factor(int tile_x, int tile_y) const`, returning the raw
/// speed factor for that tile and 0 for one that cannot be entered. A test hands it a drawn map;
/// the sim hands it the world.
///
/// Order of preference, all integer and with no tie left to chance:
///   1. the step it wanted, if it can land;
///   2. each axis alone -- the slide that already works for grazing a rim;
///   3. a remembered detour, if one is active and still leads somewhere;
///   4. a new escape: the eight neighbours of the current tile, ordered so the ones that turn least
///      from the wanted direction come first.
template <class Ground>
[[nodiscard]] StepPlan plan_step(const StepRequest& request, const Ground& ground) noexcept {
    const auto sign = [](std::int64_t v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); };

    // The tile the unit stands on, which every step is measured from.
    const int tile_x = request.x.floor_int();
    const int tile_y = request.y.floor_int();

    /// May a unit standing on this tile enter `to`? Always, unless the ground knows heights and the
    /// climb into `to` is a cliff (`climbable`: by height, at every angle). The tile it stands on
    /// is always enterable (a unit carried onto a cliff face by the slide must be able to leave
    /// it).
    [[maybe_unused]] const auto may_enter = [&](int to_x, int to_y) {
        if constexpr (HasHeights<Ground>) {
            if (to_x == tile_x && to_y == tile_y) {
                return true;
            }
            return climbable(ground.height(tile_x, tile_y), ground.height(to_x, to_y));
        } else {
            return true;
        }
    };

    /// True when a raw step can be made, with the terrain-scaled step written to `out`. This is the
    /// movement rule the sim has always used, kept in one place: the tile the raw step reaches must
    /// be enterable, and so must the tile the SCALED step actually lands on.
    const auto lands = [&](std::int64_t dx, std::int64_t dy, Step& out) {
        if (dx == 0 && dy == 0) {
            return false;
        }
        const Fixed far_x = Fixed::from_raw(static_cast<std::int32_t>(request.x.raw + dx));
        const Fixed far_y = Fixed::from_raw(static_cast<std::int32_t>(request.y.raw + dy));
        std::int64_t factor = ground.factor(far_x.floor_int(), far_y.floor_int());
        if (factor == 0) {
            return false;
        }
        if constexpr (HasHeights<Ground>) {
            // THE SLOPE. The edge a step reads is the one from this tile to the neighbour it points
            // at (so the speed is the same on every tick of the crossing, not only the one that
            // happens to cross), and the cliff is refused where the unit would LAND in a tile it
            // cannot climb into -- a wall at the tile's edge, like a crater.
            const int here = ground.height(tile_x, tile_y);
            const int step_x = sign(dx);
            const int step_y = sign(dy);
            const std::int64_t grade = edge_grade(
                here, ground.height(tile_x + step_x, tile_y + step_y), step_x != 0 && step_y != 0);
            factor = factor * grade_factor(grade) >> Fixed::kShift;
            if (!may_enter(far_x.floor_int(), far_y.floor_int())) {
                return false;
            }
        }
        const std::int64_t sx = (dx * factor) >> Fixed::kShift;
        const std::int64_t sy = (dy * factor) >> Fixed::kShift;
        const Fixed nx = Fixed::from_raw(static_cast<std::int32_t>(request.x.raw + sx));
        const Fixed ny = Fixed::from_raw(static_cast<std::int32_t>(request.y.raw + sy));
        if (ground.factor(nx.floor_int(), ny.floor_int()) == 0) {
            return false;
        }
        if constexpr (HasHeights<Ground>) {
            if (!may_enter(nx.floor_int(), ny.floor_int())) {
                return false;
            }
        }
        out = Step{sx, sy};
        return true;
    };

    /// True when a step can actually LEAVE THE TILE, not merely land inside it.
    ///
    /// This is the difference that made the corner trap survive: tanks move a fifth of a tile a
    /// tick, so `lands` above only ever asks about the tile they are already standing in, and an
    /// "escape" of a fifth of a tile into an open tile is accepted forever -- the unit jitters in
    /// place and never leaves. An escape has to be checked at the granularity of tiles.
    /// It must NOT be applied to rules 1 and 2: a unit that refuses to move within a tile of any
    /// crater cannot cross the map, and two commanders then never engage at all.
    const auto clear_way = [&](std::int64_t dx, std::int64_t dy, Step& out) {
        if (dx == 0 && dy == 0) {
            return false;
        }
        const int near_x = tile_x + (dx > 0 ? 1 : (dx < 0 ? -1 : 0));
        const int near_y = tile_y + (dy > 0 ? 1 : (dy < 0 ? -1 : 0));
        if (ground.factor(near_x, near_y) == 0) {
            return false;
        }
        return lands(dx, dy, out);
    };

    Step step{};

    // 1. While a detour is running, the ONLY thing that ends it is a straight route that is clear
    // at
    //    tile granularity. `lands` cannot decide that: at a fifth of a tile a tick it is almost
    //    always true, so an escape was cancelled one tick after it began and the unit paced in
    //    place.
    if (request.detour.active()) {
        if (clear_way(request.want_dx, request.want_dy, step)) {
            return StepPlan{step, Detour{}, true}; // past the obstacle: back on the direct line
        }
        if (clear_way(request.detour.dx, request.detour.dy, step)) {
            Detour next = request.detour;
            --next.ticks_left;
            return StepPlan{step, next, true}; // still going around it
        }
        // else: this detour has run into something too, so pick another below.
    } else {
        // 2. Straight on.
        if (lands(request.want_dx, request.want_dy, step)) {
            return StepPlan{step, Detour{}, true};
        }
        // 3. The axis slides, in the order movement has always tried them: the cheap way past an
        //    edge, with nothing to remember.
        if (lands(request.want_dx, 0, step)) {
            return StepPlan{step, Detour{}, true};
        }
        if (lands(0, request.want_dy, step)) {
            return StepPlan{step, Detour{}, true};
        }
    }

    // 4. A new escape. The wanted step is reduced to a DIRECTION so the choice is about direction,
    //    not distance. All EIGHT neighbours are candidates, nearest to the wanted direction first.
    //    Enumerating them by hand from the wanted vector is NOT enough: the opposite axes are the
    //    way out of a concave corner, and a ring written from the wanted vector alone omits them.
    const std::int64_t wx = sign(request.want_dx);
    const std::int64_t wy = sign(request.want_dy);
    std::array<std::array<std::int64_t, 2>, 8> ring{{
        {0, 1},
        {1, 1},
        {1, 0},
        {1, -1},
        {0, -1},
        {-1, -1},
        {-1, 0},
        {-1, 1},
    }};
    const auto turn = [&](const std::array<std::int64_t, 2>& dir) {
        return dir[0] * wx + dir[1] * wy; // a dot product: 2 straight on, -2 straight back
    };
    std::stable_sort(ring.begin(), ring.end(),
                     [&](const std::array<std::int64_t, 2>& a,
                         const std::array<std::int64_t, 2>& b) { return turn(a) > turn(b); });

    const std::int64_t axis = request.speed;
    const std::int64_t diagonal = request.speed * kDiagonalNumerator / kDiagonalDenominator;
    for (const auto& candidate : ring) {
        const bool diagonal_step = candidate[0] != 0 && candidate[1] != 0;
        const std::int64_t magnitude = diagonal_step ? diagonal : axis;
        const std::int64_t dx = candidate[0] * magnitude;
        const std::int64_t dy = candidate[1] * magnitude;
        if (!clear_way(dx, dy, step)) {
            continue;
        }
        return StepPlan{step, Detour{dx, dy, kDetourTicks}, true};
    }

    return StepPlan{Step{}, request.detour, false};
}

/// THE SLIDE. The drift a slope gives a unit standing at (`x`, `y`) this tick, in raw units: the
/// step towards the steepest of its eight neighbours that `open` admits (ties go to the first in a
/// fixed ring order, so no tie is left to chance), at `kSlideRawPerHeight` for every unit of drop
/// past `kSlideThreshold`. Nothing when the ground holds. Pure and stateless: a unit that is not
/// on a slope is not drifting, so it "ends when the unit is off the slope" with nothing to clear.
///
/// The caller ADDS this to the unit's commanded step; it is never a replacement for it, which is
/// the ruling that the slope wins over steering.
template <class Ground, class Open>
    requires HasHeights<Ground>
[[nodiscard]] Step plan_slide(const Ground& ground, Fixed x, Fixed y, const Open& open) noexcept {
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
    const int tile_x = x.floor_int();
    const int tile_y = y.floor_int();
    const int here = ground.height(tile_x, tile_y);
    std::int64_t steepest = 0;
    std::array<int, 2> way{};
    for (const auto& d : kRing) {
        const int nx = tile_x + d[0];
        const int ny = tile_y + d[1];
        if (!open(nx, ny)) {
            continue;
        }
        // The GRADE of the way down (`edge_grade`): how steep the ground under the unit is, which
        // is what decides whether it holds -- not the height of the face, which is the climb's.
        const bool diagonal = d[0] != 0 && d[1] != 0;
        const std::int64_t drop = -edge_grade(here, ground.height(nx, ny), diagonal);
        if (drop > steepest) {
            steepest = drop;
            way = d;
        }
    }
    if (steepest <= kSlideThreshold) {
        return Step{};
    }
    std::int64_t magnitude = (steepest - kSlideThreshold) * kSlideRawPerHeight;
    if (way[0] != 0 && way[1] != 0) {
        magnitude = magnitude * kDiagonalNumerator / kDiagonalDenominator;
    }
    magnitude = std::min(magnitude, kSlideMaxRaw);
    return Step{way[0] * magnitude, way[1] * magnitude};
}

} // namespace protekt::sim
