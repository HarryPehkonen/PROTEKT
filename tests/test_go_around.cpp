#include "protekt/app/map_file.hpp"
#include "protekt/sim/route.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/world/ground.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>

/// GO AROUND (G1). His report, from play, with a picture: "vehicles will get stuck in places they
/// can't get through ... a Turret trying to get to its assigned deployment spot, but it can't go
/// through the ramp. It should go around."
///
/// The diagnosis written down beforehand was that the navigation steers and does not plan. It was
/// half right: a planner existed (sim/route.hpp) and every unit used it. What it did not know was
/// what the MOVER does with a slope -- the slide throws a unit off the edge rows of a ramp, and
/// beats a climb the rule calls legal -- so it led a unit along a ramp's rim and up a face the
/// slide sends it back down, and then nothing noticed the unit was not getting anywhere. These
/// tests pin the repairs: a route the mover can follow, a refusal that says so, and a rule that
/// notices.
namespace protekt::sim {
namespace {

constexpr int kArena = 48;

/// A flat, open, empty map of `kArena` tiles square, to be drawn on.
[[nodiscard]] app::MapDoc arena() {
    app::MapDoc map = app::generate_map(1, kArena);
    for (std::size_t i = 0; i < map.terrain.size(); ++i) {
        map.terrain[i] = world::Terrain::Flat;
        map.heights[i] = 0;
        map.solid[i] = 0;
    }
    map.spawns.clear();
    map.volumes.clear();
    return map;
}

/// A ring of solid tiles round (24, 24): a room with no door.
void close_the_room(app::MapDoc& map) {
    for (int i = 20; i <= 28; ++i) {
        for (const int fixed : {20, 28}) {
            map.solid[map.index(i, fixed)] = 1;
            map.solid[map.index(fixed, i)] = 1;
        }
    }
}

[[nodiscard]] app::MapDoc shipped_map() {
    const app::MapParse parsed
        = app::load_map_file(std::string{PROTEKT_SOURCE_DIR} + "/assets/maps/proving_ground.map");
    EXPECT_TRUE(parsed.ok) << parsed.error;
    return parsed.map;
}

/// One player unit and a far-off enemy factory (so the battle has two sides), on whatever ground is
/// installed. The factory is eight tiles square and stands at (`base_at`, `base_at`): out of the
/// way of everything these tests drive across.
[[nodiscard]] Sim one_unit(UnitKind kind, int x, int y, int base_at = 40) {
    Scenario scenario{};
    scenario.seed = 0;
    Spawn unit{};
    unit.side = Side::Player;
    unit.kind = kind;
    unit.tile_x = x;
    unit.tile_y = y;
    scenario.spawns.push_back(unit);
    Spawn base{};
    base.side = Side::Enemy;
    base.kind = UnitKind::Factory;
    base.tile_x = base_at;
    base.tile_y = base_at;
    scenario.spawns.push_back(base);
    return Sim::create(scenario);
}

[[nodiscard]] UnitId player_unit(const Sim& sim) {
    for (const Unit& unit : sim.units()) {
        if (unit.side == Side::Player) {
            return unit.id;
        }
    }
    return UnitId{};
}

[[nodiscard]] double tiles(Fixed v) { return static_cast<double>(v.raw) / Fixed::kOne; }

struct Drive {
    int ticks{-1};         ///< the tick it arrived on; -1 when it never did
    double driven{};       ///< tiles actually travelled, summed tick by tick
    double final_gap{1e9}; ///< tiles from the goal point at the end
};

/// Drives `id` under whatever order it has until `done` or `limit` ticks, summing the distance it
/// covers.
template <class Done> [[nodiscard]] Drive run(Sim& sim, UnitId id, int limit, const Done& done) {
    Drive drive{};
    double last_x = tiles(sim.find(id)->x);
    double last_y = tiles(sim.find(id)->y);
    for (int tick = 1; tick <= limit; ++tick) {
        sim.step();
        const Unit* unit = sim.find(id);
        drive.driven += std::hypot(tiles(unit->x) - last_x, tiles(unit->y) - last_y);
        last_x = tiles(unit->x);
        last_y = tiles(unit->y);
        if (done(*unit)) {
            drive.ticks = tick;
            break;
        }
    }
    return drive;
}

[[nodiscard]] bool within(const Unit& unit, double x, double y, double tiles_away) {
    return std::hypot(tiles(unit.x) - x, tiles(unit.y) - y) <= tiles_away;
}

} // namespace

// ------------------------------------------------------------------ the reported case, on the map

TEST(GoAround, ATurretOrderedToTheFarSideOfTheRampDeploysThere) {
    // His case: a Turret 14 tiles from its deployment spot, with a cliff between. The proving
    // ground's plateau is walled and has ONE way up, the ramp at x 16..21, y 28..32; the turret
    // starts at (12, 20), due west of the spot at (26, 20), and must go south to the ramp, up it
    // and back north. Before the fix it drove up the ramp's north edge, was thrown off by the slide
    // onto the flat strip under the cliff, and pressed on the cliff face for ever, at (21.97,
    // 26.50): stance Deploy, DEPLOYING on the banner, never deploying.
    const world::ScopedGround guard(app::map_ground(shipped_map()));
    Sim sim = one_unit(UnitKind::Turret, 12, 20, 56);
    const UnitId turret = player_unit(sim);
    sim.enqueue(Order{turret, orders::Deploy{26, 20}});
    const Drive drive
        = run(sim, turret, 3000, [](const Unit& u) { return u.kind == UnitKind::DeployedTurret; });
    ASSERT_GT(drive.ticks, 0) << "it never deployed: it is at (" << tiles(sim.find(turret)->x)
                              << ", " << tiles(sim.find(turret)->y) << ")";
    EXPECT_TRUE(within(*sim.find(turret), 26.5, 20.5, 1.1)) << "deployed within a tile of its spot";
    EXPECT_EQ(sim.find(turret)->route.failure, RouteFailure::None);
    // RE-MEASURED, G6 (the press): 835 -> 900. The way is up the ramp (x 16..21, rising 20 a tile
    // east), and a turret crawls at a quarter of a tank's step: on that ramp the press takes 58%
    // of its step (the old grade took 31%), so the climb is slower. The route is the same tiles
    // up the ramp -- (16, 28), (19, 29), (22, 29) -- and no longer cuts the corner of its flank
    // at (17, 27) (the corner rule, `edge_blocked`). Old value 835 is the code at 896f2e5.
    // RE-MEASURED, G7: 900 -> 882. THE TRIANGLES move it, not the roll (with G6's parked drift
    // compiled back in and the triangles kept it is 882 too; with G6's quad-mean ground as well,
    // 900): the ramp's tiles are drawn as two triangles each, and the press the turret crawls
    // against is the triangle it is on, not the quad's mean. Old value 900 is the code at 44bcc4a.
    EXPECT_EQ(drive.ticks, 882) << "ticks to deploy";
}

TEST(GoAround, ItsRouteIsNotTheStraightLineItWentAround) {
    // The spot is 14 tiles away as the crow flies; the way there is through the ramp, so what the
    // turret drives is much longer. The number is asserted, not a path shape.
    const world::ScopedGround guard(app::map_ground(shipped_map()));
    Sim sim = one_unit(UnitKind::Turret, 12, 20, 56);
    const UnitId turret = player_unit(sim);
    sim.enqueue(Order{turret, orders::Deploy{26, 20}});
    const double straight = std::hypot(26.5 - 12.5, 20.5 - 20.5);
    const Drive drive
        = run(sim, turret, 3000, [](const Unit& u) { return u.kind == UnitKind::DeployedTurret; });
    ASSERT_GT(drive.ticks, 0);
    // RE-MEASURED, G6: the lower bound was 1.8 x straight (25.2 tiles), a margin under the old
    // measurement; the turret now drives 24.27. The bound is restated as what the map allows: the
    // ramp's rows 28..32 are the only way up, and between x 16 and 22 a unit must stay in them (the
    // faces of row 27 rise 40 to 120 a tile south: walls), so no drive there is shorter than the
    // line (12.5, 20.5) -> (16, 28) -> (22, 28) -> (26.5, 20.5): 8.28 + 6 + 8.73 = 23.0 tiles.
    const double through_the_ramp
        = std::hypot(16.0 - 12.5, 28.0 - 20.5) + 6.0 + std::hypot(26.5 - 22.0, 28.0 - 20.5);
    EXPECT_GE(drive.driven, through_the_ramp)
        << "straight " << straight << " tiles, driven " << drive.driven;
    EXPECT_LT(drive.driven, straight * 3.0) << "and it did not wander: driven " << drive.driven;
}

TEST(GoAround, ATurretComesDownOffThePlateauAndAcrossTheFootOfTheRamp) {
    // The other direction, the one that used to end pressed against the ramp's rim from below: a
    // turret on the plateau sent to the open ground south-west of it. It stood at (17, 27), under
    // an edge of the ramp too steep for the slide to let it climb, with a route that said go on.
    const world::ScopedGround guard(app::map_ground(shipped_map()));
    Sim sim = one_unit(UnitKind::Turret, 26, 20, 56);
    const UnitId turret = player_unit(sim);
    sim.enqueue(Order{turret, orders::Deploy{10, 30}});
    const Drive drive
        = run(sim, turret, 3000, [](const Unit& u) { return u.kind == UnitKind::DeployedTurret; });
    ASSERT_GT(drive.ticks, 0) << "stuck at (" << tiles(sim.find(turret)->x) << ", "
                              << tiles(sim.find(turret)->y) << ")";
    EXPECT_TRUE(within(*sim.find(turret), 10.5, 30.5, 1.1)) << "deployed within a tile of its spot";
}

TEST(GoAround, AGoalOnTheRimItCannotHoldIsApproachedAndNotLoopedOn) {
    // (39, 41) is the plateau's rim above the ambush pocket: a tile the slope throws a unit off. A
    // tank sent there used to arrive, be thrown down the cliff, plan the hundred-tile way round
    // and arrive again, for ever. It now ends beside the rim, on the plateau, and stays.
    const world::ScopedGround guard(app::map_ground(shipped_map()));
    Sim sim = one_unit(UnitKind::Tank, 38, 57, 50);
    const UnitId tank = player_unit(sim);
    sim.enqueue(Order{tank, orders::MoveTo{Fixed::from_int(39), Fixed::from_int(41)}});
    for (int tick = 0; tick < 3000; ++tick) {
        sim.step();
    }
    const Unit* unit = sim.find(tank);
    EXPECT_EQ(unit->route.failure, RouteFailure::None);
    EXPECT_TRUE(within(*unit, 39.5, 42.5, 3.0))
        << "it is at (" << tiles(unit->x) << ", " << tiles(unit->y) << ")";
    EXPECT_EQ(unit->vx.raw, 0) << "and it is standing, not going round again";
    EXPECT_EQ(unit->vy.raw, 0);
}

// ------------------------------------------------------------------------------ the refusals

TEST(GoAround, AUnitWithNoPossibleRouteRefusesAndSaysSo) {
    // A walled room: a ring of solid tiles, a goal inside it. There is no way in. The tank used to
    // press on the wall for ever with its order standing; it now ends the order on the first
    // tick and the unit's route says why.
    app::MapDoc map = arena();
    close_the_room(map);
    const world::ScopedGround guard(app::map_ground(map));
    Sim sim = one_unit(UnitKind::Tank, 5, 5);
    const UnitId tank = player_unit(sim);
    sim.enqueue(Order{tank, orders::MoveTo{Fixed::from_int(24), Fixed::from_int(24)}});
    for (int tick = 0; tick < 10; ++tick) {
        sim.step();
    }
    const Unit* unit = sim.find(tank);
    EXPECT_EQ(unit->route.failure, RouteFailure::NoRoute);
    EXPECT_EQ(unit->stance, Stance::Idle) << "the order is over: it is not still pushing";
    EXPECT_EQ(unit->route.count, 0);
    EXPECT_TRUE(within(*unit, 5.5, 5.5, 1.0)) << "and it did not drive off at the wall";
}

TEST(GoAround, AnOrderAfterARefusalStartsFresh) {
    // The refusal is a verdict on that order, not a brand on the unit: told to go somewhere it
    // can, it goes, and the failure is cleared.
    app::MapDoc map = arena();
    close_the_room(map);
    const world::ScopedGround guard(app::map_ground(map));
    Sim sim = one_unit(UnitKind::Tank, 5, 5);
    const UnitId tank = player_unit(sim);
    sim.enqueue(Order{tank, orders::MoveTo{Fixed::from_int(24), Fixed::from_int(24)}});
    for (int tick = 0; tick < 5; ++tick) {
        sim.step();
    }
    ASSERT_EQ(sim.find(tank)->route.failure, RouteFailure::NoRoute);
    sim.enqueue(Order{tank, orders::MoveTo{Fixed::from_int(15), Fixed::from_int(5)}});
    const Drive drive
        = run(sim, tank, 400, [](const Unit& u) { return within(u, 15.0, 5.0, 1.0); });
    EXPECT_GT(drive.ticks, 0);
    EXPECT_EQ(sim.find(tank)->route.failure, RouteFailure::None);
}

TEST(GoAround, ADeployOnGroundItCannotHoldIsRefusedByName) {
    // (45, 42) is the plateau's east rim, one tile from the drop. A turret that deployed there
    // would be thrown off before its gun came up; the order is refused and says why.
    const world::ScopedGround guard(app::map_ground(shipped_map()));
    Sim sim = one_unit(UnitKind::Turret, 26, 28, 56);
    const UnitId turret = player_unit(sim);
    sim.enqueue(Order{turret, orders::Deploy{45, 42}});
    for (int tick = 0; tick < 10; ++tick) {
        sim.step();
    }
    const Unit* unit = sim.find(turret);
    EXPECT_EQ(unit->route.failure, RouteFailure::NoRoute);
    EXPECT_EQ(unit->stance, Stance::Idle);
    EXPECT_EQ(unit->kind, UnitKind::Turret);
}

TEST(GoAround, TheNoProgressRuleNamesAUnitThatStaysInOnePlace) {
    // The backstop for whatever the planner did not foresee. A unit that is driving and has not
    // left a one-tile circle for kStallTicks is stuck, however it moves inside the circle: here it
    // paces back and forth, which is the Constructor's old symptom.
    Route route{};
    std::int32_t x = 5 * Fixed::kOne;
    const std::int32_t y = 5 * Fixed::kOne;
    for (int tick = 1; tick < kStallTicks; ++tick) {
        x = 5 * Fixed::kOne + (tick % 2 == 0 ? Fixed::kOne / 4 : -Fixed::kOne / 4);
        ASSERT_FALSE(route_stalled(route, x, y)) << "tick " << tick;
    }
    EXPECT_EQ(route.failure, RouteFailure::None);
    EXPECT_TRUE(route_stalled(route, x, y)) << "the kStallTicks-th tick inside the circle";
    EXPECT_EQ(route.failure, RouteFailure::NoProgress);
}

TEST(GoAround, TheNoProgressRuleLetsAUnitThatKeepsMovingAlone) {
    // Leaving the circle moves the anchor: a unit crawling at a turret's pace (a tile in thirty
    // ticks) is never stalled however long the trip.
    Route route{};
    std::int32_t x = 5 * Fixed::kOne;
    for (int tick = 0; tick < 20 * kStallTicks; ++tick) {
        x += Fixed::kOne / 30;
        ASSERT_FALSE(route_stalled(route, x, 5 * Fixed::kOne)) << "tick " << tick;
    }
    EXPECT_EQ(route.failure, RouteFailure::None);
}

TEST(GoAround, AGoalBehindAWallWithNoGapIsRefusedByNameAcrossTheWholeMap) {
    // No room, no ring: a wall across the whole map. The first plan searches every tile it can
    // reach, finds none on the far side, and the order ends there -- not after a long walk at it.
    app::MapDoc map = arena();
    for (int y = 0; y < kArena; ++y) {
        map.solid[map.index(24, y)] = 1; // a wall across the whole map: no gap at all
    }
    const world::ScopedGround guard(app::map_ground(map));
    Sim sim = one_unit(UnitKind::Tank, 8, 20);
    const UnitId tank = player_unit(sim);
    sim.enqueue(Order{tank, orders::MoveTo{Fixed::from_int(36), Fixed::from_int(20)}});
    for (int tick = 0; tick < 10; ++tick) {
        sim.step();
    }
    EXPECT_EQ(sim.find(tank)->route.failure, RouteFailure::NoRoute);
    EXPECT_EQ(sim.find(tank)->stance, Stance::Idle);
}

TEST(GoAround, AConstructorBuildsOnTheFarSideOfABuildingAndDoesNotPace) {
    // The owner's old report, "the Constructor gets stuck behind buildings ... sideways, back and
    // forth": a site whose centre is inside a footprint is a goal no tile reaches, so there was no
    // plan, and it steered straight at the site -- into the factory standing between them -- and
    // slid along its wall for ever. The plan now ends beside the site, round the factory.
    const world::ScopedGround guard(app::map_ground(arena()));
    Scenario scenario{};
    scenario.seed = 0;
    scenario.store_capacity = 400;
    scenario.start_ore = 100;
    Spawn builder{};
    builder.side = Side::Player;
    builder.kind = UnitKind::Constructor;
    builder.tile_x = 6;
    builder.tile_y = 22;
    scenario.spawns.push_back(builder);
    Spawn factory{};
    factory.side = Side::Player;
    factory.kind = UnitKind::Factory;
    factory.tile_x = 14;
    factory.tile_y = 18;
    scenario.spawns.push_back(factory);
    Spawn base{};
    base.side = Side::Enemy;
    base.kind = UnitKind::Factory;
    base.tile_x = 40;
    base.tile_y = 40;
    scenario.spawns.push_back(base);
    Sim sim = Sim::create(scenario);
    const UnitId constructor = sim.units().at(0).id;
    sim.enqueue(Order{constructor, orders::Place{UnitKind::Power, 30, 21, false}});
    const auto power = [&sim]() -> const Unit* {
        for (const Unit& u : sim.units()) {
            if (u.alive && u.kind == UnitKind::Power) {
                return &u;
            }
        }
        return nullptr;
    };
    int done = -1;
    for (int tick = 1; tick <= 3000 && done < 0; ++tick) {
        sim.step();
        if (power() != nullptr && power()->build_left == 0) {
            done = tick;
        }
    }
    ASSERT_NE(power(), nullptr) << "the site was never placed";
    EXPECT_GT(done, 0) << "the Constructor never finished it: it is at ("
                       << tiles(sim.find(constructor)->x) << ", " << tiles(sim.find(constructor)->y)
                       << ")";
    EXPECT_EQ(sim.find(constructor)->route.failure, RouteFailure::None);
}

// ------------------------------------------------------------------------------ the controls

TEST(GoAround, StraightLineOnOpenGroundIsUnchangedByTheNumberOfTicks) {
    // The regression control: a tank with nothing in its way takes the direct route and arrives
    // in exactly as many ticks as it did before the planner knew about slopes. The old code took
    // 146 (see the report); a wall-less arena has no height, no rim and no cliff for the
    // new rules to touch.
    const world::ScopedGround guard(app::map_ground(arena()));
    Sim sim = one_unit(UnitKind::Tank, 6, 20);
    const UnitId tank = player_unit(sim);
    sim.enqueue(Order{tank, orders::MoveTo{Fixed::from_int(36), Fixed::from_int(20)}});
    const Drive drive
        = run(sim, tank, 600, [](const Unit& u) { return within(u, 36.0, 20.0, 1.0); });
    ASSERT_GT(drive.ticks, 0);
    EXPECT_EQ(drive.ticks, 146);
    EXPECT_NEAR(drive.driven, 29.0, 1.5) << "a straight line, give or take the arrival radius";
}

} // namespace protekt::sim
