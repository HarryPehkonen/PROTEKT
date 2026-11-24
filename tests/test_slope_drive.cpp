#include "protekt/app/map_file.hpp"
#include "protekt/game/player.hpp"
#include "protekt/sim/commander.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/steering.hpp"
#include "protekt/world/ground.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

/// THE TANK THAT REVERSES (G4). His report, from play, in a tank on the Proving Ground: "I was in a
/// tank, going ... straight ahead, but close to the edge of a hill, parallel to it when I started
/// moving backwards. Same happened when I was going down a slope, close to the side of a
/// 'mountain'." And, at the same time: "the mouse-steered vehicle has trouble moving sideways with
/// a and d keys. It jitters."
///
/// Two causes, neither of them the slide's numbers:
///   * THE REVERSAL: the driven tank's lookahead MoveTo went through the route planner, which
///     keeps a route off ground the slope throws a unit off. Beside a rim the planner's first
///     waypoint is the centre of a tile BEHIND the tank, so the tank was steered backwards; on the
///     rim itself the slide (capped at one tank step) exactly cancelled the sideways half of that
///     steering and left only the backwards half. The helm is steered by the player, not the
///     planner, now.
///   * THE JITTER: the hover tank's Strafe is a one-tick impulse, and it was sent on every FRAME.
///     The game steps the sim at 30 Hz off the wall clock, so at 60 frames a second two strafes
///     landed in one tick and none in the next: 2, 0, 2, 0 -- or 1, 3, 2 when the frames are
///     uneven. One strafe per tick now.
///
/// Every test here drives through the real `PlayerCommander`, so what is asserted is what the
/// player's keys do, measured per tick on the displacement itself.
namespace protekt::sim {
namespace {

[[nodiscard]] app::MapDoc proving_ground() {
    const app::MapParse parsed
        = app::load_map_file(std::string{PROTEKT_SOURCE_DIR} + "/assets/maps/proving_ground.map");
    EXPECT_TRUE(parsed.ok) << parsed.error;
    return parsed.map;
}

[[nodiscard]] app::MapDoc open_field() {
    app::MapDoc map = proving_ground();
    for (std::size_t i = 0; i < map.terrain.size(); ++i) {
        map.terrain[i] = world::Terrain::Flat;
        map.heights[i] = 0;
        map.solid[i] = 0;
    }
    map.volumes.clear();
    return map;
}

constexpr float kNorth = 3.14159265F; ///< yaw 0 is +y, which is south (row 0 is the north edge)
constexpr float kWest = -1.5707963F;

/// One tick of what the player's tank did.
struct TickRecord {
    double ahead{}; ///< raw displacement along the hull's heading (the command)
    double right{}; ///< raw displacement along the hull's right (the strafe direction)
    std::int64_t dx{};
    std::int64_t dy{};
    std::int64_t drift_dx{}; ///< the press the tick began with (`press_at`)
    std::int64_t drift_dy{};
    std::int32_t vx{}; ///< the mover's own velocity, before the slide was added
    std::int32_t vy{};
    double x{}; ///< where the tick began, tiles
    double y{};
};

/// A driven unit, a far-off enemy factory, and the player at the helm.
struct Helm {
    Sim sim;
    UnitId id{};
    game::PlayerCommander player;

    /// The unit spawns on the corner of tile (`tx`, `ty`) and stands, unordered, for `settle`
    /// ticks first (the slope carries it to wherever in the tile that puts it).
    Helm(UnitKind kind, int tx, int ty, float heading, float throttle, float strafe, int settle = 0,
         std::vector<PlacedVolume> volumes = {})
        : sim(make(kind, tx, ty, std::move(volumes))) {
        for (const Unit& unit : sim.units()) {
            if (unit.side == Side::Player) {
                id = unit.id;
            }
        }
        for (int s = 0; s < settle; ++s) {
            sim.step();
        }
        sim.enqueue(Order{id, orders::TakeControl{}});
        player.set_heading(heading);
        game::InputState input{};
        input.throttle = throttle;
        input.strafe = strafe;
        player.set_input(input);
    }

    /// `volumes` are the map's terrain volumes, as the game hands them to the sim (`--mapfile`);
    /// none by default, which is every test written before G5.
    static Sim make(UnitKind kind, int tx, int ty, std::vector<PlacedVolume> volumes = {}) {
        Scenario scenario{};
        scenario.seed = 0;
        scenario.volumes = std::move(volumes);
        Spawn unit{};
        unit.side = Side::Player;
        unit.kind = kind;
        unit.tile_x = tx;
        unit.tile_y = ty;
        scenario.spawns.push_back(unit);
        Spawn base{};
        base.side = Side::Enemy;
        base.kind = UnitKind::Factory;
        base.tile_x = 56;
        base.tile_y = 56;
        scenario.spawns.push_back(base);
        return Sim::create(scenario);
    }

    /// One FRAME of the game loop as `app/game.cpp` runs it: the player thinks and the orders are
    /// enqueued, then the sim steps `steps` times (0, 1 or 2, by the wall clock). One record per
    /// tick stepped.
    void frame(int steps, std::vector<TickRecord>& out) {
        std::vector<Order> orders;
        player.think(id, observe(sim, Side::Player), orders);
        for (const Order& order : orders) {
            sim.enqueue(order);
        }
        for (int s = 0; s < steps; ++s) {
            const Unit before = *sim.find(id);
            const Step drift = press_at(0, before.x, before.y);
            sim.step();
            const Unit& after = *sim.find(id);
            TickRecord r;
            r.dx = static_cast<std::int64_t>(after.x.raw) - before.x.raw;
            r.dy = static_cast<std::int64_t>(after.y.raw) - before.y.raw;
            const double fx = std::sin(player.heading());
            const double fy = std::cos(player.heading());
            r.ahead = static_cast<double>(r.dx) * fx + static_cast<double>(r.dy) * fy;
            // The hull's right as the commander sends it: (cos h, -sin h).
            r.right = static_cast<double>(r.dx) * fy - static_cast<double>(r.dy) * fx;
            r.drift_dx = drift.dx;
            r.drift_dy = drift.dy;
            r.vx = after.vx.raw;
            r.vy = after.vy.raw;
            r.x = before.x.raw / 65536.0;
            r.y = before.y.raw / 65536.0;
            out.push_back(r);
        }
    }

    /// `ticks` ticks at one frame per tick: the pacing every scripted capture uses.
    std::vector<TickRecord> drive(int ticks) {
        std::vector<TickRecord> out;
        for (int t = 0; t < ticks; ++t) {
            frame(1, out);
        }
        return out;
    }
};

/// The tick-by-tick numbers, for a failure message: the arithmetic, not an impression.
[[nodiscard]] std::string table(const std::vector<TickRecord>& ticks) {
    std::ostringstream s;
    for (std::size_t t = 0; t < ticks.size(); ++t) {
        const TickRecord& r = ticks[t];
        s << "t" << t << " at (" << r.x << ", " << r.y << ") v(" << r.vx << ", " << r.vy
          << ") drift(" << r.drift_dx << ", " << r.drift_dy << ") d(" << r.dx << ", " << r.dy
          << ") ahead " << r.ahead << " right " << r.right << "\n";
    }
    return s.str();
}

[[nodiscard]] int backwards_ticks(const std::vector<TickRecord>& ticks) {
    int n = 0;
    for (const TickRecord& r : ticks) {
        n += r.ahead < 0 ? 1 : 0;
    }
    return n;
}

// ---- his first situation: straight ahead, along the edge of a hill ------------------------

TEST(TankThatReverses, DrivingAlongTheRimIsNeverDisplacedBackwards) {
    // The plateau (height 120, x 22..29) stands over the valley (-50, x 30..37): column 29 is the
    // rim, a drop of 170 to the east, and its slide is the capped 13107 raw east. He drives NORTH
    // along it. Before the fix, 37 of these 40 ticks moved him SOUTH: the planner aimed him at a
    // tile centre to the south-west (v = -12279, +4583 on the first such tick), the slide's
    // +13107 cancelled the westward half, and +4583 south was what was left.
    const world::ScopedGround guard(app::map_ground(proving_ground()));
    for (const int y : {26, 22, 18}) {
        for (const int settle : {0, 1, 2}) {
            Helm helm(UnitKind::Tank, 29, y, kNorth, 1.0F, 0.0F, settle);
            const std::vector<TickRecord> ticks = helm.drive(40);
            EXPECT_EQ(backwards_ticks(ticks), 0)
                << "from (29, " << y << ") after " << settle << " idle ticks:\n"
                << table(ticks);
        }
    }
}

TEST(TankThatReverses, TheSlopeStillDragsADrivenTankOffTheRim) {
    // His ruling survives at the helm: "you drift downhill, and you keep drifting even while you
    // try to steer -- the slope wins until you're off it". Driving north along the rim, the tank
    // goes north AND is carried east, off the rim into the valley; nothing about steering cancels
    // the slide.
    const world::ScopedGround guard(app::map_ground(proving_ground()));
    Helm helm(UnitKind::Tank, 29, 26, kNorth, 1.0F, 0.0F);
    const std::vector<TickRecord> ticks = helm.drive(40);
    const Unit& end = *helm.sim.find(helm.id);
    EXPECT_GE(end.x.floor_int(), 30) << "dragged off the rim into the valley\n" << table(ticks);
    EXPECT_LT(end.y.raw, 24 * Fixed::kOne) << "and still went the way he drove\n" << table(ticks);
    bool dragged = false;
    for (const TickRecord& r : ticks) {
        if (r.drift_dx > 0) {
            dragged = true; // the slide was there, and the drive went on regardless
        }
    }
    EXPECT_TRUE(dragged);
}

// ---- his second situation: down a slope, beside a mountain --------------------------------

TEST(TankThatReverses, DrivingDownTheRampBesideTheCliffStillDescends) {
    // The west ramp falls from the plateau (120) through 110, 100 ... 20 to the plain, x 21 down to
    // 16, rows 28..32; its north row, 28, has the open plain (height 0) beside it, so a tank on it
    // is beside a drop the whole way down. He drives WEST, down it, from the plateau's edge. Before
    // the fix six ticks went EAST: the planner sent him to the centre of tile (23, 29), half a tile
    // behind him, v = +4144, +12434.
    const world::ScopedGround guard(app::map_ground(proving_ground()));
    for (const int y : {28, 29, 30, 32}) {
        Helm helm(UnitKind::Tank, 23, y, kWest, 1.0F, 0.0F);
        const std::vector<TickRecord> ticks = helm.drive(60);
        EXPECT_EQ(backwards_ticks(ticks), 0) << "starting at y " << y << ":\n" << table(ticks);
        const Unit& end = *helm.sim.find(helm.id);
        EXPECT_LE(end.x.floor_int(), 15) << "it came down off the ramp, from y " << y;
    }
}

// ---- driving uphill against the press: HIS ruling, pinned at the steepest grade ------------

/// A uniform slope of `grade` per tile, falling to the east, on a drawn map.
struct UniformSlope {
    int grade{};
    [[nodiscard]] std::int64_t factor(int x, int y) const noexcept {
        return x >= 0 && y >= 0 && x < 16 && y < 16 ? Fixed::kOne : 0;
    }
    [[nodiscard]] int height(int x, int y) const noexcept {
        (void)y;
        return 1000 - grade * x;
    }
};

TEST(TankThatReverses, UphillAgainstThePressIsSlowedUpToAGradeOf38AndReversedFrom39) {
    // NOT a fix: this pins the ruling the slope slice made ("the slope wins" -- uphill too steep
    // beats you), with G6's one force. A tank driving WEST, up a slope of `grade` per tile, drives
    // 13107 west and is pressed back 13107 * sin(slope) / sin(60 percent grade). The two cross
    // between 38 and 39: 38 -> pressed 13005, net +102; 39 -> pressed 13255, net -148 (reversed).
    // That crossing IS the wall (`press_beats`; on an axis face it comes to 38), so a tank never
    // drives onto such a slope -- but one carried onto it, or standing on it, is beaten back: the
    // step here stays inside the unit's own tile. Slide.SteeringUphillDoesNotCancelIt pins 40.
    for (int grade = 33; grade <= 40; ++grade) {
        const UniformSlope ground{grade};
        StepRequest request;
        request.x = Fixed::from_raw(5 * Fixed::kOne + Fixed::kOne / 2);
        request.y = Fixed::from_raw(5 * Fixed::kOne + Fixed::kOne / 2);
        request.want_dx = -kTankSpeedRaw;
        request.speed = kTankSpeedRaw;
        const StepPlan plan = plan_step(request, ground);
        ASSERT_TRUE(plan.moved) << "grade " << grade;
        const Step drift
            = plan_press(ground, request.x, request.y, kTankSpeedRaw, -kTankSpeedRaw, 0);
        const std::int64_t net = -(plan.step.dx + drift.dx); // westward, uphill, is positive
        if (grade <= 38) {
            EXPECT_GT(net, 0) << "grade " << grade << " should slow the climb, not reverse it";
            EXPECT_LT(net, kTankSpeedRaw) << "grade " << grade;
        } else {
            EXPECT_LT(net, 0) << "grade " << grade << ": the slope wins (the ruling)";
        }
    }
}

// ---- the third symptom: the strafe jitters -----------------------------------------------

/// The right-hand component of every tick, once the strafe has eased up to full (a second).
[[nodiscard]] std::vector<double> settled_right(const std::vector<TickRecord>& ticks) {
    std::vector<double> out;
    for (std::size_t t = 40; t < ticks.size(); ++t) {
        out.push_back(ticks[t].right);
    }
    return out;
}

TEST(StrafeJitter, StrafingOnOpenGroundIsSmoothAtSixtyFramesASecond) {
    // The game steps the sim at 30 Hz off the wall clock, and at 60 frames a second that is a
    // tick every other frame. A full strafe is 3/4 of a hover tank's step, every tick, the same.
    // Before the fix each frame sent its own Strafe and the sim applied each: two per tick, and
    // the tick that came due with no frame before it got none.
    const world::ScopedGround guard(app::map_ground(open_field()));
    Helm helm(UnitKind::HoverTank, 20, 20, kNorth, 0.0F, 1.0F);
    std::vector<TickRecord> ticks;
    for (int f = 0; f < 160; ++f) {
        helm.frame(f % 2, ticks);
    }
    const std::vector<double> right = settled_right(ticks);
    ASSERT_FALSE(right.empty());
    const double full = static_cast<double>(profile(UnitKind::HoverTank).max_speed.raw)
                        * kStrafeSpeedNumerator / kStrafeSpeedDenominator;
    for (std::size_t t = 0; t < right.size(); ++t) {
        EXPECT_NEAR(std::abs(right[t]), full, 2.0) << "tick " << (t + 40) << "\n" << table(ticks);
    }
}

TEST(StrafeJitter, AnUnevenFrameRateDoesNotMakeTheStrafeUneven) {
    // Frames of 1, 3 and 2 ticks' worth of wall clock, the way a real frame rate wanders: the
    // per-tick strafe is still one strafe. (Two ticks in ONE frame are both stepped after the
    // frame's single think, so this pattern never steps more than one tick per frame.)
    const world::ScopedGround guard(app::map_ground(open_field()));
    Helm helm(UnitKind::HoverTank, 20, 20, kNorth, 0.0F, 1.0F);
    std::vector<TickRecord> ticks;
    const std::vector<int> pattern{0, 1, 0, 0, 1, 1, 0, 1, 0, 0, 0, 1};
    for (int f = 0; f < 240; ++f) {
        helm.frame(pattern[static_cast<std::size_t>(f) % pattern.size()], ticks);
    }
    const std::vector<double> right = settled_right(ticks);
    ASSERT_GT(right.size(), 20U);
    const double full = static_cast<double>(profile(UnitKind::HoverTank).max_speed.raw)
                        * kStrafeSpeedNumerator / kStrafeSpeedDenominator;
    for (std::size_t t = 1; t < right.size(); ++t) {
        EXPECT_NEAR(right[t], right[t - 1], 2.0) << "tick " << (t + 40) << "\n" << table(ticks);
        // ...and it is the strafe, not its absence: an even sequence of zeros would pass above.
        EXPECT_NEAR(std::abs(right[t]), full, 2.0) << "tick " << (t + 40);
    }
}

TEST(StrafeJitter, StrafingTowardsTheRimDoesNotOscillate) {
    // Near a slope: a hover tank on the plateau facing north, strafing towards the valley rim and
    // over it. Whatever the slide adds, the sideways displacement does not change sign from one
    // tick to the next: the sequence is asserted, sign by sign.
    const world::ScopedGround guard(app::map_ground(proving_ground()));
    // Facing north the commander's right is (cos h, -sin h) = (-1, 0), west; so face SOUTH to
    // strafe east, towards the rim at column 29.
    Helm helm(UnitKind::HoverTank, 26, 20, 0.0F, 0.0F, 1.0F);
    std::vector<TickRecord> ticks;
    for (int f = 0; f < 120; ++f) {
        helm.frame(f % 2, ticks);
    }
    int flips = 0;
    for (std::size_t t = 1; t < ticks.size(); ++t) {
        const double a = static_cast<double>(ticks[t - 1].dx);
        const double b = static_cast<double>(ticks[t].dx);
        flips += (a > 0 && b < 0) || (a < 0 && b > 0) ? 1 : 0;
    }
    EXPECT_EQ(flips, 0) << table(ticks);
    EXPECT_GE(helm.sim.find(helm.id)->x.floor_int(), 30) << "it strafed over the rim";
}

TEST(StrafeJitter, StrafingAlongTheRimWhileDrivingDoesNotOscillate) {
    // W and D together, beside the rim: the drive is north along column 28, the strafe is east,
    // towards the drop. Before the fix the planner's route and the strafe fought here.
    const world::ScopedGround guard(app::map_ground(proving_ground()));
    Helm helm(UnitKind::HoverTank, 27, 26, kNorth, 1.0F, -1.0F);
    std::vector<TickRecord> ticks;
    for (int f = 0; f < 80; ++f) {
        helm.frame(f % 2, ticks);
    }
    int flips = 0;
    for (std::size_t t = 1; t < ticks.size(); ++t) {
        const double a = static_cast<double>(ticks[t - 1].dx);
        const double b = static_cast<double>(ticks[t].dx);
        flips += (a > 0 && b < 0) || (a < 0 && b > 0) ? 1 : 0;
    }
    EXPECT_EQ(flips, 0) << table(ticks);
    EXPECT_EQ(backwards_ticks(ticks), 0) << table(ticks);
}

// ---- G5: the top of the steep hill, and the straight passage --------------------------------
//
// Harri, driving the Proving Ground: "I can now climb up a VERY steep hill, but not get over the
// very top of it. And there is also something blocking me in a straight passage. ... What's
// stopping me from going over the top of the steep hill is the 'r' in the map. NO! I just proved
// that it's not. It's the slope."

constexpr float kEast = 1.5707963F;
constexpr float kSouth = 0.0F;

TEST(SteepHillTop, HisHillTheRampSideBesideTheRoughStripIsACliffNow) {
    // HIS HILL, found by driving every tile of the map in all eight directions: the west ramp's
    // flank at column 17 rises 40 in ONE edge (0 to 40), beside the rough strip at (18..21, 27);
    // the same 40 stands at the north ramp's flanks. Under the old limit of 40 a tank crawled up it
    // at the 0.375 grade to within 0.05 tile of the top and stayed there for ever: on the tick it
    // crossed the edge it was slid back by the tile it had just entered (drop 40 behind it, 6552
    // raw, against a climb of 4915). "I feel like I can climb too high on the slope": he could. A
    // rise of 40 is past what a tank's thrust beats the slide on, so it is a CLIFF now
    // (G6: the face is a wall because the press on it beats the engine, `press_beats`): refused
    // at the foot and never entered, and drawn as a cliff by `protekt-map draw`.
    const world::ScopedGround guard(app::map_ground(proving_ground()));
    struct Climb {
        int x;
        int y;
        float heading;
        int dx;
        int dy;
    };
    for (const Climb c : {Climb{17, 27, kSouth, 0, 1}, Climb{17, 33, kNorth, 0, -1},
                          Climb{37, 9, kEast, 1, 0}, Climb{46, 9, kWest, -1, 0}}) {
        Helm helm(UnitKind::Tank, c.x, c.y, c.heading, 1.0F, 0.0F);
        const std::vector<TickRecord> ticks = helm.drive(60);
        // It may go round (the mover's escape takes it to the 20-high flank beside, which it
        // climbs): what it may never do is stand in the tile across the 40.
        for (const TickRecord& r : ticks) {
            EXPECT_FALSE(static_cast<int>(r.x) == c.x + c.dx && static_cast<int>(r.y) == c.y + c.dy)
                << "from (" << c.x << ", " << c.y << ") it went up a 40\n"
                << table(ticks);
        }
    }
}

/// The open field with a STEP of `rise` at column 12 (west of it 0, from it on `rise`), or, when
/// `ramp` is true, a slope of `rise` a tile for four tiles (columns 12..15) and level from there.
[[nodiscard]] app::MapDoc stepped_field(int rise, bool ramp) {
    app::MapDoc map = open_field();
    for (int y = 0; y < map.height; ++y) {
        for (int x = 0; x < map.width; ++x) {
            const int tiles = ramp ? std::clamp(x - 11, 0, 4) : (x >= 12 ? 1 : 0);
            map.heights[map.index(x, y)] = rise * tiles;
        }
    }
    return map;
}

TEST(SteepHillTop, EveryClimbableStepHasACrossableTop) {
    // HIS TABLE: "leaving the top of a slope -- the ground holds or it does not -- but a climbable
    // slope has a crossable top". A single edge of every climbable rise, up to the limit of 38,
    // onto level ground: a unit driving east gets over it and on -- a tank, and a Walker, which
    // has HALF a tank's step. Before G5 the slide was read from where the unit's move ENDED, so on
    // the tick it crossed the brow it climbed at the edge's grade and was then slid back down that
    // same edge by the tile above: a Walker on a step of 36 climbed 2867 raw and was slid back
    // 3276, at the brow, for ever. The slide is read from where the tick began now, as the grade
    // is, and on the top the Walker's level drive (6553) beats it.
    // G6: the step's face is tile 11 (a tile's height is its corner's), and on it the press is
    // felt where the face IS: at 38 a tank nets 102 raw a tick up it and a Walker 51, so the face
    // takes them some 640 and 1290 ticks. Slow at the very limit, by the physics; 1500 ticks is
    // room for every rise. (It was 240 when the climb was the grade's 0.41 of the step.)
    for (const UnitKind kind : {UnitKind::Tank, UnitKind::Walker}) {
        for (const int rise : {20, 33, 34, 35, 36, 37, 38}) {
            const world::ScopedGround guard(app::map_ground(stepped_field(rise, false)));
            Helm helm(kind, 8, 20, kEast, 1.0F, 0.0F);
            const std::vector<TickRecord> ticks = helm.drive(1500);
            EXPECT_GE(helm.sim.find(helm.id)->x.floor_int(), 15)
                << (kind == UnitKind::Tank ? "tank" : "walker") << ": a step of " << rise
                << " held it at its top\n"
                << table(ticks);
        }
    }
    // ...and one past the limit is never climbed: on the face the press beats the engine (it is
    // past the steepest grade) and at its top the wall refuses it -- it never stands on the top.
    for (const int rise : {39, 40}) {
        const world::ScopedGround guard(app::map_ground(stepped_field(rise, false)));
        Helm helm(UnitKind::Tank, 8, 20, kEast, 1.0F, 0.0F);
        const std::vector<TickRecord> ticks = helm.drive(240);
        for (const TickRecord& r : ticks) {
            EXPECT_LT(r.x, 12.0) << "rise " << rise << ": it got up\n" << table(ticks);
        }
        EXPECT_LT(helm.sim.find(helm.id)->x.raw, 12 * Fixed::kOne) << "rise " << rise;
    }
}

TEST(SteepHillTop, AClimbableRampHasACrossableTop) {
    // A RAMP, four tiles of `rise` each: up to the limit a tank's climb beats the press on every
    // tile (that is what the limit IS), so it reaches the level top and crosses it. (The physics
    // under the limit, 38/39, is pinned by
    // TankThatReverses.UphillAgainstThePressIsSlowedUpToAGradeOf38AndReversedFrom39.)
    for (const int rise : {20, 33, 36, 38}) {
        const world::ScopedGround guard(app::map_ground(stepped_field(rise, true)));
        Helm helm(UnitKind::Tank, 8, 20, kEast, 1.0F, 0.0F);
        // At 38 the net climb is 102 raw a tick (0.0016 tile): each tile of the ramp takes some
        // 640 ticks, the four about 2570. 2800 is room for every rise. (It was 900 when the net
        // was the two old mechanisms' 410.)
        const std::vector<TickRecord> ticks = helm.drive(2800);
        EXPECT_GE(helm.sim.find(helm.id)->x.floor_int(), 18)
            << "a ramp of " << rise << " a tile was not crossed\n"
            << table(ticks);
    }
}

// ---- G6: THE PRESS, at the helm ------------------------------------------------------------
//
// "I think the press back from a hill should make mathematical sense." The slope is ONE force read
// from the ground UNDER the unit: the drawn ground puts a tile's height at its corner, so the
// ground under a unit in tile (x, y) is the face between the four corners (x..x+1, y..y+1).

TEST(PressAtTheHelm, LevelGroundBesideACliffPressesNothing) {
    // HIS REPORT: "moved in the VICINITY of a hill". `stepped_field(60)` is a plateau at 60 from
    // column 12 east, over a plain at 0; the face is the ground between corners 11 and 12. A tank
    // in tile 12 stands on LEVEL ground -- all four corners at 60 -- beside that drop. Driven north
    // along the brink it goes straight north, not one raw unit sideways; parked, it does not
    // move at all.
    const world::ScopedGround guard(app::map_ground(stepped_field(60, false)));
    Helm driven(UnitKind::Tank, 12, 30, kNorth, 1.0F, 0.0F);
    const std::vector<TickRecord> ticks = driven.drive(40);
    for (const TickRecord& r : ticks) {
        EXPECT_EQ(r.dx, 0) << "pressed sideways on level ground\n" << table(ticks);
    }
    EXPECT_LT(driven.sim.find(driven.id)->y.raw, 26 * Fixed::kOne) << table(ticks);

    Helm parked(UnitKind::Tank, 12, 30, kNorth, 0.0F, 0.0F);
    const Unit start = *parked.sim.find(parked.id);
    const std::vector<TickRecord> still = parked.drive(40);
    EXPECT_EQ(parked.sim.find(parked.id)->x.raw, start.x.raw) << table(still);
    EXPECT_EQ(parked.sim.find(parked.id)->y.raw, start.y.raw) << table(still);
}

/// The open field with a RIDGE across it: from column 12 the ground rises `rise` a tile for four
/// tiles to a crest at column 16, and falls the same way to the plain at column 20.
[[nodiscard]] app::MapDoc ridge_field(int rise) {
    app::MapDoc map = open_field();
    for (int y = 0; y < map.height; ++y) {
        for (int x = 0; x < map.width; ++x) {
            map.heights[map.index(x, y)] = rise * std::max(0, 4 - std::abs(x - 16));
        }
    }
    return map;
}

TEST(PressAtTheHelm, AClimbableRidgeIsCrossedAndTheFarSideIsFaster) {
    // HIS TABLE: "a climbable top is crossable". A tank driven east over a ridge of every
    // climbable rise reaches the plain beyond it, and comes down the far side faster than it went
    // up: the press that held it back on the way up is the press that carries it on the way down.
    for (const int rise : {10, 20, 30, 36}) {
        const world::ScopedGround guard(app::map_ground(ridge_field(rise)));
        Helm helm(UnitKind::Tank, 8, 30, kEast, 1.0F, 0.0F);
        const std::vector<TickRecord> ticks = helm.drive(900);
        EXPECT_GE(helm.sim.find(helm.id)->x.floor_int(), 21)
            << "a ridge of " << rise << " a tile was not crossed\n"
            << table(ticks);
        double up = 0;
        double down = 0;
        for (const TickRecord& r : ticks) {
            if (r.x >= 12.5 && r.x < 15.5) {
                up = std::max(up, r.ahead);
            }
            if (r.x >= 16.5 && r.x < 19.5) {
                down = std::max(down, r.ahead);
            }
        }
        EXPECT_GT(down, up) << "rise " << rise << ": the far side was no faster";
    }
}

TEST(PressAtTheHelm, AnEasingStrafeIntoAClimbableSlopeIsNeverCarriedBackwards) {
    // G6, found re-measuring the hover tests: a held strafe EASES in over a second, so on its first
    // ticks a hover strafing uphill commands a few hundred raw a tick -- and the press's component
    // along its heading, felt in full whatever the throttle, carried it BACKWARDS down a slope its
    // engine climbs at full throttle. "The drift ... must never negate your intent": a command is
    // a SPEED, and the engine has its whole thrust to hold it (`driven_press`), so a gentle drive
    // up a slope the engine can climb keeps its gentle speed. (A tank at the helm never meets
    // this: its throttle sets how far ahead it aims, not how fast it drives.) On a slope rising 20
    // a tile to the WEST, a hover facing south strafes with A, to its left -- west, uphill: not
    // one tick of the ramp moves it east, and it climbs.
    app::MapDoc map = open_field();
    for (int y = 0; y < map.height; ++y) {
        for (int x = 0; x < map.width; ++x) {
            map.heights[map.index(x, y)] = std::clamp(20 * (30 - x), -500, 500);
        }
    }
    const world::ScopedGround guard(app::map_ground(map));
    // Facing south (yaw 0, +y) the hull's right is (cos 0, -sin 0) = +x, east: A is west.
    Helm helm(UnitKind::HoverTank, 24, 30, kSouth, 0.0F, -1.0F);
    const Unit start = *helm.sim.find(helm.id);
    std::vector<TickRecord> ticks;
    for (int t = 0; t < 90; ++t) {
        helm.frame(1, ticks);
    }
    for (std::size_t t = 0; t < ticks.size(); ++t) {
        EXPECT_LE(ticks[t].dx, 0) << "tick " << t << " carried it east, down the slope\n"
                                  << table(ticks);
    }
    EXPECT_LT(helm.sim.find(helm.id)->x.raw, start.x.raw - Fixed::kOne) << table(ticks);
}

TEST(StraightPassage, WhatTurnsHimAsideInTheValleyIsTheKerbAndTheRockfallNotTheSlope) {
    // HIS STRAIGHT PASSAGE is the valley (x 30..37, y 14..49), the map's one long straight run.
    // The three candidates, each put to the test:
    //   (a) the 39/40 band, driveable but not plannable: it cannot stop him. The tank at the helm
    //       is steered straight by its heading and plans no route (G4), and the valley floor has
    //       no edge of 39 or 40 to begin with.
    //   (b) an impassable tile with nothing drawn on it: THIS. The map's "kerb" (a terrain volume
    //       0..40 high across x 30..35 of row 24) and "rockfall" (the solid mask at x 30..32,
    //       y 32..33) stop a tank, and the map drawing shows both hatched in red -- but the GAME
    //       draws neither: nothing in src/game or src/render reads `Scenario::volumes` or
    //       `world::ground_solid`. In the game they are ground like any other.
    //   (c) a building mask wider than its building: there is no building in the valley.
    // Driving south down column 33: with the volumes the game hands the sim, the tank is turned off
    // its column at the kerb and runs on in column 36; without them it stays in column 33.
    const app::MapDoc map = proving_ground();
    const world::ScopedGround guard(app::map_ground(map));
    const auto at = [&](int x, int y) { return map.heights[map.index(x, y)]; };
    for (int y = 14; y < 50; ++y) {
        for (int x = 30; x < 37; ++x) {
            EXPECT_LT(std::abs(at(x + 1, y) - at(x, y)), 39) << "(a): an edge of 39/40 at " << x;
            EXPECT_LT(std::abs(at(x, y + 1) - at(x, y)), 39) << "(a): an edge of 39/40 at " << y;
        }
    }
    Helm plain(UnitKind::Tank, 33, 16, kSouth, 1.0F, 0.0F);
    const std::vector<TickRecord> clear = plain.drive(120);
    EXPECT_EQ(plain.sim.find(plain.id)->x.floor_int(), 33) << table(clear);
    EXPECT_GE(plain.sim.find(plain.id)->y.floor_int(), 30) << table(clear);
    EXPECT_EQ(plain.sim.find(plain.id)->route.count, 0) << "(a): the helm plans nothing";

    Helm kerbed(UnitKind::Tank, 33, 16, kSouth, 1.0F, 0.0F, 0, app::placed_volumes(map));
    const std::vector<TickRecord> turned = kerbed.drive(120);
    EXPECT_EQ(kerbed.sim.find(kerbed.id)->x.floor_int(), 36) << "(b): the kerb\n" << table(turned);
    EXPECT_GE(kerbed.sim.find(kerbed.id)->y.floor_int(), 30) << table(turned);

    // The rockfall: column 31 is turned aside at row 32 with no volume at all -- the solid mask.
    Helm rocks(UnitKind::Tank, 31, 26, kSouth, 1.0F, 0.0F);
    const std::vector<TickRecord> walled = rocks.drive(90);
    EXPECT_EQ(rocks.sim.find(rocks.id)->x.floor_int(), 33) << "(b): the rockfall\n"
                                                           << table(walled);
}

} // namespace
} // namespace protekt::sim
