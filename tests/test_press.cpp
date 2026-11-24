#include "protekt/sim/sim.hpp"
#include "protekt/sim/steering.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

/// THE PRESS, MEASURED (G6). "I think the press back from a hill should make mathematical sense."
///
/// The slope is ONE force: opposite the gradient of the ground under the unit, growing with its
/// steepness, added to what the unit is commanded. These are MEASUREMENTS of the whole tick's
/// motion -- the mover's step plus the slope's say -- on drawn planes, at a range of rises and a
/// range of headings, so the shape of the law is asserted and not one hand-picked point on it.
/// The tables print, so a failure shows the arithmetic and not an impression.
namespace protekt::sim {
namespace {

constexpr std::int64_t kStep = kTankSpeedRaw; ///< a tank's step on level ground: 0.2 tiles

/// A tilted plane: height = `rise_x` * x + `rise_y` * y, in height units, all of it open ground.
/// Every tile's corners lie on the plane, so the ground under a unit has exactly this gradient
/// wherever it stands, and a step that stays inside the tile meets nothing else.
struct Plane {
    int rise_x{};
    int rise_y{};
    [[nodiscard]] std::int64_t factor(int x, int y) const noexcept {
        return x >= 0 && y >= 0 && x < 64 && y < 64 ? Fixed::kOne : 0;
    }
    [[nodiscard]] int height(int x, int y) const noexcept { return (rise_x * x) + (rise_y * y); }
};

/// ONE TICK of a driven unit on `ground`, as the sim composes it: the mover's step for the order,
/// then the slope's say, read from the ground the tick began on. The unit stands in the middle of
/// tile (8, 8) and asks for the full step (`want_dx`, `want_dy`).
Step tick(const Plane& ground, std::int64_t want_dx, std::int64_t want_dy) {
    StepRequest request;
    request.x = Fixed::from_raw(8 * Fixed::kOne + Fixed::kOne / 2);
    request.y = Fixed::from_raw(8 * Fixed::kOne + Fixed::kOne / 2);
    request.want_dx = want_dx;
    request.want_dy = want_dy;
    request.speed = kStep;
    const StepPlan plan = plan_step(request, ground);
    const Step command = plan.moved ? plan.step : Step{};
    const Step slope = plan_press(ground, request.x, request.y, kStep, command.dx, command.dy);
    return Step{command.dx + slope.dx, command.dy + slope.dy};
}

// ---- 1. monotonic in steepness ------------------------------------------------------------

TEST(PressMeasured, EverySteeperSlopeIsStrictlyWorseUphillAndStrictlyBetterDownhill) {
    // A plane falling EAST by `rise` a tile: driving west is uphill, east downhill. From level to
    // twice the steepest grade, the net displacement per tick, raw. Not one step may be kinder to a
    // steeper slope: that is what a force that grows with the gradient means, and a cap or a
    // floor anywhere in the law shows up here as two equal rows.
    std::ostringstream rows;
    rows << "rise  uphill(net west)  downhill(net east)\n";
    std::int64_t last_up = 0;
    std::int64_t last_down = 0;
    for (int rise = 0; rise <= 80; rise += 2) {
        const Plane ground{-rise, 0};
        const std::int64_t up = -tick(ground, -kStep, 0).dx;
        const std::int64_t down = tick(ground, kStep, 0).dx;
        rows << std::setw(4) << rise << "  " << std::setw(16) << up << "  " << std::setw(18) << down
             << "\n";
        if (rise == 0) {
            EXPECT_EQ(up, kStep) << "level ground is the level step";
            EXPECT_EQ(down, kStep);
        } else {
            EXPECT_LT(up, last_up)
                << "a rise of " << rise << " was no worse uphill than " << rise - 2;
            EXPECT_GT(down, last_down)
                << "a rise of " << rise << " was no better downhill than " << rise - 2;
        }
        last_up = up;
        last_down = down;
    }
    std::cout << rows.str();
}

// ---- 2. the right sign at every angle --------------------------------------------------------

TEST(PressMeasured, AtEveryHeadingTheSlopeNeverInvertsTheDriveUnlessItTrulyOpposesIt) {
    // A plane falling EAST, from gentle (20) to past the steepest grade (45, 60, 100), and the unit
    // driven at 24 headings, every 15 degrees. Each tick's net displacement is split into AHEAD
    // (along the heading) and ACROSS (along the heading's left normal), and both are checked
    // against the law worked out here in floating point, independently of the integer code:
    //     P      = 13107 * sin(atan(rise / 64)) * sqrt(34) / 3     the press, pointing east
    //     ahead  = 13107 + P * cos(heading)                        the tracks roll: all of it
    //     across = max(0, P * |sin(heading)| - 13107), downhill    the tracks grip: past the grip
    // The rule under his table: "the drift may change your direction and your speed, freely and
    // often; it must never negate your intent". So AHEAD goes negative ONLY where the slope
    // opposes the heading by more than the engine (P * cos(heading) < -13107), which on climbable
    // ground is nowhere; and a unit crossing ground steeper than it can climb is dragged to the
    // downhill side -- the veer -- by what the slope has past its grip.
    constexpr double kV = 13107.0;
    for (const int rise : {20, 30, 36, 45, 60, 100}) {
        const Plane ground{-rise, 0};
        const double t = rise / 64.0;
        const double p = kV * (t / std::sqrt(1.0 + (t * t))) * std::sqrt(34.0) / 3.0;
        std::ostringstream rows;
        rows << std::fixed << std::setprecision(0) << "rise " << rise << " (press " << p
             << "): heading  ahead(want)  across(want)\n";
        for (int k = 0; k < 24; ++k) {
            const double a = k * 15.0 * 3.14159265358979 / 180.0;
            const double hx = std::cos(a);
            const double hy = std::sin(a);
            const auto want_dx = static_cast<std::int64_t>(std::lround(hx * kStep));
            const auto want_dy = static_cast<std::int64_t>(std::lround(hy * kStep));
            const Step net = tick(ground, want_dx, want_dy);
            const double ahead
                = (static_cast<double>(net.dx) * hx) + (static_cast<double>(net.dy) * hy);
            const double across
                = (static_cast<double>(net.dx) * -hy) + (static_cast<double>(net.dy) * hx);
            const double want_ahead = kV + (p * hx);
            const double downhill_side = -hy < 0 ? -1.0 : 1.0; // east on the left normal (-hy, hx)
            const double want_across = std::max(0.0, (p * std::abs(hy)) - kV) * downhill_side;
            rows << "  " << std::setw(3) << k * 15 << "  " << std::setw(7) << ahead << "("
                 << std::setw(7) << want_ahead << ")  " << std::setw(7) << across << "("
                 << std::setw(7) << want_across << ")\n";
            EXPECT_NEAR(ahead, want_ahead, 8.0) << "rise " << rise << ", heading " << k * 15;
            EXPECT_NEAR(across, want_across, 8.0) << "rise " << rise << ", heading " << k * 15;
            if (want_ahead > 8.0) {
                EXPECT_GT(ahead, 0.0)
                    << "rise " << rise << ", heading " << k * 15
                    << ": the drive was inverted by a slope that does not beat it";
            }
            if (rise <= steepest_drivable_rise()) {
                EXPECT_GT(ahead, 0.0) << "rise " << rise << ", heading " << k * 15
                                      << ": inverted on climbable ground";
            }
        }
        std::cout << rows.str();
    }
}

TEST(PressMeasured, ADiagonalSlopeHasItsRealSteepness) {
    // His report: every diagonal cliff was climbable. On a plane rising `r` in x AND `r` in y the
    // steepness is r * sqrt(2) a tile, up the gradient, whatever the axes say. Driven straight up
    // that gradient a tank makes headway exactly while that TRUE steepness is under the climb
    // grade (38.4): 26 and 26 is 36.8 (it climbs), 28 and 28 is 39.6 (it is beaten back), though
    // each axis alone reads only 28.
    const auto up_the_diagonal = [](int r) {
        const Plane ground{r, r}; // rises to the south-east
        const std::int64_t along = kStep * 181 / 256;
        const Step net = tick(ground, along, along);
        return net.dx + net.dy; // positive: up the slope
    };
    EXPECT_GT(up_the_diagonal(26), 0) << "a true steepness of 36.8 is climbable";
    EXPECT_LT(up_the_diagonal(28), 0) << "a true steepness of 39.6 beats a tank";
}

} // namespace
} // namespace protekt::sim
