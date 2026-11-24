#include "protekt/sim/route.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/steering.hpp"
#include "protekt/world/world.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

/// THE SLOPE (M1). Uphill costs speed, downhill grants it, a cliff is a wall, and a slope that is
/// steep enough SLIDES whatever stands on it -- and the slide is added to whatever the unit is
/// commanded, so steering does not cancel it. One factor for every unit (Harri's ruling, pinned
/// below so the simplification cannot be quietly undone).
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

const auto always_open = [](int, int) { return true; };

// ---- the rule, on drawn ground ------------------------------------------------------------

TEST(Slope, LevelGroundIsTheSpeedItAlwaysWas) {
    // The control for the next two: a level edge changes nothing.
    const auto plan = plan_step(standing(2, 2, kStep, 0), ramp_east(100, 0));
    ASSERT_TRUE(plan.moved);
    EXPECT_EQ(plan.step.dx, 13107);
}

TEST(Slope, UphillIsSlowerThanLevel) {
    // A rise of 16 over the edge ahead: grade = 1 - 16/64 = 3/4 = 49152 raw, and the step is
    // 13107 * 49152 >> 16 = 9830.
    const auto plan = plan_step(standing(2, 2, kStep, 0), ramp_east(100, 16));
    ASSERT_TRUE(plan.moved);
    EXPECT_EQ(plan.step.dx, 9830);
    EXPECT_LT(plan.step.dx, 13107);
}

TEST(Slope, DownhillIsFasterThanLevel) {
    // A drop of 16: grade = 1 + 16/64 = 5/4 = 81920 raw, and the step is 13107 * 81920 >> 16 =
    // 16383.
    const auto plan = plan_step(standing(2, 2, kStep, 0), ramp_east(100, -16));
    ASSERT_TRUE(plan.moved);
    EXPECT_EQ(plan.step.dx, 16383);
    EXPECT_GT(plan.step.dx, 13107);
}

TEST(Slope, TheGradeIsBoundedAtBothEnds) {
    // A drop of 40 asks for 1 + 40/64 = 1.625, but a descent is capped at 3/2: 13107 * 3/2 = 19660.
    const auto down = plan_step(standing(2, 2, kStep, 0), ramp_east(100, -40));
    ASSERT_TRUE(down.moved);
    EXPECT_EQ(down.step.dx, 19660);
    // A rise of 40 asks for 1 - 40/64 = 0.375, above the floor of 1/4: 13107 * 24576 >> 16 = 4915.
    const auto up = plan_step(standing(2, 2, kStep, 0), ramp_east(100, 40));
    ASSERT_TRUE(up.moved);
    EXPECT_EQ(up.step.dx, 4915);
}

TEST(Slope, PastTheClimbLimitTheStepIsRefused) {
    // The limit is 38 and these write the NUMBER, not the constant: the first version of this test
    // said `kClimbLimit` and `kClimbLimit + 1`, and it stayed green when the sabotage run set the
    // limit to a million, because it moved with it. A different limit is a decision, and should
    // have to edit this -- as G5 did: it was 40, a free number, and the slide physics never let a
    // tank climb a slope of 39 or 40 (5119 raw of climb against 5733 of slide at 39). The limit is
    // now derived from that physics (`steepest_headway_rise`). The unit is 0.05 tile from the east
    // edge of tile 2, so a level step of 0.2 would carry it into tile 3. At a rise of exactly the
    // limit it does; one past the limit, it does not: a cliff is a wall, and the unit is NOT in the
    // steep tile afterwards.
    const std::int32_t near_edge = Fixed::kOne - Fixed::kOne / 20;
    const auto at_limit = plan_step(standing(2, 2, kStep, 0, near_edge), ramp_east(0, 38));
    ASSERT_TRUE(at_limit.moved);
    EXPECT_GE(2 * Fixed::kOne + near_edge + at_limit.step.dx, 3 * Fixed::kOne)
        << "a climb of exactly the limit is allowed: the unit gets over the edge";

    const auto cliff = plan_step(standing(2, 2, kStep, 0, near_edge), ramp_east(0, 39));
    const std::int64_t after = 2 * Fixed::kOne + near_edge + (cliff.moved ? cliff.step.dx : 0);
    EXPECT_LT(after, 3 * Fixed::kOne) << "the unit entered a tile too steep to climb";
}

TEST(Slope, ADiagonalEdgeIsMeasuredOverItsLongerRun) {
    // Corner to corner is 1.41 tiles, so the same height change is a gentler slope: a rise of 40
    // read over the diagonal is 40 * 181 / 256 = 28, so the step to the south-east is scaled by
    // that, not by the axis figure of 0.375.
    HeightGround ground = ramp_east(100, 0);
    ground.heights.at(3).at(3) = 140; // (3,3) is 40 above (2,2), diagonally
    const auto plan = plan_step(standing(2, 2, kStep, kStep), ground);
    ASSERT_TRUE(plan.moved);
    // rise 28 -> grade = 65536 - 28 * 65536 / 64 = 36864; step = 13107 * 36864 >> 16 = 7372.
    EXPECT_EQ(plan.step.dx, 7372);
    EXPECT_EQ(plan.step.dy, 7372);
}

// ---- the slide, on drawn ground -------------------------------------------------------------

TEST(Slide, AGentleSlopeHolds) {
    // Drops up to the threshold (32) do not move a unit, and one past it does, by one raw step of
    // 819 -- the law is the excess over the threshold.
    const HeightGround holds = ramp_east(100, -kSlideThreshold);
    const Step none
        = plan_slide(holds, standing(2, 2, 0, 0).x, standing(2, 2, 0, 0).y, always_open);
    EXPECT_EQ(none.dx, 0);
    EXPECT_EQ(none.dy, 0);

    const HeightGround gives = ramp_east(100, -(kSlideThreshold + 1));
    const Step drift
        = plan_slide(gives, standing(2, 2, 0, 0).x, standing(2, 2, 0, 0).y, always_open);
    EXPECT_EQ(drift.dx, 819);
    EXPECT_EQ(drift.dy, 0);
}

TEST(Slide, ANoOrderUnitDriftsDownhill) {
    // The ruling: "you drift downhill". Nothing is commanded; the drop east is 40, so the drift is
    // (40 - 32) * 819 = 6552 raw east and nothing sideways.
    const HeightGround ground = ramp_east(100, -40);
    const StepRequest at = standing(2, 2, 0, 0);
    const Step drift = plan_slide(ground, at.x, at.y, always_open);
    EXPECT_EQ(drift.dx, 6552);
    EXPECT_EQ(drift.dy, 0);
}

TEST(Slide, ASheerFaceSlidesNoFasterThanATankDrives) {
    // The world's worst edge is a drop of 55: (55 - 32) * 819 = 18837 would be a step and a half,
    // so the drift is capped at one tank step, 13107.
    const HeightGround ground = ramp_east(100, -55);
    const StepRequest at = standing(2, 2, 0, 0);
    EXPECT_EQ(plan_slide(ground, at.x, at.y, always_open).dx, 13107);
}

TEST(Slide, TheDriftIsTowardsTheLowerGroundWhicheverWayThatLies) {
    // The same slope drawn the other way round: the unit stands on the east side of the drop and
    // is carried WEST.
    HeightGround ground{};
    for (auto& row : ground.heights) {
        for (int x = 0; x < kCols; ++x) {
            row.at(static_cast<std::size_t>(x)) = x < 3 ? 60 : 100;
        }
    }
    const StepRequest at = standing(3, 2, 0, 0);
    const Step drift = plan_slide(ground, at.x, at.y, always_open);
    EXPECT_EQ(drift.dx, -(40 - kSlideThreshold) * kSlideRawPerHeight);
    EXPECT_EQ(drift.dy, 0);
}

TEST(Slide, SteeringUphillDoesNotCancelIt) {
    // The unit stands at the foot-side of a drop of 40 to the east, and the player drives it WEST,
    // up the slope that is under it. The ruling bites hardest at the steepest climbable edge, so
    // the slope is drawn so that west is UP by the climb limit: tiles 0 and 1 are 40 higher than 2,
    // and tile 3 is 40 lower. The climb gives 13107 * 0.375 = 4915 west; the slide gives
    // (40 - 32) * 819 = 6552 east. The slide is applied in full, and it wins: the net is EAST.
    HeightGround ground{};
    for (auto& row : ground.heights) {
        for (int x = 0; x < kCols; ++x) {
            row.at(static_cast<std::size_t>(x)) = x < 2 ? 140 : (x == 2 ? 100 : 60);
        }
    }
    StepRequest at = standing(2, 2, -kStep, 0, 0);
    at.x = Fixed::from_raw(2 * Fixed::kOne + Fixed::kOne / 2);
    const auto drive = plan_step(at, ground);
    ASSERT_TRUE(drive.moved);
    // 13107 * 0.375 = 4915.1; the mover has always shifted, which floors, so west reads one more.
    EXPECT_EQ(drive.step.dx, -4916) << "the climb itself is slowed, not stopped";
    const Step drift = plan_slide(ground, at.x, at.y, always_open);
    EXPECT_EQ(drift.dx, 6552) << "the drift does not depend on what is commanded";
    EXPECT_GT(drive.step.dx + drift.dx, 0) << "the slope wins over the steering";
}

TEST(Slide, ItStopsWhenTheGroundLevels) {
    // A slope of 40 per tile for two tiles, then flat. Let go of the unit: it slides, and it
    // comes to rest on the level ground and STAYS: ten more ticks move nothing.
    HeightGround ground{};
    for (auto& row : ground.heights) {
        row = {200, 160, 120, 80, 80, 80, 80, 80};
    }
    Fixed x = Fixed::from_raw(Fixed::kOne / 2);
    const Fixed y = Fixed::from_raw(2 * Fixed::kOne + Fixed::kOne / 2);
    int ticks = 0;
    const auto on_grid = [&](int tx, int ty) { return ground.inside(tx, ty); };
    for (; ticks < 2000; ++ticks) {
        const Step drift = plan_slide(ground, x, y, on_grid);
        if (drift.dx == 0 && drift.dy == 0) {
            break;
        }
        x = Fixed::from_raw(static_cast<std::int32_t>(x.raw + drift.dx));
    }
    ASSERT_LT(ticks, 2000) << "the slide never ended";
    EXPECT_GE(x.floor_int(), 3) << "it should have come down off the slope";
    const Fixed rest = x;
    for (int more = 0; more < 10; ++more) {
        const Step drift = plan_slide(ground, x, y, on_grid);
        EXPECT_EQ(drift.dx, 0);
        EXPECT_EQ(drift.dy, 0);
    }
    EXPECT_EQ(x.raw, rest.raw);
}

TEST(Slide, ItDoesNotCarryAUnitOntoGroundItCannotEnter) {
    // The steepest way down is into a tile that is shut (a crater, a building, the map's edge): the
    // slide does not go there, it takes the next steepest or none.
    const HeightGround ground = ramp_east(100, -40);
    const StepRequest at = standing(2, 2, 0, 0);
    const auto shut_east = [](int x, int) { return x != 3; };
    const Step drift = plan_slide(ground, at.x, at.y, shut_east);
    EXPECT_EQ(drift.dx, 0);
}

// ---- G5: the wall that is a wall at every angle, and ONE limit -----------------------------
//
// Harri, driving: "I feel like I can climb too high on the slope. The problem may be the dx or dy
// -- it's not so dramatic, except when you consider the angle." The run-scaling of a diagonal edge
// (x 181/256) is right for a GRADE and wrong for a WALL: compared against the climb limit, it let
// a diagonal of height 56 through as "39". And the planner judged an edge by its own estimate
// (`drivable_rise`: a tank's limit 38, a turret's 33) while the mover judged it by the rule (40),
// so the two disagreed about rises the mover would drive.

/// Flat ground at 100 with ONE tile raised by `rise`: (3, 2) when `diagonal` is false (east of the
/// unit's tile (2, 2)), (3, 3) when it is true (south-east of it).
HeightGround one_raised(int rise, bool diagonal) {
    HeightGround ground = ramp_east(100, 0);
    ground.heights.at(diagonal ? 3 : 2).at(3) = 100 + rise;
    return ground;
}

/// THE MOVER'S VERDICT: a unit of full step `speed` standing 0.01 tile short of the edge (or the
/// corner) of tile (2, 2) steps straight at the raised tile. True when the step lands in it.
bool mover_enters(int rise, bool diagonal, std::int64_t speed) {
    const std::int32_t near = Fixed::kOne - Fixed::kOne / 100;
    StepRequest request;
    request.x = Fixed::from_raw(2 * Fixed::kOne + near);
    request.y = Fixed::from_raw(2 * Fixed::kOne + (diagonal ? near : Fixed::kOne / 2));
    const std::int64_t along = diagonal ? speed * kDiagonalNumerator / kDiagonalDenominator : speed;
    request.want_dx = along;
    request.want_dy = diagonal ? along : 0;
    request.speed = speed;
    const StepPlan plan = plan_step(request, one_raised(rise, diagonal));
    if (!plan.moved) {
        return false;
    }
    const Fixed x = Fixed::from_raw(static_cast<std::int32_t>(request.x.raw + plan.step.dx));
    const Fixed y = Fixed::from_raw(static_cast<std::int32_t>(request.y.raw + plan.step.dy));
    return x.floor_int() == 3 && y.floor_int() == (diagonal ? 3 : 2);
}

/// THE PLANNER'S VERDICT on the very same edge, for a unit of full step `speed` on open ground
/// (no tile held by a slide, so only the edge itself is judged).
bool planner_opens(int rise, bool diagonal, std::int64_t speed) {
    const HeightGround ground = one_raised(rise, diagonal);
    const auto height = [&](int x, int y) { return ground.height(x, y); };
    const auto no_slide = [](int, int) { return std::array<int, 2>{0, 0}; };
    (void)speed; // one rule for every unit: the planner's verdict on an edge takes no speed
    return !route_detail::edge_blocked(height, no_slide, 2, 2, 3, diagonal ? 3 : 2);
}

TEST(WallAtEveryAngle, AHeightPastTheClimbLimitIsRefusedOnTheAxisAndOnTheDiagonal) {
    // The limit is 38, written as the number (see PastTheClimbLimitTheStepIsRefused). Under the old
    // limit of 40, a rise of 41 to 56 read 29 to 39 on a diagonal, under it: every diagonal cliff
    // in the world (the worst edge is 55) was climbable. A wall is decided by the HEIGHT, at every
    // angle.
    for (const int rise : {39, 40, 41, 45, 50, 55, 56, 80}) {
        for (const bool diagonal : {false, true}) {
            EXPECT_FALSE(mover_enters(rise, diagonal, kStep))
                << "the mover let a tank up a rise of " << rise << (diagonal ? " (diagonal)" : "");
            EXPECT_FALSE(planner_opens(rise, diagonal, kStep))
                << "the planner opened a rise of " << rise << (diagonal ? " (diagonal)" : "");
        }
    }
    // ...and up to the limit, it is not a wall at either angle.
    for (const int rise : {0, 20, 33, 37, 38}) {
        for (const bool diagonal : {false, true}) {
            EXPECT_TRUE(mover_enters(rise, diagonal, kStep))
                << "rise " << rise << (diagonal ? " (diagonal)" : "");
        }
    }
}

TEST(WallAtEveryAngle, ForEveryRiseTheMoverAndThePlannerAgree) {
    // ONE limit, in one place: for every rise, axis and diagonal, a tank and a turret (a quarter of
    // a tank's step), the planner opens exactly the edges the mover will drive. Before G5 a tank's
    // 39 and 40 were driven but not planned (38 was the planner's tank limit), a turret's 34..40
    // likewise (33), and a diagonal of 41..55 was driven by both though it is a cliff.
    for (const std::int64_t speed : {kStep, kStep / 4}) {
        for (int rise = -80; rise <= 80; ++rise) {
            for (const bool diagonal : {false, true}) {
                EXPECT_EQ(mover_enters(rise, diagonal, speed), planner_opens(rise, diagonal, speed))
                    << "rise " << rise << (diagonal ? " diagonal" : " axis") << ", speed " << speed;
            }
        }
    }
}

// ---- the ground the other tests moved to -----------------------------------------------------

TEST(QuietGround, NothingSlidesOnTheStripTheOldFixturesMovedTo) {
    // Seed 42 is hillside country, and since M1 an idle unit on a steep slope slides. The follow,
    // hold-fire and projectile tests used to stand round (100,100), a long hillside; they stand on
    // this strip now, x 100..189 and y 14..21, where no tile slides anything. If the world changes
    // and this fails, THOSE tests' fixtures have moved under them, and this says so first.
    for (int y = 14; y < 22; ++y) {
        for (int x = 100; x < 190; ++x) {
            const Step drift = slide_at(kSeed, Fixed::from_int(x), Fixed::from_int(y));
            EXPECT_EQ(drift.dx, 0) << "tile " << x << "," << y;
            EXPECT_EQ(drift.dy, 0) << "tile " << x << "," << y;
        }
    }
}

// ---- the planner, on drawn ground ----------------------------------------------------------

/// The whole map open, with a plateau two tiles wide at x = 20, 21 that rises 100 over the plain
/// (a cliff on either side) and has one gap, at y = 30, where it is level with the plain.
struct CliffWall {
    [[nodiscard]] std::int64_t factor(int x, int y) const noexcept {
        return x >= 0 && y >= 0 && x < kRouteTiles && y < kRouteTiles ? Fixed::kOne : 0;
    }
    [[nodiscard]] int height(int x, int y) const noexcept {
        return (x == 20 || x == 21) && y != 30 ? 100 : 0;
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
        through_gap = through_gap || route.points.at(static_cast<std::size_t>(i)).y == 30;
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
    int down_x{}; ///< the unit vector of the steepest way down: one of the four axes
    int down_y{};
    int drop{};
};

/// A tile of the real world with open ground all round and a steepest way down that is an AXIS
/// neighbour of at least `min_drop`, with the opposite neighbour climbable. Found by looking, so
/// the test is about a place that exists, not one that is assumed.
std::optional<SteepPlace> find_steep(int min_drop, int max_drop) {
    constexpr std::array<std::array<int, 2>, 4> kAxes{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
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
            const int here = world::sample(kSeed, x, y).height;
            int steepest = 0;
            std::array<int, 2> way{};
            for (const auto& axis : kAxes) {
                const int drop = here - world::sample(kSeed, x + axis[0], y + axis[1]).height;
                if (drop > steepest) {
                    steepest = drop;
                    way = axis;
                }
            }
            // No diagonal may be steeper than the axis chosen (the slide reads all eight).
            bool clean = steepest >= min_drop && steepest <= max_drop;
            for (int dy = -1; dy <= 1 && clean; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx != 0 && dy != 0
                        && (here - world::sample(kSeed, x + dx, y + dy).height) * 181 / 256
                               >= steepest) {
                        clean = false;
                    }
                }
            }
            const int up = world::sample(kSeed, x - way[0], y - way[1]).height - here;
            if (clean && up <= kClimbLimit) {
                return SteepPlace{x, y, way[0], way[1], steepest};
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
    EXPECT_TRUE(find_steep(34, kClimbLimit).has_value());
    EXPECT_TRUE(find_level().has_value());
}

TEST(SlopeInTheSim, AUnitWithNoOrderDriftsDownhill) {
    const auto place = find_steep(34, kClimbLimit);
    if (!place.has_value()) {
        FAIL() << "the world offers no such place";
    }
    Sim sim = sim_with(UnitKind::Tank, place->x, place->y);
    const Unit start = the_player(sim);
    // Where the world says it drifts, and the drift is nonzero: the slope here is past the
    // threshold.
    const Step expected = slide_at(kSeed, start.x, start.y);
    ASSERT_GT(std::abs(expected.dx) + std::abs(expected.dy), 0);
    sim.step();
    const Unit& after = the_player(sim);
    const std::int64_t along
        = (after.x.raw - start.x.raw) * place->down_x + (after.y.raw - start.y.raw) * place->down_y;
    EXPECT_GT(along, 0) << "an idle unit on a slope must move down it";
    EXPECT_EQ(after.x.raw - start.x.raw, expected.dx);
    EXPECT_EQ(after.y.raw - start.y.raw, expected.dy);
}

TEST(SlopeInTheSim, ADrivenUnitStillDriftsAndTheDriveIsAddedToIt) {
    const auto place = find_steep(34, kClimbLimit);
    if (!place.has_value()) {
        FAIL() << "the world offers no such place";
    }
    // The same unit on the same spot, once idle and once ordered UPHILL. The order's contribution
    // is what the two differ by; the slide is in both. Run a few ticks so the tank has turned.
    Sim idle = sim_with(UnitKind::Tank, place->x, place->y);
    Sim driven = sim_with(UnitKind::Tank, place->x, place->y);
    const Unit start = the_player(driven);
    const UnitId id = the_player(driven).id;
    driven.enqueue(Order{id, orders::MoveTo{Fixed::from_int(place->x - place->down_x * 6),
                                            Fixed::from_int(place->y - place->down_y * 6)}});
    // Tick one: both units are on the same spot, so both receive the same slide.
    const Step slide = slide_at(kSeed, start.x, start.y);
    idle.step();
    driven.step();
    const Unit& a = the_player(idle);
    const Unit& b = the_player(driven);
    EXPECT_EQ(a.x.raw - start.x.raw, slide.dx);
    EXPECT_EQ(a.y.raw - start.y.raw, slide.dy);
    // The driven unit's displacement differs from the idle one's only by what it drove: so the
    // slide has NOT been replaced by the order, and the order has not been dropped.
    const std::int64_t drove_along
        = ((b.x.raw - a.x.raw) * -place->down_x) + ((b.y.raw - a.y.raw) * -place->down_y);
    EXPECT_GT(drove_along, 0) << "the order should still drive the unit uphill";
    // The order drove straight up the axis, so what it added has nothing sideways in it: the slide
    // is exactly what the idle unit got, and the rest is the drive.
    const std::int64_t sideways
        = ((b.x.raw - a.x.raw) * place->down_y) + ((b.y.raw - a.y.raw) * place->down_x);
    EXPECT_EQ(std::abs(sideways), 0) << "the slide was altered by the order";
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

TEST(SlopeInTheSim, ALightAndAHeavyUnitSlideIdentically) {
    // Harri's second ruling: one factor for every unit. A scout (light, fast) and a walker (heavy,
    // slow) on the same slope are carried by the same amount on the same tick. If someone makes the
    // factor per-kind, this is the test that says it was a decision.
    const auto place = find_steep(34, kClimbLimit);
    if (!place.has_value()) {
        FAIL() << "the world offers no such place";
    }
    Sim light = sim_with(UnitKind::Scout, place->x, place->y);
    Sim heavy = sim_with(UnitKind::Walker, place->x, place->y);
    const Unit light_start = the_player(light);
    const Unit heavy_start = the_player(heavy);
    ASSERT_EQ(light_start.x.raw, heavy_start.x.raw);
    ASSERT_EQ(light_start.y.raw, heavy_start.y.raw);
    light.step();
    heavy.step();
    const Step expected = slide_at(kSeed, light_start.x, light_start.y);
    ASSERT_GT(std::abs(expected.dx) + std::abs(expected.dy), 0);
    EXPECT_EQ(the_player(light).x.raw - light_start.x.raw, expected.dx);
    EXPECT_EQ(the_player(light).y.raw - light_start.y.raw, expected.dy);
    EXPECT_EQ(the_player(heavy).x.raw - heavy_start.x.raw, expected.dx);
    EXPECT_EQ(the_player(heavy).y.raw - heavy_start.y.raw, expected.dy);
}

} // namespace
} // namespace protekt::sim
