#include "protekt/sim/steering.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace protekt::sim {
namespace {

/// A drawn map: '#' is a crater, '.' is open ground. A hand-drawn map is the point: the corner this
/// rule exists for can be DRAWN, so the test says what it means and does not depend on what seed 42
/// happens to look like.
struct DrawnGround {
    std::array<std::string, 7> rows{};
    [[nodiscard]] std::int64_t factor(int tile_x, int tile_y) const noexcept {
        if (tile_y < 0 || tile_y >= static_cast<int>(rows.size())) {
            return 0;
        }
        const std::string& row = rows.at(static_cast<std::size_t>(tile_y));
        if (tile_x < 0 || tile_x >= static_cast<int>(row.size())) {
            return 0;
        }
        return row[static_cast<std::size_t>(tile_x)] == '#' ? 0 : Fixed::kOne;
    }
};

/// The concave corner in miniature, and the shape of the bug: the unit at (2,2) wants to go
/// south-east, and the east, south AND diagonal tiles are crater. Only the north side is open.
DrawnGround concave_corner() {
    return DrawnGround{{".......", //
                        ".......", //
                        "...#...", //   (3,2) east: crater
                        "..##...", //   (2,3) south and (3,3) south-east: crater
                        ".......", //
                        ".......", //
                        "......."}};
}

DrawnGround open_field() {
    return DrawnGround{
        {".......", ".......", ".......", ".......", ".......", ".......", "......."}};
}

/// (3,3) is open and all eight of its neighbours are crater: nowhere to go at all.
DrawnGround walled_in() {
    return DrawnGround{{".......", //
                        ".......", //
                        "..###..", //
                        "..#.#..", //
                        "..###..", //
                        ".......", //
                        "......."}};
}

/// One tile a tick, so a step visibly crosses a tile boundary and the geometry reads at a glance.
/// (The game's tanks move about a fifth of a tile a tick; the RULE is the same, and this makes it
/// legible.)
constexpr std::int64_t kSpeed = Fixed::kOne;

[[nodiscard]] Fixed at(int tile) noexcept {
    return Fixed::from_raw(
        static_cast<std::int32_t>(static_cast<std::uint32_t>(tile) << Fixed::kShift));
}

StepRequest request_at(int x, int y, std::int64_t want_dx, std::int64_t want_dy) noexcept {
    StepRequest request;
    request.x = at(x);
    request.y = at(y);
    request.want_dx = want_dx;
    request.want_dy = want_dy;
    request.speed = kSpeed;
    return request;
}

TEST(Steering, AStepWithNothingInTheWayIsTakenAsAsked) {
    const auto plan = plan_step(request_at(2, 2, kSpeed, kSpeed), open_field());
    EXPECT_TRUE(plan.moved);
    EXPECT_EQ(plan.step.dx, kSpeed);
    EXPECT_EQ(plan.step.dy, kSpeed);
    EXPECT_FALSE(plan.detour.active()) << "no obstacle, so no detour to remember";
}

TEST(Steering, AGrazeOnARimSlidesAlongItWithoutStartingADetour) {
    // The wanted diagonal is blocked by the crater at (3,3), but the east tile is clear: this is
    // the slide the movement has always had, and it needs no memory of its own.
    DrawnGround ground = open_field();
    ground.rows[3][2] = '#';
    ground.rows[3][3] = '#';
    const auto plan = plan_step(request_at(2, 2, kSpeed, kSpeed), ground);
    EXPECT_TRUE(plan.moved);
    EXPECT_EQ(plan.step.dy, 0) << "the blocked axis must be dropped";
    EXPECT_EQ(plan.step.dx, kSpeed);
    EXPECT_FALSE(plan.detour.active());
}

TEST(Steering, AConcaveCornerKeepsTheUnitMovingInsteadOfStandingStill) {
    // The bug, exactly: at seed 42 a tank sat on one tile for 3000 ticks with a crater ahead of it.
    // The assertion is behavioural -- it must keep moving -- rather than "it must turn west",
    // because which way out is a property of the terrain, not of the rule.
    const DrawnGround ground = concave_corner();
    Fixed x = at(2);
    Fixed y = at(2);
    Detour detour{};
    int moved_ticks = 0;
    int longest_same_tile_run = 0;
    int same_tile_run = 0;
    int last_tile = -1;
    std::vector<int> tiles_visited;

    for (int tick = 0; tick < 90; ++tick) {
        StepRequest request;
        request.x = x;
        request.y = y;
        // Steering re-aims at the goal every tick, which is exactly why a one-tick dodge cannot
        // work.
        request.want_dx = x.floor_int() < 6 ? kSpeed : 0;
        request.want_dy = y.floor_int() < 6 ? kSpeed : 0;
        request.speed = kSpeed;
        request.detour = detour;

        const StepPlan plan = plan_step(request, ground);
        detour = plan.detour;
        if (!plan.moved) {
            ++same_tile_run;
        } else {
            x = Fixed::from_raw(static_cast<std::int32_t>(x.raw + plan.step.dx));
            y = Fixed::from_raw(static_cast<std::int32_t>(y.raw + plan.step.dy));
            ++moved_ticks;
            const int tile = y.floor_int() * 16 + x.floor_int();
            same_tile_run = tile == last_tile ? same_tile_run + 1 : 0;
            if (tile != last_tile
                && std::find(tiles_visited.begin(), tiles_visited.end(), tile)
                       == tiles_visited.end()) {
                tiles_visited.push_back(tile);
            }
            last_tile = tile;
        }
        longest_same_tile_run = std::max(longest_same_tile_run, same_tile_run);
    }

    EXPECT_GT(moved_ticks, 60) << "a unit in a concave corner must not stop dead";
    EXPECT_LT(longest_same_tile_run, 40) << "and it must not pace back and forth inside one tile";
    // Without this the test passes on a unit that jitters inside one tile: "moved" is true for any
    // sub-tile step, so the assertion that carries the meaning is about TILES VISITED.
    EXPECT_GT(static_cast<int>(tiles_visited.size()), 3)
        << "the escape has to actually leave the pocket";
}

TEST(Steering, ADetourIsFollowedAndCountsDown) {
    StepRequest request = request_at(2, 2, kSpeed, kSpeed);
    request.detour = Detour{-kSpeed, 0, 5};
    const auto plan = plan_step(request, concave_corner());
    EXPECT_TRUE(plan.moved);
    EXPECT_EQ(plan.step.dx, -kSpeed);
    EXPECT_EQ(plan.detour.ticks_left, 4) << "a detour that does not count down never ends";
}

TEST(Steering, ADetourSurvivesTheWeakStepTestThatWouldWalkStraightBackIn) {
    // The mechanism at seed 42, isolated. The unit is mid-tile, so the step it wants lands inside
    // the tile it is already in -- `lands` says yes, and the tile it is heading towards is crater.
    // If the detour were cancelled by that weak test, the unit would step back into the rim every
    // tick and pace forever, which is exactly what it did.
    DrawnGround ground = open_field();
    ground.rows[2][3] = '#'; // east: the tile it is heading towards
    ground.rows[3][3] = '#'; // and the diagonal
    StepRequest request;
    request.x = Fixed::from_raw(static_cast<std::int32_t>(2 * Fixed::kOne + Fixed::kOne / 2));
    request.y = request.x;
    request.want_dx = kSpeed / 4;
    request.want_dy = kSpeed / 4;
    request.speed = kSpeed / 4;
    request.detour = Detour{0, -(kSpeed / 4), 5};

    const auto plan = plan_step(request, ground);
    EXPECT_TRUE(plan.moved);
    EXPECT_LE(plan.step.dy, 0) << "it must keep going around, not step back at the rim";
    EXPECT_NE(plan.step.dx, request.want_dx) << "the straight step was taken, so the detour died";
    EXPECT_EQ(plan.detour.ticks_left, 4);
}

TEST(Steering, TheDetourIsDroppedTheMomentTheStraightRouteIsClear) {
    StepRequest request = request_at(2, 2, kSpeed, kSpeed);
    request.detour = Detour{-kSpeed, 0, 5};
    const auto plan = plan_step(request, open_field());
    EXPECT_TRUE(plan.moved);
    EXPECT_FALSE(plan.detour.active())
        << "otherwise a unit would keep walking around a rim it has already passed";
}

TEST(Steering, AUnitWithNoWayOutNeverLeavesItsTile) {
    // Not "cannot move": a sub-tile step can shuffle inside the tile it is already in, which is
    // legitimate. What it must never do is enter a crater, so the assertion is about the tile.
    Fixed x = at(3);
    Fixed y = at(3);
    Detour detour{};
    for (int tick = 0; tick < 40; ++tick) {
        StepRequest request;
        request.x = x;
        request.y = y;
        request.want_dx = kSpeed;
        request.want_dy = kSpeed;
        request.speed = kSpeed;
        request.detour = detour;
        const StepPlan plan = plan_step(request, walled_in());
        detour = plan.detour;
        if (plan.moved) {
            x = Fixed::from_raw(static_cast<std::int32_t>(x.raw + plan.step.dx));
            y = Fixed::from_raw(static_cast<std::int32_t>(y.raw + plan.step.dy));
        }
    }
    EXPECT_EQ(x.floor_int(), 3) << "it walked into a crater";
    EXPECT_EQ(y.floor_int(), 3) << "it walked into a crater";
}

TEST(Steering, AUnitBoxedInOnEverySideReportsThatItCannotMove) {
    // With nothing enterable in the step's reach at all, `moved` is false and the caller stops it.
    struct AllCrater {
        [[nodiscard]] std::int64_t factor(int, int) const noexcept { return 0; }
    };
    const auto plan = plan_step(request_at(3, 3, kSpeed, kSpeed), AllCrater{});
    EXPECT_FALSE(plan.moved);
    EXPECT_EQ(plan.step.dx, 0);
    EXPECT_EQ(plan.step.dy, 0);
}

TEST(Steering, TerrainHasItsSayOnHowFarTheStepGoes) {
    // A mud tile halves the step: the rule has always been "the RAW step must reach enterable
    // ground, and the SCALED step must land on enterable ground too".
    struct HalfSpeed {
        [[nodiscard]] std::int64_t factor(int, int) const noexcept { return Fixed::kOne / 2; }
    };
    const auto plan = plan_step(request_at(2, 2, kSpeed, 0), HalfSpeed{});
    EXPECT_TRUE(plan.moved);
    EXPECT_EQ(plan.step.dx, kSpeed / 2) << "the terrain gets its say on how far";
}

} // namespace
} // namespace protekt::sim
