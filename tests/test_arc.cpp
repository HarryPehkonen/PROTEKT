#include "protekt/game/player.hpp"
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

/// THE ARC (G8). His words: "If I drive off the edge of a cliff at full speed, I should fly in an
/// arc. Just like in real life."
///
/// Every test here is a MEASUREMENT through the real `Sim` on a drawn ground, per tick, and the
/// numbers it is held to are worked out HERE from the physics, in floating point:
///   g = 9.81 m/s^2; a tile is 2 m and 64 height units, a second is 30 ticks, so g is
///       9.81 / 2 * 64 / 900 = 0.3488 height units a tick^2;
///   a body that leaves level ground at horizontal speed v falls h in t = sqrt(2 h / g) and has
///       gone v t meanwhile -- the time does not depend on v, so the distance is v's alone.
/// The cliff: level ground at height 0 up to corner column 32, a face falling 64 a tile (45
/// degrees, steeper than the 38.4 a tile any unit climbs: a cliff, `press_beats`) from corner 32
/// to 33, and level ground at -64 beyond it.
namespace protekt::sim {
namespace {

constexpr int kSide = 64;
constexpr double kGHeight = 9.81 / 2.0 * 64.0 / (30.0 * 30.0); ///< height units a tick^2
constexpr int kDrop = 64;
constexpr double kEdge = 32.0;
constexpr double kFoot = 33.0;

/// Height `h(x, y)` at every corner of a 64 x 64 field of open Flat ground.
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

int cliff_height(int x, int) { return -kDrop * std::clamp(x - 32, 0, 1); }

/// The drawn ground's height at a point, height units: the triangle of the tile the point is in
/// (split on the (x, y)-(x+1, y+1) diagonal, as `game::ground_metres` draws it). Worked here,
/// from the corners, not read from the sim.
double drawn_height(const std::function<int(int, int)>& h, double px, double py) {
    const int tx = static_cast<int>(std::floor(px));
    const int ty = static_cast<int>(std::floor(py));
    const double fx = px - tx;
    const double fy = py - ty;
    const double h00 = h(tx, ty);
    const double h10 = h(tx + 1, ty);
    const double h01 = h(tx, ty + 1);
    const double h11 = h(tx + 1, ty + 1);
    return fx >= fy ? h00 + (fx * (h10 - h00)) + (fy * (h11 - h10))
                    : h00 + (fx * (h11 - h01)) + (fy * (h01 - h00));
}

/// One unit of the player's on the corner of tile (`tx`, `ty`), an enemy factory far off.
Sim lone(UnitKind kind, int tx, int ty) {
    Scenario scenario{};
    scenario.seed = 0;
    Spawn unit{};
    unit.side = Side::Player;
    unit.kind = kind;
    unit.tile_x = tx;
    unit.tile_y = ty;
    scenario.spawns.push_back(unit);
    Spawn base{};
    base.side = Side::Enemy;
    base.kind = UnitKind::Factory;
    base.tile_x = 4;
    base.tile_y = 60;
    scenario.spawns.push_back(base);
    return Sim::create(scenario);
}

UnitId player_id(const Sim& sim) {
    for (const Unit& unit : sim.units()) {
        if (unit.side == Side::Player) {
            return unit.id;
        }
    }
    return UnitId{};
}

/// One tick's measurement: where it ended, how high, how far it went.
struct Tick {
    double x{};      ///< tiles, after the tick
    double ground{}; ///< the drawn ground under it, height units (worked here)
    double lift{};   ///< the sim's `lift`, height units
    double z{};      ///< its own height: ground + lift
    std::int64_t dx{};
    std::int64_t dy{};
    bool air{};
};

struct Curve {
    std::vector<Tick> ticks;
    [[nodiscard]] int first_air() const {
        for (std::size_t i = 0; i < ticks.size(); ++i) {
            if (ticks[i].air) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
    /// The first tick back on the ground after the first flight, or -1.
    [[nodiscard]] int landed() const {
        const int from = first_air();
        if (from < 0) {
            return -1;
        }
        for (std::size_t i = static_cast<std::size_t>(from); i < ticks.size(); ++i) {
            if (!ticks[i].air) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
};

Tick measure(const Sim& sim, UnitId id, const Unit& before, const std::function<int(int, int)>& h) {
    const Unit& u = *sim.find(id);
    Tick t;
    t.x = u.x.raw / 65536.0;
    t.ground = drawn_height(h, t.x, u.y.raw / 65536.0);
    t.lift = u.lift.raw / 65536.0;
    t.z = t.ground + t.lift;
    t.dx = static_cast<std::int64_t>(u.x.raw) - before.x.raw;
    t.dy = static_cast<std::int64_t>(u.y.raw) - before.y.raw;
    t.air = airborne(u);
    return t;
}

std::string table(const std::string& title, const Curve& c) {
    std::ostringstream s;
    s << "---- " << title << "\n tick       x   ground     lift   height   dx/tick  air\n";
    for (std::size_t i = 0; i < c.ticks.size(); ++i) {
        const Tick& t = c.ticks[i];
        s << std::setw(5) << i << std::fixed << std::setprecision(3) << std::setw(8) << t.x
          << std::setw(9) << t.ground << std::setw(9) << t.lift << std::setw(9) << t.z
          << std::setw(10) << t.dx << (t.air ? "   AIR" : "") << "\n";
    }
    return s.str();
}

/// The player at the helm, heading east at full throttle, from tile (`tx`, 32).
Curve helm_east(int tx, int ticks) {
    Sim sim = lone(UnitKind::Tank, tx, 32);
    const UnitId id = player_id(sim);
    sim.enqueue(Order{id, orders::TakeControl{}});
    game::PlayerCommander player;
    player.set_heading(1.5707963F); // yaw 0 is +y; a quarter turn is +x
    game::InputState input{};
    input.throttle = 1.0F;
    player.set_input(input);
    Curve c;
    for (int t = 0; t < ticks; ++t) {
        std::vector<Order> orders;
        player.think(id, observe(sim, Side::Player), orders);
        for (const Order& order : orders) {
            sim.enqueue(order);
        }
        const Unit before = *sim.find(id);
        sim.step();
        c.ticks.push_back(measure(sim, id, before, cliff_height));
    }
    return c;
}

/// A unit of `kind` ordered (no helm) to a point far east, past the cliff.
Curve ordered_east(UnitKind kind, int tx, int ticks) {
    Sim sim = lone(kind, tx, 32);
    const UnitId id = player_id(sim);
    sim.enqueue(
        Order{id, orders::MoveTo{Fixed::from_int(50), Fixed::from_raw(32 * 65536 + 32768)}});
    Curve c;
    for (int t = 0; t < ticks; ++t) {
        const Unit before = *sim.find(id);
        sim.step();
        c.ticks.push_back(measure(sim, id, before, cliff_height));
    }
    return c;
}

// ---- 1. the arc is real --------------------------------------------------------------------

TEST(Arc, OffACliffAtFullSpeedTheTankFliesAndItsHorizontalSpeedIsUnchanged) {
    const world::ScopedGround guard(field(cliff_height));
    const Curve c = helm_east(20, 140);
    const std::string printed = table("the helm, full throttle, east off a 64-unit cliff", c);
    std::cout << printed;
    const int up = c.first_air();
    ASSERT_GE(up, 0) << "it never left the ground: it went down the cliff with the ground\n"
                     << printed;
    const int down = c.landed();
    ASSERT_GT(down, up) << printed;
    // It left the ground AT the edge, at full speed: the tick before, it was on the level.
    EXPECT_LE(c.ticks[static_cast<std::size_t>(up) - 1].x, kEdge) << printed;
    const std::int64_t v = c.ticks[static_cast<std::size_t>(up)].dx;
    EXPECT_EQ(v, kTankSpeedRaw) << "it left at its full speed";
    // HORIZONTAL SPEED PERSISTS: every tick in the air is the same step, straight on.
    for (int i = up; i < down; ++i) {
        EXPECT_EQ(c.ticks[static_cast<std::size_t>(i)].dx, v) << "tick " << i << "\n" << printed;
        EXPECT_EQ(c.ticks[static_cast<std::size_t>(i)].dy, 0) << "tick " << i;
    }
    // ITS HEIGHT IS ITS OWN, AND GRAVITY'S: in the air it falls by g more each tick -- the second
    // difference of its height is -g -- whatever the ground under it does.
    for (int i = up + 2; i < down; ++i) {
        const double a = c.ticks[static_cast<std::size_t>(i)].z
                         - (2.0 * c.ticks[static_cast<std::size_t>(i) - 1].z)
                         + c.ticks[static_cast<std::size_t>(i) - 2].z;
        EXPECT_NEAR(a, -kGHeight, 0.001) << "tick " << i << "\n" << printed;
    }
    // AND IT NEVER DROPS WITH THE GROUND: the first tick over the face it is still at the
    // height of the level it left, give or take one tick of gravity.
    EXPECT_GT(c.ticks[static_cast<std::size_t>(up)].z, -kGHeight - 0.001) << printed;
    // THE LANDING, where the arc puts it: from the edge at 0.2 tiles a tick, a fall of 64 takes
    // sqrt(2 * 64 / g) = 19.2 ticks, and it has gone 3.83 tiles meanwhile -- 2.8 past the foot.
    const double t_fall = std::sqrt(2.0 * kDrop / kGHeight);
    const double want = kEdge + (0.2 * t_fall);
    const Tick& land = c.ticks[static_cast<std::size_t>(down)];
    std::cout << "flew " << down - up << " ticks; landed at x = " << land.x << ", the arc says "
              << want << "\n";
    EXPECT_NEAR(land.x, want, 0.25) << printed;
    EXPECT_GT(land.x, kFoot + 2.0) << "it must come down well clear of the cliff's foot";
    EXPECT_NEAR(land.ground, -kDrop, 0.001) << "on the level below, at the terrain's own height";
    EXPECT_EQ(land.lift, 0.0);
    // And it drives on from there.
    EXPECT_GT(c.ticks.back().x, land.x + 1.0) << printed;
}

// ---- 2. speed is the gate --------------------------------------------------------------------

TEST(Arc, TheSameEdgeAtACrawlDropsNearlyStraightDownAndSpeedIsWhatMakesTheArc) {
    // Two launches off the one edge, both by order (no helm): a TANK (0.2 tiles a tick) and a
    // TURRET, the slowest thing that moves (a quarter of that, 0.05). The fall takes the same time
    // for both -- the drop decides it -- so how far each goes while it falls is its speed's alone.
    const world::ScopedGround guard(field(cliff_height));
    const Curve fast = ordered_east(UnitKind::Tank, 24, 120);
    const Curve slow = ordered_east(UnitKind::Turret, 28, 400);
    const std::string printed = table("a tank, by order, off the cliff", fast)
                                + table("a turret, by order, off the same cliff", slow);
    std::cout << printed;
    const auto flight = [](const Curve& c) {
        const int up = c.first_air();
        const int down = c.landed();
        if (up < 1 || down <= up) {
            return -1.0;
        }
        return c.ticks[static_cast<std::size_t>(down)].x
               - c.ticks[static_cast<std::size_t>(up) - 1].x;
    };
    const double far = flight(fast);
    const double near = flight(slow);
    std::cout << "horizontal distance in the air: tank " << far << " tiles, turret " << near
              << " tiles\n";
    ASSERT_GT(far, 0.0) << "the tank never flew\n" << printed;
    // The tank flies clear of the face: the arc of test 1.
    EXPECT_GT(far, 3.0) << printed;
    // The crawl goes nearly straight down: it is back on the ground before it is past the face's
    // foot -- at a quarter of the speed, a quarter of the distance, about a tile at most.
    EXPECT_LT(near, 1.1) << printed;
    if (near > 0.0) {
        EXPECT_NEAR(far / near, 4.0, 1.0) << "the distance is the speed's";
    }
    // Both end up on the level below.
    EXPECT_NEAR(fast.ticks.back().ground, -kDrop, 0.001) << printed;
    EXPECT_NEAR(slow.ticks.back().ground, -kDrop, 0.001) << printed;
}

// ---- 3. nothing else moved: ordinary ground is driven, not flown -----------------------------

TEST(Arc, AClimbableRidgeAndADrivableDescentAreDrivenAtFullSpeedAndNeverFlown) {
    // His table's other rows depend on a unit on drivable ground STAYING on it: a ridge rising
    // and falling 30 a tile (climbable: under 38.4) and a descent of 38 a tile, at full speed.
    // The ground falls away at the brow faster than gravity follows (0.2 tiles a tick down 30 a
    // tile is 6 units a tick), but it is ground a unit could drive back up: no cliff, no flight.
    const auto ridge
        = [](int x, int) { return 30 * std::max(0, 3 - std::abs(x - 32)); }; // up 3, down 3
    const auto descent = [](int x, int) { return -38 * std::clamp(x - 30, 0, 8); };
    for (const auto& [name, h] :
         {std::pair<std::string, std::function<int(int, int)>>{"ridge 30", ridge},
          std::pair<std::string, std::function<int(int, int)>>{"descent 38", descent}}) {
        const world::ScopedGround guard(field(h));
        const Curve c = helm_east(20, 400);
        EXPECT_LT(c.first_air(), 0) << name << ": it flew over ground it could drive";
        EXPECT_GT(c.ticks.back().x, 40.0) << name << ": and it got across";
    }
}

} // namespace
} // namespace protekt::sim
