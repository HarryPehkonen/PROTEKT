#include "protekt/game/pick.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "protekt/game/scene.hpp"
#include "protekt/sim/scenario.hpp"

namespace protekt::game {
namespace {

using sim::Scenario;
using sim::Side;
using sim::Spawn;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;

/// The world position the SCENE puts a unit at, derived the same way the mesh does: the tile's
/// origin in metres, standing on the ground the terrain gives that tile. If picking and drawing
/// ever disagree about this, a click lands somewhere the player cannot see, so the test computes it
/// from the public conversion rather than trusting the implementation's copy.
struct WorldPoint {
    float x{};
    float y{};
    float z{};
};

WorldPoint centre_of(const sim::Unit& unit) {
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = height_metres(kSeed, unit.x.floor_int(), unit.y.floor_int());
    const float height = unit.kind == UnitKind::Factory ? 4.5F : 1.8F;
    return WorldPoint{x, ground + (height * 0.5F), z};
}

Camera camera_looking_at(WorldPoint target, float eye_height, float distance) {
    Camera camera{};
    camera.x = target.x;
    camera.y = target.y + eye_height;
    camera.z = target.z - distance;
    camera.yaw = 0.0F; // +Z, straight at the target
    camera.pitch = 0.0F;
    return camera;
}

sim::Sim two_tanks(std::vector<Spawn> spawns) {
    return sim::Sim::create(Scenario{kSeed, std::move(spawns)});
}

const render::Extent kExtent{1280, 720};

} // namespace

// ---------------------------------------------------------------- projection

TEST(Pick, ProjectRoundTripsThroughARayPoint) {
    Camera camera{64.0F, 40.0F, 96.0F, 0.6F, -0.2F};
    struct Case {
        float x;
        float y;
        float d;
    };
    const std::vector<Case> cases{{0.0F, 0.0F, 20.0F},
                                  {640.0F, 360.0F, 50.0F},
                                  {1279.0F, 719.0F, 8.0F},
                                  {100.0F, 600.0F, 120.0F},
                                  {900.0F, 120.0F, 33.0F}};
    for (const Case& c : cases) {
        const RayPoint p = ray_point(camera, kExtent, c.x, c.y, c.d);
        const Projected back = project(camera, kExtent, p.x, p.y, p.z);
        EXPECT_GT(back.depth, 0.0F);
        EXPECT_NEAR(back.x, c.x, 0.5F) << "pixel " << c.x << "," << c.y << " at " << c.d << " m";
        EXPECT_NEAR(back.y, c.y, 0.5F) << "pixel " << c.x << "," << c.y << " at " << c.d << " m";
    }
}

TEST(Pick, ProjectPutsTheCentreOfTheScreenOnTheForwardAxis) {
    Camera camera{10.0F, 5.0F, 10.0F, 0.0F, 0.0F};
    const Projected p = project(camera, kExtent, camera.x, camera.y, camera.z + 30.0F);
    EXPECT_NEAR(p.x, 640.0F, 0.5F);
    EXPECT_NEAR(p.y, 360.0F, 0.5F);
    EXPECT_NEAR(p.depth, 30.0F, 0.01F);
}

TEST(Pick, AUnitBehindTheCameraProjectsBehindIt) {
    Camera camera{10.0F, 5.0F, 100.0F, 0.0F, 0.0F}; // looking towards +Z
    const Projected p = project(camera, kExtent, camera.x, camera.y, camera.z - 20.0F);
    EXPECT_LE(p.depth, 0.0F) << "depth must say 'behind', because the pixel alone cannot";
}

// ---------------------------------------------------------------- units

TEST(Pick, AClickOnTheUnitPicksIt) {
    sim::Sim sim = sim::Sim::create(sim::skirmish(kSeed));
    ASSERT_FALSE(sim.units().empty());
    const sim::Unit& tank = sim.units().front();
    const Camera camera = camera_looking_at(centre_of(tank), 0.0F, 24.0F);
    SceneOptions options{};
    options.seed = kSeed;
    options.camera = camera;

    const Projected p
        = project(camera, kExtent, centre_of(tank).x, centre_of(tank).y, centre_of(tank).z);
    ASSERT_GT(p.depth, 0.0F);
    const Pick hit = pick(sim, options, p.x, p.y);
    ASSERT_TRUE(hit.has_unit()) << "a click on the middle of a tank hit nothing";
    EXPECT_EQ(hit.unit, tank.id);
}

TEST(Pick, AClickWellAwayFromTheUnitPicksNothing) {
    sim::Sim sim = sim::Sim::create(sim::skirmish(kSeed));
    const sim::Unit& tank = sim.units().front();
    const Camera camera = camera_looking_at(centre_of(tank), 0.0F, 24.0F);
    SceneOptions options{};
    options.seed = kSeed;
    options.camera = camera;
    const Projected p
        = project(camera, kExtent, centre_of(tank).x, centre_of(tank).y, centre_of(tank).z);
    // 300 px to the side and 250 px up: far outside a tank at 24 m, and above its box.
    const Pick miss = pick(sim, options, p.x - 300.0F, p.y + 250.0F);
    EXPECT_FALSE(miss.has_unit());
}

TEST(Pick, TheNearerOfTwoAlignedTanksIsPicked) {
    // Two tanks on the same sight line, ONE TILE apart, so their pick boxes (0.75 tiles either
    // side) genuinely overlap and a single ray meets both. The FAR one is spawned first on purpose:
    // units are stored in spawn order, so an implementation that takes the first box the ray meets
    // picks the wrong one. Both of those details are load-bearing -- the first version of this test
    // was 8 tiles apart with the near tank first, and sabotage proved it agreed with the right
    // answer and the wrong one equally.
    sim::Sim sim = two_tanks(
        {{UnitKind::Tank, Side::Player, 40, 41}, {UnitKind::Tank, Side::Player, 40, 40}});
    ASSERT_EQ(sim.units().size(), 2U);
    const sim::Unit& far_tank = sim.units().at(0);
    const sim::Unit& near_tank = sim.units().at(1);
    const Camera camera = camera_looking_at(centre_of(near_tank), 0.0F, 20.0F);
    SceneOptions options{};
    options.seed = kSeed;
    options.camera = camera;

    const Projected p = project(camera, kExtent, centre_of(near_tank).x, centre_of(near_tank).y,
                                centre_of(near_tank).z);
    const Pick hit = pick(sim, options, p.x, p.y);
    ASSERT_TRUE(hit.has_unit());
    EXPECT_EQ(hit.unit, near_tank.id) << "the far tank is behind the near one";
    EXPECT_NE(hit.unit, far_tank.id);
}

TEST(Pick, TwoUnitsOnTheSameTileAreSeparatedByUnitId) {
    sim::Sim sim = two_tanks(
        {{UnitKind::Tank, Side::Player, 40, 40}, {UnitKind::Tank, Side::Player, 40, 40}});
    ASSERT_EQ(sim.units().size(), 2U);
    const Camera camera = camera_looking_at(centre_of(sim.units().at(0)), 0.0F, 20.0F);
    SceneOptions options{};
    options.seed = kSeed;
    options.camera = camera;
    const Projected p = project(camera, kExtent, centre_of(sim.units().at(0)).x,
                                centre_of(sim.units().at(0)).y, centre_of(sim.units().at(0)).z);
    const Pick hit = pick(sim, options, p.x, p.y);
    ASSERT_TRUE(hit.has_unit());
    EXPECT_EQ(hit.unit, sim.units().at(0).id) << "ties must resolve to the LOWER id, always";
}

TEST(Pick, ADeadUnitIsNotPickable) {
    // Three enemy tanks around the one we care about, so it actually DIES: the first version of
    // this test stepped a lone tank 4000 ticks and assumed it had been killed, but its enemy was a
    // hundred tiles away and nothing ever fired. A precondition that never happens is not a test.
    sim::Sim sim = two_tanks({{UnitKind::Tank, Side::Player, 40, 40},
                              {UnitKind::Tank, Side::Enemy, 40, 41},
                              {UnitKind::Tank, Side::Enemy, 40, 42},
                              {UnitKind::Tank, Side::Enemy, 41, 40}});
    const sim::Unit& target = sim.units().at(0);
    const Camera camera = camera_looking_at(centre_of(target), 0.0F, 20.0F);
    SceneOptions options{};
    options.seed = kSeed;
    options.camera = camera;
    const Projected p
        = project(camera, kExtent, centre_of(target).x, centre_of(target).y, centre_of(target).z);
    ASSERT_TRUE(pick(sim, options, p.x, p.y).has_unit());

    // Kill it out from under the pick: a selection must never point at a corpse.
    std::uint32_t ticks = 0;
    while (ticks < 5000U && sim.find(target.id)->alive) {
        sim.step();
        ++ticks;
    }
    ASSERT_FALSE(sim.find(target.id)->alive) << "it never died, so this test would prove nothing";
    const Pick after = pick(sim, options, p.x, p.y);
    EXPECT_FALSE(after.has_unit() && after.unit == target.id);
}

TEST(Pick, AUnitBehindTheCameraIsNeverPicked) {
    sim::Sim sim = two_tanks({{UnitKind::Tank, Side::Player, 40, 40}});
    const sim::Unit& tank = sim.units().at(0);
    Camera camera = camera_looking_at(centre_of(tank), 0.0F, 20.0F);
    camera.yaw = 3.14159265F; // turn around: the tank is now behind the camera
    SceneOptions options{};
    options.seed = kSeed;
    options.camera = camera;
    const Pick nothing = pick(sim, options, 640.0F, 360.0F);
    EXPECT_FALSE(nothing.has_unit());
}

// ---------------------------------------------------------------- ground

TEST(Pick, AClickOnTheGroundReturnsTheTileItLandedOn) {
    sim::Sim sim = two_tanks({{UnitKind::Tank, Side::Player, 40, 40}});
    SceneOptions options{};
    options.seed = kSeed;
    options.camera = Camera{82.0F, 40.0F, 82.0F, 0.0F, -0.5F}; // looking down at ~29 degrees
    const Pick ground = pick(sim, options, 640.0F, 500.0F);    // below centre: the ground, closer
    ASSERT_TRUE(ground.has_tile()) << "a click below the horizon must find ground";
    EXPECT_NEAR(ground.tile_x, 41, 1);
    EXPECT_GE(ground.tile_y, 41);
    EXPECT_GT(ground.distance_tiles, 0.0F);
    EXPECT_LE(ground.distance_tiles, kPickMaxDistanceTiles);
}

TEST(Pick, AClickAboveAHorizonlessCameraFindsNoGround) {
    // Pitch UP: every ray leaves the terrain for the sky, so there is no tile to order anything to.
    sim::Sim sim = two_tanks({{UnitKind::Tank, Side::Player, 40, 40}});
    SceneOptions options{};
    options.seed = kSeed;
    options.camera = Camera{82.0F, 40.0F, 82.0F, 0.0F, 0.4F};
    const Pick sky = pick(sim, options, 640.0F, 100.0F);
    EXPECT_FALSE(sky.has_tile()) << "a ray into the sky must say so, not invent a tile";
}

TEST(Pick, AClickOnAUnitAlsoReportsTheGroundBehindIt) {
    // The player asked one question; both answers are true, and the caller decides which it wants.
    sim::Sim sim = sim::Sim::create(sim::skirmish(kSeed));
    const sim::Unit& tank = sim.units().front();
    const Camera camera = camera_looking_at(centre_of(tank), 0.0F, 24.0F);
    SceneOptions options{};
    options.seed = kSeed;
    options.camera = camera;
    const Projected p
        = project(camera, kExtent, centre_of(tank).x, centre_of(tank).y, centre_of(tank).z);
    const Pick hit = pick(sim, options, p.x, p.y);
    ASSERT_TRUE(hit.has_unit());
    EXPECT_TRUE(hit.has_tile()) << "there is ground under the tank and the click met it";
    EXPECT_LE(hit.distance_tiles, kPickMaxDistanceTiles);
}

// ---------------------------------------------------------------- determinism

TEST(Pick, AHoveringHullIsPickedWhereItIsDrawnNotWhereItWouldSit) {
    // A hovering hull is DRAWN above the ground. A click that meets the hull must find the unit,
    // and a click that meets only the air under it must not: if the pick box stays on the ground
    // while the hull floats, a click just below the tank falls through the gap and picks the unit
    // anyway, and a click on the tank itself lands behind it. The box has to move with the hull.
    sim::Sim sim = sim::Sim::create(sim::skirmish(kSeed));
    const sim::Unit& tank = sim.units().front();
    const WorldPoint hull = centre_of(tank);
    const float ground = height_metres(kSeed, tank.x.floor_int(), tank.y.floor_int());
    const float hull_height = 1.8F;
    const float lift = 1.0F;

    SceneOptions grounded{};
    grounded.seed = kSeed;
    grounded.camera = camera_looking_at(hull, 0.0F, 24.0F);

    // On the ground the hull's midpoint is inside the box: the tank is picked.
    const Projected p_mid = project(grounded.camera, kExtent, hull.x, hull.y, hull.z);
    const Pick mid_hit = pick(sim, grounded, p_mid.x, p_mid.y);
    ASSERT_TRUE(mid_hit.has_unit()) << "a click at the hull's midpoint must find the tank";
    EXPECT_EQ(mid_hit.unit.value, tank.id.value);

    // Now lift the hull a metre, exactly as the scene draws it.
    SceneOptions hovering = grounded;
    hovering.hover = SceneOptions::Hovering{tank.id, ground + lift};

    // A click half a metre up meets the AIR under the raised hull. It must NOT find the tank.
    const float under = ground + 0.5F;
    const Projected p_under = project(hovering.camera, kExtent, hull.x, under, hull.z);
    const Pick under_hit = pick(sim, hovering, p_under.x, p_under.y);
    EXPECT_FALSE(under_hit.has_unit() && under_hit.unit.value == tank.id.value)
        << "with the hull a metre up, a click half a metre up is under the tank, not on it";

    // And the raised hull's own midpoint is still found, because that is where it is drawn.
    const float raised_mid = ground + lift + (hull_height * 0.5F);
    const Projected p_raised = project(hovering.camera, kExtent, hull.x, raised_mid, hull.z);
    const Pick raised_hit = pick(sim, hovering, p_raised.x, p_raised.y);
    ASSERT_TRUE(raised_hit.has_unit()) << "the raised hull must still be pickable";
    EXPECT_EQ(raised_hit.unit.value, tank.id.value);

    // A frame with no hover is unchanged: the same click finds the tank on the ground.
    const Pick plain = pick(sim, grounded, p_mid.x, p_mid.y);
    EXPECT_EQ(plain.unit.value, mid_hit.unit.value)
        << "ABSENT hover must leave picking exactly as it was";
}

TEST(Pick, PickingIsDeterministic) {
    sim::Sim sim = sim::Sim::create(sim::skirmish(kSeed));
    SceneOptions options{};
    options.seed = kSeed;
    options.camera = camera_looking_at(centre_of(sim.units().front()), 4.0F, 24.0F);
    for (int i = 0; i < 400; ++i) {
        const float x = static_cast<float>((i * 37) % 1280);
        const float y = static_cast<float>((i * 53) % 720);
        const Pick a = pick(sim, options, x, y);
        const Pick b = pick(sim, options, x, y);
        ASSERT_EQ(a.unit.value, b.unit.value) << "pixel " << x << "," << y;
        ASSERT_EQ(a.tile_x, b.tile_x) << "pixel " << x << "," << y;
        ASSERT_EQ(a.tile_y, b.tile_y) << "pixel " << x << "," << y;
        ASSERT_EQ(a.distance_tiles, b.distance_tiles) << "pixel " << x << "," << y;
    }
}

} // namespace protekt::game
