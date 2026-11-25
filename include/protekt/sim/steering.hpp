#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>

#include "protekt/sim/fixed.hpp"
#include "protekt/world/world.hpp"

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

/// THE SLOPE (M1, G6). Everything below is integer arithmetic on the INTEGER tile heights, so the
/// slope is as hashable as the world it is read from.
///
/// THE ONE FORCE (G6): the slope is a PRESS -- one vector, opposite the gradient of the ground
/// under the unit, growing with its steepness -- ADDED to whatever the unit is commanded
/// (`plan_press`). There was a grade (a speed multiplier, floored and capped) and a separate slide
/// (a drift past a threshold, capped), both read from one slope and neither aware of the other, so
/// they had to be tuned against each other -- and the arithmetic of that tuning is what threw a
/// driven tank backwards. A single force cannot disagree with itself: its component along the
/// heading IS the slow climb and the fast descent, its component across the heading -- past the
/// grip of the tracks, which is the grip that climbs the steepest grade -- IS the veer, and it can
/// only beat a driver who is heading into it (`driven_press`).
///
/// THE ONE FACTOR FOR EVERY UNIT: a declared simplification. Harri's ruling was "one factor for
/// every unit -- a declared simplification that can become per-kind later, once you've played it".
/// Here that is ONE THRUST-TO-WEIGHT for every unit: each unit's engine can just hold it on the
/// steepest grade, so the press on a unit is the same FRACTION of its own full step on the same
/// ground, whatever the unit. (A press of the same raw size for every unit would beat a turret's
/// quarter-step on any slope past 8 a tile -- most of the world -- and give every speed its own
/// climb limit beside the one wall: the very disagreement G5 removed.)
///
/// Heights are in the world's units (kMinHeight..kMaxHeight, +-512); `world::kHeightUnitsPerTile`
/// of them stand as tall as a tile is wide, so a rise of 64 a tile is 45 degrees. Measured over
/// seeds 1, 7 and 42 (re-measured in G6), a tile edge rises by at most 8 half the time, 23 to 25
/// for 94% of edges, 38 to 41 for 99.8%, and 56 at the very worst (seed 42; this said 55); and
/// 0.6% to 1.1% of tiles are steeper than the steepest grade below, about the share the old slide
/// threshold set sliding.

/// THE STEEPEST GRADE A VEHICLE CLIMBS, and the only physical number in the press: 60 percent,
/// a rise of 3 in a run of 5 (31 degrees). That is the gradient a main battle tank is specified to
/// climb (the figure quoted for the M1 Abrams and the Leopard 2 alike). At this grade the press
/// equals a unit's full thrust, so a unit driving straight up it stands still. In this world's
/// units that is 0.6 * 64 = 38.4 height units a tile -- and the WALL comes out of it
/// (`press_beats`): there is no climb limit of its own.
inline constexpr std::int64_t kSteepestGradeRise = 3;
inline constexpr std::int64_t kSteepestGradeRun = 5;

/// THE GRADIENT of the ground under a unit, DOUBLED (twice the rise per tile) so every face's is an
/// integer.
///
/// THE CORNERS (G7): the drawn ground puts tile (x, y)'s height at the lattice CORNER (x, y), and
/// draws the tile as TWO TRIANGLES split on the (x, y)-(x+1, y+1) diagonal (`game::ground_metres`'
/// `blend_corners`): the UPPER one (fraction fx >= fy) through corners 00, 10, 11, the LOWER one
/// (fx < fy) through 00, 01, 11. Each is a plane, and its gradient is exact:
///     upper:  dh/dx = h10 - h00,  dh/dy = h11 - h10
///     lower:  dh/dx = h11 - h01,  dh/dy = h01 - h00
/// The slope under a unit is the triangle it stands on (`gradient_under`) -- the ground the player
/// SEES. (G6 read the mean of the four corners' edges, the quad's average slope: on a tile whose
/// corners are not in one plane -- 642 of 800 points sampled on seed 42 -- that is a slope drawn
/// nowhere, and a corner the player sees as a cliff read as half a climb.) Nothing outside the
/// unit's own tile is read: level ground beside a cliff has a gradient of zero, however near the
/// edge it stands. There is no "run" to guess and no 181/256 anywhere in the force: a diagonal
/// slope has both components, and its steepness is their magnitude.
struct Gradient {
    std::int64_t x2{}; ///< twice the rise per tile along +x, height units
    std::int64_t y2{}; ///< twice the rise per tile along +y
};

/// The two drawn triangles of tile (`x`, `y`), from a `height(x, y)` callable: [0] the upper
/// (fx >= fy), [1] the lower. Doubled, as every `Gradient` is.
template <class Height>
[[nodiscard]] constexpr std::array<Gradient, 2> face_triangles(const Height& height, int x, int y) {
    const std::int64_t h00 = height(x, y);
    const std::int64_t h10 = height(x + 1, y);
    const std::int64_t h01 = height(x, y + 1);
    const std::int64_t h11 = height(x + 1, y + 1);
    return {Gradient{2 * (h10 - h00), 2 * (h11 - h10)}, Gradient{2 * (h11 - h01), 2 * (h01 - h00)}};
}

/// floor(sqrt(n)) for n >= 0, in a constexpr the wall can be derived with.
[[nodiscard]] constexpr std::int64_t isqrt_floor(std::int64_t n) noexcept {
    if (n <= 0) {
        return 0;
    }
    std::int64_t lo = 0;
    std::int64_t hi = std::int64_t{1} << 32;
    while (hi - lo > 1) {
        const std::int64_t mid = lo + ((hi - lo) / 2);
        if (mid <= n / mid) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/// A doubled gradient's "one tile": the run the doubled rise is measured over, in height units.
inline constexpr std::int64_t kDoubledRun = std::int64_t{2} * world::kHeightUnitsPerTile;

/// THE PRESS on a unit of full step `thrust` (raw per tick) on ground of gradient `g`: opposite the
/// gradient, of magnitude
///     thrust * sin(slope) / sin(steepest grade)
/// -- the component of gravity along the slope, measured in the unit's own thrust, so it is
/// exactly that thrust at the steepest grade. sin(slope) = |g| / sqrt(64^2 + |g|^2).
///
/// k, THE CONSTANT IT IS PROPORTIONAL BY, with its unit: on the gentle ground the world is mostly
/// made of sin(slope) ~ |g| / 64, so the press is k * |g| with
///     k = thrust * sqrt(34) / 3 / 64 = thrust / 32.9
/// a SPEED PER UNIT OF STEEPNESS: raw per tick, for each height unit a tile of slope. For a tank
/// (13107 raw, 0.2 tiles a tick) it is 398 raw a tick per height unit a tile. It is not chosen:
/// it is the one value at which a 60 percent grade exactly cancels the engine.
///
/// On a sheer face the sine stops growing: the press is the whole weight, 1.94 thrusts, and never
/// more -- with no cap written anywhere, because that is what gravity along a vertical face IS. A
/// force that went on growing with rise-over-run would be infinite on a vertical face and throw a
/// unit through a wall.
///
/// In integers: S = 2^20 * sqrt(rise^2 + run^2) / (rise * sqrt(kDoubledRun^2 + |2g|^2)), and the
/// press is -thrust * 2g * S / 2^20. Division truncates toward zero, so a slope one way and the
/// same slope the other way press by exactly opposite amounts.
[[nodiscard]] constexpr Step press_of(Gradient g, std::int64_t thrust) noexcept {
    const std::int64_t a2 = (g.x2 * g.x2) + (g.y2 * g.y2);
    if (a2 == 0) {
        return Step{};
    }
    constexpr std::int64_t kHyp2
        = (kSteepestGradeRise * kSteepestGradeRise) + (kSteepestGradeRun * kSteepestGradeRun);
    const std::int64_t s = isqrt_floor(
        (kHyp2 << 40)
        / (kSteepestGradeRise * kSteepestGradeRise * ((kDoubledRun * kDoubledRun) + a2)));
    return Step{-(thrust * g.x2 * s) / (std::int64_t{1} << 20),
                -(thrust * g.y2 * s) / (std::int64_t{1} << 20)};
}

/// Is ground of gradient `g` steeper than the steepest grade (tan(slope) > rise / run)? Then the
/// press beats any unit's whole thrust. Exact, in integers: run^2 * |2g|^2 > rise^2 *
/// kDoubledRun^2.
[[nodiscard]] constexpr bool too_steep_to_climb(Gradient g) noexcept {
    const std::int64_t a2 = (g.x2 * g.x2) + (g.y2 * g.y2);
    return kSteepestGradeRun * kSteepestGradeRun * a2
           > kSteepestGradeRise * kSteepestGradeRise * kDoubledRun * kDoubledRun;
}

/// WHAT THE PRESS HAS PAST A WHOLE THRUST on ground of gradient `g`, for a unit of full step
/// `thrust`: zero on ground a unit climbs, and on steeper ground the press less one thrust,
/// continuously from nothing at the steepest grade. It is the planner's measure of ground that
/// THROWS a unit (`route_detail::slide_way_of`: past half a step, no engine holds a line across
/// it). (G6 also used it as the parked unit's drift, so a parked unit was held on everything it
/// could climb and never rolled back; G7 gives the parked unit gravity and friction instead,
/// `slide_tick`.)
[[nodiscard]] constexpr Step press_past_thrust(Gradient g, std::int64_t thrust) noexcept {
    if (!too_steep_to_climb(g)) {
        return Step{};
    }
    const Step press = press_of(g, thrust);
    const std::int64_t magnitude = isqrt_floor((press.dx * press.dx) + (press.dy * press.dy));
    if (magnitude <= thrust) {
        return Step{};
    }
    const std::int64_t past = magnitude - thrust;
    return Step{press.dx * past / magnitude, press.dy * past / magnitude};
}

/// THE DRIVEN UNIT of full step `thrust`, heading along (`hx`, `hy`) at |h| raw a tick: the press
/// acts on it as on any vehicle -- its tracks ROLL along the heading and GRIP across it. So the
/// press's component along the heading is felt in full at full throttle (that IS the slow climb,
/// the fast descent, and the too-steep hill that beats the engine), and a gentler drive uphill is
/// held at its commanded speed while the engine has the thrust to spare -- and its component
/// across the heading is held by the same grip that
/// holds a parked unit, and carries the unit sideways only by what it has past that grip (the
/// veer, on ground steeper than a unit climbs). One force, and one grip, read the one way a
/// vehicle reads them: nothing here can turn a driver round unless the slope genuinely opposes
/// his heading by more than his engine. A zero heading is no drive, and no press of the engine's:
/// a unit not driven is gravity's and friction's (`slide_tick`).
[[nodiscard]] constexpr Step driven_press(Gradient g, std::int64_t thrust, std::int64_t hx,
                                          std::int64_t hy) noexcept {
    const std::int64_t h2 = (hx * hx) + (hy * hy);
    if (h2 == 0) {
        return Step{};
    }
    const Step press = press_of(g, thrust);
    const std::int64_t dot = (press.dx * hx) + (press.dy * hy);
    // ALONG THE HEADING (G6): `c` is the commanded speed and `a` the press along the heading, both
    // raw a tick; `thrust` (T) is the most the engine can push. A command is a SPEED, and the
    // engine has up to its whole thrust to hold it: uphill (a < 0) it holds `c` while it has the
    // headroom, and gives way only past that -- net = min(c, T + a), slowed once the slope takes
    // more than T - c, and carried back only when it takes more than T. Downhill the press adds
    // in full: net = c + a, faster. At full throttle (c = T) both are c + a: the one law the
    // measurements pin, unchanged. Without the headroom, a command weaker than the slope -- the
    // last part-step of every approach, an easing strafe -- was carried BACKWARDS down ground its
    // engine climbs at full throttle, and never arrived.
    const std::int64_t c = isqrt_floor(h2);
    const std::int64_t a = c == 0 ? 0 : dot / c;
    const std::int64_t net = a < 0 ? std::min(c, thrust + a) : c + a;
    const Step along = c == 0 ? Step{} : Step{(net - c) * hx / c, (net - c) * hy / c};
    const Step pure{dot * hx / h2, dot * hy / h2}; // the press's own component along the heading
    Step across{press.dx - pure.dx, press.dy - pure.dy};
    const std::int64_t side = isqrt_floor((across.dx * across.dx) + (across.dy * across.dy));
    if (side <= thrust) {
        across = Step{};
    } else {
        const std::int64_t past = side - thrust;
        across = Step{across.dx * past / side, across.dy * past / side};
    }
    return Step{along.dx + across.dx, along.dy + across.dy};
}

/// The thrust the wall is judged with: any thrust gives the same answer (the press is the same
/// fraction of every unit's step), and this one is small enough that a press times a heading
/// squared cannot overflow, and large enough that the press at the steepest grade still lands on
/// the right side of it (at 38 a tile 4064 against 4096; at 39 4142).
inline constexpr std::int64_t kWallThrust = 4096;

/// THE WALL (G6), the ONE place it is decided: does the press on ground of gradient `g` beat a unit
/// heading along (`hx`, `hy`) -- oppose that heading by at least the unit's whole thrust? Then the
/// unit makes no headway that way, and the face is a WALL to it. There is no climb limit to choose:
/// the wall is a consequence of the press, and the mover (`plan_step`), the press itself
/// (`sim.cpp`'s `press`), the route planner (`route_detail::edge_blocked`) and the map drawing all
/// ask this and nothing else.
///
/// What it comes to: a face is a wall along a heading when its steepness ALONG that heading -- the
/// gradient's component on it, |g| cos(angle) -- is past the steepest grade (in sin, not tan: the
/// press is gravity along the face). Straight up a face that is 38 a tile, 4064 < 4096: drivable;
/// 39, 4142: a wall. A diagonal face is its true steepness, |g|, whichever axes it lies across --
/// no height difference is compared and no run is guessed. Across a face, or down it, the press
/// never beats a heading, however steep: a unit may drive along a cliff or off one (the fall).
///
/// The heading is reduced until neither component passes 2^14, keeping its direction, so the
/// arithmetic stays inside 64 bits whatever step is asked about.
[[nodiscard]] constexpr bool press_beats(Gradient g, std::int64_t hx, std::int64_t hy) noexcept {
    while (hx > (std::int64_t{1} << 14) || hx < -(std::int64_t{1} << 14)
           || hy > (std::int64_t{1} << 14) || hy < -(std::int64_t{1} << 14)) {
        hx /= 2;
        hy /= 2;
    }
    const std::int64_t h2 = (hx * hx) + (hy * hy);
    if (h2 == 0) {
        return false;
    }
    const Step press = press_of(g, kWallThrust);
    const std::int64_t against = -((press.dx * hx) + (press.dy * hy)); // |press| |h| cos
    return against > 0 && against * against >= h2 * kWallThrust * kWallThrust;
}

/// May a unit drive INTO tile (`x`, `y`) heading along (`hx`, `hy`)? Yes unless the press on one
/// of that tile's two drawn triangles (`face_triangles`) beats it that way (`press_beats`): a face
/// the player sees as a cliff is a wall, whichever half of the tile it is drawn in. ONE answer for
/// the whole tile, so the mover and the route planner, which plans in tiles, still ask the same
/// question (G5). `height(x, y)` is the ground's heights.
template <class Height>
[[nodiscard]] constexpr bool drivable(const Height& height, int x, int y, std::int64_t hx,
                                      std::int64_t hy) {
    const std::array<Gradient, 2> faces = face_triangles(height, x, y);
    return !press_beats(faces[0], hx, hy) && !press_beats(faces[1], hx, hy);
}

/// What the wall comes to on one axis face, as a height: the steepest rise a tile that a unit
/// drives straight up and still makes headway. A CONSEQUENCE, read off `press_beats`, for anything
/// that must compare two heights (the goal's level, a test's map) -- never a rule of its own.
[[nodiscard]] constexpr int steepest_drivable_rise() noexcept {
    int rise = 0;
    while (!press_beats(Gradient{std::int64_t{2} * (rise + 1), 0}, 1, 0)) {
        ++rise;
    }
    return rise;
}
static_assert(steepest_drivable_rise() == 38,
              "the wall moved: it is the press's, so the press moved -- restate it here");
static_assert(!too_steep_to_climb(Gradient{std::int64_t{2} * steepest_drivable_rise(), 0})
                  && too_steep_to_climb(Gradient{std::int64_t{2} * (steepest_drivable_rise() + 1),
                                                 0}),
              "the wall and the steepest grade are one quantity");

/// GRAVITY (G7), the one magnitude: g = 9.81 m/s^2, as a rational number of raw units (Q16.16
/// tiles) per tick per tick. Its value in this world is `kGravity` (units.hpp), where the tick and
/// the tile's metres are known.
struct Acceleration {
    std::int64_t num{}; ///< raw per tick^2 is num / den
    std::int64_t den{1};
};

/// FRICTION (G7): the coefficient of friction between a unit at rest and the ground under it, a
/// pure ratio (friction force over the weight pressing it to the ground), mu = 2 / 5 = 0.40. It is
/// the 40 percent SIDE SLOPE an M1 Abrams is specified to hold: the steepest ground a tank is held
/// on BROADSIDE, the way it is not driving. A parked unit has no heading in this sim -- nothing
/// records which way its hull lies -- so the grade that holds it is the one that holds a tank
/// whichever way it lies, which is the broadside figure (straight up or down its tracks, on its
/// brakes, a real tank holds more). On ground of slope t = tan(angle) it slides from rest exactly
/// when gravity along the ground beats it, t > mu: 0.40 * 64 = 25.6 height units a tile (21.8
/// degrees) -- the same steepness whichever way the ground falls, because only |g| enters.
///
/// DECLARED, as the 60 percent grade was: the Leopard 2's sheet says 30 percent side slope, which
/// would set sliding at 19.2 a tile. The M1's figure is the one taken, because the steepest-grade
/// figure is the M1's too and the two together describe one vehicle.
inline constexpr std::int64_t kFrictionNum = 2;
inline constexpr std::int64_t kFrictionDen = 5;

/// THE THREE THRESHOLDS, which are now three quantities and no picked number:
///     a slope you SLIDE on (parked)  = gravity beats friction            t > 0.40 (25.6 a tile)
///     a slope you CANNOT CLIMB       = gravity beats the engine           t > 0.60 (38.4 a tile)
///     between them a unit climbs under thrust and rolls back without it.
/// Is ground of gradient `g` held by friction alone? Exact, in integers: mu^2 * kDoubledRun^2 >=
/// |2g|^2, with mu = num / den.
[[nodiscard]] constexpr bool friction_holds(Gradient g) noexcept {
    const std::int64_t a2 = (g.x2 * g.x2) + (g.y2 * g.y2);
    return kFrictionDen * kFrictionDen * a2
           <= kFrictionNum * kFrictionNum * kDoubledRun * kDoubledRun;
}
static_assert(friction_holds(Gradient{std::int64_t{2} * 25, 0})
                  && !friction_holds(Gradient{std::int64_t{2} * 26, 0}),
              "friction holds to 25.6 a tile: mu moved, restate it");
static_assert(!friction_holds(Gradient{std::int64_t{2} * steepest_drivable_rise(), 0}),
              "between sliding and the wall there is ground you climb under thrust and roll back "
              "down without it -- his report: friction must give before the engine does");

/// ONE TICK OF A UNIT NOBODY IS DRIVING (G7): its slide velocity `v` (raw a tick) after a tick on
/// ground of gradient `g`, under `gravity`. "Think of an incline as acceleration downhill, plus
/// friction" -- Coulomb's, with the one coefficient for at rest and for sliding:
///
/// On ground rising t = tan(angle), gravity along the ground is g sin, and its HORIZONTAL part --
/// the sim moves units in the plane -- g sin cos = g t / (1 + t^2), straight downhill. Friction
/// resists with up to mu times the weight on the ground, mu g cos, horizontally mu g cos^2 = mu g /
/// (1 + t^2), against the motion (or, at rest, against the pull). With t = |2g| / kDoubledRun that
/// is, in integers and with no square root:
///     pull     = -gravity * 2g * D / (D^2 + |2g|^2)        D = kDoubledRun
///     friction =  gravity * mu * D^2 / (D^2 + |2g|^2)
/// The tick: w = v + pull; if friction can stop w it does (|w| <= friction: v = 0, which is also
/// "at rest and held" when friction_holds); else v = w less friction along w. So from rest on
/// uniform ground the unit gains a = g (t - mu) / (1 + t^2) every tick, and on level ground a
/// moving one loses mu g (142.9 raw a tick^2) until it stops. Velocity first, then position
/// (semi-implicit): the caller moves the unit by the returned velocity.
///
/// Worked in 1/256 of a raw unit so a gentle slope's acceleration (11 raw a tick^2 at 28 a tile)
/// is not rounded to the nearest raw unit before it is summed; returned in raw.
[[nodiscard]] constexpr Step slide_tick(Gradient g, Step v, Acceleration gravity) noexcept {
    constexpr std::int64_t kFine = 256;
    constexpr std::int64_t kD = kDoubledRun;
    const std::int64_t a2 = (g.x2 * g.x2) + (g.y2 * g.y2);
    const std::int64_t denom = gravity.den * ((kD * kD) + a2);
    const std::int64_t wx = (v.dx * kFine) - (gravity.num * kFine * g.x2 * kD / denom);
    const std::int64_t wy = (v.dy * kFine) - (gravity.num * kFine * g.y2 * kD / denom);
    const std::int64_t friction
        = gravity.num * kFine * kFrictionNum * kD * kD / (denom * kFrictionDen);
    const std::int64_t w = isqrt_floor((wx * wx) + (wy * wy));
    if (w <= friction) {
        return Step{};
    }
    return Step{wx * (w - friction) / w / kFine, wy * (w - friction) / w / kFine};
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

    /// May a unit standing on this tile enter `to`, heading along (`hx`, `hy`)? Always, unless the
    /// ground knows heights and `to`'s face is a wall that way (`drivable`: the press beats the
    /// engine). The tile it stands on is always enterable: what its own face does to it is the
    /// press, not a wall.
    [[maybe_unused]] const auto may_enter
        = [&](int to_x, int to_y, std::int64_t hx, std::int64_t hy) {
              if constexpr (HasHeights<Ground>) {
                  if (to_x == tile_x && to_y == tile_y) {
                      return true;
                  }
                  const auto height = [&](int x, int y) { return ground.height(x, y); };
                  return drivable(height, to_x, to_y, hx, hy);
              } else {
                  (void)hx;
                  (void)hy;
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
        const std::int64_t factor = ground.factor(far_x.floor_int(), far_y.floor_int());
        if (factor == 0) {
            return false;
        }
        if constexpr (HasHeights<Ground>) {
            // THE WALL: refused where the unit would LAND in a tile whose face the press makes a
            // wall along this step -- at the tile's edge, like a crater. The step is NOT scaled
            // by the slope: what the slope does to a moving unit is the press (`plan_press`),
            // added by the caller, and one force is not also a speed multiplier.
            if (!may_enter(far_x.floor_int(), far_y.floor_int(), dx, dy)) {
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
            if (!may_enter(nx.floor_int(), ny.floor_int(), dx, dy)) {
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

/// THE GRADIENT under a unit standing at (`x`, `y`): the drawn triangle of its tile that it stands
/// on (`Gradient`, `face_triangles`) -- the upper one when its fraction across the tile in x is at
/// least its fraction in y, exactly as the renderer splits it.
template <class Ground>
    requires HasHeights<Ground>
[[nodiscard]] Gradient gradient_under(const Ground& ground, Fixed x, Fixed y) noexcept {
    const auto height = [&](int tx, int ty) { return ground.height(tx, ty); };
    const std::array<Gradient, 2> faces = face_triangles(height, x.floor_int(), y.floor_int());
    constexpr std::int32_t kMask = Fixed::kOne - 1;
    return (x.raw & kMask) >= (y.raw & kMask) ? faces[0] : faces[1];
}

/// THE PRESS (G6): what the slope does to a unit of full step `thrust` standing at (`x`, `y`) this
/// tick, raw, when it drove (`heading_dx`, `heading_dy`) under its own power -- zero for a unit
/// that did not, which is gravity's and friction's instead (`slide_tick`, G7). Pure and
/// stateless: off the slope there is no press, with nothing to clear.
///
/// The caller ADDS this to the unit's commanded step; it is never a replacement for it. Its
/// component along the heading is the slow climb and the fast descent, its component across the
/// heading past the grip is the veer -- one vector, so the two cannot be tuned against each other.
template <class Ground>
    requires HasHeights<Ground>
[[nodiscard]] Step plan_press(const Ground& ground, Fixed x, Fixed y, std::int64_t thrust,
                              std::int64_t heading_dx, std::int64_t heading_dy) noexcept {
    return driven_press(gradient_under(ground, x, y), thrust, heading_dx, heading_dy);
}

} // namespace protekt::sim
