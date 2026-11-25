#include "protekt/sim/route.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/steering.hpp"
#include "protekt/world/world.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

/// THE SLOPE (M1, G6, G7). One force, the PRESS, on a DRIVEN unit: uphill it costs speed, downhill
/// it grants it, across it drags, a cliff is a wall -- and the press is added to whatever the unit
/// is commanded, so steering does not cancel it. A unit nobody drives is gravity's and friction's
/// (G7): it rolls on ground steeper than friction holds, 25.6 a tile (`test_gravity.cpp`).
/// One factor for every unit (Harri's ruling, pinned below so the simplification cannot be quietly
/// undone).
///
/// Two layers, as every movement test here is. The RULE is asserted on a DRAWN height map, with the
/// numbers written out from the rule and none read back from the code under test; the SIM is
/// asserted on a place found in the real world, so the slope is a fact the mover meets and not
/// only a fact about a template function.
namespace protekt::sim {
namespace {

constexpr std::uint64_t kSeed = 42; ///< the world the real-ground tests stand on

constexpr int kRows = 5;
constexpr int kCols = 8;

/// A drawn height map: every tile is open ground (speed factor one) and has the height drawn for
/// it. Outside the grid there is nothing to stand on.
struct HeightGround {
    std::array<std::array<int, kCols>, kRows> heights{};
    [[nodiscard]] bool inside(int x, int y) const noexcept {
        return x >= 0 && y >= 0 && x < kCols && y < kRows;
    }
    [[nodiscard]] std::int64_t factor(int x, int y) const noexcept {
        return inside(x, y) ? Fixed::kOne : 0;
    }
    [[nodiscard]] int height(int x, int y) const noexcept {
        return inside(x, y)
                   ? heights.at(static_cast<std::size_t>(y)).at(static_cast<std::size_t>(x))
                   : 0;
    }
};

/// Heights that change by `rise` for every tile EAST of column 2 (so column 2 is the foot or the
/// brow, and columns 0..2 are level at `base`).
HeightGround ramp_east(int base, int rise) {
    HeightGround ground{};
    for (int y = 0; y < kRows; ++y) {
        for (int x = 0; x < kCols; ++x) {
            ground.heights.at(static_cast<std::size_t>(y)).at(static_cast<std::size_t>(x))
                = base + (x > 2 ? rise : 0);
        }
    }
    return ground;
}

constexpr std::int64_t kStep = kTankSpeedRaw; ///< a tank's step on level ground: 0.2 tiles

/// Unit at the middle of tile (`tx`, `ty`), `frac` raw in from the tile's west edge.
StepRequest standing(int tx, int ty, std::int64_t want_dx, std::int64_t want_dy,
                     std::int32_t frac = Fixed::kOne / 2) {
    StepRequest request;
    request.x = Fixed::from_raw(tx * Fixed::kOne + frac);
    request.y = Fixed::from_raw(ty * Fixed::kOne + Fixed::kOne / 2);
    request.want_dx = want_dx;
    request.want_dy = want_dy;
    request.speed = kStep;
    return request;
}

// ---- the rule, on drawn ground ------------------------------------------------------------
//
// G6: THE PRESS. The slope is one force, opposite the gradient of the ground under the unit, of
// magnitude (a tank's thrust) * sin(slope) / sin(the steepest grade, 60 percent). The drawn
// ground puts a tile's height at its CORNER, so in `ramp_east` the face is tile 2 (corners 2 and
// 3), and tiles 0, 1 and 3 on are level. Every number below is worked from that formula by hand
// (13107 * sin(atan(rise / 64)) * sqrt(34) / 3, floored), none read back from the code.

TEST(Slope, LevelGroundIsTheSpeedItAlwaysWas) {
    // The control for the rest: a level edge changes nothing.
    const auto plan = plan_step(standing(2, 2, kStep, 0), ramp_east(100, 0));
    ASSERT_TRUE(plan.moved);
    EXPECT_EQ(plan.step.dx, 13107);
}

TEST(Slope, TheMoverNoLongerScalesAStepBySlope) {
    // OLD (M1): the step was multiplied by a GRADE, 1 - rise/64 bounded to 1/4..3/2, and a SLIDE
    // was added on top: two mechanisms from one slope. G6: the mover's step is the engine's, on
    // any slope; what the slope does is the press, and only the press.
    for (const int rise : {-40, -16, 16, 38}) {
        const auto plan = plan_step(standing(2, 2, kStep, 0), ramp_east(100, rise));
        ASSERT_TRUE(plan.moved) << "rise " << rise;
        EXPECT_EQ(plan.step.dx, 13107) << "rise " << rise;
    }
}

TEST(Press, LevelGroundHasNoneEvenBesideAFace) {
    // His report: moved "in the vicinity of a hill". Tile 3 is level -- its four corners all at
    // 140 -- and stands at the top of the 40 face in tile 2: nothing presses it. Tile 1, level at
    // the foot, likewise.
    const HeightGround ground = ramp_east(100, 40);
    for (const int tile : {1, 3}) {
        const StepRequest at = standing(tile, 2, 0, 0);
        const Step press = plan_press(ground, at.x, at.y, kStep, kStep, 0);
        EXPECT_EQ(press.dx, 0) << "tile " << tile;
        EXPECT_EQ(press.dy, 0) << "tile " << tile;
    }
}

TEST(Press, UphillAndDownhillAreOneForce) {
    // Rising east by 16 (14 degrees): 13107 * 0.2425 * 1.9437 = 6178.7, pressing WEST. Falling
    // east by 16: the same, pressing EAST. Driving east, that is the slow climb (13107 - 6178)
    // and the fast descent (13107 + 6178) -- there is no other term.
    const StepRequest at = standing(2, 2, 0, 0);
    EXPECT_EQ(plan_press(ramp_east(100, 16), at.x, at.y, kStep, kStep, 0).dx, -6178);
    EXPECT_EQ(plan_press(ramp_east(100, -16), at.x, at.y, kStep, kStep, 0).dx, 6178);
    EXPECT_EQ(plan_press(ramp_east(100, 16), at.x, at.y, kStep, kStep, 0).dy, 0);
    // ...and it grows with the slope: 30 -> 10812, 36 -> 12489.
    EXPECT_EQ(plan_press(ramp_east(100, -30), at.x, at.y, kStep, kStep, 0).dx, 10812);
    EXPECT_EQ(plan_press(ramp_east(100, -36), at.x, at.y, kStep, kStep, 0).dx, 12489);
}

TEST(Press, AtTheSteepestGradeItIsATanksWholeThrust) {
    // 60 percent is 38.4 a tile. At 38 the press is 13005, under a tank's 13107: it climbs, at
    // 102 raw a tick. At 39 it is 13255: the slope beats it by 148. That crossing IS the wall on
    // an axis face (`steepest_drivable_rise`, 38), read off the press, not chosen.
    const StepRequest at = standing(2, 2, 0, 0);
    EXPECT_EQ(plan_press(ramp_east(100, 38), at.x, at.y, kStep, kStep, 0).dx, -13005);
    EXPECT_EQ(plan_press(ramp_east(100, 39), at.x, at.y, kStep, kStep, 0).dx, -13255);
    EXPECT_EQ(steepest_drivable_rise(), 38);
}

TEST(Press, ASheerFaceIsBoundedByItsWeightNotByACap) {
    // Gravity along a face is at most the whole weight: sin(slope) never passes one. A face of
    // 512 in one tile (83 degrees) presses 25266 raw; one of 1024 (86 degrees) 25420 -- both
    // under the 25483 (13107 * sqrt(34) / 3) of a vertical face, and no cap is written anywhere.
    const StepRequest at = standing(2, 2, 0, 0);
    EXPECT_EQ(plan_press(ramp_east(0, -512), at.x, at.y, kStep, kStep, 0).dx, 25266);
    EXPECT_EQ(plan_press(ramp_east(0, -1024), at.x, at.y, kStep, kStep, 0).dx, 25420);
}

TEST(Press, ADiagonalSlopeIsItsMagnitudeNotItsAxes) {
    // A plane rising 27 east AND 27 south: each axis reads 27, the slope is 27 * sqrt(2) = 38.18
    // a tile, up the diagonal. The press is (-9229, -9229) -- a magnitude of 13051, between the
    // axis presses at 38 (13005) and 39 (13255), exactly where 38.18 belongs. No 181/256.
    HeightGround ground{};
    for (int y = 0; y < kRows; ++y) {
        for (int x = 0; x < kCols; ++x) {
            ground.heights.at(static_cast<std::size_t>(y)).at(static_cast<std::size_t>(x))
                = (27 * x) + (27 * y);
        }
    }
    const StepRequest at = standing(2, 2, 0, 0);
    const Step press = press_of(gradient_under(ground, at.x, at.y), kStep);
    EXPECT_EQ(press.dx, -9229);
    EXPECT_EQ(press.dy, -9229);
}

TEST(Press, TheSameGroundPressesEveryUnitByTheSameFractionOfItsStep) {
    // One thrust-to-weight for every unit (the one factor): on a face falling 30, a tank (13107)
    // is pressed 10812 and a turret (a quarter of the step, 3276) a quarter as hard, 2702 -- so
    // both make headway up it at the same fraction of their own speed, and both are beaten at the
    // same grade.
    const StepRequest at = standing(2, 2, 0, 0);
    EXPECT_EQ(plan_press(ramp_east(100, -30), at.x, at.y, kTankSpeedRaw, kTankSpeedRaw, 0).dx,
              10812);
    EXPECT_EQ(plan_press(ramp_east(100, -30), at.x, at.y, kTurretSpeedRaw, kTurretSpeedRaw, 0).dx,
              2702);
    // Driving east up a 38: 3276 east against a press of 3250 west.
    EXPECT_GT(
        kTurretSpeedRaw
            + plan_press(ramp_east(100, 38), at.x, at.y, kTurretSpeedRaw, kTurretSpeedRaw, 0).dx,
        0)
        << "a turret climbs a 38 too";
    EXPECT_LT(
        kTurretSpeedRaw
            + plan_press(ramp_east(100, 39), at.x, at.y, kTurretSpeedRaw, kTurretSpeedRaw, 0).dx,
        0)
        << "and is beaten by a 39, as a tank is";
}

// ---- the parked unit, on drawn ground -------------------------------------------------------
//
// G7: a unit nobody drives is no longer "on its brakes, held on everything it can climb" (G6) --
// it is gravity's and friction's (`slide_tick`): from rest on ground rising t a tile per tile it
// gains a = g (t - mu) / (1 + t^2) raw a tick, every tick, with g = 357.17 raw a tick^2 (9.81
// m/s^2 at 2 m a tile and 30 ticks a second) and mu = 0.40. RESTATED from G6's pins, which held
// a parked unit to 38 and drifted it by the press past a thrust (148 at 39, 4315 at 60, 394 at
// 40) at a constant speed. The numbers below are that formula by hand, floored.

constexpr Acceleration kEarth{std::int64_t{981} * Fixed::kOne, std::int64_t{180000}};

TEST(Slide, FrictionHoldsAParkedUnitTo25AndAHalfATile) {
    // Falling east by 25 (t = 0.39): friction holds, nothing. By 26 (t = 0.406): gravity beats
    // it by 1.92 raw a tick^2, east -- and a G6 parked unit was held there, and on to 38.
    const Gradient holds{std::int64_t{2} * -25, 0};
    EXPECT_EQ(slide_tick(holds, Step{}, kEarth).dx, 0);
    EXPECT_EQ(slide_tick(holds, Step{}, kEarth).dy, 0);
    const Step gives = slide_tick(Gradient{std::int64_t{2} * -26, 0}, Step{}, kEarth);
    EXPECT_EQ(gives.dx, 1);
    EXPECT_EQ(gives.dy, 0);
    EXPECT_TRUE(friction_holds(holds));
    EXPECT_FALSE(friction_holds(Gradient{std::int64_t{2} * 38, 0})) << "a climb rolls you back";
}

TEST(Slide, ANoOrderUnitRollsDownhill) {
    // The face east falls 60 (t = 0.9375): 357.17 * 0.5375 / 1.8789 = 102.18 raw a tick, east,
    // in the first tick from rest, and nothing sideways.
    const Step v = slide_tick(Gradient{std::int64_t{2} * -60, 0}, Step{}, kEarth);
    EXPECT_EQ(v.dx, 102);
    EXPECT_EQ(v.dy, 0);
}

TEST(Slide, TheRollIsTowardsTheLowerGroundWhicheverWayThatLies) {
    // A face RISING 40 to the east (t = 0.625): 357.17 * 0.225 / 1.3906 = 57.79, carried WEST.
    const Step v = slide_tick(Gradient{std::int64_t{2} * 40, 0}, Step{}, kEarth);
    EXPECT_EQ(v.dx, -57);
    EXPECT_EQ(v.dy, 0);
}

TEST(Slide, SteeringUphillDoesNotCancelIt) {
    // The unit is on a face that falls 40 to the east and the player drives it WEST, up it. The
    // engine gives 13107 west; the press, the same whatever is commanded, is 13501 east. It is
    // added in full, and 40 is past the steepest grade, so the slope wins: the net is EAST, 394.
    const HeightGround ground = ramp_east(100, -40);
    const StepRequest at = standing(2, 2, -kStep, 0);
    const auto drive = plan_step(at, ground);
    ASSERT_TRUE(drive.moved);
    EXPECT_EQ(drive.step.dx, -13107) << "the engine is the engine";
    const Step press = plan_press(ground, at.x, at.y, kStep, kStep, 0);
    EXPECT_EQ(press.dx, 13501) << "the press does not depend on what is commanded";
    EXPECT_EQ(drive.step.dx + press.dx, 394) << "the slope wins over the steering";
}

TEST(Slide, ItStopsWhenTheGroundLevels) {
    // A face of 60 a tile for three tiles, then flat. Let go of the unit: it rolls, gathering
    // speed, runs out onto the level ground, where friction takes mu g (142.87 raw a tick^2) off
    // it every tick, comes to rest and STAYS: ten more ticks move nothing.
    HeightGround ground{};
    for (auto& row : ground.heights) {
        row = {200, 140, 80, 20, 20, 20, 20, 20};
    }
    Fixed x = Fixed::from_raw(Fixed::kOne / 2);
    const Fixed y = Fixed::from_raw(2 * Fixed::kOne + Fixed::kOne / 2);
    Step v{};
    std::int64_t fastest = 0;
    int ticks = 0;
    for (; ticks < 2000; ++ticks) {
        v = slide_tick(gradient_under(ground, x, y), v, kEarth);
        fastest = std::max(fastest, v.dx);
        if (v.dx == 0 && v.dy == 0) {
            break;
        }
        x = Fixed::from_raw(static_cast<std::int32_t>(x.raw + v.dx));
    }
    ASSERT_LT(ticks, 2000) << "the roll never ended";
    EXPECT_GT(fastest, 102) << "it should have gathered speed on the face";
    EXPECT_GE(x.floor_int(), 3) << "it should have come down off the slope";
    EXPECT_LT(x.floor_int(), 7) << "friction should have stopped it on the level";
    const Fixed rest = x;
    for (int more = 0; more < 10; ++more) {
        v = slide_tick(gradient_under(ground, x, y), v, kEarth);
        EXPECT_EQ(v.dx, 0);
        EXPECT_EQ(v.dy, 0);
    }
    EXPECT_EQ(x.raw, rest.raw);
}

// ---- G5, G6: the wall that is a wall at every angle, and no limit of its own -----------------
//
// Harri, driving: "I feel like I can climb too high on the slope. The problem may be the dx or dy
// -- it's not so dramatic, except when you consider the angle." G5 made the wall a HEIGHT
// difference against one limit (38), the same for mover and planner. G6: the wall is the PRESS's
// -- a face is a wall along a heading when the press on it beats the engine that way
// (`press_beats`) -- so there is no limit to pick, and a diagonal face is judged by its true
// steepness, not by a height difference with a run guessed for it.

/// Level ground at 100 with a kink: from corner column 3 the ground rises `rx` a tile east, and,
/// from corner row 3, `ry` a tile south. Tile (2, 2)'s face (corners 2..3) is level, so a unit
/// standing on it feels nothing; tile (3, 2)'s face rises (`rx`, 0) and tile (3, 3)'s (`rx`, `ry`).
HeightGround kinked(int rx, int ry) {
    HeightGround ground{};
    for (int y = 0; y < kRows; ++y) {
        for (int x = 0; x < kCols; ++x) {
            ground.heights.at(static_cast<std::size_t>(y)).at(static_cast<std::size_t>(x))
                = 100 + (rx * std::max(0, x - 3)) + (ry * std::max(0, y - 3));
        }
    }
    return ground;
}

/// THE MOVER'S VERDICT: a unit of full step `speed` standing 0.01 tile short of the edge (or the
/// corner) of tile (2, 2) steps straight at (3, 2) or, `diagonal`, (3, 3). True when that STRAIGHT
/// step is taken and lands there. (Refused, the mover may still find its way onto the face by an
/// escape at another heading -- a face too steep straight up can be under the grade obliquely, the
/// way a road zig-zags up a hill -- and that is a different edge, judged on its own heading.)
bool mover_enters(const HeightGround& ground, bool diagonal, std::int64_t speed) {
    const std::int32_t near = Fixed::kOne - Fixed::kOne / 100;
    StepRequest request;
    request.x = Fixed::from_raw(2 * Fixed::kOne + near);
    request.y = Fixed::from_raw(2 * Fixed::kOne + (diagonal ? near : Fixed::kOne / 2));
    const std::int64_t along = diagonal ? speed * kDiagonalNumerator / kDiagonalDenominator : speed;
    request.want_dx = along;
    request.want_dy = diagonal ? along : 0;
    request.speed = speed;
    const StepPlan plan = plan_step(request, ground);
    if (!plan.moved || plan.step.dx != request.want_dx || plan.step.dy != request.want_dy) {
        return false;
    }
    const Fixed x = Fixed::from_raw(static_cast<std::int32_t>(request.x.raw + plan.step.dx));
    const Fixed y = Fixed::from_raw(static_cast<std::int32_t>(request.y.raw + plan.step.dy));
    return x.floor_int() == 3 && y.floor_int() == (diagonal ? 3 : 2);
}

/// THE PLANNER'S VERDICT on the very same edge (no tile held by a slide, so only the edge itself is
/// judged). It takes no speed: one rule for every unit.
bool planner_opens(const HeightGround& ground, bool diagonal) {
    const auto height = [&](int x, int y) { return ground.height(x, y); };
    const auto no_slide = [](int, int) { return std::array<int, 2>{0, 0}; };
    return !route_detail::edge_blocked(height, no_slide, 2, 2, 3, diagonal ? 3 : 2);
}

TEST(WallAtEveryAngle, AFaceIsAWallExactlyWhereThePressBeatsTheEngine) {
    // Straight up an axis face: drivable to 38 a tile, a wall from 39 -- where a tank's engine
    // (13107) meets the press (13005 at 38, 13255 at 39). No 38 is written in the rule.
    for (int rise = 0; rise <= 80; ++rise) {
        const HeightGround ground = kinked(rise, 0);
        EXPECT_EQ(mover_enters(ground, false, kStep), rise <= 38) << "axis, rise " << rise;
    }
    // Straight up a DIAGONAL face, rising `r` east and `r` south: its true steepness is r * sqrt 2.
    // 27 and 27 is 38.18 (drivable), 28 and 28 is 39.60 (a wall) -- though each axis reads 28,
    // under the 38. Before G5 a diagonal was scaled by 181/256 and every diagonal cliff let
    // through; G6 reads the face's own gradient, so the diagonal is honest without any scaling.
    for (int r = 0; r <= 60; ++r) {
        const HeightGround ground = kinked(r, r);
        EXPECT_EQ(mover_enters(ground, true, kStep), r <= 27) << "diagonal, r " << r;
    }
}

TEST(WallAtEveryAngle, AFaceIsNoWallAcrossItOrDownIt) {
    // A face past any grade a unit climbs (100 a tile, 57 degrees) is a wall ONLY along headings
    // into it: driven along it (across the gradient) or down it, the press never beats a heading.
    // This is what lets a unit drive along a cliff's foot, or off its top -- the fall.
    for (int k = 0; k < 24; ++k) {
        const double a = k * 15.0 * 3.14159265358979 / 180.0;
        const auto hx = static_cast<std::int64_t>(std::lround(std::cos(a) * kStep));
        const auto hy = static_cast<std::int64_t>(std::lround(std::sin(a) * kStep));
        const Gradient steep{std::int64_t{2} * 100, 0}; // rising east, 100 a tile
        // The press is 1.637 thrusts at 100 a tile (sin 57.4 degrees * sqrt 34 / 3), pointing
        // west: it beats a heading whose eastward share passes 1 / 1.637 = 0.611, i.e. within
        // 52.3 degrees of due east.
        const double cos_east = std::cos(a);
        const bool beaten = press_beats(steep, hx, hy);
        if (cos_east <= 0.0) {
            EXPECT_FALSE(beaten) << "heading " << k * 15 << " is across or down the face";
        }
        if (cos_east > 0.65) {
            EXPECT_TRUE(beaten) << "heading " << k * 15 << " is into the face";
        }
        if (cos_east > 0.0 && cos_east < 0.55) {
            EXPECT_FALSE(beaten) << "heading " << k * 15 << " climbs it obliquely, under the grade";
        }
    }
}

TEST(WallAtEveryAngle, ForEveryFaceTheMoverAndThePlannerAgree) {
    // ONE rule, in one place: for every face, axis and diagonal, a tank and a turret (a quarter of
    // a tank's step), the planner opens exactly the edges the mover will drive. Before G5 a tank's
    // 39 and 40 were driven but not planned, and a diagonal of 41..55 was driven by both though it
    // is a cliff; in G5 both judged a HEIGHT, and the press on a diagonal face of 39..54 disagreed
    // with them both. Now all three ask `press_beats`. The one place the planner is stricter is
    // its corner rule: a diagonal is opened only if the two faces beside the corner it cuts are no
    // wall along it either (a unit following the line crosses them), so where one of those is a
    // wall the planner refuses an edge the mover's single step -- through the corner point itself
    // -- would take. Asked with the same `press_beats`, and asserted here as exactly that.
    const auto height_of
        = [](const HeightGround& g) { return [&g](int x, int y) { return g.height(x, y); }; };
    for (const std::int64_t speed : {kStep, kStep / 4}) {
        for (int rx = -80; rx <= 80; rx += 1) {
            for (const int ry : {0, rx, -rx, rx / 2}) {
                const HeightGround ground = kinked(rx, ry);
                EXPECT_EQ(mover_enters(ground, false, speed), planner_opens(ground, false))
                    << "face (" << rx << ", " << ry << ") axis, speed " << speed;
                const auto height = height_of(ground);
                const bool sides_clear
                    = drivable(height, 3, 2, 1, 1) && drivable(height, 2, 3, 1, 1);
                EXPECT_EQ(mover_enters(ground, true, speed) && sides_clear,
                          planner_opens(ground, true))
                    << "face (" << rx << ", " << ry << ") diagonal, speed " << speed;
            }
        }
    }
}

// ---- the ground the other tests moved to -----------------------------------------------------

TEST(QuietGround, NothingRollsOnTheStripTheOldFixturesMovedTo) {
    // Seed 42 is hillside country, and since M1 an idle unit on a steep slope slides. The follow,
    // hold-fire and projectile tests used to stand round (100,100), a long hillside; they stand on
    // this strip now. If the world changes and this fails, THOSE tests' fixtures have moved under
    // them, and this says so first.
    // RESTATED, G7: an idle unit now ROLLS wherever friction does not hold it (25.6 a tile, not
    // the steepest grade's 38.4), and the strip was x 100..189, y 14..21: 14 of its tiles roll on
    // one triangle or the other -- (100..102, 14..16), (178, 20), (162..166, 21). The follow and
    // hold-fire fixtures on it pass as they stand; what is asserted clear is x 103..160, y 14..20,
    // both drawn triangles of every tile.
    const auto height = [](int x, int y) { return world::sample(kSeed, x, y).height; };
    for (int y = 14; y < 21; ++y) {
        for (int x = 103; x < 161; ++x) {
            const auto faces = face_triangles(height, x, y);
            EXPECT_TRUE(friction_holds(faces[0]) && friction_holds(faces[1]))
                << "tile " << x << "," << y;
        }
    }
}

// ---- the planner, on drawn ground ----------------------------------------------------------

/// The whole map open, with a plateau two tiles wide at x = 20, 21 that rises 100 over the plain
/// (a cliff on either side) and has one gap, at y = 29..31, where it is level with the plain.
/// THREE rows (G6): a tile's height is its CORNER's, so a gap one corner wide is a V between two
/// faces 100 high, with not one level tile in it; three corners make two level rows of ground.
struct CliffWall {
    [[nodiscard]] std::int64_t factor(int x, int y) const noexcept {
        return x >= 0 && y >= 0 && x < kRouteTiles && y < kRouteTiles ? Fixed::kOne : 0;
    }
    [[nodiscard]] int height(int x, int y) const noexcept {
        return (x == 20 || x == 21) && (y < 29 || y > 31) ? 100 : 0;
    }
};

TEST(SlopeRoute, ARouteGoesThroughTheGapInACliffAndNotOverIt) {
    // The mover refuses a cliff (above), so a planner that did not know would lead a unit up to the
    // face and leave it there: the skirmish's enemy tanks stood at a rim for four hundred ticks
    // before the planner was taught. With the wall in the way the route's only way across is the
    // gap, so a corner of the route sits on it.
    Route route;
    ASSERT_TRUE(plan_route(CliffWall{}, TileXY{10, 10}, TileXY{30, 10}, route));
    ASSERT_GT(route.count, 0);
    bool through_gap = false;
    for (int i = 0; i < route.count; ++i) {
        const int y = route.points.at(static_cast<std::size_t>(i)).y;
        through_gap = through_gap || (y >= 29 && y <= 31);
    }
    EXPECT_TRUE(through_gap) << "the route does not use the gap";
}

TEST(SlopeRoute, AGroundWithNoHeightsIsPlannedAsItAlwaysWas) {
    struct Open {
        [[nodiscard]] std::int64_t factor(int x, int y) const noexcept {
            return x >= 0 && y >= 0 && x < kRouteTiles && y < kRouteTiles ? Fixed::kOne : 0;
        }
    };
    Route route;
    ASSERT_TRUE(plan_route(Open{}, TileXY{10, 10}, TileXY{30, 10}, route));
    EXPECT_EQ(route.count, 1) << "an open field is one straight leg";
}

// ---- the sim, on the real world ------------------------------------------------------------

struct SteepPlace {
    int x{};
    int y{};
    Gradient slope{}; ///< the gradient of the ground under a unit at the tile's corner
};

/// A tile of the real world, with open ground all round, whose OWN face (the drawn triangle at its
/// corner) is steeper than a tank climbs (so a parked unit on it rolls, and a driven one feels the
/// press past its grip), and whose first tick of roll lands on ground the unit can stand on and
/// enter. Found by looking, so the test is about a place that exists, not one that is
/// assumed.
std::optional<SteepPlace> find_steep() {
    struct World {
        [[nodiscard]] int height(int x, int y) const noexcept {
            return world::sample(kSeed, x, y).height;
        }
    };
    for (int y = 12; y < 240; ++y) {
        for (int x = 12; x < 240; ++x) {
            bool open = true;
            for (int dy = -1; dy <= 1 && open; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    open = open && tile_passable(kSeed, x + dx, y + dy)
                           && world::sample(kSeed, x + dx, y + dy).terrain != world::Terrain::Ridge;
                }
            }
            if (!open) {
                continue;
            }
            const Gradient g = gradient_under(World{}, Fixed::from_int(x), Fixed::from_int(y));
            const Step drift = slide_at(kSeed, Fixed::from_int(x), Fixed::from_int(y));
            if (!too_steep_to_climb(g) || (drift.dx == 0 && drift.dy == 0)) {
                continue;
            }
            const Fixed to_x = Fixed::from_raw(
                static_cast<std::int32_t>((std::int64_t{x} * Fixed::kOne) + drift.dx));
            const Fixed to_y = Fixed::from_raw(
                static_cast<std::int32_t>((std::int64_t{y} * Fixed::kOne) + drift.dy));
            const auto height = [](int hx, int hy) { return world::sample(kSeed, hx, hy).height; };
            const bool stays = to_x.floor_int() == x && to_y.floor_int() == y;
            if (stays || drivable(height, to_x.floor_int(), to_y.floor_int(), drift.dx, drift.dy)) {
                return SteepPlace{x, y, g};
            }
        }
    }
    return std::nullopt;
}

/// A level patch: nine tiles with no drop past a few units, so nothing slides.
std::optional<std::array<int, 2>> find_level() {
    for (int y = 12; y < 240; ++y) {
        for (int x = 12; x < 240; ++x) {
            bool level = true;
            const int here = world::sample(kSeed, x, y).height;
            for (int dy = -2; dy <= 2 && level; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    level = level && tile_passable(kSeed, x + dx, y + dy)
                            && std::abs(world::sample(kSeed, x + dx, y + dy).height - here) <= 6;
                }
            }
            if (level) {
                return std::array<int, 2>{x, y};
            }
        }
    }
    return std::nullopt;
}

Sim sim_with(UnitKind kind, int tile_x, int tile_y) {
    Scenario scenario{};
    scenario.seed = kSeed;
    Spawn unit{};
    unit.side = Side::Player;
    unit.kind = kind;
    unit.tile_x = tile_x;
    unit.tile_y = tile_y;
    scenario.spawns.push_back(unit);
    Spawn base{};
    base.side = Side::Enemy;
    base.kind = UnitKind::Factory;
    base.tile_x = 240;
    base.tile_y = 240;
    scenario.spawns.push_back(base);
    return Sim::create(scenario);
}

const Unit& the_player(const Sim& sim) {
    for (const Unit& unit : sim.units()) {
        if (unit.side == Side::Player) {
            return unit;
        }
    }
    return sim.units().front();
}

TEST(SlopeInTheSim, ThereIsASteepPlaceToStandOn) {
    EXPECT_TRUE(find_steep().has_value());
    EXPECT_TRUE(find_level().has_value());
}

TEST(SlopeInTheSim, AUnitWithNoOrderDriftsDownhill) {
    const auto place = find_steep();
    if (!place.has_value()) {
        FAIL() << "the world offers no such place";
    }
    Sim sim = sim_with(UnitKind::Tank, place->x, place->y);
    const Unit start = the_player(sim);
    // Where the world says it drifts, and the drift is nonzero: the ground here is past the
    // steepest grade.
    const Step expected = slide_at(kSeed, start.x, start.y);
    ASSERT_GT(std::abs(expected.dx) + std::abs(expected.dy), 0);
    sim.step();
    const Unit& after = the_player(sim);
    // Down the slope: against the gradient.
    const std::int64_t down = -((after.x.raw - start.x.raw) * place->slope.x2)
                              - ((after.y.raw - start.y.raw) * place->slope.y2);
    EXPECT_GT(down, 0) << "an idle unit on a slope must move down it";
    EXPECT_EQ(after.x.raw - start.x.raw, expected.dx);
    EXPECT_EQ(after.y.raw - start.y.raw, expected.dy);
}

TEST(SlopeInTheSim, ADrivenUnitFeelsThePressAndTheDriveIsAddedToIt) {
    const auto place = find_steep();
    if (!place.has_value()) {
        FAIL() << "the world offers no such place";
    }
    // Ordered UPHILL, against the gradient, on the steep place: the tick's displacement is the
    // drive PLUS the press of the ground it began on as a vehicle feels it (`driven_press`:
    // along its heading in full, across it past the grip). Worked from the parts here, and the
    // sim must agree to the raw unit -- so the press was neither dropped nor replaced by the
    // order, and the order was not dropped either.
    Sim driven = sim_with(UnitKind::Tank, place->x, place->y);
    const Unit start = the_player(driven);
    const std::int64_t len = std::max<std::int64_t>(
        1, static_cast<std::int64_t>(std::sqrt(static_cast<double>(
               (place->slope.x2 * place->slope.x2) + (place->slope.y2 * place->slope.y2)))));
    driven.enqueue(
        Order{start.id,
              orders::MoveTo{
                  Fixed::from_raw(static_cast<std::int32_t>(
                      start.x.raw + (std::int64_t{6} * Fixed::kOne * place->slope.x2 / len))),
                  Fixed::from_raw(static_cast<std::int32_t>(
                      start.y.raw + (std::int64_t{6} * Fixed::kOne * place->slope.y2 / len)))}});
    driven.step();
    const Unit& b = the_player(driven);
    ASSERT_NE(b.vx.raw | b.vy.raw, 0) << "the order should still drive the unit";
    // The commanded step: the velocity, on this tile's terrain (as the mover scales it).
    const std::int64_t factor
        = terrain_speed_factor(world::sample(kSeed, place->x, place->y).terrain).raw;
    const std::int64_t cx = (static_cast<std::int64_t>(b.vx.raw) * factor) >> Fixed::kShift;
    const std::int64_t cy = (static_cast<std::int64_t>(b.vy.raw) * factor) >> Fixed::kShift;
    const Step felt = driven_press(place->slope, kStep * factor >> Fixed::kShift, cx, cy);
    EXPECT_EQ(b.x.raw - start.x.raw, cx + felt.dx);
    EXPECT_EQ(b.y.raw - start.y.raw, cy + felt.dy);
    EXPECT_NE(felt.dx | felt.dy, 0) << "the press was dropped";
}

TEST(SlopeInTheSim, LevelGroundDoesNotDrift) {
    const auto place = find_level();
    if (!place.has_value()) {
        FAIL() << "the world offers no such place";
    }
    Sim sim = sim_with(UnitKind::Tank, (*place)[0], (*place)[1]);
    const Unit start = the_player(sim);
    EXPECT_EQ(slide_at(kSeed, start.x, start.y).dx, 0);
    EXPECT_EQ(slide_at(kSeed, start.x, start.y).dy, 0);
    for (int tick = 0; tick < 30; ++tick) {
        sim.step();
    }
    EXPECT_EQ(the_player(sim).x.raw, start.x.raw);
    EXPECT_EQ(the_player(sim).y.raw, start.y.raw);
}

TEST(SlopeInTheSim, ALightAndAHeavyUnitRollByOneFactor) {
    // Harri's second ruling: one factor for every unit. G7 (RESTATED from G6): the factor of a
    // roll is GRAVITY, which pulls every mass alike, so a scout (light, fast) and a walker (heavy,
    // slow) let go on the same slope roll by the SAME raw amount -- what `slide_at` says, which
    // takes no step. (G6 carried each by the same FRACTION of its own step, because its parked
    // drift was the engine's press past the engine's grip; nothing about the engine enters a
    // roll.) If someone makes it per-kind, this is the test that says it was a decision.
    const auto place = find_steep();
    if (!place.has_value()) {
        FAIL() << "the world offers no such place";
    }
    Sim light = sim_with(UnitKind::Scout, place->x, place->y);
    Sim heavy = sim_with(UnitKind::Walker, place->x, place->y);
    const Unit light_start = the_player(light);
    const Unit heavy_start = the_player(heavy);
    ASSERT_EQ(light_start.x.raw, heavy_start.x.raw);
    ASSERT_EQ(light_start.y.raw, heavy_start.y.raw);
    ASSERT_NE(profile(UnitKind::Scout).max_speed.raw, profile(UnitKind::Walker).max_speed.raw)
        << "the test needs two different engines";
    light.step();
    heavy.step();
    const Step roll = slide_at(kSeed, light_start.x, light_start.y);
    ASSERT_GT(std::abs(roll.dx) + std::abs(roll.dy), 0);
    EXPECT_EQ(the_player(light).x.raw - light_start.x.raw, roll.dx);
    EXPECT_EQ(the_player(light).y.raw - light_start.y.raw, roll.dy);
    EXPECT_EQ(the_player(heavy).x.raw - heavy_start.x.raw, roll.dx);
    EXPECT_EQ(the_player(heavy).y.raw - heavy_start.y.raw, roll.dy);
}

} // namespace
} // namespace protekt::sim
