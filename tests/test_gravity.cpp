#include "protekt/game/player.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/commander.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/steering.hpp"
#include "protekt/world/ground.hpp"
#include "protekt/world/world.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

/// GRAVITY AND FRICTION (G7). His report: "If I do go up a hill that's too steep, I'm not even
/// rolling back when I don't hit the thrust. I should be rolling back, I think. Maybe if we think
/// of an incline as acceleration downhill...plus friction."
///
/// Every test here is a MEASUREMENT through the real `Sim` on a drawn ground, and every number it
/// is held to is worked out HERE, in floating point, from the physics -- none is read back from
/// the code under test:
///   g  = 9.81 m/s^2, at 2 m a tile and 30 ticks a second = 9.81 / 2 / 900 tiles a tick^2
///        = 357.2 raw a tick^2 (Q16.16);
///   mu = 0.40, the friction coefficient (M1 Abrams side-slope figure, 40 percent);
///   on ground rising t = tan(slope), a unit free to slide is pushed down it, HORIZONTALLY, by
///        gravity   g sin cos      = g t / (1 + t^2)
///        and held by friction up to mu g cos^2 = g mu / (1 + t^2),
///   so it slides from rest exactly when t > mu (25.6 height units a tile), and then gains
///        a = g (t - mu) / (1 + t^2)   raw a tick, every tick.
namespace protekt::sim {
namespace {

constexpr double kG = 9.81 / 2.0 / (30.0 * 30.0) * 65536.0; ///< raw a tick^2
constexpr double kMu = 0.40;
constexpr int kSide = 64;

/// Height `h(x, y)` for every corner of a 64 x 64 field of open Flat ground, clamped to the
/// world's band.
world::MapGround field(const std::function<int(int, int)>& h) {
    world::MapGround ground;
    ground.width = kSide;
    ground.height = kSide;
    ground.tiles.resize(static_cast<std::size_t>(kSide) * kSide);
    ground.solid.assign(static_cast<std::size_t>(kSide) * kSide, 0);
    for (int y = 0; y < kSide; ++y) {
        for (int x = 0; x < kSide; ++x) {
            world::Tile& tile
                = ground.tiles[(static_cast<std::size_t>(y) * kSide) + static_cast<std::size_t>(x)];
            tile.terrain = world::Terrain::Flat;
            tile.height = static_cast<std::int16_t>(std::clamp(h(x, y), -500, 500));
        }
    }
    return ground;
}

/// A plane through corner (32, 32) at height 0, rising `ax` a tile east and `ay` a tile south.
world::MapGround plane(int ax, int ay) {
    return field([ax, ay](int x, int y) { return (ax * (x - 32)) + (ay * (y - 32)); });
}

/// A tank of the player's on the corner of tile (`tx`, `ty`), an enemy factory far off, and no
/// order: nothing drives it.
Sim parked_tank(int tx, int ty) {
    Scenario scenario{};
    scenario.seed = 0;
    Spawn unit{};
    unit.side = Side::Player;
    unit.kind = UnitKind::Tank;
    unit.tile_x = tx;
    unit.tile_y = ty;
    scenario.spawns.push_back(unit);
    Spawn base{};
    base.side = Side::Enemy;
    base.kind = UnitKind::Factory;
    base.tile_x = 60;
    base.tile_y = 60;
    scenario.spawns.push_back(base);
    return Sim::create(scenario);
}

const Unit& player_of(const Sim& sim) {
    for (const Unit& unit : sim.units()) {
        if (unit.side == Side::Player) {
            return unit;
        }
    }
    return sim.units().front();
}

struct Track {
    std::vector<double> down; ///< displacement down the slope each tick, raw
    std::vector<double> side; ///< displacement across it, raw
    std::int64_t total_dx{};
    std::int64_t total_dy{};
};

/// `ticks` ticks of a parked tank on `ground`, its displacement each tick split along the
/// downhill direction (-ax, -ay) and across it.
Track roll(const world::MapGround& ground, int ax, int ay, int ticks, int tx = 32, int ty = 32) {
    const world::ScopedGround guard(ground);
    Sim sim = parked_tank(tx, ty);
    const double len = std::hypot(static_cast<double>(ax), static_cast<double>(ay));
    const double ux = len == 0.0 ? 0.0 : -ax / len;
    const double uy = len == 0.0 ? 0.0 : -ay / len;
    Track track;
    const Unit start = player_of(sim);
    for (int t = 0; t < ticks; ++t) {
        const Unit before = player_of(sim);
        sim.step();
        const Unit& after = player_of(sim);
        const auto dx = static_cast<double>(after.x.raw - before.x.raw);
        const auto dy = static_cast<double>(after.y.raw - before.y.raw);
        track.down.push_back((dx * ux) + (dy * uy));
        track.side.push_back((dx * -uy) + (dy * ux));
    }
    track.total_dx = static_cast<std::int64_t>(player_of(sim).x.raw) - start.x.raw;
    track.total_dy = static_cast<std::int64_t>(player_of(sim).y.raw) - start.y.raw;
    return track;
}

/// The horizontal acceleration of a unit sliding from rest on ground rising t a tile per tile.
double slide_acceleration(double t) { return t > kMu ? kG * (t - kMu) / (1.0 + (t * t)) : 0.0; }

std::string table(const Track& track) {
    std::ostringstream s;
    s << std::fixed << std::setprecision(1) << " tick   down/tick   accel   across\n";
    for (std::size_t i = 0; i < track.down.size(); ++i) {
        const double accel = i == 0 ? track.down[i] : track.down[i] - track.down[i - 1];
        s << std::setw(5) << i + 1 << std::setw(12) << track.down[i] << std::setw(8) << accel
          << std::setw(9) << track.side[i] << "\n";
    }
    return s.str();
}

// ---- 1. the zero-input slide -----------------------------------------------------------------

TEST(Gravity, AParkedTankOnASteepSlopeRollsDownItAndGathersSpeed) {
    // His first report. Ground falling east by `rise` a tile -- steeper than friction holds (25.6),
    // and all of it ground a tank DRIVES up (38 and under) -- and no key pressed. The tank must
    // ROLL: displaced downhill every tick, by more every tick, the gain each tick (the
    // acceleration) the physics' a = g (t - mu) / (1 + t^2), and nothing sideways.
    for (const int rise : {28, 32, 38, 45, 60}) {
        const Track track = roll(plane(-rise, 0), -rise, 0, 30);
        const double want = slide_acceleration(rise / 64.0);
        std::cout << "rise " << rise << ": a = " << std::fixed << std::setprecision(2) << want
                  << " raw/tick^2 wanted\n"
                  << table(track);
        ASSERT_EQ(track.down.size(), 30U);
        EXPECT_GT(track.down.front(), 0.0) << "rise " << rise << ": it did not move at all";
        for (std::size_t i = 1; i < track.down.size(); ++i) {
            const double accel = track.down[i] - track.down[i - 1];
            EXPECT_NEAR(accel, want, 2.0) << "rise " << rise << ", tick " << i + 1;
            EXPECT_EQ(track.side[i], 0.0) << "rise " << rise << ", tick " << i + 1;
        }
        // Over 30 ticks from rest, s = a n (n + 1) / 2 (the semi-implicit tick: v then x).
        EXPECT_NEAR(static_cast<double>(track.total_dx), want * 30.0 * 31.0 / 2.0,
                    want * 30.0 * 31.0 / 2.0 * 0.03 + 30.0)
            << "rise " << rise;
    }
}

// ---- 2. the zero-input hold ------------------------------------------------------------------

TEST(Gravity, AParkedTankOnAGentleSlopeStaysPutForEver) {
    // His second sentence: friction. On ground no steeper than friction holds -- t <= mu, 25.6 a
    // tile -- a parked tank does not move by one raw unit, ever: not creeping, not settling.
    for (const int rise : {0, 5, 10, 20, 25}) {
        const Track track = roll(plane(-rise, 0), -rise, 0, 300);
        EXPECT_EQ(track.total_dx, 0) << "rise " << rise;
        EXPECT_EQ(track.total_dy, 0) << "rise " << rise;
    }
}

// ---- 3. the threshold is the same at every angle ---------------------------------------------

TEST(Gravity, WhereSlidingBeginsIsTheSameSteepnessWhicheverWayTheGroundFalls) {
    // Planes falling every which way: 24 directions, and at each every steepness from 18 to 34 a
    // tile. A parked tank either moves within 20 ticks or it does not. The ground that holds it
    // and the ground that carries it must be separated by STEEPNESS ALONE: every plane that holds
    // is gentler than every plane that carries, whatever its direction -- an axis face and a
    // diagonal one begin to slide at the same steepness.
    std::ostringstream rows;
    rows << "heading  first steepness that slides\n";
    double steepest_held = 0.0;
    double gentlest_carried = 1e9;
    for (int k = 0; k < 24; ++k) {
        const double a = k * 15.0 * 3.14159265358979 / 180.0;
        double first = -1.0;
        for (int m = 18; m <= 34; ++m) {
            const int ax = static_cast<int>(std::lround(m * std::cos(a)));
            const int ay = static_cast<int>(std::lround(m * std::sin(a)));
            const double steep = std::hypot(static_cast<double>(ax), static_cast<double>(ay));
            const Track track = roll(plane(ax, ay), ax, ay, 20);
            const bool moved = track.total_dx != 0 || track.total_dy != 0;
            if (moved) {
                gentlest_carried = std::min(gentlest_carried, steep);
                if (first < 0.0) {
                    first = steep;
                }
                // ...and downhill, never up or across it.
                const double down = (-ax * static_cast<double>(track.total_dx))
                                    + (-ay * static_cast<double>(track.total_dy));
                EXPECT_GT(down, 0.0) << "heading " << k * 15 << ", steepness " << steep;
            } else {
                steepest_held = std::max(steepest_held, steep);
            }
        }
        rows << std::setw(7) << k * 15 << "  " << std::fixed << std::setprecision(2) << first
             << "\n";
    }
    std::cout << rows.str() << "steepest held " << steepest_held << ", gentlest carried "
              << gentlest_carried << "\n";
    EXPECT_LT(steepest_held, gentlest_carried) << rows.str();
    // And where that is: tan(slope) = mu, 0.40 * 64 = 25.6 a tile.
    EXPECT_LE(steepest_held, 25.6);
    EXPECT_GT(gentlest_carried, 25.6);
}

// ---- the roll back ends --------------------------------------------------------------------

TEST(Gravity, ARollBackComesToRestOnTheLevelAndStaysThere) {
    // Level ground at 0 west of corner column 28, a face falling 32 a tile east from 28 to 34, and
    // level ground beyond. Let go of the tank on the face: it rolls down, runs out onto the level,
    // where friction (mu g, 142.9 raw a tick^2) stops it, and then it STAYS. Without friction on
    // the flat it would roll on to the map's edge.
    const world::MapGround ground
        = field([](int x, int) { return -32 * std::clamp(x - 28, 0, 6); });
    // From x = 29 the face is five tiles: from rest at a = 357.17 * 0.1 / 1.25 = 28.57 raw a
    // tick^2 that takes sqrt(2 * 5 * 65536 / 28.57) = 151 ticks and ends at 4330 raw a tick; on
    // the level friction takes 142.87 a tick off it, 30 ticks more, over about a tile.
    const Track track = roll(ground, -32, 0, 300, 29, 32);
    std::size_t stopped = 0;
    for (std::size_t i = 0; i < track.down.size(); ++i) {
        if (track.down[i] != 0.0) {
            stopped = i + 1;
        }
    }
    ASSERT_GT(stopped, 0U) << "it never moved";
    ASSERT_LT(stopped, 200U) << "it never stopped\n" << table(track);
    EXPECT_GT(stopped, 170U) << "it stopped sooner than gravity and friction allow\n"
                             << table(track);
    for (std::size_t i = stopped; i < track.down.size(); ++i) {
        EXPECT_EQ(track.down[i], 0.0) << "tick " << i + 1 << ": it set off again";
    }
    // It came off the face onto the level: past corner 34, short of the map's edge.
    const double end_x = 29.0 + (static_cast<double>(track.total_dx) / 65536.0);
    EXPECT_GT(end_x, 34.0) << table(track);
    EXPECT_LT(end_x, 36.5) << table(track);
    std::cout << "came to rest at x = " << end_x << " after " << stopped << " ticks\n";
}

// ---- 4. gravity still cannot invert a driver ---------------------------------------------------

constexpr float kEast = 1.5707963F; ///< yaw 0 is +y; a quarter turn is +x

/// The player at the helm of the tank on `ground`, heading `heading` at `throttle`.
struct Helm {
    Sim sim;
    UnitId id{};
    game::PlayerCommander player;
    Helm(int tx, int ty, float heading, float throttle)
        : sim(parked_tank(tx, ty)), id(player_of(sim).id) {
        sim.enqueue(Order{id, orders::TakeControl{}});
        player.set_heading(heading);
        set(throttle);
    }
    void set(float throttle) {
        game::InputState input{};
        input.throttle = throttle;
        player.set_input(input);
    }
    /// One tick; the raw displacement along +x.
    std::int64_t tick() {
        std::vector<Order> orders;
        player.think(id, observe(sim, Side::Player), orders);
        for (const Order& order : orders) {
            sim.enqueue(order);
        }
        const std::int32_t before = sim.find(id)->x.raw;
        sim.step();
        return static_cast<std::int64_t>(sim.find(id)->x.raw) - before;
    }
};

TEST(Gravity, ThrustHoldsAClimbAndLettingGoRollsItBack) {
    // HIS TABLE, the rows that must not move and the row that must: a face rising 32 a tile east
    // -- climbable (under 38.4), and steeper than friction holds (over 25.6). With the throttle
    // held, the tank goes UP it every tick once the helm's order has landed (gravity may slow
    // him, never negate his intent -- before it lands nobody is driving, and it rolls). Let go:
    // once the helm's last order is spent, it rolls back DOWN, faster each tick.
    const world::ScopedGround guard(plane(32, 0));
    Helm helm(30, 32, kEast, 1.0F);
    std::ostringstream rows;
    bool driving = false;
    for (int t = 0; t < 30; ++t) {
        const std::int64_t dx = helm.tick();
        rows << "thrust t" << t << " dx " << dx << "\n";
        driving = driving || dx > 0;
        if (driving) {
            EXPECT_GT(dx, 0) << "tick " << t << ": thrust uphill was inverted";
        }
    }
    EXPECT_TRUE(driving) << rows.str();
    helm.set(0.0F);
    std::vector<std::int64_t> back;
    for (int t = 0; t < 70; ++t) {
        back.push_back(helm.tick());
        rows << "released t" << t << " dx " << back.back() << "\n";
    }
    // After release the helm's throttle EASES down (`kThrottleEasePerTick`: "a second, not a
    // lurch"), and uphill a command above what the slope leaves (net = min(c, T + a), 1715 here)
    // climbs at that net until it falls under it -- 26 ticks, measured. From then on the tank is
    // not driven, rolls west, and gathers speed.
    for (std::size_t t = 40; t < back.size(); ++t) {
        EXPECT_LT(back[t], 0) << "released tick " << t << ": it did not roll back\n" << rows.str();
        EXPECT_LT(back[t], back[t - 1]) << "released tick " << t << ": it did not gather speed\n"
                                        << rows.str();
    }
}

// ---- 5. the corners the renderer draws ---------------------------------------------------------

TEST(Corners, TheSlopeUnderAUnitIsTheTriangleThePlayerSees) {
    // The renderer draws tile (x, y) as two triangles between its four corner heights, split on
    // the (x, y)-(x+1, y+1) diagonal (`game::ground_metres`). The sim's slope under a unit must be
    // the slope of the triangle it stands on -- measured here off the DRAWN ground, as a centred
    // difference in metres inside the triangle, and turned into height units a tile (32 to the
    // metre, 2 m a tile). On seed 42's real ground, many tiles, both triangles.
    constexpr std::uint64_t kSeed = 42;
    struct World {
        [[nodiscard]] int height(int x, int y) const noexcept {
            return world::sample(kSeed, x, y).height;
        }
    };
    int differ = 0;
    int checked = 0;
    for (int ty = 40; ty < 60; ++ty) {
        for (int tx = 40; tx < 60; ++tx) {
            for (const auto& [fx, fy] :
                 {std::array<double, 2>{0.70, 0.25}, std::array<double, 2>{0.25, 0.70}}) {
                const double px = tx + fx;
                const double py = ty + fy;
                constexpr double kE = 0.05;
                const auto at = [&](double x, double y) {
                    return static_cast<double>(
                        game::ground_metres(kSeed, static_cast<float>(x), static_cast<float>(y)));
                };
                // metres a tile, then height units a tile (32 units a metre)
                const double sx = (at(px + kE, py) - at(px - kE, py)) / (2.0 * kE) * 32.0;
                const double sy = (at(px, py + kE) - at(px, py - kE)) / (2.0 * kE) * 32.0;
                const Gradient g = gradient_under(
                    World{}, Fixed::from_raw(static_cast<std::int32_t>(std::lround(px * 65536.0))),
                    Fixed::from_raw(static_cast<std::int32_t>(std::lround(py * 65536.0))));
                EXPECT_NEAR(static_cast<double>(g.x2) / 2.0, sx, 0.05)
                    << "tile " << tx << "," << ty << " at (" << fx << ", " << fy << ")";
                EXPECT_NEAR(static_cast<double>(g.y2) / 2.0, sy, 0.05)
                    << "tile " << tx << "," << ty << " at (" << fx << ", " << fy << ")";
                ++checked;
                differ += std::abs((static_cast<double>(g.x2) / 2.0) - sx) > 0.05 ? 1 : 0;
            }
        }
    }
    std::cout << differ << " of " << checked << " points read a slope the player does not see\n";
}

TEST(Corners, AFaceThePlayerSeesAsACliffIsAWall) {
    // One corner raised: tile (3, 2)'s corner (4, 2) stands 60 over the other three. Drawn, the
    // tile is two triangles: the one under that corner rises 60 east AND falls 60 south (a face of
    // 85 a tile), the other is level. The quad's MEAN reads (30, -30): 42 a tile, and only 30 of
    // it along east -- a tank would drive east straight into a face the player sees as a cliff.
    // Judged by the triangles the player sees, heading east into it is a wall; and a face no
    // steeper than a climb either way (raise the corner 20) is not.
    const auto corner
        = [](int raise) { return [raise](int x, int y) { return x == 4 && y == 2 ? raise : 0; }; };
    EXPECT_FALSE(drivable(corner(60), 3, 2, 1, 0)) << "east into the drawn cliff";
    EXPECT_TRUE(drivable(corner(20), 3, 2, 1, 0)) << "east into a drawn climb";
    // Heading west, the steep triangle FALLS away from the unit: not a wall.
    EXPECT_TRUE(drivable(corner(60), 3, 2, -1, 0)) << "west, down the drawn face";
}

} // namespace
} // namespace protekt::sim
