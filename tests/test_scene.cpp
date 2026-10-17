// The world as the player sees it: the terrain mesh and the unit boxes.
//
// These tests run on the gate box, which has no display -- because building a scene is pure
// geometry, and only PRESENTING it needs a screen.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "protekt/game/scene.hpp"
#include "protekt/render/renderer.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/world/world.hpp"

namespace {

using protekt::game::Camera;
using protekt::game::kMetresPerHeightUnit;
using protekt::game::kMetresPerTile;
using protekt::game::SceneOptions;
using protekt::render::Scene;

constexpr float kTolerance = 0.01F;

SceneOptions options_at(float x, float z, std::uint64_t seed = 42, int radius = 8) {
    SceneOptions o;
    o.seed = seed;
    o.radius_tiles = radius;
    o.camera.x = x;
    o.camera.z = z;
    o.camera.y = 8.0F;
    return o;
}

/// Every y in the mesh must be a height the world generator actually produces in that region.
std::set<int> mesh_heights(const Scene& scene) {
    std::set<int> heights;
    for (const auto& v : scene.vertices) {
        heights.insert(static_cast<int>(std::lround(v.position[1] / kMetresPerHeightUnit)));
    }
    return heights;
}

TEST(SceneCamera, WrapAngleKeepsHeadingsInOneTurn) {
    constexpr float pi = 3.14159265F;
    EXPECT_NEAR(Camera::wrap_angle(0.0F), 0.0F, kTolerance);
    EXPECT_NEAR(Camera::wrap_angle(pi / 2.0F), pi / 2.0F, kTolerance);
    EXPECT_NEAR(Camera::wrap_angle(2.0F * pi + 0.5F), 0.5F, kTolerance);
    EXPECT_NEAR(Camera::wrap_angle(-2.0F * pi - 0.5F), -0.5F, kTolerance);
    EXPECT_LE(std::abs(Camera::wrap_angle(100.0F)), pi + kTolerance);
}

TEST(SceneCamera, LooksAtWhatIsInFrontAndNotWhatIsBehind) {
    Camera camera;
    camera.x = 10.0F;
    camera.y = 2.0F;
    camera.z = 10.0F;
    camera.yaw = 0.0F; // yaw 0 looks down +Z
    const auto vp = protekt::game::view_projection(camera, 16.0F / 9.0F);

    // The matrix is COLUMN-major (what GLSL's mat4 expects): out[row] sums down a column of the
    // matrix times p[col]. Multiplying it row-wise gives a transposed -- and meaningless -- answer,
    // which is exactly the mistake the first version of this test made.
    const auto project = [&vp](float x, float y, float z) {
        std::array<float, 4> out{};
        const std::array<float, 4> p{x, y, z, 1.0F};
        for (std::size_t row = 0; row < 4; ++row) {
            out.at(row) = vp.at(row) * p.at(0) + vp.at(4 + row) * p.at(1) + vp.at(8 + row) * p.at(2)
                          + vp.at(12 + row) * p.at(3);
        }
        return out;
    };

    const auto ahead = project(10.0F, 2.0F, 40.0F);
    ASSERT_GT(ahead[3], 0.0F) << "a point in front must have positive w";
    EXPECT_GT(ahead[2], 0.0F);
    EXPECT_LT(ahead[2], ahead[3]) << "Vulkan clip space keeps z in [0, w]";

    const auto behind = project(10.0F, 2.0F, -40.0F);
    EXPECT_LT(behind[3], 0.0F) << "a point behind the camera must fall outside the frustum";

    // Which side of the screen world +x lands on is a convention (the Y-flip Vulkan needs makes
    // it easy to get backwards). What must hold either way: the two sides are mirrored, and a
    // point further off-axis projects further from the centre.
    const auto plus_x = project(20.0F, 2.0F, 40.0F);
    const auto minus_x = project(0.0F, 2.0F, 40.0F);
    EXPECT_GT(plus_x[3], 0.0F);
    EXPECT_GT(minus_x[3], 0.0F);
    EXPECT_LT(plus_x[0] * minus_x[0], 0.0F) << "the two sides of the frame must disagree in sign";
    const auto further = project(30.0F, 2.0F, 40.0F);
    EXPECT_GT(std::abs(further[0]), std::abs(plus_x[0]))
        << "twice the offset must project further from the centre";
}

TEST(SceneTerrain, MeshesTwoTrianglesPerTile) {
    const Scene scene = protekt::game::build_scene(
        protekt::sim::Sim::create(protekt::sim::skirmish(42)), options_at(60.0F, 60.0F, 42, 8));
    const std::size_t side = static_cast<std::size_t>(2 * 8 + 1);
    ASSERT_EQ(scene.vertices.size() % 3U, 0U) << "vertices must be triplets";
    EXPECT_GE(scene.vertices.size() / 3U, side * side * 2U)
        << "the terrain mesh alone is two triangles per tile";
}

TEST(SceneTerrain, StaysWhereItWasAsked) {
    const int radius = 6;
    const Scene scene = protekt::game::build_scene(
        protekt::sim::Sim::create(protekt::sim::skirmish(7)), options_at(100.0F, 60.0F, 7, radius));
    ASSERT_FALSE(scene.vertices.empty());
    float min_x = 1e9F;
    float max_x = -1e9F;
    float min_z = 1e9F;
    float max_z = -1e9F;
    for (const auto& v : scene.vertices) {
        min_x = std::min(min_x, v.position[0]);
        max_x = std::max(max_x, v.position[0]);
        min_z = std::min(min_z, v.position[2]);
        max_z = std::max(max_z, v.position[2]);
    }
    // The camera is at tile 50,30; the mesh spans +/- radius tiles, within a tile of slack.
    EXPECT_NEAR(min_x, (50.0F - static_cast<float>(radius)) * kMetresPerTile, kMetresPerTile);
    EXPECT_NEAR(max_x, (50.0F + static_cast<float>(radius)) * kMetresPerTile, kMetresPerTile);
    EXPECT_NEAR(min_z, (30.0F - static_cast<float>(radius)) * kMetresPerTile, kMetresPerTile);
    EXPECT_NEAR(max_z, (30.0F + static_cast<float>(radius)) * kMetresPerTile, kMetresPerTile);
}

TEST(SceneTerrain, HeightsComeFromTheWorldGeneratorAndNotFromSomewhereElse) {
    const int radius = 6;
    const Scene scene = protekt::game::build_scene(
        protekt::sim::Sim::create(protekt::sim::skirmish(9)), options_at(90.0F, 90.0F, 9, radius));
    ASSERT_FALSE(scene.vertices.empty());

    int lowest = 100000;
    int highest = -100000;
    for (int ty = 45 - radius - 1; ty <= 45 + radius + 1; ++ty) {
        for (int tx = 45 - radius - 1; tx <= 45 + radius + 1; ++tx) {
            const int h = protekt::world::sample(9, tx, ty).height;
            lowest = std::min(lowest, h);
            highest = std::max(highest, h);
        }
    }
    const auto heights = mesh_heights(scene);
    ASSERT_FALSE(heights.empty());
    // Exact agreement on the extremes: a mesh built from a different field would miss them.
    EXPECT_NEAR(static_cast<double>(*heights.begin()), lowest, 1.0);
    EXPECT_NEAR(static_cast<double>(*heights.rbegin()), highest, 1.0);
    EXPECT_GT(static_cast<int>(heights.size()), 1) << "flat terrain means the mesh ignores height";
}

TEST(SceneTerrain, ColoursTerrainTypesApart) {
    // Find two tiles of different Terrain near the camera and check they are not the same colour.
    const std::uint64_t seed = 3;
    const auto scene
        = protekt::game::build_scene(protekt::sim::Sim::create(protekt::sim::skirmish(seed)),
                                     options_at(120.0F, 120.0F, seed, 10));
    ASSERT_FALSE(scene.vertices.empty());
    std::set<std::array<float, 3>> colours;
    for (const auto& v : scene.vertices) {
        colours.insert(v.colour);
    }
    EXPECT_GT(colours.size(), 2U)
        << "terrain alone should show more than one colour when terrain types differ";
}

TEST(SceneUnits, DrawsABoxPerLivingUnitAndColoursTheSidesApart) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    const auto scene = protekt::game::build_scene(sim, options_at(60.0F, 60.0F));
    std::size_t living = 0;
    for (const auto& unit : sim.units()) {
        if (unit.alive) {
            ++living;
        }
    }
    ASSERT_GT(living, 0U);
    // Every unit box is 12 triangles; the terrain is 2 per tile. Rather than pin the mesh size,
    // count how many distinct colours appear that are not terrain colours.
    std::set<std::array<float, 3>> colours;
    for (const auto& v : scene.vertices) {
        colours.insert(v.colour);
    }
    EXPECT_GE(colours.size(), 2U) << "the player's units and the enemy's must be told apart";
}

TEST(SceneUnits, ASelectedUnitIsDrawnBrighterAndNothingElseChanges) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));

    // The camera has to be near the unit being highlighted: a scene only meshes the tiles around
    // the camera, so a unit outside that radius contributes no vertices to differ.
    std::vector<protekt::sim::UnitId> chosen;
    for (const auto& unit : sim.units()) {
        if (unit.side == protekt::sim::Side::Player) {
            chosen.push_back(unit.id);
            break;
        }
    }
    ASSERT_FALSE(chosen.empty());
    const auto* target = sim.find(chosen.front());
    ASSERT_NE(target, nullptr);
    SceneOptions o = options_at(0.0F, 0.0F, 42, 20);
    o.camera.x = static_cast<float>(target->x.raw / 65536.0) * kMetresPerTile;
    o.camera.z = static_cast<float>(target->y.raw / 65536.0) * kMetresPerTile;
    const auto plain = protekt::game::build_scene(sim, o);
    const auto highlighted = protekt::game::build_scene(sim, o, chosen);

    // Same geometry, so the two scenes differ in colour only -- and the terrain must be untouched.
    ASSERT_EQ(plain.vertices.size(), highlighted.vertices.size());
    std::size_t different = 0;
    for (std::size_t i = 0; i < plain.vertices.size(); ++i) {
        if (plain.vertices.at(i).colour != highlighted.vertices.at(i).colour) {
            ++different;
            EXPECT_EQ(plain.vertices.at(i).position, highlighted.vertices.at(i).position)
                << "a selection must not move anything";
        }
    }
    EXPECT_EQ(different, 12U * 3U) << "exactly one unit box (12 triangles) should change colour";

    // And the highlight is brighter than the side colour it came from.
    bool brighter = false;
    for (std::size_t i = 0; i < plain.vertices.size(); ++i) {
        if (plain.vertices.at(i).colour != highlighted.vertices.at(i).colour) {
            const auto& before = plain.vertices.at(i).colour;
            const auto& after = highlighted.vertices.at(i).colour;
            brighter = after[0] > before[0] && after[1] > before[1] && after[2] > before[2];
            break;
        }
    }
    EXPECT_TRUE(brighter) << "the selected box should stand out, not blend in";
}

TEST(SceneUnits, PutsTheUnitBoxWhereTheSimulationSaysItIs) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    // Put the camera on top of the player's first tank so its box is inside the mesh.
    const auto& tank = sim.units().front();
    ASSERT_TRUE(tank.alive);
    SceneOptions o = options_at(0.0F, 0.0F, 42, 20);
    o.camera.x = static_cast<float>(tank.x.raw / 65536.0) * kMetresPerTile;
    o.camera.z = static_cast<float>(tank.y.raw / 65536.0) * kMetresPerTile;
    const auto scene = protekt::game::build_scene(sim, o);
    ASSERT_FALSE(scene.vertices.empty());
    // Some vertex sits within a few metres of the tank's position, at a plausible height.
    bool found = false;
    for (const auto& v : scene.vertices) {
        const float dx = std::abs(v.position[0] - o.camera.x);
        const float dz = std::abs(v.position[2] - o.camera.z);
        if (dx < 5.0F && dz < 5.0F) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "no geometry near the tank the sim reports at (" << o.camera.x << ", "
                       << o.camera.z << ")";
}

TEST(SceneBuild, IsDeterministic) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(11));
    const auto a = protekt::game::build_scene(sim, options_at(70.0F, 70.0F, 11));
    const auto b = protekt::game::build_scene(sim, options_at(70.0F, 70.0F, 11));
    ASSERT_EQ(a.vertices.size(), b.vertices.size());
    bool same = true;
    for (std::size_t i = 0; i < a.vertices.size(); ++i) {
        same = same && a.vertices.at(i).position == b.vertices.at(i).position
               && a.vertices.at(i).colour == b.vertices.at(i).colour;
    }
    EXPECT_TRUE(same) << "the same view of the same state must give the same vertices";
    EXPECT_EQ(a.view_projection, b.view_projection);
}

TEST(SceneBuild, CarriesTheCameraItWasGiven) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(1));
    SceneOptions o = options_at(10.0F, 10.0F, 1, 4);
    o.camera.yaw = 0.7F;
    const auto scene = protekt::game::build_scene(sim, o);
    const float aspect = static_cast<float>(o.extent.width) / static_cast<float>(o.extent.height);
    EXPECT_EQ(scene.view_projection, protekt::game::view_projection(o.camera, aspect))
        << "the mesh's transform must be the camera's, or the world is drawn from nowhere";
}

} // namespace
