#pragma once

#include <algorithm>
#include <array>
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

    /// True when a raw step can be made, with the terrain-scaled step written to `out`. This is the
    /// movement rule the sim has always used, kept in one place: the tile the raw step reaches must
    /// be enterable, and so must the tile the SCALED step actually lands on.
    const auto lands = [&](std::int64_t dx, std::int64_t dy, Step& out) {
        if (dx == 0 && dy == 0) {
            return false;
        }
        const Fixed far_x = Fixed::from_raw(static_cast<std::int32_t>(request.x.raw + dx));
        const Fixed far_y = Fixed::from_raw(static_cast<std::int32_t>(request.y.raw + dy));
        const std::int64_t factor = ground.factor(far_x.floor_int(), far_y.floor_int());
        if (factor == 0) {
            return false;
        }
        const std::int64_t sx = (dx * factor) >> Fixed::kShift;
        const std::int64_t sy = (dy * factor) >> Fixed::kShift;
        const Fixed nx = Fixed::from_raw(static_cast<std::int32_t>(request.x.raw + sx));
        const Fixed ny = Fixed::from_raw(static_cast<std::int32_t>(request.y.raw + sy));
        if (ground.factor(nx.floor_int(), ny.floor_int()) == 0) {
            return false;
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
    /// It is deliberately NOT applied to rules 1 and 2: a unit that refused to move within a tile
    /// of any crater could not cross the map, which measured as two commanders never engaging.
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
    //    not distance. All EIGHT neighbours are candidates, nearest to the wanted direction first:
    //    an earlier version listed them by hand from the wanted vector and silently left out the
    //    opposite axes, so a corner whose only way out was due west found no escape at all.
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

} // namespace protekt::sim
