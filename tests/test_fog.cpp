#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <set>
#include <utility>
#include <vector>

#include "protekt/game/nav_marks.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

// DISTANCE FOG: aerial perspective. The ground dissolves into the sky's own horizon colour with
// distance, applied where the scene's vertices are built, to what the WORLD draws and nothing the
// HUD draws. Fog is paint: a pure function of distance and colour, never world state.

namespace {

using protekt::game::build_scene;
using protekt::game::fog_far_metres;
using protekt::game::fogged;
using protekt::game::ground_metres;
using protekt::game::kFogNearMetres;
using protekt::game::kMetresPerTile;
using protekt::game::kSkyHorizonRing;
using protekt::game::kSkyRingColours;
using protekt::game::kSkyVertexCount;
using protekt::game::kTerrainLight;
using protekt::game::Scene;
using protekt::game::SceneOptions;
using protekt::game::SceneVertex;
using protekt::math::Vec3;
using protekt::sim::Side;
using protekt::sim::Spawn;
using protekt::sim::UnitKind;

constexpr int kRadius = 20;
const Vec3& horizon() { return kSkyRingColours.at(kSkyHorizonRing); }

float distance_to(const Vec3& a, const Vec3& b) {
    return std::sqrt(((a[0] - b[0]) * (a[0] - b[0])) + ((a[1] - b[1]) * (a[1] - b[1]))
                     + ((a[2] - b[2]) * (a[2] - b[2])));
}

/// How far a colour still is from the fog: 0 is fully fogged.
float from_fog(const Vec3& colour) { return distance_to(colour, horizon()); }

SceneOptions rig() {
    SceneOptions o;
    o.seed = 42;
    o.radius_tiles = kRadius;
    o.camera.x = 100.0F;
    o.camera.y = 8.0F;
    o.camera.z = 100.0F;
    o.show_ship_gauge = false;
    o.show_ledger = false;
    return o;
}

std::size_t terrain_vertices() {
    const auto side = static_cast<std::size_t>(kRadius) * 2U + 2U;
    return side * side * 6U;
}

float eye_distance(const SceneOptions& o, const SceneVertex& v) {
    return distance_to(v.position, Vec3{o.camera.x, o.camera.y, o.camera.z});
}

protekt::sim::Sim one_unit(UnitKind kind, Side side, int tile_x, int tile_y) {
    protekt::sim::Scenario scenario;
    scenario.seed = 42;
    scenario.spawns.push_back(Spawn{kind, side, tile_x, tile_y});
    return protekt::sim::Sim::create(scenario);
}

// The function ------------------------------------------------------------------------------

TEST(Fog, ANearVertexKeepsItsExactColour) {
    const float far = fog_far_metres(kRadius);
    for (const Vec3& colour : {Vec3{0.1F, 0.7F, 0.3F}, Vec3{1.0F, 1.0F, 1.0F}, Vec3{0, 0, 0}}) {
        EXPECT_EQ(fogged(colour, 0.0F, far), colour);
        EXPECT_EQ(fogged(colour, kFogNearMetres * 0.5F, far), colour);
        EXPECT_EQ(fogged(colour, kFogNearMetres, far), colour)
            << "AT the near distance is still near: a thing driving toward the camera must not "
               "thin out";
    }
}

TEST(Fog, AVertexAtOrBeyondTheFarDistanceIsTheHorizonColourExactly) {
    const float far = fog_far_metres(kRadius);
    for (const Vec3& colour : {Vec3{0.1F, 0.7F, 0.3F}, Vec3{1.0F, 1.0F, 1.0F}, Vec3{0, 0, 0}}) {
        for (const float distance : {far, far + 0.001F, far * 1.5F, 1.0e6F}) {
            EXPECT_EQ(fogged(colour, distance, far), horizon())
                << "bitwise, not close: the rim must be the sky's own colour or a seam shows";
        }
    }
}

TEST(Fog, TheFarEndIsTheRadiusTheSceneCullsTo) {
    EXPECT_EQ(fog_far_metres(kRadius), static_cast<float>(kRadius) * kMetresPerTile)
        << "the fog and the cull must agree, or the rim ends in a line against the sky";
}

TEST(Fog, FartherIsStrictlyNearerTheFogBetweenTheTwoEnds) {
    const float far = fog_far_metres(kRadius);
    for (const Vec3& colour : {Vec3{0.1F, 0.7F, 0.3F}, Vec3{0, 0, 0}, Vec3{1, 1, 1}}) {
        float previous = from_fog(colour) + 1.0F;
        for (float d = kFogNearMetres + 1.0F; d < far; d += 3.0F) {
            const float now = from_fog(fogged(colour, d, far));
            EXPECT_LT(now, previous) << "at " << d << " m: a farther vertex is hazier";
            previous = now;
        }
    }
}

TEST(Fog, TheRampIsThickEnoughToSeeWhereThePlayerLooks) {
    // Harri, 2026-10-30: "Can you make the fog thicker?" -- after "I couldn't tell there was fog."
    // Measured with the previous ramp on a 1280x720 capture of the live view: the fog reached 111
    // of 720 rows and the whole frame differed from a fog-free render by 0.011/255, because with a
    // 12 m start the ground nearer than 19 m -- most of the frame -- was untouched and the `t^2`
    // ease kept the rest crisp. This rig meshes 20 tiles, so its fog is solid at 40 m; the numbers
    // below are what a thicker ramp gives there. The same ramp under `t^2` gives 0.18 and 0.51,
    // which is the invisible thing this test exists to stop coming back.
    const float far = fog_far_metres(kRadius);
    const auto fraction = [&](float distance) {
        return distance_to(fogged(kTerrainLight, distance, far), kTerrainLight)
               / distance_to(horizon(), kTerrainLight);
    };
    EXPECT_EQ(fraction(kFogNearMetres), 0.0F) << "at the near distance the ground is untouched";
    EXPECT_EQ(fraction(4.0F), 0.0F) << "and inside it";
    EXPECT_NEAR(fraction(20.0F), 0.43F, 0.02F) << "the near-mid ground must be visibly hazed";
    EXPECT_NEAR(fraction(30.0F), 0.71F, 0.02F) << "the mid ground is most of the way to the sky";
    EXPECT_EQ(fraction(far), 1.0F) << "and the rim is the horizon colour exactly";
}

// The scene ----------------------------------------------------------------------------------

TEST(Fog, TheTerrainFadesFromItsOwnPaintToTheHorizon) {
    SceneOptions o = rig();
    // The driver's eye is low: at the fog's near distance the ground beneath it must still be there
    // unfogged, and a camera 8 m up (as `rig()` leaves it) has no terrain vertex within 5 m of it
    // at all, which is how this test caught the ramp's change.
    o.camera.y = ground_metres(o.seed, 50.0F, 50.0F) + 1.0F;
    const auto sim = one_unit(UnitKind::Tank, Side::Player, 50, 50);
    const Scene scene = build_scene(sim, o);
    const float far = fog_far_metres(kRadius);
    std::size_t near_count = 0;
    std::size_t far_count = 0;
    std::size_t between = 0;
    for (std::size_t i = 0; i < terrain_vertices(); ++i) {
        const SceneVertex& v = scene.vertices.at(i);
        const float d = eye_distance(o, v);
        if (d <= kFogNearMetres) {
            ++near_count;
            // Untouched by the fog: still the grey of the slope's light (a quad's light is its
            // own, so only a FLAT quad is exactly white), never tinted toward the horizon.
            EXPECT_EQ(v.colour[0], v.colour[1]) << "inside the near distance, terrain is unfogged";
            EXPECT_EQ(v.colour[1], v.colour[2]) << "inside the near distance, terrain is unfogged";
            EXPECT_GT(v.colour[0], 0.85F);
            EXPECT_LT(v.colour[0], 1.05F);
        } else if (d >= far) {
            ++far_count;
            EXPECT_EQ(v.colour, horizon()) << "at the rim the terrain IS the horizon";
        } else {
            ++between;
            if (d > kFogNearMetres + 2.0F) {
                EXPECT_NE(v.colour, kTerrainLight)
                    << "a ramp starts from nothing: only past 2 m of it is it visible";
            }
            EXPECT_NE(v.colour, horizon());
        }
        EXPECT_EQ(v.alpha, 1.0F) << "fog moves colour, never alpha";
    }
    EXPECT_GT(near_count, 0U);
    EXPECT_GT(far_count, 0U);
    EXPECT_GT(between, 0U);
}

TEST(Fog, TheSkyIsNotFogged) {
    const SceneOptions o = rig();
    const auto sim = one_unit(UnitKind::Tank, Side::Player, 50, 50);
    const Scene scene = build_scene(sim, o);
    EXPECT_EQ(scene.vertices.at(terrain_vertices()).colour, kSkyRingColours.at(0))
        << "the sky is what the fog fades TO; fogging it would repaint its own gradient";
    EXPECT_NE(scene.vertices.at(terrain_vertices() + kSkyVertexCount - 1).colour, horizon());
}

TEST(Fog, AHullBeyondTheFarDistanceIsTheHorizonColour) {
    const SceneOptions o = rig();
    // 20 tiles on both axes: inside the square the scene meshes, 57 m out, past the 40 m fog.
    const auto sim = one_unit(UnitKind::Tank, Side::Enemy, 70, 70);
    const Scene scene = build_scene(sim, o);
    const std::size_t first = terrain_vertices() + kSkyVertexCount;
    ASSERT_GT(scene.vertices.size(), first) << "the tank was not drawn: the rig is wrong";
    for (std::size_t i = first; i < scene.vertices.size(); ++i) {
        const SceneVertex& v = scene.vertices.at(i);
        ASSERT_GT(eye_distance(o, v), fog_far_metres(kRadius));
        EXPECT_EQ(v.colour, horizon()) << "the hull and its shadow belong to the world: fogged";
    }
}

TEST(Fog, ANavBeaconBeyondTheFarDistanceIsTheHorizonColour) {
    SceneOptions o = rig();
    protekt::game::NavMarks marks;
    ASSERT_EQ(marks.place(70, 50).outcome, protekt::game::NavMarks::Outcome::Placed);
    o.nav_marks = &marks;
    const auto sim = one_unit(UnitKind::Tank, Side::Player, 50, 50);
    const Scene without = build_scene(sim, rig());
    const Scene with = build_scene(sim, o);
    ASSERT_GT(with.vertices.size(), without.vertices.size()) << "no beacon was drawn";
    for (std::size_t i = without.vertices.size(); i < with.vertices.size(); ++i) {
        EXPECT_GT(eye_distance(o, with.vertices.at(i)), fog_far_metres(kRadius));
        EXPECT_EQ(with.vertices.at(i).colour, horizon());
    }
}

// The boundary: the HUD ----------------------------------------------------------------------

/// Two enemy tanks 24 and 32 m in front of the camera -- both past the near distance, so
/// inside the fog's ramp -- and the viewer, who wears the HUD.
struct Rig {
    protekt::sim::Sim sim;
    SceneOptions options;
    protekt::sim::UnitId nearest_enemy;
    protekt::sim::UnitId farthest_enemy;
};

Rig hud_rig() {
    protekt::sim::Scenario scenario;
    scenario.seed = 42;
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Player, 48, 50});
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Enemy, 50, 50});
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Enemy, 52, 54});
    auto sim = protekt::sim::Sim::create(scenario);
    SceneOptions o = rig();
    o.camera.x = 50.0F * kMetresPerTile;
    o.camera.z = 38.0F * kMetresPerTile;
    o.camera.y = 14.0F;
    o.camera.pitch = -0.4F;
    o.viewer_unit = sim.units().front().id;
    o.extent = {800, 450};
    const protekt::sim::UnitId nearest = sim.units().at(1).id;
    const protekt::sim::UnitId farthest = sim.units().at(2).id;
    return Rig{std::move(sim), o, nearest, farthest};
}

using Palette = std::set<std::pair<std::array<float, 3>, float>>;

/// The distinct (colour, alpha) pairs of a run of vertices.
Palette palette_of(const Scene& scene, std::size_t first) {
    Palette palette;
    for (std::size_t i = first; i < scene.vertices.size(); ++i) {
        palette.emplace(scene.vertices.at(i).colour, scene.vertices.at(i).alpha);
    }
    return palette;
}

TEST(Fog, AHealthBarOverADistantUnitIsAsLegibleAsOneOverANearUnit) {
    const Rig r = hud_rig();
    const Scene scene = build_scene(r.sim, r.options);
    const protekt::sim::Unit* near_unit = r.sim.find(r.nearest_enemy);
    const protekt::sim::Unit* far_unit = r.sim.find(r.farthest_enemy);
    ASSERT_NE(near_unit, nullptr);
    ASSERT_NE(far_unit, nullptr);
    const auto metres = [&](const protekt::sim::Unit& u) {
        constexpr float kOne = static_cast<float>(protekt::sim::Fixed::kOne);
        return std::hypot(
            (static_cast<float>(u.x.raw) / kOne * kMetresPerTile) - r.options.camera.x,
            (static_cast<float>(u.y.raw) / kOne * kMetresPerTile) - r.options.camera.z);
    };
    ASSERT_GT(metres(*near_unit), kFogNearMetres) << "the near unit must already be in the haze";
    ASSERT_GT(metres(*far_unit), metres(*near_unit) + 4.0F);

    // Both bars are 18 vertices at the tail: outline, plate, fill. The far unit's is the second.
    const std::size_t bar = 18;
    ASSERT_GE(scene.vertices.size(), 2 * bar);
    const std::size_t end = scene.vertices.size();
    const auto colours = [&](std::size_t first) {
        std::vector<std::array<float, 3>> out;
        for (std::size_t i = first; i < first + bar; i += 6) {
            out.push_back(scene.vertices.at(i).colour);
        }
        return out;
    };
    EXPECT_EQ(colours(end - bar), colours(end - (2 * bar)))
        << "a bar is a metre label: the same outline, plate and fill at any range";
    EXPECT_EQ(colours(end - bar).at(0), (std::array<float, 3>{0.0F, 0.0F, 0.0F}));
    EXPECT_EQ(colours(end - bar).at(2), (std::array<float, 3>{1.0F, 0.25F, 0.25F}));
    for (std::size_t i = end - (2 * bar); i < end; ++i) {
        EXPECT_EQ(scene.vertices.at(i).alpha, 1.0F);
    }
}

TEST(Fog, TheAttackReadoutOverADistantUnitIsAsLegibleAsOneOverANearUnit) {
    const Rig r = hud_rig();
    const auto readout = [&](protekt::sim::UnitId target) {
        SceneOptions with = r.options;
        with.attack_target = target;
        const Scene base = build_scene(r.sim, r.options);
        const Scene scene = build_scene(r.sim, with);
        EXPECT_GT(scene.vertices.size(), base.vertices.size()) << "no readout was drawn";
        return palette_of(scene, base.vertices.size());
    };
    EXPECT_EQ(readout(r.farthest_enemy), readout(r.nearest_enemy))
        << "the label and its health bar are HUD: range changes where they sit, never their ink";
}

TEST(Fog, AHudPanelIsTheSameInkWhereverTheCameraStands) {
    // The ledger is a panel on the HUD layer. Move the camera far from the action and across other
    // ground: the panel's colours and alphas must not move with the world behind it.
    const Rig r = hud_rig();
    SceneOptions here = r.options;
    here.show_ledger = true;
    SceneOptions there = here;
    there.camera.x += 90.0F;
    there.camera.z += 90.0F;
    SceneOptions off = r.options;
    const std::size_t prefix_here = build_scene(r.sim, off).vertices.size();
    off.camera = there.camera;
    const std::size_t prefix_there = build_scene(r.sim, off).vertices.size();
    const Palette a = palette_of(build_scene(r.sim, here), prefix_here);
    const Palette b = palette_of(build_scene(r.sim, there), prefix_there);
    ASSERT_FALSE(a.empty()) << "no ledger was drawn";
    EXPECT_EQ(a, b);
}

// Determinism --------------------------------------------------------------------------------

TEST(Fog, TheSameSimAndOptionsMakeTheSameVerticesTwice) {
    const Rig r = hud_rig();
    const Scene first = build_scene(r.sim, r.options);
    const Scene second = build_scene(r.sim, r.options);
    ASSERT_EQ(first.vertices.size(), second.vertices.size());
    for (std::size_t i = 0; i < first.vertices.size(); ++i) {
        const SceneVertex& a = first.vertices.at(i);
        const SceneVertex& b = second.vertices.at(i);
        ASSERT_TRUE(a.position == b.position && a.colour == b.colour && a.alpha == b.alpha
                    && a.u == b.u && a.v == b.v)
            << "vertex " << i;
    }
}

TEST(Fog, DrawingTheSceneLeavesTheSimHashWhereItWas) {
    const Rig r = hud_rig();
    const std::uint64_t before = r.sim.state_hash();
    (void)build_scene(r.sim, r.options);
    EXPECT_EQ(r.sim.state_hash(), before) << "fog is paint, not world";
}

} // namespace
