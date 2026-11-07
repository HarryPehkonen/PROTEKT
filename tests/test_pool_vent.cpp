// A BIOMETAL POOL LOOKS LIKE IT IS VENTING (Harri, 2026-11-01, from BattleZone 2): a shallow
// circular vent with a pale rim and a visible plume of steam, so it is obvious from the ground that
// something is coming out of the ground there -- and not a decal.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <vector>

#include <gtest/gtest.h>

#include "protekt/game/scene.hpp"
#include "protekt/sim/pool.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace {

using protekt::game::Scene;
using protekt::game::SceneOptions;
using protekt::game::SceneVertex;
using protekt::sim::Sim;

constexpr std::uint64_t kSeed = 42;
constexpr int kPoolX = 60;
constexpr int kPoolY = 60;

Sim pool_world(bool with_pool = true) {
    protekt::sim::Scenario s;
    s.seed = kSeed;
    if (with_pool) {
        s.pools = {protekt::sim::Pool{kPoolX, kPoolY}};
    }
    s.spawns = {protekt::sim::Spawn{protekt::sim::UnitKind::Tank, protekt::sim::Side::Player,
                                    kPoolX - 6, kPoolY}};
    return Sim::create(s);
}

SceneOptions rig() {
    SceneOptions o;
    o.seed = kSeed;
    o.radius_tiles = 12;
    o.camera.x = (static_cast<float>(kPoolX) - 6.0F) * protekt::game::kMetresPerTile;
    o.camera.z = static_cast<float>(kPoolY) * protekt::game::kMetresPerTile;
    o.camera.y = 8.0F;
    o.show_ship_gauge = false;
    o.show_ledger = false;
    return o;
}

std::vector<SceneVertex> in_ink(const Scene& scene, const std::array<float, 3>& ink) {
    std::vector<SceneVertex> found;
    for (const SceneVertex& v : scene.vertices) {
        if (std::abs(v.colour.at(0) - ink.at(0)) < 0.002F
            && std::abs(v.colour.at(1) - ink.at(1)) < 0.002F
            && std::abs(v.colour.at(2) - ink.at(2)) < 0.002F) {
            found.push_back(v);
        }
    }
    return found;
}

float ground() {
    return protekt::game::ground_metres(kSeed, static_cast<float>(kPoolX) + 0.5F,
                                        static_cast<float>(kPoolY) + 0.5F);
}

TEST(PoolVent, APoolIsDrawnInTheWorldAndNothingIsDrawnWhereThereIsNoPool) {
    const SceneOptions options = rig();
    const Scene with = protekt::game::build_scene(pool_world(true), options);
    const Scene without = protekt::game::build_scene(pool_world(false), options);
    EXPECT_GT(with.vertices.size(), without.vertices.size());
    EXPECT_FALSE(in_ink(with, protekt::game::kPoolSteamColour).empty());
    EXPECT_TRUE(in_ink(without, protekt::game::kPoolSteamColour).empty());
}

TEST(PoolVent, ThePoolsOnlyOwnGeometryIsTheSeeThroughPlumeNoDiscNoRim) {
    // The pool's look on the ground is painted into the terrain (test_site_ground.cpp); the pool
    // layer adds nothing solid. Switching it off leaves the opaque vertices exactly as they were.
    SceneOptions off = rig();
    off.show_pools = false;
    const Scene on = protekt::game::build_scene(pool_world(), rig());
    const Scene none = protekt::game::build_scene(pool_world(), off);
    const auto opaque = [](const Scene& scene) {
        return std::count_if(scene.vertices.begin(), scene.vertices.end(),
                             [](const SceneVertex& v) { return v.alpha >= 1.0F; });
    };
    EXPECT_EQ(opaque(on), opaque(none));
    EXPECT_GT(on.vertices.size(), none.vertices.size());
}

TEST(PoolVent, ASteamPlumeRisesWellAboveTheVentSoItIsObviousFromTheGround) {
    const Scene scene = protekt::game::build_scene(pool_world(), rig());
    const auto steam = in_ink(scene, protekt::game::kPoolSteamColour);
    ASSERT_FALSE(steam.empty());
    float top = ground();
    for (const SceneVertex& v : steam) {
        top = std::max(top, v.position[1]);
        EXPECT_LT(v.alpha, 1.0F) << "steam is see-through";
    }
    EXPECT_GE(top - ground(), protekt::game::kPoolPlumeHeightMetres * 0.7F)
        << "taller than a tank, a column and not a puddle";
}

TEST(PoolVent, EveryPuffOfTheSteamIsTheIcosphereAnExplosionUses) {
    // AK: ONE shape source for a puff. The steam used to call the eight-sided prism (84 vertices)
    // directly while the blast and the weld drew icospheres; a plume may differ in size and colour
    // but not in shape. Measured from the vertices the scene built: each run of 60 is twenty
    // equilateral triangles on twelve corners, every corner in five triangles and the same
    // distance from the run's centre, with the edge of an icosahedron (1.0515 of that radius).
    const Scene scene = protekt::game::build_scene(pool_world(), rig());
    const auto steam = in_ink(scene, protekt::game::kPoolSteamColour);
    constexpr std::size_t kSphere = 60;
    ASSERT_EQ(steam.size(), protekt::game::kPoolPlumePuffs * kSphere)
        << "ten puffs of sixty vertices (a prism is eighty-four)";
    const auto dist = [](const std::array<float, 3>& p, const std::array<float, 3>& q) {
        return std::sqrt(((p[0] - q[0]) * (p[0] - q[0])) + ((p[1] - q[1]) * (p[1] - q[1]))
                         + ((p[2] - q[2]) * (p[2] - q[2])));
    };
    for (std::size_t puff = 0; puff < protekt::game::kPoolPlumePuffs; ++puff) {
        const std::size_t first = puff * kSphere;
        std::map<std::array<long, 3>, int> triangles_at;
        std::array<float, 3> centre{};
        for (std::size_t i = 0; i < kSphere; ++i) {
            const auto& p = steam.at(first + i).position;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                centre.at(axis) += p.at(axis) / static_cast<float>(kSphere);
            }
            ++triangles_at[{std::lround(p[0] * 1000.0F), std::lround(p[1] * 1000.0F),
                            std::lround(p[2] * 1000.0F)}];
        }
        EXPECT_EQ(triangles_at.size(), std::size_t{12}) << "puff " << puff << ": twelve corners";
        for (const auto& [corner, count] : triangles_at) {
            EXPECT_EQ(count, 5) << "puff " << puff << ": five triangles at a corner";
        }
        const float radius = dist(steam.at(first).position, centre);
        for (std::size_t tri = 0; tri < kSphere; tri += 3) {
            for (std::size_t e = 0; e < 3; ++e) {
                const auto& p = steam.at(first + tri + e).position;
                const auto& q = steam.at(first + tri + ((e + 1) % 3)).position;
                EXPECT_NEAR(dist(p, centre), radius, radius * 0.01F) << "puff " << puff;
                EXPECT_NEAR(dist(p, q), radius * 1.0515F, radius * 0.02F) << "puff " << puff;
            }
        }
    }
}

TEST(PoolVent, ThePlumeMovesWithTheTickAndIsADerivedPictureNotStoredState) {
    Sim sim = pool_world();
    const SceneOptions options = rig();
    const Scene before = protekt::game::build_scene(sim, options);
    for (int i = 0; i < 20; ++i) {
        sim.step();
    }
    const Scene after = protekt::game::build_scene(sim, options);
    const auto a = in_ink(before, protekt::game::kPoolSteamColour);
    const auto b = in_ink(after, protekt::game::kPoolSteamColour);
    ASSERT_EQ(a.size(), b.size());
    bool moved = false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        moved = moved || a.at(i).position != b.at(i).position;
    }
    EXPECT_TRUE(moved) << "the steam rises";
}

TEST(PoolVent, ThePoolsLayerCanBeSwitchedOffAndAFarPoolIsCulled) {
    SceneOptions off = rig();
    off.show_pools = false;
    EXPECT_TRUE(
        in_ink(protekt::game::build_scene(pool_world(), off), protekt::game::kPoolSteamColour)
            .empty());
    SceneOptions far = rig();
    far.camera.x = 10.0F * protekt::game::kMetresPerTile;
    far.camera.z = 10.0F * protekt::game::kMetresPerTile;
    EXPECT_TRUE(
        in_ink(protekt::game::build_scene(pool_world(), far), protekt::game::kPoolSteamColour)
            .empty())
        << "beyond the meshed radius, as litter is";
}

} // namespace
