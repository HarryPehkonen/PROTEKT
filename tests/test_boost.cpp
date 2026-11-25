#include "protekt/game/player.hpp"
#include "protekt/sim/commander.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/steering.hpp"
#include "protekt/world/ground.hpp"
#include "protekt/world/world.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

/// BOOST IS FRICTIONLESS (G10). His words: "When I went on a ramp and stopped, I didn't roll down
/// noticeably, which I think should happen, ESPECIALLY when I hit boost with the e key. Hitting
/// boost should reduce friction to zero, just like with a hovercraft."
///
/// A ramp built to be driven up is gentler than friction holds (25.6 a tile), so a parked tank
/// HOLDS on it -- the row he confirmed in G7, kept. His answer is the key, not a lower threshold:
/// hold on the ramp, press E, and SLIDE.
///
/// Every number here is worked out HERE, in floating point, from the physics, as test_gravity.cpp
/// works its own:
///   g = 9.81 m/s^2 at 2 m a tile and 30 ticks a second = 357.17 raw a tick^2 (Q16.16 tiles);
///   on ground rising t = tan(slope) gravity pushes a free body down it, HORIZONTALLY, by
///       g sin cos = g t / (1 + t^2)
///   and with the grip released nothing opposes it (mu = 0): that is the acceleration, every tick,
///   from rest, whatever the steepness -- every slope is a slide.
///
/// HIS ACCEPTANCE TABLE, every row, and the test that measures it (the rows' tests elsewhere are
/// named, and this slice does not change them):
///   1. parked on a STEEP slope, no thrust: rolls back --
///      Gravity.AParkedTankOnASteepSlopeRollsDownItAndGathersSpeed
///   2. parked on a GENTLE slope, no thrust: stays put --
///      Gravity.AParkedTankOnAGentleSlopeStaysPutForEver, and the unboosted half of
///      Frictionless.OnARampTheTankThatHoldsUnboostedSlidesBoosted
///   3. parked on a RAMP, boost held: SLIDES (NEW) --
///      Frictionless.OnARampTheTankThatHoldsUnboostedSlidesBoosted
///   4. driving, boost held, downhill: past the normal downhill speed (NEW) --
///      Frictionless.DrivingDownhillBoostedOutrunsTheNormalDownhillSpeed
///   5. crossing a slope: veers downhill, steering holds the line --
///      PressMeasured.AtEveryHeadingTheSlopeNeverInvertsTheDriveUnlessItTrulyOpposesIt,
///      TankThatReverses.DrivingAlongTheRimIsNeverDisplacedBackwards, and the unboosted half of
///      Frictionless.CrossingARampBoostedTheGripNoLongerHoldsTheLine
///   6. driving DOWNHILL, no boost: faster than on the flat --
///      PressMeasured.EverySteeperSlopeIsStrictlyWorseUphillAndStrictlyBetterDownhill, and the
///      unboosted half of Frictionless.DrivingDownhillBoostedOutrunsTheNormalDownhillSpeed
///   7. too-steep climb WITH thrust: slows, stops, slides back --
///      TankThatReverses.UphillAgainstThePressIsSlowedUpToAGradeOf38AndReversedFrom39
///   8. a climbable hill's top: crossable --
///      SteepHillTop.EveryClimbableStepHasACrossableTop
///   9. off a cliff at speed: flies in an arc --
///      Arc.OffACliffAtFullSpeedTheTankFliesAndItsHorizontalSpeedIsUnchanged, and boosted,
///      Frictionless.OffACliffBoostedTheTankStillFlies
namespace protekt::sim {
namespace {

constexpr double kG = 9.81 / 2.0 / (30.0 * 30.0) * 65536.0; ///< raw a tick^2
constexpr double kMu = 0.40;
constexpr int kSide = 64;
constexpr float kEast = 1.5707963F; ///< yaw 0 is +y; a quarter turn is +x

/// Height `h(x, y)` at every corner of a 64 x 64 field of open Flat ground, clamped to +-500.
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

/// A RAMP: ground falling `rise` a tile east, through corner (32, 32) at height 0.
world::MapGround ramp(int rise) {
    return field([rise](int x, int) { return -rise * (x - 32); });
}

/// Tanks of the player's on the corners of the given tiles, an enemy factory far off, no orders.
Sim tanks(const std::vector<std::array<int, 2>>& at) {
    Scenario scenario{};
    scenario.seed = 0;
    for (const auto& [tx, ty] : at) {
        Spawn unit{};
        unit.side = Side::Player;
        unit.kind = UnitKind::Tank;
        unit.tile_x = tx;
        unit.tile_y = ty;
        scenario.spawns.push_back(unit);
    }
    Spawn base{};
    base.side = Side::Enemy;
    base.kind = UnitKind::Factory;
    base.tile_x = 60;
    base.tile_y = 60;
    scenario.spawns.push_back(base);
    return Sim::create(scenario);
}

UnitId first_tank(const Sim& sim) {
    for (const Unit& unit : sim.units()) {
        if (unit.side == Side::Player) {
            return unit.id;
        }
    }
    return UnitId{};
}

/// A position in tiles, for the printed tables.
double tiles(Fixed f) { return static_cast<double>(f.raw) / 65536.0; }

/// The horizontal acceleration of a body free of friction on ground rising t a tile per tile.
double frictionless(double t) { return kG * t / (1.0 + (t * t)); }

/// `ticks` ticks of the parked tank at (`tx`, 32) on `ground`, boosted or not: its displacement
/// along +x (east, downhill on every ground here) each tick, raw.
std::vector<std::int64_t> parked(const world::MapGround& ground, int tx, bool boost, int ticks) {
    const world::ScopedGround guard(ground);
    Sim sim = tanks({{tx, 32}});
    const UnitId id = first_tank(sim);
    if (boost) {
        sim.enqueue(Order{id, orders::Boost{true}});
    }
    std::vector<std::int64_t> dx;
    for (int t = 0; t < ticks; ++t) {
        const std::int32_t before = sim.find(id)->x.raw;
        sim.step();
        dx.push_back(static_cast<std::int64_t>(sim.find(id)->x.raw) - before);
        EXPECT_EQ(sim.find(id)->y.raw, Fixed::from_int(32).raw) << "it went sideways";
    }
    return dx;
}

// ---- 1. the ramp: held without the key, a slide with it -----------------------------------------

TEST(Frictionless, OnARampTheTankThatHoldsUnboostedSlidesBoosted) {
    // One ramp, one tank, the same 60 ticks, twice. Ramps gentler than friction holds -- 10, 20 and
    // 25 a tile, all under 25.6 -- so unboosted it does not move by one raw unit (his G7 row). With
    // the grip released (E held) it slides from the first tick, gaining g t / (1 + t^2) every tick:
    // the whole of gravity's pull, with no friction to take any of it.
    for (const int rise : {10, 20, 25}) {
        const std::vector<std::int64_t> held = parked(ramp(rise), 30, false, 60);
        const std::vector<std::int64_t> slid = parked(ramp(rise), 30, true, 60);
        const double want = frictionless(rise / 64.0);
        std::ostringstream rows;
        rows << "ramp " << rise << " a tile (t = " << std::fixed << std::setprecision(3)
             << rise / 64.0 << "): frictionless a = " << std::setprecision(2) << want
             << " raw/tick^2\n tick  unboosted dx  boosted dx  gain\n";
        for (std::size_t i = 0; i < slid.size(); ++i) {
            const std::int64_t gain = i == 0 ? slid[i] : slid[i] - slid[i - 1];
            rows << std::setw(5) << i + 1 << std::setw(14) << held[i] << std::setw(12) << slid[i]
                 << std::setw(6) << gain << "\n";
        }
        std::cout << rows.str();
        for (std::size_t i = 0; i < held.size(); ++i) {
            EXPECT_EQ(held[i], 0) << "ramp " << rise << ", tick " << i + 1
                                  << ": unboosted it moved -- the gentle-slope row regressed";
        }
        for (std::size_t i = 0; i < slid.size(); ++i) {
            EXPECT_GT(slid[i], 0) << "ramp " << rise << ", tick " << i + 1
                                  << ": boosted, it did not slide";
            if (i > 0) {
                EXPECT_NEAR(static_cast<double>(slid[i] - slid[i - 1]), want, 2.0)
                    << "ramp " << rise << ", tick " << i + 1 << ": not gravity's whole pull";
            }
        }
    }
}

// ---- 2. let go of E: the grip comes back
// ---------------------------------------------------------

TEST(Frictionless, LetGoTheGroundGripsAgainAndFrictionStopsTheSlide) {
    // Ramp 20: boosted for 40 ticks it slides; the key let go, the grip is back, and friction
    // (mu g / (1 + t^2)) beats the pull (g t / (1 + t^2)) on ground this gentle: the slide loses
    // g (mu - t) / (1 + t^2) a tick until it stops, and then it holds for ever.
    const double t = 20.0 / 64.0;
    const double loss = kG * (kMu - t) / (1.0 + (t * t));
    const world::ScopedGround guard(ramp(20));
    Sim sim = tanks({{20, 32}});
    const UnitId id = first_tank(sim);
    sim.enqueue(Order{id, orders::Boost{true}});
    std::vector<std::int64_t> dx;
    for (int i = 0; i < 40; ++i) {
        const std::int32_t before = sim.find(id)->x.raw;
        sim.step();
        dx.push_back(static_cast<std::int64_t>(sim.find(id)->x.raw) - before);
    }
    ASSERT_GT(dx.back(), 0) << "it never slid";
    sim.enqueue(Order{id, orders::Boost{false}});
    std::vector<std::int64_t> after;
    for (int i = 0; i < 200; ++i) {
        const std::int32_t before = sim.find(id)->x.raw;
        sim.step();
        after.push_back(static_cast<std::int64_t>(sim.find(id)->x.raw) - before);
    }
    std::ostringstream rows;
    rows << "let go at " << dx.back() << " raw/tick; friction's net loss " << std::fixed
         << std::setprecision(2) << loss << " raw/tick^2\n";
    std::size_t stopped = after.size();
    for (std::size_t i = 0; i < after.size(); ++i) {
        rows << " +" << i + 1 << ": " << after[i] << "\n";
        if (after[i] == 0 && stopped == after.size()) {
            stopped = i;
        }
    }
    ASSERT_LT(stopped, after.size()) << "it never stopped\n" << rows.str();
    for (std::size_t i = 1; i < stopped; ++i) {
        EXPECT_NEAR(static_cast<double>(after[i - 1] - after[i]), loss, 2.0) << "tick +" << i + 1;
    }
    for (std::size_t i = stopped; i < after.size(); ++i) {
        EXPECT_EQ(after[i], 0) << "tick +" << i + 1 << ": set off again on a gentle ramp";
    }
    std::cout << rows.str().substr(0, 400) << "... stopped after " << stopped << " ticks\n";
}

// ---- 3. driving downhill, boosted
// ----------------------------------------------------------------

/// The player at the helm of the tank at (`tx`, 32), heading east at full throttle, E held or not.
struct Helm {
    Sim sim;
    UnitId id{};
    game::PlayerCommander player;
    std::int64_t last_dy{}; ///< the last tick's displacement along +y, raw
    Helm(int tx, bool boost, float heading = kEast, int ty = 32)
        : sim(tanks({{tx, ty}})), id(first_tank(sim)) {
        sim.enqueue(Order{id, orders::TakeControl{}});
        player.set_heading(heading);
        game::InputState input{};
        input.throttle = 1.0F;
        input.boost = boost;
        player.set_input(input);
    }
    /// One tick through the real commander; the raw displacement along +x.
    std::int64_t tick() {
        std::vector<Order> orders;
        player.think(id, observe(sim, Side::Player), orders);
        for (const Order& order : orders) {
            sim.enqueue(order);
        }
        const std::int32_t before = sim.find(id)->x.raw;
        const std::int32_t before_y = sim.find(id)->y.raw;
        sim.step();
        last_dy = static_cast<std::int64_t>(sim.find(id)->y.raw) - before_y;
        return static_cast<std::int64_t>(sim.find(id)->x.raw) - before;
    }
};

TEST(Frictionless, DrivingDownhillBoostedOutrunsTheNormalDownhillSpeed) {
    // Ramp 20, driven east (downhill) at full throttle from x = 12, through the real
    // PlayerCommander, E held or not. Unboosted the tank settles at the normal downhill speed --
    // the engine's step plus the press, c + P, with P = 13107 sin(atan(t)) sqrt(34) / 3 = 7599 --
    // and HOLDS it: the ground's grip spends what gravity gives past that (his row "driving
    // DOWNHILL, no boost: faster than on the flat", kept). Boosted, nothing spends it: from the
    // tick the drive lands the tank is faster than unboosted, and the gap grows by gravity's whole
    // pull, g t / (1 + t^2), every tick.
    const double t = 20.0 / 64.0;
    const double press = 13107.0 * (t / std::sqrt(1.0 + (t * t))) * std::sqrt(34.0) / 3.0;
    const double pull = frictionless(t);
    const world::ScopedGround guard(ramp(20));
    Helm plain(12, false);
    Helm boosted(12, true);
    std::vector<std::int64_t> a;
    std::vector<std::int64_t> b;
    std::ostringstream rows;
    rows << std::fixed << std::setprecision(1)
         << "normal downhill speed c + P = " << 13107.0 + press << "; pull " << pull
         << " raw/tick^2\n tick  unboosted  boosted   gap\n";
    for (int i = 0; i < 60; ++i) {
        a.push_back(plain.tick());
        b.push_back(boosted.tick());
        rows << std::setw(5) << i + 1 << std::setw(11) << a.back() << std::setw(9) << b.back()
             << std::setw(6) << b.back() - a.back() << "\n";
    }
    std::cout << rows.str();
    // Full throttle is reached after the helm's one-second ease (30 ticks); from 35 on, both are
    // at it, and the unboosted speed is the steady c + P.
    for (std::size_t i = 35; i < a.size(); ++i) {
        EXPECT_NEAR(static_cast<double>(a[i]), 13107.0 + press, 8.0)
            << "tick " << i + 1 << ": the unboosted downhill speed moved";
    }
    for (std::size_t i = 2; i < b.size(); ++i) {
        EXPECT_GT(b[i], a[i]) << "tick " << i + 1 << ": boosted was not faster";
        EXPECT_GT(b[i] - a[i], b[i - 1] - a[i - 1]) << "tick " << i + 1 << ": the gap did not grow";
    }
    // The gap after n boosted ticks is n pulls (the roll, gravity's alone).
    const double gap = static_cast<double>(b.back() - a.back());
    EXPECT_NEAR(gap, pull * 59.0, pull * 3.0) << rows.str();
}

// ---- 3b. what the boost does to the other rows: MEASURED, for his ruling ----------------------

TEST(Frictionless, ClimbingARampBoostedTheSlopeWinsInTime) {
    // Ramp 20, driven WEST (uphill) at full throttle from x = 50, E held or not. Unboosted the
    // engine holds the climb at the net the press leaves it, T - P = 13107 - 7599 = 5508 raw a
    // tick, for ever: "it must never negate your intent". Boosted, the engine's step and the press
    // are the same, but the ground no longer spends gravity's pull, so the roll back gathers 101.7
    // a tick -- and in time it outruns the climb, and the tank slides back with the throttle held.
    // That is what "friction zero" means on a hill; it is MEASURED here, and printed, for him to
    // rule on. It does NOT touch the unboosted row.
    const world::ScopedGround guard(ramp(20));
    Helm plain(50, false, -kEast);
    Helm boosted(50, true, -kEast);
    std::ostringstream rows;
    rows << " tick  unboosted(west)  boosted(west)\n";
    int reversed = -1;
    std::vector<std::int64_t> a;
    std::vector<std::int64_t> b;
    for (int i = 0; i < 120; ++i) {
        a.push_back(-plain.tick());
        b.push_back(-boosted.tick());
        if (i % 10 == 9) {
            rows << std::setw(5) << i + 1 << std::setw(17) << a.back() << std::setw(15) << b.back()
                 << "\n";
        }
        if (reversed < 0 && i > 5 && b.back() < 0) {
            reversed = i + 1;
        }
    }
    std::cout << rows.str() << "boosted, the climb turned into a slide back at tick " << reversed
              << " (" << reversed / 30.0 << " s)\n";
    for (std::size_t i = 35; i < a.size(); ++i) {
        EXPECT_NEAR(static_cast<double>(a[i]), 5508.0, 8.0)
            << "tick " << i + 1 << ": the unboosted climb moved";
    }
    EXPECT_GT(reversed, 0) << "boosted, the climb never gave way\n" << rows.str();
    const double pull = frictionless(20.0 / 64.0);
    EXPECT_NEAR(static_cast<double>(a.back() - b.back()), pull * 120.0, pull * 3.0) << rows.str();
}

TEST(Frictionless, CrossingARampBoostedTheGripNoLongerHoldsTheLine) {
    // Ramp 20 falling east, driven SOUTH (+y), across it. Unboosted, the press across the heading
    // (7599) is under the tracks' grip (13107): no veer at all, the line holds. Boosted, the roll
    // across is gravity's alone and gathers 101.7 a tick: the tank drifts downhill, more each
    // tick, and no steering in the engine's step takes it back. MEASURED, for his ruling.
    const world::ScopedGround guard(ramp(20));
    Helm plain(20, false, 0.0F, 8);
    Helm boosted(20, true, 0.0F, 8);
    std::ostringstream rows;
    rows << " tick  unboosted east-drift  boosted east-drift\n";
    std::int64_t drift_plain = 0;
    std::int64_t drift_boosted = 0;
    for (int i = 0; i < 60; ++i) {
        drift_plain += plain.tick();
        drift_boosted += boosted.tick();
        if (i % 10 == 9) {
            rows << std::setw(5) << i + 1 << std::setw(23) << drift_plain << std::setw(20)
                 << drift_boosted << "\n";
        }
    }
    std::cout << rows.str();
    EXPECT_EQ(drift_plain, 0) << "unboosted, the line no longer holds\n" << rows.str();
    const double pull = frictionless(20.0 / 64.0);
    EXPECT_NEAR(static_cast<double>(drift_boosted), pull * 60.0 * 61.0 / 2.0, pull * 60.0)
        << rows.str();
}

TEST(Frictionless, OffACliffBoostedTheTankStillFlies) {
    // His arc row, with E held: level ground at 0 to corner 32, a face falling 64 a tile (a cliff)
    // to corner 33, level at -64 beyond. Driven east at full throttle from x = 20, boosted. It
    // must leave the ground at the edge and land past the foot, as unboosted (test_arc.cpp).
    const world::ScopedGround guard(
        field([](int x, int) { return -64 * std::clamp(x - 32, 0, 1); }));
    Helm helm(20, true);
    bool flew = false;
    double landed_x = 0.0;
    for (int i = 0; i < 200 && landed_x == 0.0; ++i) {
        helm.tick();
        const Unit& unit = *helm.sim.find(helm.id);
        if (unit.lift.raw != 0) {
            flew = true;
        } else if (flew) {
            landed_x = tiles(unit.x);
        }
    }
    std::cout << "boosted off the cliff: flew " << flew << ", landed at x = " << landed_x << "\n";
    EXPECT_TRUE(flew) << "boosted, it dropped instead of flying";
    EXPECT_GT(landed_x, 33.0) << "it did not clear the face";
}

// ---- 4. THE LUG: the terminal speed on a long descent ------------------------------------------

TEST(Frictionless, OnALongDescentTheSpeedIsTheHeightFallenAndNothingElse) {
    // With friction gone, what bounds the speed? MEASURED, on the longest descent a 64-tile field
    // holds: level at 500 to corner 7, a ramp falling 20 a tile (gentler than friction holds, so
    // unboosted a tank parks on it) from corner 7 to 57 -- 1000 height units, 15.6 tiles, 31 m --
    // and level at -500 beyond. A parked tank is let go at x = 8 with E held.
    //
    // The bound is ENERGY: a frictionless body that has fallen h has speed sqrt(2 g h) along the
    // ground, whatever the slope it fell down; the sim moves units in the plane, so its horizontal
    // speed is that times cos(slope). No cap is written anywhere, and none is found: on the level
    // at the foot nothing spends the speed at all -- it coasts, until it meets what stops it (the
    // edge of the field here; a wall, a building, a crater, or the key let go, in play).
    const auto h = [](int x, int) { return -20 * (x - 32); };
    const world::ScopedGround guard(field(h));
    Sim sim = tanks({{8, 32}});
    const UnitId id = first_tank(sim);
    sim.enqueue(Order{id, orders::Boost{true}});
    const double t = 20.0 / 64.0;
    const double cosine = 1.0 / std::sqrt(1.0 + (t * t));
    const double x0 = tiles(sim.find(id)->x);
    std::ostringstream rows;
    rows << " tick       x   fallen(m)   speed(raw/tick)  sqrt(2gh)cos   m/s  km/h\n";
    double top = 0.0;
    double at_foot = 0.0;
    std::vector<double> coasting;
    for (int i = 0; i < 400; ++i) {
        const double before = tiles(sim.find(id)->x);
        sim.step();
        const double x = tiles(sim.find(id)->x);
        const double u = (x - before) * 65536.0;
        // Height fallen, from the drawn ground (the ramp's line, clamped at its ends).
        const double fallen_units = 20.0 * (std::min(x, 57.0) - x0);
        const double fallen_tiles = fallen_units / 64.0;
        const double want = std::sqrt(2.0 * kG * fallen_tiles * 65536.0) * cosine;
        const double ms = u / 65536.0 * 2.0 * 30.0;
        if (i % 10 == 9 || x >= 57.0) {
            rows << std::setw(5) << i + 1 << std::fixed << std::setprecision(2) << std::setw(8) << x
                 << std::setw(12) << fallen_tiles * 2.0 << std::setw(18) << std::setprecision(0)
                 << u << std::setw(14) << want << std::setprecision(1) << std::setw(7) << ms
                 << std::setw(6) << ms * 3.6 << "\n";
        }
        top = std::max(top, u);
        if (x < 57.0) {
            if (fallen_tiles > 1.0) {
                EXPECT_NEAR(u, want, want * 0.02)
                    << "tick " << i + 1 << ": the speed is not the height fallen";
            }
            at_foot = u;
        } else if (u > 0.0 && x < 62.0) {
            coasting.push_back(u);
        }
        if (u == 0.0 && x > 57.0) {
            break; // stopped by the field's edge
        }
    }
    std::cout << rows.str() << "top speed " << top << " raw/tick = " << top / 65536.0 * 60.0
              << " m/s, at the foot " << at_foot << "\n";
    // 31 m of fall: sqrt(2 * 9.81 * 31.25) = 24.8 m/s along the ground, 23.6 m/s across the map.
    const double bound = std::sqrt(2.0 * kG * (1000.0 / 64.0) * 65536.0) * cosine;
    EXPECT_NEAR(at_foot, bound, bound * 0.03) << rows.str();
    EXPECT_LE(top, bound * 1.01) << "faster than the height it fell can make it\n" << rows.str();
    ASSERT_FALSE(coasting.empty()) << rows.str();
    for (const double u : coasting) {
        EXPECT_NEAR(u, coasting.front(), 2.0) << "the level spent speed with the grip released";
    }
}

TEST(Frictionless, DrivenDownALongDescentTheEngineAndThePressRideOnTopOfTheFall) {
    // The same descent, DRIVEN east at full throttle with E held, from x = 8. Measured and printed
    // for him to rule on: the speed at the foot is the engine's step plus the press (the normal
    // downhill speed, c + P) plus the frictionless fall's sqrt(2 g h) cos -- the engine does not
    // stop pushing because the ground stopped holding. Bounded by those three and nothing else.
    const auto h = [](int x, int) { return -20 * (x - 32); };
    const world::ScopedGround guard(field(h));
    Helm helm(8, true);
    const double t = 20.0 / 64.0;
    const double press = 13107.0 * (t / std::sqrt(1.0 + (t * t))) * std::sqrt(34.0) / 3.0;
    double top = 0.0;
    int ticks = 0;
    while (ticks < 300 && tiles(helm.sim.find(helm.id)->x) < 56.0) {
        top = std::max(top, static_cast<double>(helm.tick()));
        ++ticks;
    }
    const double fallen = 20.0 * (tiles(helm.sim.find(helm.id)->x) - 8.0) / 64.0;
    const double fall = std::sqrt(2.0 * kG * fallen * 65536.0) / std::sqrt(1.0 + (t * t));
    std::cout << std::fixed << std::setprecision(0) << "driven, boosted: top " << top
              << " raw/tick (" << std::setprecision(1) << top / 65536.0 * 60.0 << " m/s, "
              << top / 65536.0 * 216.0 << " km/h) after " << ticks
              << " ticks; c + P = " << 13107.0 + press << ", the fall alone " << fall << "\n";
    EXPECT_GT(top, 13107.0 + press) << "no faster than the normal downhill speed";
    EXPECT_LE(top, 13107.0 + press + fall + 50.0) << "faster than engine + press + fall";
}

// ---- 5. the grip is the driver's ---------------------------------------------------------------

TEST(Frictionless, TheHullTheDriverLeavesHasItsGripBack) {
    // Two tanks on ramp 20. The player drives A and holds E: A slides. He takes B: A is nobody's
    // to boost, so the ground holds it again and friction stops it on this gentle ramp.
    const world::ScopedGround guard(ramp(20));
    Sim sim = tanks({{20, 20}, {20, 40}});
    const UnitId a = first_tank(sim);
    const UnitId b{a.value + 1};
    sim.enqueue(Order{a, orders::TakeControl{}});
    sim.enqueue(Order{a, orders::Boost{true}});
    for (int i = 0; i < 30; ++i) {
        sim.step();
    }
    ASSERT_GT(sim.find(a)->roll_vx.raw, 0) << "A did not slide while boosted";
    ASSERT_TRUE(sim.find(a)->grip_released);
    sim.enqueue(Order{b, orders::TakeControl{}});
    sim.step();
    EXPECT_FALSE(sim.find(a)->grip_released) << "the hull left behind kept the driver's boost";
    for (int i = 0; i < 200; ++i) {
        sim.step();
    }
    EXPECT_EQ(sim.find(a)->roll_vx.raw, 0) << "A did not come to rest once gripped again";
}

TEST(Frictionless, AReplayOfABoostedSlideIsTheSameSlide) {
    // Determinism: the same boosted slide, run twice from the same orders, hashes alike tick by
    // tick -- and differs from the unboosted run, so the boost is in the state the hash covers.
    const world::ScopedGround guard(ramp(20));
    const auto run = [](bool boost) {
        Sim sim = tanks({{20, 32}});
        if (boost) {
            sim.enqueue(Order{first_tank(sim), orders::Boost{true}});
        }
        std::vector<std::uint64_t> hashes;
        for (int i = 0; i < 60; ++i) {
            sim.step();
            hashes.push_back(sim.state_hash());
        }
        return hashes;
    };
    EXPECT_EQ(run(true), run(true));
    EXPECT_NE(run(true), run(false));
}

} // namespace
} // namespace protekt::sim
