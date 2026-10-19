// The world as the player sees it: the terrain mesh and the unit boxes.
//
// These tests run on the gate box, which has no display -- because building a scene is pure
// geometry, and only PRESENTING it needs a screen.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "protekt/game/pick.hpp"
#include "protekt/game/radar.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/input/bindings.hpp"
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

/// The height the hover reports, as a plain float -- or a test failure.
///
/// clang-tidy's bugprone-unchecked-optional-access understands `assert`, an `if` guard and
/// `value_or`, but NOT a GTest assertion: `ASSERT_TRUE(o.has_value()); ... *o ...` is still
/// reported as unchecked. The guard lives here so every call site reads as the value it is.
float hover_height(const std::optional<float>& z) {
    if (!z.has_value()) {
        ADD_FAILURE() << "the hover reported no height when one was required";
        return 0.0F;
    }
    return *z;
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

/// The reticle tests. A reticle is a mark in WORLD space, placed along the click's own ray, so it
/// can be built and measured with no display at all -- which is the whole reason the mark is worth
/// drawing this way rather than with a font this project does not have.
using protekt::game::Aim;
using protekt::game::build_scene;
using protekt::game::kReticleVertexCount;
using protekt::game::pick;
using protekt::game::Pick;
using protekt::game::project;

/// The reticle is the tail of the vertex list, and this is the tail.
std::vector<protekt::render::SceneVertex> reticle_of(const Scene& scene) {
    if (scene.vertices.size() < kReticleVertexCount) {
        return {};
    }
    return {scene.vertices.end() - static_cast<std::ptrdiff_t>(kReticleVertexCount),
            scene.vertices.end()};
}

/// A pixel. `bool` out-parameters rather than `std::optional` returns because every caller then has
/// to handle the false branch where clang-tidy can see it -- an optional dereferenced after a gtest
/// assertion reads as unchecked access, since the checker cannot see through the macro.
struct Pixel {
    float x{};
    float y{};
};

/// Points the camera down +Z from eight metres behind `unit`, and fills `out` with the pixel at the
/// middle of the unit's box. False when the unit is not in the frame from there: callers report
/// that as a failure to aim, never as a wrong colour.
bool aim_at(const protekt::sim::Sim& state, SceneOptions& options, const protekt::sim::Unit& unit,
            Pixel& out) {
    constexpr float kFixedOne = static_cast<float>(protekt::sim::Fixed::kOne);
    const float ux = static_cast<float>(unit.x.raw) / kFixedOne * kMetresPerTile;
    const float uz = static_cast<float>(unit.y.raw) / kFixedOne * kMetresPerTile;
    const float ground
        = protekt::game::height_metres(options.seed, unit.x.floor_int(), unit.y.floor_int());
    const float mid = ground + 0.9F; // half of a tank's box: build_unit_box uses 1.8 m for a tank
    options.camera.x = ux;
    options.camera.z = uz - 8.0F;
    options.camera.y = ground + 2.0F;
    options.camera.yaw = 0.0F; // yaw 0 looks down +Z, which is where the unit now is
    options.camera.pitch = 0.0F;
    const protekt::game::Projected p = project(options.camera, options.extent, ux, mid, uz);
    if (p.depth <= 0.0F) {
        return false;
    }
    if (!pick(state, options, p.x, p.y).has_unit()) {
        return false;
    }
    out = Pixel{p.x, p.y};
    return true;
}

/// The first unit of `side` the camera can be aimed at, with `options` left pointing at it.
const protekt::sim::Unit* first_aimable(const protekt::sim::Sim& state, SceneOptions& options,
                                        protekt::sim::Side side) {
    for (const protekt::sim::Unit& unit : state.units()) {
        Pixel aim{};
        if (unit.alive && unit.side == side && aim_at(state, options, unit, aim)) {
            return &unit;
        }
    }
    return nullptr;
}

/// The first pixel in a small sweep that has terrain under it and no unit: the pointer over open
/// ground.
bool pixel_over_ground(const protekt::sim::Sim& state, const SceneOptions& options, Pixel& out) {
    for (const float y : {420.0F, 500.0F, 560.0F}) {
        for (const float x : {320.0F, 480.0F, 640.0F, 800.0F, 960.0F}) {
            const Pick picked = pick(state, options, x, y);
            if (picked.has_tile() && !picked.has_unit()) {
                out = Pixel{x, y};
                return true;
            }
        }
    }
    return false;
}

TEST(Reticle, OnlyAPointerDrawsSomethingAtTheEye) {
    // The property that keeps every existing capture honest: with no pointer, not one vertex is
    // drawn near the eye -- and with one, the reticle is exactly the 24 that are. The camera sits
    // far above the tallest terrain, so "near the eye" can only ever mean the reticle: a terrain
    // vertex would be hundreds of metres away, and the reticle is placed 1.5 m along the click's
    // own ray.
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    SceneOptions options = options_at(60.0F, 60.0F);
    options.camera.y = 200.0F;
    const auto near_the_eye = [&options](const Scene& scene) {
        const Camera& camera = options.camera;
        int count = 0;
        for (const auto& v : scene.vertices) {
            const float dx = v.position[0] - camera.x;
            const float dy = v.position[1] - camera.y;
            const float dz = v.position[2] - camera.z;
            if (std::sqrt((dx * dx) + (dy * dy) + (dz * dz)) < 2.5F) {
                ++count;
            }
        }
        return count;
    };

    EXPECT_EQ(near_the_eye(build_scene(sim, options)), 0);
    options.aim = Aim{640.0F, 360.0F};
    EXPECT_EQ(near_the_eye(build_scene(sim, options)), static_cast<int>(kReticleVertexCount));
}

TEST(Reticle, AnAimPointAddsExactlyTheReticleAndNothingElse) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    SceneOptions options = options_at(60.0F, 60.0F);
    const Scene before = build_scene(sim, options);
    options.aim = Aim{640.0F, 360.0F};
    const Scene after = build_scene(sim, options);
    ASSERT_EQ(after.vertices.size(), before.vertices.size() + kReticleVertexCount);
    // Every vertex before the tail is untouched: the reticle is added, not woven in.
    for (std::size_t i = 0; i < before.vertices.size(); ++i) {
        EXPECT_EQ(after.vertices.at(i).position, before.vertices.at(i).position);
        EXPECT_EQ(after.vertices.at(i).colour, before.vertices.at(i).colour);
    }
}

TEST(Reticle, TurnsRedOverAHostileAndGreenOverYourOwn) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    SceneOptions options = options_at(60.0F, 60.0F);
    options.viewer_side = protekt::sim::Side::Player;

    const protekt::sim::Unit* enemy = first_aimable(sim, options, protekt::sim::Side::Enemy);
    ASSERT_NE(enemy, nullptr) << "this scenario must have an enemy the camera can be aimed at";
    Pixel at_enemy{};
    ASSERT_TRUE(aim_at(sim, options, *enemy, at_enemy)) << "could not aim the camera at the enemy";
    options.aim = Aim{at_enemy.x, at_enemy.y};
    const std::vector<protekt::render::SceneVertex> red = reticle_of(build_scene(sim, options));
    ASSERT_EQ(red.size(), kReticleVertexCount);
    for (const auto& v : red) {
        EXPECT_GT(v.colour[0], v.colour[1] + 0.5F)
            << "a click on a hostile FIRES: that mark is red";
    }

    const protekt::sim::Unit* own = first_aimable(sim, options, protekt::sim::Side::Player);
    ASSERT_NE(own, nullptr) << "this scenario must have one of your own units to aim at";
    Pixel at_own{};
    ASSERT_TRUE(aim_at(sim, options, *own, at_own)) << "could not aim the camera at your own unit";
    options.aim = Aim{at_own.x, at_own.y};
    const std::vector<protekt::render::SceneVertex> green = reticle_of(build_scene(sim, options));
    ASSERT_EQ(green.size(), kReticleVertexCount);
    for (const auto& v : green) {
        EXPECT_GT(v.colour[1], v.colour[0] + 0.4F)
            << "a click on your own unit SELECTS: that mark is green";
    }
}

TEST(Reticle, IsWhiteOnTheGroundWhereAClickWouldOnlyOrder) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    SceneOptions options = options_at(200.0F, 200.0F); // away from the base, so the ground is open
    options.camera.pitch = -0.35F;                     // tipped down, so the pointer can see ground
    Pixel over_ground{};
    ASSERT_TRUE(pixel_over_ground(sim, options, over_ground))
        << "this test needs a pixel with terrain and no unit on it";
    options.aim = Aim{over_ground.x, over_ground.y};
    for (const auto& v : reticle_of(build_scene(sim, options))) {
        EXPECT_GT(v.colour[0], 0.8F);
        EXPECT_GT(v.colour[1], 0.8F);
        EXPECT_GT(v.colour[2], 0.8F);
    }
}

TEST(Reticle, StaysInFrontOfTheEyeWhereNothingCanHideIt) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    SceneOptions options = options_at(60.0F, 60.0F);
    options.aim = Aim{400.0F, 300.0F};
    const Camera& camera = options.camera;
    for (const auto& v : reticle_of(build_scene(sim, options))) {
        const float dx = v.position[0] - camera.x;
        const float dy = v.position[1] - camera.y;
        const float dz = v.position[2] - camera.z;
        const float distance = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
        EXPECT_GT(distance, 1.0F) << "the mark is not at the eye";
        EXPECT_LT(distance, 2.5F) << "the mark is close enough that terrain cannot occlude it";
    }
}

TEST(Reticle, IsTheSameEveryTime) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    SceneOptions options = options_at(60.0F, 60.0F);
    options.aim = Aim{512.0F, 288.0F};
    const Scene a = build_scene(sim, options);
    const Scene b = build_scene(sim, options);
    ASSERT_EQ(a.vertices.size(), b.vertices.size());
    for (std::size_t i = 0; i < a.vertices.size(); ++i) {
        EXPECT_EQ(a.vertices.at(i).position, b.vertices.at(i).position);
        EXPECT_EQ(a.vertices.at(i).colour, b.vertices.at(i).colour);
    }
}

// ---------------------------------------------------------------- the aim cue on the unit

/// The colour of the first vertex of `unit`'s box in `scene`: a box is the only thing at the unit's
/// own position that is not terrain, found by the vertex sitting at the unit's centre column.
std::array<float, 3> box_colour(const Scene& scene, const protekt::sim::Unit& unit) {
    constexpr float kFixedOne = static_cast<float>(protekt::sim::Fixed::kOne);
    const float ux = static_cast<float>(unit.x.raw) / kFixedOne * kMetresPerTile;
    const float uz = static_cast<float>(unit.y.raw) / kFixedOne * kMetresPerTile;
    const float half = kMetresPerTile * 0.3F;
    for (const auto& v : scene.vertices) {
        if (std::abs(v.position[0] - (ux - half)) < 1e-3F
            && std::abs(v.position[2] - (uz - half)) < 1e-3F
            && v.position[1]
                   > protekt::game::height_metres(42, unit.x.floor_int(), unit.y.floor_int())
                         + 1.0F) {
            return v.colour; // a top corner of the box: terrain is never this high above itself
        }
    }
    return {-1.0F, -1.0F, -1.0F};
}

/// The aim cue needs the reticle's own pixel, so the camera is put 8 m behind the unit as the
/// reticle tests do. `aimed` is the scene with the crosshair on the unit; `elsewhere` has the same
/// camera and the crosshair over the sky.
struct AimScenes {
    Scene aimed;
    Scene elsewhere;
};

AimScenes scenes_for(const protekt::sim::Sim& state, SceneOptions& options,
                     const protekt::sim::Unit& unit, std::span<const protekt::sim::UnitId> chosen) {
    Pixel at{};
    EXPECT_TRUE(aim_at(state, options, unit, at));
    AimScenes out;
    options.aim = Aim{at.x, at.y};
    out.aimed = build_scene(state, options, chosen);
    options.aim = Aim{at.x, 2.0F}; // the top edge of the frame: sky, no unit
    out.elsewhere = build_scene(state, options, chosen);
    return out;
}

TEST(AimCue, TheUnitUnderTheCrosshairIsTintedAndTheSameUnitElsewhereIsNot) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    for (const auto side : {protekt::sim::Side::Player, protekt::sim::Side::Enemy}) {
        SceneOptions options = options_at(60.0F, 60.0F);
        const protekt::sim::Unit* unit = first_aimable(sim, options, side);
        ASSERT_NE(unit, nullptr);
        const AimScenes scenes = scenes_for(sim, options, *unit, {});
        const auto marked = box_colour(scenes.aimed, *unit);
        const auto plain = box_colour(scenes.elsewhere, *unit);
        ASSERT_GE(plain[0], 0.0F) << "the unit's box must be in the scene";
        EXPECT_NE(marked, plain) << "the aimed unit must be marked";
    }
}

TEST(AimCue, AnOwnUnitAndAHostileAreMarkedDifferently) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    SceneOptions options = options_at(60.0F, 60.0F);
    const protekt::sim::Unit* own = first_aimable(sim, options, protekt::sim::Side::Player);
    ASSERT_NE(own, nullptr);
    const auto own_mark = box_colour(scenes_for(sim, options, *own, {}).aimed, *own);
    const protekt::sim::Unit* enemy = first_aimable(sim, options, protekt::sim::Side::Enemy);
    ASSERT_NE(enemy, nullptr);
    const auto enemy_mark = box_colour(scenes_for(sim, options, *enemy, {}).aimed, *enemy);
    EXPECT_NE(own_mark, enemy_mark);
    EXPECT_GT(own_mark[1], own_mark[2] + 0.2F) << "yours is warm amber, not blue";
    EXPECT_GT(enemy_mark[0], enemy_mark[2] + 0.4F) << "a hostile stays in the red family";
}

TEST(AimCue, ASelectedUnitUnderTheCrosshairStillReadsAsSelected) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    SceneOptions options = options_at(60.0F, 60.0F);
    const protekt::sim::Unit* own = first_aimable(sim, options, protekt::sim::Side::Player);
    ASSERT_NE(own, nullptr);
    const std::vector<protekt::sim::UnitId> chosen{own->id};
    const AimScenes scenes = scenes_for(sim, options, *own, chosen);
    EXPECT_EQ(box_colour(scenes.aimed, *own), box_colour(scenes.elsewhere, *own))
        << "selection takes precedence: aiming at a selected unit must not change its colour";
    const auto selected = box_colour(scenes.aimed, *own);
    EXPECT_GT(selected[2], 0.9F) << "still the white-mixed blue of a selection";
}

TEST(AimCue, SceneIsDeterministicWithAnAimPoint) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    SceneOptions options = options_at(60.0F, 60.0F);
    const protekt::sim::Unit* own = first_aimable(sim, options, protekt::sim::Side::Player);
    ASSERT_NE(own, nullptr);
    const AimScenes a = scenes_for(sim, options, *own, {});
    const AimScenes b = scenes_for(sim, options, *own, {});
    ASSERT_EQ(a.aimed.vertices.size(), b.aimed.vertices.size());
    for (std::size_t i = 0; i < a.aimed.vertices.size(); ++i) {
        EXPECT_EQ(a.aimed.vertices.at(i).position, b.aimed.vertices.at(i).position);
        EXPECT_EQ(a.aimed.vertices.at(i).colour, b.aimed.vertices.at(i).colour);
    }
}

// ---------------------------------------------------------------- the radar, as drawn

/// The radar's own vertices, found as the block that appears when a viewer unit is named. Colours
/// are not duplicated here on purpose: what the test needs is the DIFFERENCE between two scenes,
/// and that difference is exactly the panel.
std::vector<protekt::render::SceneVertex> radar_vertices(const Scene& with_viewer,
                                                         const Scene& without) {
    EXPECT_GT(with_viewer.vertices.size(), without.vertices.size());
    std::size_t first = 0;
    while (first < without.vertices.size()
           && with_viewer.vertices.at(first).colour == without.vertices.at(first).colour
           && with_viewer.vertices.at(first).position == without.vertices.at(first).position) {
        ++first;
    }
    EXPECT_EQ(first, without.vertices.size()) << "the radar is appended after everything else";
    return {with_viewer.vertices.begin() + static_cast<std::ptrdiff_t>(first),
            with_viewer.vertices.end()};
}

TEST(RadarDraws, ThePanelLandsOnItsPixelsAndInFrontOfTheNearPlane) {
    // Two mistakes this catches, both made once each: forgetting to add the panel's corner to a
    // mark's panel-relative pixels (which puts every unit in the frame's top-left corner), and
    // placing the HUD too close to the eye -- a mark at 10 cm is only 7 cm FORWARD at the frame's
    // corner, so it is silently clipped by the 10 cm near plane and nothing appears at all.
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    protekt::sim::UnitId player{};
    for (const auto& unit : sim.units()) {
        if (unit.side == protekt::sim::Side::Player && unit.kind == protekt::sim::UnitKind::Tank) {
            player = unit.id;
            break;
        }
    }
    ASSERT_TRUE(player.valid());

    SceneOptions plain = options_at(60.0F, 60.0F);
    SceneOptions watched = plain;
    watched.viewer_unit = player;
    const std::vector<protekt::render::SceneVertex> radar = radar_vertices(
        protekt::game::build_scene(sim, watched), protekt::game::build_scene(sim, plain));
    ASSERT_FALSE(radar.empty());

    const protekt::game::RadarPanel panel = protekt::game::radar_panel(watched.extent);
    const protekt::math::Basis basis
        = protekt::math::basis_from(watched.camera.yaw, watched.camera.pitch);
    for (const auto& v : radar) {
        const float forward = ((v.position[0] - watched.camera.x) * basis.forward[0])
                              + ((v.position[1] - watched.camera.y) * basis.forward[1])
                              + ((v.position[2] - watched.camera.z) * basis.forward[2]);
        EXPECT_GT(forward, protekt::math::kNearPlane)
            << "a HUD vertex inside the near plane is clipped and never drawn";
        const protekt::game::Projected p = protekt::game::project(
            watched.camera, watched.extent, v.position[0], v.position[1], v.position[2]);
        // The border sits just outside the panel's face, which is the widest thing drawn.
        const float slack = protekt::game::kRadarBorderPixels + 1.0F;
        EXPECT_GE(p.x, panel.x - slack);
        EXPECT_LE(p.x, panel.x + panel.size + slack);
        // The health bar stands above the panel, so the top is allowed that much further.
        const float bar_room = (std::max(8.0F, panel.size * 0.08F)) + 4.0F;
        EXPECT_GE(p.y, panel.y - slack - bar_room);
        EXPECT_LE(p.y, panel.y + panel.size + slack);
    }
}

TEST(RadarDraws, AnInvalidViewerUnitDrawsNoPanelAtAll) {
    // "No viewer" and "a viewer that is not in the sim" must both mean no radar: a panel with no
    // centre would be a lie about where the tank is, and an empty panel is not a radar either.
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    const SceneOptions none = options_at(60.0F, 60.0F);
    SceneOptions stranger = none;
    stranger.viewer_unit = protekt::sim::UnitId{9999};
    EXPECT_EQ(protekt::game::build_scene(sim, none).vertices.size(),
              protekt::game::build_scene(sim, stranger).vertices.size());
}

TEST(RadarDraws, MarksCountUpTheWayTheRulesSay) {
    // The panel and its bar are counted below and each mark is 6, except the viewer's triangle and
    // an arrow, which are 3.
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    protekt::sim::UnitId player{};
    for (const auto& unit : sim.units()) {
        if (unit.side == protekt::sim::Side::Player && unit.kind == protekt::sim::UnitKind::Tank) {
            player = unit.id;
            break;
        }
    }
    SceneOptions plain = options_at(60.0F, 60.0F);
    SceneOptions watched = plain;
    watched.viewer_unit = player;
    const auto marks = protekt::game::radar_marks(sim.units(), player, watched.camera.yaw,
                                                  protekt::game::radar_panel(watched.extent));
    const std::vector<protekt::render::SceneVertex> radar = radar_vertices(
        protekt::game::build_scene(sim, watched), protekt::game::build_scene(sim, plain));
    // The panel itself: a 48-slice ring (a quad a slice) and a 48-slice disc (a triangle a slice),
    // and above it the health bar's track and its fill (a quad each, the viewer being unhurt).
    std::size_t expected = (48 * 6) + (48 * 3) + 6 + 6;
    for (const auto& mark : marks) {
        switch (mark.shape) {
        case protekt::game::RadarShape::Square:
            expected += 6; // one quad
            break;
        case protekt::game::RadarShape::Hollow:
            expected += 24; // four bars, each a quad
            break;
        case protekt::game::RadarShape::Triangle:
        case protekt::game::RadarShape::Arrow:
            expected += 3; // one triangle
            break;
        }
    }
    EXPECT_EQ(radar.size(), expected);
}

TEST(RadarDraws, AHostileInRangeIsDrawnRedAndOneOutOfRangeIsNotDrawnAtAll) {
    using protekt::sim::Side;
    using protekt::sim::Spawn;
    using protekt::sim::UnitKind;

    const auto scene_of = [](int enemy_tile_x) {
        protekt::sim::Scenario scenario;
        scenario.seed = 42;
        scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Player, 50, 50});
        scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Enemy, enemy_tile_x, 52});
        const auto sim = protekt::sim::Sim::create(scenario);
        protekt::sim::UnitId player{};
        for (const auto& unit : sim.units()) {
            if (unit.side == Side::Player) {
                player = unit.id;
            }
        }
        SceneOptions plain;
        plain.seed = scenario.seed;
        plain.radius_tiles = 8;
        plain.camera.x = 100.0F;
        plain.camera.z = 104.0F;
        plain.camera.y = 8.0F;
        SceneOptions watched = plain;
        watched.viewer_unit = player;
        return radar_vertices(protekt::game::build_scene(sim, watched),
                              protekt::game::build_scene(sim, plain));
    };
    const auto is_red = [](const protekt::render::SceneVertex& v) {
        return v.colour.at(0) > (v.colour.at(1) + 0.3F) && v.colour.at(0) > (v.colour.at(2) + 0.3F);
    };

    // Five tiles away: inside the radar's radius, so the enemy is on the panel, and red.
    const std::vector<protekt::render::SceneVertex> near_enemy = scene_of(55);
    ASSERT_FALSE(near_enemy.empty());
    std::size_t reds = 0;
    for (const auto& v : near_enemy) {
        if (is_red(v)) {
            ++reds;
        }
    }
    EXPECT_EQ(reds, 6U) << "one red square per hostile in range";

    // Three hundred tiles away: the same frame, but the enemy is not on the panel and nothing about
    // him is drawn -- not a dot, not a hint at the rim.
    const std::vector<protekt::render::SceneVertex> far_enemy = scene_of(350);
    ASSERT_FALSE(far_enemy.empty());
    for (const auto& v : far_enemy) {
        EXPECT_FALSE(is_red(v)) << "a hostile outside the radius must not be visible on the radar";
    }
}

// ---------------------------------------------------------------- the health bar and the round
// radar
//
// Colours are asserted as literals on purpose: a test that compared against the constant the code
// draws with would pass whatever the constant was.

constexpr std::array<float, 3> kBorderRgb{0.80F, 0.82F, 0.85F};
constexpr std::array<float, 3> kFaceRgb{0.04F, 0.05F, 0.06F};
constexpr std::array<float, 3> kGreenRgb{0.30F, 0.85F, 0.35F};
constexpr std::array<float, 3> kAmberRgb{1.0F, 0.75F, 0.20F};
constexpr std::array<float, 3> kRedRgb{1.0F, 0.30F, 0.25F};
constexpr float kPixelEps = 0.1F;

/// The HUD of one frame, sorted by what each vertex is. Sorting is by colour and by WHERE the
/// vertex lands in pixels, never by position in the list, so it holds however the drawing is built.
struct Hud {
    protekt::game::RadarPanel panel{};
    std::vector<std::array<float, 2>> bar_track;
    std::vector<std::array<float, 2>> bar_fill;
    std::vector<std::array<float, 3>> bar_fill_colours;
    std::vector<std::array<float, 2>> face;
    std::vector<std::array<float, 2>> border;
    std::vector<std::array<float, 2>> marks;
};

Hud hud_of(const protekt::sim::Sim& sim, SceneOptions watched) {
    SceneOptions plain = watched;
    plain.viewer_unit = protekt::sim::UnitId{};
    Hud hud;
    hud.panel = protekt::game::radar_panel(watched.extent);
    const float above = hud.panel.y - protekt::game::kRadarBorderPixels - 0.5F;
    for (const auto& v : radar_vertices(protekt::game::build_scene(sim, watched),
                                        protekt::game::build_scene(sim, plain))) {
        const protekt::game::Projected p = protekt::game::project(
            watched.camera, watched.extent, v.position[0], v.position[1], v.position[2]);
        const std::array<float, 2> at{p.x, p.y};
        if (v.colour == kBorderRgb) {
            hud.border.push_back(at);
        } else if (p.y < above && v.colour == kFaceRgb) {
            hud.bar_track.push_back(at);
        } else if (p.y < above) {
            hud.bar_fill.push_back(at);
            hud.bar_fill_colours.push_back(v.colour);
        } else if (v.colour == kFaceRgb) {
            hud.face.push_back(at);
        } else {
            hud.marks.push_back(at);
        }
    }
    return hud;
}

float min_x(const std::vector<std::array<float, 2>>& points) {
    if (points.empty()) {
        // Never deref an empty list: a broken HUD must FAIL a test, not crash the run before the
        // later tests get their turn. NaN compares false against everything, so every EXPECT built
        // on this value fails, which is what the reader wants to see.
        return std::numeric_limits<float>::quiet_NaN();
    }
    float lowest = points.front().at(0);
    for (const auto& p : points) {
        lowest = std::min(lowest, p.at(0));
    }
    return lowest;
}
float max_x(const std::vector<std::array<float, 2>>& points) {
    if (points.empty()) {
        // Never deref an empty list: a broken HUD must FAIL a test, not crash the run before the
        // later tests get their turn. NaN compares false against everything, so every EXPECT built
        // on this value fails, which is what the reader wants to see.
        return std::numeric_limits<float>::quiet_NaN();
    }
    float highest = points.front().at(0);
    for (const auto& p : points) {
        highest = std::max(highest, p.at(0));
    }
    return highest;
}
float min_y(const std::vector<std::array<float, 2>>& points) {
    if (points.empty()) {
        // Never deref an empty list: a broken HUD must FAIL a test, not crash the run before the
        // later tests get their turn. NaN compares false against everything, so every EXPECT built
        // on this value fails, which is what the reader wants to see.
        return std::numeric_limits<float>::quiet_NaN();
    }
    float lowest = points.front().at(1);
    for (const auto& p : points) {
        lowest = std::min(lowest, p.at(1));
    }
    return lowest;
}
float max_y(const std::vector<std::array<float, 2>>& points) {
    if (points.empty()) {
        // Never deref an empty list: a broken HUD must FAIL a test, not crash the run before the
        // later tests get their turn. NaN compares false against everything, so every EXPECT built
        // on this value fails, which is what the reader wants to see.
        return std::numeric_limits<float>::quiet_NaN();
    }
    float highest = points.front().at(1);
    for (const auto& p : points) {
        highest = std::max(highest, p.at(1));
    }
    return highest;
}

SceneOptions battle_options() {
    SceneOptions plain;
    plain.seed = 42;
    plain.radius_tiles = 8;
    plain.camera.x = 100.0F;
    plain.camera.z = 104.0F;
    plain.camera.y = 8.0F;
    return plain;
}

/// A factory (400 hp, unarmed) shot at by a tank (8 hp a hit) until the factory is down to exactly
/// `hp`. The sim has no setter and must not get one, and the damage is exact, so this is the honest
/// way to get a viewer at a chosen health. `hp == 0` runs it until the factory is dead.
struct Damaged {
    protekt::sim::Sim sim;
    protekt::sim::UnitId viewer;
};
Damaged factory_down_to(std::uint16_t hp) {
    using protekt::sim::Side;
    using protekt::sim::Spawn;
    using protekt::sim::UnitKind;
    protekt::sim::Scenario scenario;
    scenario.seed = 42;
    scenario.spawns.push_back(Spawn{UnitKind::Factory, Side::Player, 50, 50});
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Enemy, 55, 52});
    auto sim = protekt::sim::Sim::create(scenario);
    protekt::sim::UnitId factory{};
    for (const auto& unit : sim.units()) {
        if (unit.kind == UnitKind::Factory) {
            factory = unit.id;
        }
    }
    for (int guard = 0; guard < 5000 && sim.find(factory)->hp > hp; ++guard) {
        sim.step();
    }
    return Damaged{std::move(sim), factory};
}

SceneOptions watching(const Damaged& d) {
    SceneOptions options = battle_options();
    options.viewer_unit = d.viewer;
    return options;
}

float track_width(const Hud& hud) { return max_x(hud.bar_track) - min_x(hud.bar_track); }
float fill_width(const Hud& hud) {
    return hud.bar_fill.empty() ? 0.0F : max_x(hud.bar_fill) - min_x(hud.bar_fill);
}

TEST(HealthBar, FullHealthFillsTheWholeTrack) {
    const Damaged d = factory_down_to(400);
    ASSERT_EQ(d.sim.find(d.viewer)->hp, 400);
    const Hud hud = hud_of(d.sim, watching(d));
    ASSERT_FALSE(hud.bar_track.empty());
    ASSERT_FALSE(hud.bar_fill.empty());
    EXPECT_NEAR(fill_width(hud), track_width(hud), kPixelEps);
    EXPECT_NEAR(min_x(hud.bar_fill), min_x(hud.bar_track), kPixelEps);
}

TEST(HealthBar, HalfHealthFillsHalfTheTrack) {
    const Damaged d = factory_down_to(200);
    ASSERT_EQ(d.sim.find(d.viewer)->hp, 200);
    const Hud hud = hud_of(d.sim, watching(d));
    ASSERT_FALSE(hud.bar_track.empty());
    ASSERT_FALSE(hud.bar_fill.empty());
    EXPECT_NEAR(fill_width(hud), track_width(hud) * 0.5F, 0.25F);
    EXPECT_NEAR(min_x(hud.bar_fill), min_x(hud.bar_track), kPixelEps) << "drawn from the left";
}

TEST(HealthBar, NoFillWhenTheHullIsGone) {
    const Damaged d = factory_down_to(0);
    ASSERT_FALSE(d.sim.find(d.viewer)->alive);
    // The radar still draws for a dead viewer, and its marks may be green, so look at the fill by
    // where it lands -- inside the bar -- and not by colour alone.
    const Hud hud = hud_of(d.sim, watching(d));
    EXPECT_TRUE(hud.bar_fill.empty());
}

TEST(HealthBar, ADeadViewerStillGetsTheDarkTrackAndNoFill) {
    const Damaged d = factory_down_to(0);
    ASSERT_FALSE(d.sim.find(d.viewer)->alive);
    const Hud hud = hud_of(d.sim, watching(d));
    EXPECT_FALSE(hud.bar_track.empty()) << "death must not blank the health bar";
    EXPECT_TRUE(hud.bar_fill.empty());
    EXPECT_NEAR(min_x(hud.bar_track), hud.panel.x - protekt::game::kRadarBorderPixels, kPixelEps);
}

TEST(HealthBar, TheColourChangesAtTheThresholds) {
    // The sim's damage is exact, so the sides of each threshold are reached exactly: a 400 hp
    // factory is at 0.50 at 200 hp and 0.20 at 80 hp, and each hit is 8 hp (0.02).
    struct Case {
        std::uint16_t hp;
        std::array<float, 3> colour;
    };
    const std::array<Case, 6> cases{{{208, kGreenRgb}, // 0.52
                                     {200, kGreenRgb}, // 0.50 is the first green
                                     {192, kAmberRgb}, // 0.48
                                     {88, kAmberRgb},  // 0.22
                                     {80, kAmberRgb},  // 0.20 is the last amber
                                     {72, kRedRgb}}};  // 0.18
    for (const Case& c : cases) {
        const Damaged d = factory_down_to(c.hp);
        ASSERT_EQ(d.sim.find(d.viewer)->hp, c.hp);
        const Hud hud = hud_of(d.sim, watching(d));
        ASSERT_FALSE(hud.bar_fill_colours.empty()) << "hp " << c.hp;
        for (const auto& colour : hud.bar_fill_colours) {
            EXPECT_EQ(colour, c.colour) << "hp " << c.hp;
        }
    }
}

TEST(HealthBar, SitsAboveTheRadarAndSharesItsWidth) {
    const Damaged d = factory_down_to(400);
    const Hud hud = hud_of(d.sim, watching(d));
    ASSERT_FALSE(hud.bar_track.empty());
    const float border = protekt::game::kRadarBorderPixels;
    EXPECT_NEAR(min_x(hud.bar_track), hud.panel.x - border, kPixelEps);
    EXPECT_NEAR(max_x(hud.bar_track), hud.panel.x + hud.panel.size + border, kPixelEps);
    EXPECT_LT(max_y(hud.bar_track), hud.panel.y - border) << "above the ring, with a gap";
    EXPECT_GT(max_y(hud.bar_track), hud.panel.y - border - 16.0F) << "and only a small gap";
    const float height = max_y(hud.bar_track) - min_y(hud.bar_track);
    EXPECT_GE(height, 8.0F - kPixelEps);
    EXPECT_NEAR(height, std::max(8.0F, hud.panel.size * 0.08F), kPixelEps);
    EXPECT_GE(min_y(hud.bar_track), 0.0F) << "on the screen";
}

TEST(HealthBar, IsAbsentWithoutAViewerUnitInTheWorld) {
    const Damaged alive = factory_down_to(400);
    SceneOptions none = battle_options();
    EXPECT_EQ(protekt::game::build_scene(alive.sim, none).vertices.size(),
              protekt::game::build_scene(alive.sim, battle_options()).vertices.size());
    SceneOptions stranger = battle_options();
    stranger.viewer_unit = protekt::sim::UnitId{9999};
    EXPECT_EQ(protekt::game::build_scene(alive.sim, stranger).vertices.size(),
              protekt::game::build_scene(alive.sim, none).vertices.size());
}

// Round radar -------------------------------------------------------------------------------

float from_centre(const Hud& hud, const std::array<float, 2>& p) {
    const float middle = hud.panel.size * 0.5F;
    return std::hypot(p.at(0) - (hud.panel.x + middle), p.at(1) - (hud.panel.y + middle));
}

TEST(Radar, TheFaceIsADiscNotARectangle) {
    const Damaged d = factory_down_to(400);
    const Hud hud = hud_of(d.sim, watching(d));
    ASSERT_FALSE(hud.face.empty());
    const float radius = hud.panel.size * 0.5F;
    for (const auto& p : hud.face) {
        EXPECT_LE(from_centre(hud, p), radius + kPixelEps);
    }
    EXPECT_NEAR(min_x(hud.face), hud.panel.x, kPixelEps);
    EXPECT_NEAR(max_x(hud.face), hud.panel.x + hud.panel.size, kPixelEps);
    EXPECT_NEAR(min_y(hud.face), hud.panel.y, kPixelEps);
    EXPECT_NEAR(max_y(hud.face), hud.panel.y + hud.panel.size, kPixelEps);
}

TEST(Radar, TheBorderIsARing) {
    const Damaged d = factory_down_to(400);
    const Hud hud = hud_of(d.sim, watching(d));
    ASSERT_FALSE(hud.border.empty());
    const float inner = hud.panel.size * 0.5F;
    const float outer = inner + protekt::game::kRadarBorderPixels;
    for (const auto& p : hud.border) {
        EXPECT_GE(from_centre(hud, p), inner - kPixelEps);
        EXPECT_LE(from_centre(hud, p), outer + kPixelEps);
    }
}

/// A viewer in the middle and one hostile at each offset, in tiles from it.
protekt::sim::Sim ring_of_units() {
    using protekt::sim::Side;
    using protekt::sim::Spawn;
    using protekt::sim::UnitKind;
    protekt::sim::Scenario scenario;
    scenario.seed = 42;
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Player, 60, 60});
    const std::array<std::array<int, 2>, 16> offsets{{{40, 0},
                                                      {-40, 0},
                                                      {0, 40},
                                                      {0, -40},
                                                      {24, 32},
                                                      {-24, 32},
                                                      {24, -32},
                                                      {-24, -32},
                                                      {32, 24},
                                                      {-32, 24},
                                                      {32, -24},
                                                      {-32, -24},
                                                      {10, 0},
                                                      {0, -10},
                                                      {7, 7},
                                                      {-20, 20}}};
    for (const auto& o : offsets) {
        scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Enemy, 60 + o.at(0), 60 + o.at(1)});
    }
    // Your own base far out of range, so there is an arrow at the rim as well.
    scenario.spawns.push_back(Spawn{UnitKind::Factory, Side::Player, 60 + 70, 60});
    return protekt::sim::Sim::create(scenario);
}

TEST(Radar, EveryMarkLiesWithinTheInscribedRadius) {
    // The projection is linear in distance, so a mark at the maximum range is exactly
    // `middle - size * inset` from the centre, and nothing in range is further. (An out-of-range
    // base's ARROW is drawn at the rim by design and is held to the outer circle below instead.)
    const auto sim = ring_of_units();
    protekt::sim::UnitId viewer{};
    for (const auto& unit : sim.units()) {
        if (unit.kind == protekt::sim::UnitKind::Tank && unit.side == protekt::sim::Side::Player) {
            viewer = unit.id;
        }
    }
    ASSERT_TRUE(viewer.valid());
    const protekt::game::RadarPanel panel = protekt::game::radar_panel({1280, 720});
    const float middle = panel.size * 0.5F;
    const float limit = middle - (panel.size * 0.06F);
    float furthest = 0.0F;
    for (const float yaw : {0.0F, 0.7F, 2.0F, -1.3F, 3.1F}) {
        std::size_t in_range = 0;
        for (const auto& mark : protekt::game::radar_marks(sim.units(), viewer, yaw, panel)) {
            if (mark.shape == protekt::game::RadarShape::Arrow) {
                continue;
            }
            ++in_range;
            const float distance = std::hypot(mark.px - middle, mark.py - middle);
            EXPECT_LE(distance, limit + kPixelEps) << "yaw " << yaw;
            furthest = std::max(furthest, distance);
        }
        EXPECT_EQ(in_range, 17U) << "the viewer and sixteen hostiles, all in range";
    }
    EXPECT_NEAR(furthest, limit, kPixelEps) << "the maximum range lands exactly on the limit";
}

TEST(Radar, NothingIsDrawnOutsideTheOuterCircle) {
    const auto sim = ring_of_units();
    protekt::sim::UnitId viewer{};
    for (const auto& unit : sim.units()) {
        if (unit.kind == protekt::sim::UnitKind::Tank && unit.side == protekt::sim::Side::Player) {
            viewer = unit.id;
        }
    }
    for (const float yaw : {0.0F, 0.7F, 2.0F, -1.3F, 3.1F}) {
        SceneOptions watched = battle_options();
        watched.viewer_unit = viewer;
        watched.camera.yaw = yaw;
        const Hud hud = hud_of(sim, watched);
        ASSERT_FALSE(hud.marks.empty());
        const float outer = (hud.panel.size * 0.5F) + protekt::game::kRadarBorderPixels;
        for (const auto* group : {&hud.face, &hud.border, &hud.marks}) {
            for (const auto& p : *group) {
                EXPECT_LE(from_centre(hud, p), outer + kPixelEps) << "yaw " << yaw;
            }
        }
    }
}

} // namespace

// ---------------------------------------------------------------- smooth elevation
//
// Seed 42 is the sloped world used throughout: the tiles (106,95) and (107,95) differ by 1.25 m
// (the steepest x-step in [10,120)^2), and tiles (32,13),(33,13),(32,14),(33,14) are all the same
// height -- the one flat cell the generator leaves near the origin.

namespace {

using protekt::game::ground_metres;
using protekt::game::height_metres;
using protekt::sim::Fixed;

constexpr std::uint64_t kSlopeSeed = 42;
constexpr float kSlopeStepMetres = 1.25F; // |h(107,95) - h(106,95)| for seed 42

Fixed tiles(float value) {
    return Fixed::from_raw(static_cast<std::int32_t>(std::lround(value * 65536.0F)));
}

} // namespace

TEST(GroundMetres, AtALatticePointItIsThatTilesHeight) {
    for (int ty = 90; ty < 100; ++ty) {
        for (int tx = 100; tx < 110; ++tx) {
            EXPECT_EQ(ground_metres(kSlopeSeed, Fixed::from_int(tx), Fixed::from_int(ty)),
                      height_metres(kSlopeSeed, tx, ty))
                << "tile " << tx << "," << ty;
        }
    }
}

TEST(GroundMetres, ApproachingABoundaryFromEitherSideConvergesToTheSameValue) {
    // x = 107 is the boundary between tiles 106 and 107, on the 1.25 m slope.
    const Fixed y = Fixed::from_int(95);
    const float at = ground_metres(kSlopeSeed, Fixed::from_int(107), y);
    const float below = ground_metres(kSlopeSeed, Fixed::from_raw((107 << 16) - 1), y);
    const float above = ground_metres(kSlopeSeed, Fixed::from_raw((107 << 16) + 1), y);
    EXPECT_NEAR(below, at, 1e-3F);
    EXPECT_NEAR(above, at, 1e-3F);
    EXPECT_NEAR(below, above, 1e-3F);
}

TEST(GroundMetres, CrossingABoundaryNeverStepsByMoreThanAFractionOfTheTileDifference) {
    // Walk x from 105.5 to 108.5 in 1/32-tile steps along y = 95, across the 1.25 m slope.
    const Fixed y = Fixed::from_int(95);
    float previous = ground_metres(kSlopeSeed, tiles(105.5F), y);
    float largest = 0.0F;
    float total = 0.0F;
    for (int i = 1; i <= 96; ++i) {
        const float here
            = ground_metres(kSlopeSeed, tiles(105.5F + (static_cast<float>(i) / 32.0F)), y);
        largest = std::max(largest, std::fabs(here - previous));
        total += std::fabs(here - previous);
        previous = here;
    }
    ASSERT_GT(total, kSlopeStepMetres * 0.9F) << "the walk must actually climb the slope";
    EXPECT_LT(largest, kSlopeStepMetres / 16.0F) << "a 1/32-tile step is a 1/32 share of a slope";
}

TEST(GroundMetres, OnFlatGroundItIsConstant) {
    const float flat = height_metres(kSlopeSeed, 32, 13);
    for (int i = 0; i <= 8; ++i) {
        for (int j = 0; j <= 8; ++j) {
            EXPECT_EQ(ground_metres(kSlopeSeed, tiles(32.0F + (static_cast<float>(i) / 8.0F)),
                                    tiles(13.0F + (static_cast<float>(j) / 8.0F))),
                      flat);
        }
    }
}

TEST(GroundMetres, SameInputGivesTheSameOutputAndTheFloatFormAgreesWithTheFixedForm) {
    const Fixed x = tiles(106.37F);
    const Fixed y = tiles(95.81F);
    EXPECT_EQ(ground_metres(kSlopeSeed, x, y), ground_metres(kSlopeSeed, x, y));
    EXPECT_NEAR(ground_metres(kSlopeSeed, x, y), ground_metres(kSlopeSeed, 106.37F, 95.81F), 1e-3F);
}

TEST(GroundMetres, InsideATileItFollowsTheSameTriangleTheMeshDraws) {
    // The mesh draws each tile as two triangles split along p00-p11, so the ground a point stands
    // on is the plane of the triangle it falls in. A bilinear blend of the four corners instead
    // misses that plane by up to a quarter of the tile's twist -- on this tile, at its middle,
    // by 6.25 cm, which is a quarter of the hull's 0.25 m ride height.
    const int tx = -36;
    const int ty = 19;
    const float h00 = height_metres(kSlopeSeed, tx, ty);
    const float h10 = height_metres(kSlopeSeed, tx + 1, ty);
    const float h01 = height_metres(kSlopeSeed, tx, ty + 1);
    const float h11 = height_metres(kSlopeSeed, tx + 1, ty + 1);
    const float twist = h00 - h10 - h01 + h11;
    ASSERT_GT(std::fabs(twist), 0.20F)
        << "the tile must actually be twisted for this to mean anything";

    // fx == fy == 0.5 sits on the diagonal; either plane gives the same value there.
    const float middle = ground_metres(kSlopeSeed, tiles(tx + 0.5F), tiles(ty + 0.5F));
    EXPECT_NEAR(middle, h00 + ((h10 - h00) * 0.5F) + ((h11 - h10) * 0.5F), 1e-4F);

    // fx = 0.75, fy = 0.25: below the diagonal, so the p00,p10,p11 plane.
    const float high_x = ground_metres(kSlopeSeed, tiles(tx + 0.75F), tiles(ty + 0.25F));
    EXPECT_NEAR(high_x, h00 + ((h10 - h00) * 0.75F) + ((h11 - h10) * 0.25F), 1e-4F);

    // fx = 0.25, fy = 0.75: above it, so the p00,p01,p11 plane.
    const float low_x = ground_metres(kSlopeSeed, tiles(tx + 0.25F), tiles(ty + 0.75F));
    EXPECT_NEAR(low_x, h00 + ((h01 - h00) * 0.75F) + ((h11 - h01) * 0.25F), 1e-4F);
}

TEST(GroundMetres, ATanksFootingInTheSceneIsContinuousAsItDrivesAcrossATileEdge) {
    using namespace protekt::sim;
    Scenario scenario;
    scenario.seed = kSlopeSeed;
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Player, 104, 95});
    Sim sim = Sim::create(scenario);
    const UnitId tank = sim.units().at(0).id;
    sim.enqueue(Order{tank, orders::MoveTo{Fixed::from_int(112), Fixed::from_int(95)}});

    SceneOptions options
        = options_at(106.0F * kMetresPerTile, 95.0F * kMetresPerTile, kSlopeSeed, 4);
    constexpr std::size_t kBoxVertices = 36;
    const auto footing = [&] {
        const std::vector<protekt::render::SceneVertex> v
            = protekt::game::build_scene(sim, options).vertices;
        float lowest = 1e9F;
        for (std::size_t i = v.size() - kBoxVertices; i < v.size(); ++i) {
            lowest = std::min(lowest, v.at(i).position.at(1));
        }
        return lowest;
    };
    float previous = footing();
    float largest = 0.0F;
    int edges_on_the_slope = 0;
    int previous_tile = sim.units().at(0).x.floor_int();
    for (int i = 0; i < 90; ++i) {
        sim.step();
        const float here = footing();
        largest = std::max(largest, std::fabs(here - previous));
        previous = here;
        const int tile = sim.units().at(0).x.floor_int();
        if (tile != previous_tile && tile == 107) {
            ++edges_on_the_slope; // crossed the 106 -> 107 edge, where the tiles differ by 1.25 m
        }
        previous_tile = tile;
    }
    EXPECT_EQ(edges_on_the_slope, 1) << "the tank must cross the sloped edge during the drive";
    // A tank moves 0.2 tile a tick; the old per-tile footing stepped the whole 1.25 m in one.
    EXPECT_LT(largest, kSlopeStepMetres / 3.0F);
}

// ---------------------------------------------------------------- the hover

namespace {

using protekt::game::Hover;
using protekt::game::kDampPerTick;
using protekt::game::kRideHeightMetres;
using protekt::game::kSpringPerTick;
using protekt::game::suspend;
using protekt::game::Suspension;

constexpr float kStepMetres = 2.0F;

/// Ticks for the spring's envelope, which decays by about exp(-kDampPerTick / 2) a tick, to fall to
/// 1% of a step -- doubled for margin. Derived from the constants so retuning cannot silently
/// leave a test waiting for the wrong amount of time.
std::uint32_t settle_ticks() {
    return 2U * static_cast<std::uint32_t>(std::ceil(std::log(100.0F) / (kDampPerTick / 2.0F)));
}

constexpr float settle_band() { return kStepMetres * 0.01F; }

TEST(Hover, AStepInTheGroundOvershootsOnceAndComesBack) {
    Suspension state{0.0F, 0.0F};
    float peak = 0.0F;
    bool passed = false;
    bool came_back = false;
    for (std::uint32_t i = 0; i < settle_ticks(); ++i) {
        state = suspend(state, kStepMetres, 1);
        peak = std::max(peak, state.z);
        passed = passed || state.z > kStepMetres + settle_band();
        came_back = came_back || (passed && state.z < peak - settle_band());
    }
    EXPECT_TRUE(passed) << "an underdamped hull must pass the target";
    EXPECT_TRUE(came_back) << "and then return from the overshoot";
}

TEST(Hover, SettlesWithinTheBandAndStaysThere) {
    // The window is derived from the constants, so a degenerate constant collapses it -- and an
    // empty loop asserts nothing while still reporting PASS. Pin the window before trusting it.
    const std::uint32_t window = settle_ticks();
    ASSERT_GT(window, 0U) << "the settle window must be a real number of ticks";
    ASSERT_LT(window, 100000U) << "and must not have degenerated";
    Suspension state = suspend({0.0F, 0.0F}, kStepMetres, window);
    for (std::uint32_t i = 0; i < window; ++i) {
        EXPECT_LT(std::fabs(state.z - kStepMetres), settle_band());
        state = suspend(state, kStepMetres, 1);
    }
}

TEST(Hover, AHugeStepNeverExceedsABoundedOvershoot) {
    // Damping ratio d / (2 sqrt k) gives the overshoot fraction exp(-pi z / sqrt(1 - z^2)); allow
    // double that for the discrete-time error.
    const float zeta = kDampPerTick / (2.0F * std::sqrt(kSpringPerTick));
    ASSERT_LT(zeta, 1.0F);
    const float fraction = std::exp(-3.14159265F * zeta / std::sqrt(1.0F - (zeta * zeta)));
    constexpr float kBigStep = 16.0F;
    Suspension state{0.0F, 0.0F};
    for (std::uint32_t i = 0; i < 4U * settle_ticks(); ++i) {
        state = suspend(state, kBigStep, 1);
        ASSERT_LE(state.z, kBigStep * (1.0F + (2.0F * fraction)));
        ASSERT_GE(state.z, 0.0F);
    }
}

TEST(Hover, FromRestOnFlatGroundItFloatsAtRideHeightAndStays) {
    Hover hover;
    EXPECT_FALSE(hover.z().has_value());
    constexpr float kGround = 3.0F;
    for (protekt::sim::Tick tick = 0; tick < 3U * settle_ticks(); ++tick) {
        hover.advance(tick, kGround);
        EXPECT_FLOAT_EQ(hover_height(hover.z()), kGround + kRideHeightMetres);
        // The equality above proves the code READS the constant. It cannot catch a wrong VALUE,
        // because both sides move together. These literal bounds pin the value itself.
        EXPECT_GT(hover_height(hover.z()) - kGround, 0.10F)
            << "a hover tank must float visibly, not sit on the ground";
        EXPECT_LT(hover_height(hover.z()) - kGround, 0.60F) << "and must not look detached from it";
    }
}

TEST(Hover, TheSameTicksGiveTheSameHeightEveryTime) {
    const auto run = [] {
        Hover hover;
        for (protekt::sim::Tick tick = 0; tick < 120; ++tick) {
            hover.advance(tick, tick < 40 ? 0.0F : 1.5F);
        }
        return hover_height(hover.z());
    };
    EXPECT_EQ(run(), run());
}

TEST(Hover, TwoTicksInOneFrameLandWhereTwoFramesOfOneTickDo) {
    const Suspension start{1.0F, 0.1F};
    const Suspension one_by_one = suspend(suspend(start, 3.0F, 1), 3.0F, 1);
    const Suspension together = suspend(start, 3.0F, 2);
    EXPECT_EQ(one_by_one.z, together.z);
    EXPECT_EQ(one_by_one.z_vel, together.z_vel);

    // The batch frame sees the final ground for both ticks, so only the constant-ground case is
    // claimed below; a changing ground within one frame is not claimed by this test.
    Hover steady_frames;
    Hover steady_batch;
    steady_frames.advance(0, 2.0F);
    steady_batch.advance(0, 2.0F);
    steady_frames.advance(1, 5.0F);
    steady_frames.advance(2, 5.0F);
    steady_batch.advance(2, 5.0F);
    EXPECT_EQ(steady_frames.z(), steady_batch.z());
}

TEST(Hover, AFrameWithNoTickMovesNothing) {
    Hover hover;
    hover.advance(5, 0.0F);
    hover.advance(6, 1.0F);
    const std::optional<float> before = hover.z();
    hover.advance(6, 1.0F);
    EXPECT_EQ(hover.z(), before);
}

TEST(Hover, OnARampTheHullLagsBelowTheRisingGround) {
    Hover hover;
    hover.advance(0, 0.0F);
    constexpr float kRisePerTick = 0.05F;
    float ground = 0.0F;
    for (protekt::sim::Tick tick = 1; tick <= 10; ++tick) {
        ground += kRisePerTick;
        hover.advance(tick, ground);
    }
    // At rest the hull rides kRideHeightMetres above the ground; climbing, it trails that line.
    EXPECT_LT(hover_height(hover.z()), ground + kRideHeightMetres);
    EXPECT_GT(hover_height(hover.z()), kRideHeightMetres) << "but it has started to rise";
}

TEST(Hover, TheHoveringUnitIsDrawnAtTheHullHeightAndOthersStayOnTheGround) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    const protekt::sim::Unit tank = sim.units().at(0);
    // Centred on the tank: a unit outside the meshed radius is not drawn at all.
    SceneOptions options
        = options_at(static_cast<float>(tank.x.raw) / static_cast<float>(protekt::sim::Fixed::kOne)
                         * kMetresPerTile,
                     static_cast<float>(tank.y.raw) / static_cast<float>(protekt::sim::Fixed::kOne)
                         * kMetresPerTile);
    const auto base = protekt::game::build_scene(sim, options);
    const float ground = protekt::game::ground_metres(42, tank.x, tank.y);
    options.hover = SceneOptions::Hovering{tank.id, ground + 5.0F};
    const auto raised = protekt::game::build_scene(sim, options);
    ASSERT_EQ(base.vertices.size(), raised.vertices.size());
    std::size_t moved = 0;
    for (std::size_t i = 0; i < base.vertices.size(); ++i) {
        const float dy = raised.vertices.at(i).position.at(1) - base.vertices.at(i).position.at(1);
        if (dy != 0.0F) {
            EXPECT_NEAR(dy, 5.0F, kTolerance);
            ++moved;
        }
    }
    EXPECT_EQ(moved, 36U) << "exactly one box (36 vertices) moves";
}

} // namespace

// ---------------------------------------------------------------- boost

namespace {

using protekt::game::kBoostLiftMetres;

/// Ticks for a boost lift to settle: the same envelope as `settle_ticks`, over the lift.
std::uint32_t boost_window() { return settle_ticks(); }

/// A hover at rest on flat ground at height 0, ticked to `tick` so it has a clock.
Hover resting_hover(protekt::sim::Tick& tick) {
    Hover hover;
    hover.advance(0, 0.0F);
    tick = 0;
    return hover;
}

TEST(Boost, RaisesTheHullAndItSettlesAtTheRaisedHeightWhileHeld) {
    // The lift is pinned by a human-chosen literal band, not by restating the constant: visible
    // against the 1.8 m hull, not detached from it.
    EXPECT_GT(kBoostLiftMetres, 0.5F);
    EXPECT_LT(kBoostLiftMetres, 1.5F);
    const std::uint32_t window = boost_window();
    ASSERT_GT(window, 0U);
    ASSERT_LT(window, 100000U);
    protekt::sim::Tick tick = 0;
    Hover hover = resting_hover(tick);
    for (std::uint32_t i = 0; i < window; ++i) {
        hover.advance(++tick, 0.0F, true);
    }
    EXPECT_GT(hover_height(hover.z()), kRideHeightMetres + 0.5F)
        << "boost must lift visibly above rest";
    EXPECT_NEAR(hover_height(hover.z()), kRideHeightMetres + kBoostLiftMetres,
                kBoostLiftMetres * 0.01F);
    for (std::uint32_t i = 0; i < window; ++i) {
        hover.advance(++tick, 0.0F, true);
        EXPECT_NEAR(hover_height(hover.z()), kRideHeightMetres + kBoostLiftMetres,
                    kBoostLiftMetres * 0.01F)
            << "and stays there while held";
    }
}

TEST(Boost, OvershootsOnceBeforeSettling) {
    const std::uint32_t window = boost_window();
    ASSERT_GT(window, 0U);
    const float top = kRideHeightMetres + kBoostLiftMetres;
    const float band = kBoostLiftMetres * 0.01F;
    protekt::sim::Tick tick = 0;
    Hover hover = resting_hover(tick);
    float peak = 0.0F;
    bool passed = false;
    bool came_back = false;
    int crossings_up = 0;
    bool above = false;
    for (std::uint32_t i = 0; i < window; ++i) {
        hover.advance(++tick, 0.0F, true);
        const float z = hover_height(hover.z());
        peak = std::max(peak, z);
        passed = passed || z > top + band;
        came_back = came_back || (passed && z < peak - band);
        const bool now_above = z > top + band;
        if (now_above && !above) {
            ++crossings_up;
        }
        above = now_above;
    }
    EXPECT_TRUE(passed) << "the hull must rise past the raised height";
    EXPECT_TRUE(came_back) << "and come down a bit lower";
    EXPECT_EQ(crossings_up, 1) << "once, not a ringing";
    EXPECT_LT(peak, top + (0.2F * kBoostLiftMetres)) << "and only a little";
}

TEST(Boost, ReleasingBringsTheHullBackToRestingHeight) {
    const std::uint32_t window = boost_window();
    ASSERT_GT(window, 0U);
    protekt::sim::Tick tick = 0;
    Hover hover = resting_hover(tick);
    for (std::uint32_t i = 0; i < window; ++i) {
        hover.advance(++tick, 0.0F, true);
    }
    ASSERT_GT(hover_height(hover.z()), kRideHeightMetres + 0.5F);
    for (std::uint32_t i = 0; i < window; ++i) {
        hover.advance(++tick, 0.0F, false);
    }
    EXPECT_NEAR(hover_height(hover.z()), kRideHeightMetres, kBoostLiftMetres * 0.01F);
}

TEST(Boost, TwoOneTickCallsEqualOneTwoTickCallBitForBit) {
    Hover frames;
    Hover batch;
    frames.advance(0, 1.0F);
    batch.advance(0, 1.0F);
    for (const bool boost : {true, false}) {
        // From a mid-rise state, so the spring has velocity and the equality is not trivial.
        frames.advance(3, 1.0F, true);
        batch.advance(3, 1.0F, true);
        frames.advance(4, 1.0F, boost);
        frames.advance(5, 1.0F, boost);
        batch.advance(5, 1.0F, boost); // the same two ticks, arriving in one frame
        ASSERT_TRUE(frames.z().has_value());
        EXPECT_EQ(frames.z(), batch.z());
    }
    EXPECT_GT(hover_height(frames.z()), kRideHeightMetres)
        << "the comparison was made while moving";
}

TEST(Boost, AFrameWithNoTickAndBoostHeldChangesNothing) {
    protekt::sim::Tick tick = 0;
    Hover hover = resting_hover(tick);
    hover.advance(5, 0.0F, true);
    const std::optional<float> before = hover.z();
    hover.advance(5, 0.0F, true);
    EXPECT_EQ(hover.z(), before);
    EXPECT_GT(hover_height(before), kRideHeightMetres)
        << "it had risen, so a no-op is a real claim";
}

TEST(Boost, AbsentBoostIsBitIdenticalToTheHoverWithoutIt) {
    Hover plain;
    Hover explicit_off;
    for (protekt::sim::Tick tick = 0; tick < 120; ++tick) {
        const float ground = tick < 40 ? 0.0F : 1.5F;
        plain.advance(tick, ground);
        explicit_off.advance(tick, ground, false);
        ASSERT_EQ(plain.z(), explicit_off.z());
    }
    // And the value itself is the old hover's: floating at ground + ride height when undisturbed.
    Hover flat;
    for (protekt::sim::Tick tick = 0; tick < 50; ++tick) {
        flat.advance(tick, 2.0F);
    }
    EXPECT_FLOAT_EQ(hover_height(flat.z()), 2.25F);
}

TEST(Boost, HeldKeyIsEHeldNotToggled) {
    protekt::input::HeldKeys keys{};
    EXPECT_FALSE(keys.boost);
    protekt::input::apply(keys, protekt::input::KeyEvent{8, true});
    EXPECT_TRUE(keys.boost) << "E (HID 8) is boost";
    protekt::input::apply(keys, protekt::input::KeyEvent{8, true});
    EXPECT_TRUE(keys.boost) << "an OS repeat changes nothing";
    protekt::input::apply(keys, protekt::input::KeyEvent{8, false});
    EXPECT_FALSE(keys.boost) << "release turns it off";
    EXPECT_FALSE(keys.any()) << "and it is not a movement key";
}

// ---- the colour calibration view: three bars, red, green, blue, and nothing else ----

using protekt::game::build_calibration_scene;

SceneOptions calibration_options() {
    SceneOptions o = options_at(100.0F, 100.0F);
    o.camera.yaw = 0.7F;
    o.camera.pitch = -0.2F;
    return o;
}

/// The colour groups of a scene, in order of first appearance: runs of vertices sharing one colour.
std::vector<std::vector<protekt::render::SceneVertex>> colour_groups(const Scene& scene) {
    std::vector<std::vector<protekt::render::SceneVertex>> groups;
    for (const auto& v : scene.vertices) {
        if (groups.empty() || groups.back().front().colour != v.colour) {
            groups.emplace_back();
        }
        groups.back().push_back(v);
    }
    return groups;
}

TEST(Calibration, EmitsExactlyThreeUniformColourGroupsRedGreenBlue) {
    const Scene scene = build_calibration_scene(calibration_options());
    const auto groups = colour_groups(scene);
    ASSERT_EQ(groups.size(), 3U);
    const std::array<std::array<float, 3>, 3> expected{
        {{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}};
    for (std::size_t i = 0; i < 3; ++i) {
        ASSERT_FALSE(groups.at(i).empty());
        for (const auto& v : groups.at(i)) {
            EXPECT_EQ(v.colour, expected.at(i)) << "bar " << i;
        }
    }
}

TEST(Calibration, TheBarsSpanTheFullFrameWidthAndEachTheFullHeight) {
    const SceneOptions options = calibration_options();
    const Scene scene = build_calibration_scene(options);
    const auto groups = colour_groups(scene);
    ASSERT_EQ(groups.size(), 3U);
    const float width = static_cast<float>(options.extent.width);
    const float height = static_cast<float>(options.extent.height);
    float previous_right = 0.0F;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        float left = 1e9F;
        float right = -1e9F;
        float top = 1e9F;
        float bottom = -1e9F;
        for (const auto& v : groups.at(i)) {
            const auto p = protekt::game::project(options.camera, options.extent, v.position[0],
                                                  v.position[1], v.position[2]);
            ASSERT_GT(p.depth, 0.0F);
            left = std::min(left, p.x);
            right = std::max(right, p.x);
            top = std::min(top, p.y);
            bottom = std::max(bottom, p.y);
        }
        EXPECT_NEAR(top, 0.0F, 0.5F) << "bar " << i;
        EXPECT_NEAR(bottom, height, 0.5F) << "bar " << i;
        EXPECT_NEAR(left, previous_right, 0.5F) << "bar " << i << " starts where the last ended";
        EXPECT_GT(right, left);
        previous_right = right;
    }
    EXPECT_NEAR(previous_right, width, 0.5F);
}

TEST(Calibration, TheSceneIsBitIdenticalBeforeAndAfterTheCalibrationView) {
    using namespace protekt::sim;
    Scenario scenario;
    scenario.seed = 42;
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Player, 100, 100});
    Sim sim = Sim::create(scenario);
    SceneOptions options = calibration_options();
    options.viewer_unit = sim.units().at(0).id;
    options.aim = protekt::game::Aim{640.0F, 360.0F};

    const Scene before = build_scene(sim, options);
    const Tick tick = sim.tick();
    const std::uint64_t hash = sim.state_hash();

    protekt::input::InputFrame press{};
    press.keys.push_back(protekt::input::KeyEvent{protekt::input::kKeyC, true});
    ASSERT_TRUE(protekt::input::calibration_toggled(press));
    const Scene bars = build_calibration_scene(options); // toggled on
    EXPECT_FALSE(bars.vertices.empty());
    ASSERT_TRUE(protekt::input::calibration_toggled(press)); // toggled off

    const Scene after = build_scene(sim, options);
    EXPECT_EQ(sim.tick(), tick);
    EXPECT_EQ(sim.state_hash(), hash);
    ASSERT_EQ(before.vertices.size(), after.vertices.size());
    EXPECT_EQ(before.view_projection, after.view_projection);
    for (std::size_t i = 0; i < before.vertices.size(); ++i) {
        EXPECT_EQ(before.vertices.at(i).position, after.vertices.at(i).position);
        EXPECT_EQ(before.vertices.at(i).colour, after.vertices.at(i).colour);
    }
}

TEST(Calibration, TwoBuildsProduceIdenticalVertexData) {
    const SceneOptions options = calibration_options();
    const Scene a = build_calibration_scene(options);
    const Scene b = build_calibration_scene(options);
    ASSERT_EQ(a.vertices.size(), b.vertices.size());
    EXPECT_EQ(a.view_projection, b.view_projection);
    for (std::size_t i = 0; i < a.vertices.size(); ++i) {
        EXPECT_EQ(a.vertices.at(i).position, b.vertices.at(i).position);
        EXPECT_EQ(a.vertices.at(i).colour, b.vertices.at(i).colour);
    }
}

// ---------------------------------------------------------------- the hit flash

using protekt::game::HitFlash;
using protekt::game::kHitFlashSeconds;
using protekt::game::kHitFlashTicks;
using protekt::game::unit_was_hit;

TEST(HitFlash, IsOffUntilTheFirstHit) {
    const HitFlash flash;
    EXPECT_FLOAT_EQ(flash.level(0), 0.0F);
    EXPECT_FLOAT_EQ(flash.level(1000), 0.0F);
}

TEST(HitFlash, ARealHitSetsItFullThenItDecaysToZeroAndStaysThere) {
    HitFlash flash;
    flash.hit(100);
    EXPECT_FLOAT_EQ(flash.level(100), 1.0F);
    float previous = 1.0F;
    for (std::uint32_t t = 101; t < 100 + kHitFlashTicks; ++t) {
        EXPECT_LT(flash.level(t), previous) << "tick " << t;
        previous = flash.level(t);
    }
    EXPECT_FLOAT_EQ(flash.level(100 + kHitFlashTicks), 0.0F);
    EXPECT_FLOAT_EQ(flash.level(100 + kHitFlashTicks + 500), 0.0F);
}

TEST(HitFlash, TheDurationIsTheSecondsPublished) {
    EXPECT_NEAR(static_cast<float>(kHitFlashTicks) / 30.0F, kHitFlashSeconds, 1.0F / 30.0F);
}

TEST(HitFlash, ASecondHitRearmsItFromTheTop) {
    HitFlash flash;
    flash.hit(100);
    const std::uint32_t later = 100 + (kHitFlashTicks / 2);
    ASSERT_LT(flash.level(later), 1.0F);
    flash.hit(later);
    EXPECT_FLOAT_EQ(flash.level(later), 1.0F);
    EXPECT_GT(flash.level(100 + kHitFlashTicks), 0.0F) << "the first hit's end no longer ends it";
}

TEST(HitFlash, OnlyALandedBoltOnThatUnitCounts) {
    using protekt::sim::BattleEvent;
    using protekt::sim::EventKind;
    // A HitTaken names the victim; a Fired does NOT mean a hit any more, because the damage now
    // lands a flight-time later. Reading Fired here is what silently killed the flash once
    // projectiles arrived, so this test insists on the event that means what we want.
    const auto hit = [](std::uint32_t victim) {
        return BattleEvent{5, EventKind::HitTaken, protekt::sim::UnitId{9},
                           protekt::sim::UnitId{victim}, 90};
    };
    const auto fired_at = [](std::uint32_t victim) {
        return BattleEvent{5, EventKind::Fired, protekt::sim::UnitId{9},
                           protekt::sim::UnitId{victim}, 100};
    };
    EXPECT_TRUE(unit_was_hit(std::vector{hit(1)}, protekt::sim::UnitId{1}));
    EXPECT_FALSE(unit_was_hit(std::vector{hit(2)}, protekt::sim::UnitId{1})) << "someone else";
    EXPECT_FALSE(unit_was_hit(std::vector{fired_at(1)}, protekt::sim::UnitId{1}))
        << "a shot LEAVING the gun is not a hit";
    EXPECT_FALSE(unit_was_hit(std::vector<BattleEvent>{}, protekt::sim::UnitId{1}));
}

TEST(SceneHitFlash, ZeroDrawsNothingAtAll) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::Scenario{
        42, {{protekt::sim::UnitKind::Tank, protekt::sim::Side::Player, 40, 40}}});
    SceneOptions plain = options_at(80.0F, 80.0F);
    SceneOptions zero = plain;
    zero.hit_flash = 0.0F;
    const Scene a = protekt::game::build_scene(sim, plain);
    const Scene b = protekt::game::build_scene(sim, zero);
    ASSERT_EQ(a.vertices.size(), b.vertices.size());
    for (std::size_t i = 0; i < a.vertices.size(); ++i) {
        EXPECT_EQ(a.vertices.at(i).colour, b.vertices.at(i).colour);
        EXPECT_EQ(a.vertices.at(i).position, b.vertices.at(i).position);
    }
}

TEST(SceneHitFlash, AFlashIsAWashTowardRedNotAnOpaqueOverlay) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::Scenario{
        42, {{protekt::sim::UnitKind::Tank, protekt::sim::Side::Player, 40, 40}}});
    SceneOptions plain = options_at(80.0F, 80.0F);
    SceneOptions flashing = plain;
    flashing.hit_flash = 1.0F;
    const Scene a = protekt::game::build_scene(sim, plain);
    const Scene b = protekt::game::build_scene(sim, flashing);
    ASSERT_EQ(a.vertices.size(), b.vertices.size()) << "a tint, not new geometry";

    std::set<std::array<float, 3>> distinct;
    for (std::size_t i = 0; i < a.vertices.size(); ++i) {
        const auto& was = a.vertices.at(i).colour;
        const auto& now = b.vertices.at(i).colour;
        EXPECT_EQ(a.vertices.at(i).position, b.vertices.at(i).position);
        EXPECT_LE(now[1], was[1] + 1e-6F) << "green only falls";
        EXPECT_LE(now[2], was[2] + 1e-6F) << "blue only falls";
        EXPECT_LT(now[1] + now[2], was[1] + was[2] + 1e-6F);
        distinct.insert(now);
    }
    EXPECT_GT(distinct.size(), 3U) << "the battle is still readable under the wash";
    // The wash is red-dominant on average and strictly changes the frame.
    EXPECT_NE(a.vertices.front().colour, b.vertices.front().colour);
}

TEST(SceneHitFlash, AHalfFlashIsHalfAsStrongAsAFullOne) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::Scenario{
        42, {{protekt::sim::UnitKind::Tank, protekt::sim::Side::Player, 40, 40}}});
    SceneOptions plain = options_at(80.0F, 80.0F);
    SceneOptions half = plain;
    half.hit_flash = 0.5F;
    SceneOptions full = plain;
    full.hit_flash = 1.0F;
    const Scene a = protekt::game::build_scene(sim, plain);
    const Scene h = protekt::game::build_scene(sim, half);
    const Scene f = protekt::game::build_scene(sim, full);
    const float d_half = a.vertices.front().colour[1] - h.vertices.front().colour[1];
    const float d_full = a.vertices.front().colour[1] - f.vertices.front().colour[1];
    ASSERT_GT(d_full, 0.0F);
    EXPECT_NEAR(d_half, d_full * 0.5F, 1e-5F);
}

} // namespace

// A bolt you can see: every projectile the sim has in the air is one small box in the scene.

namespace {

using protekt::sim::Side;
using protekt::sim::UnitKind;

constexpr std::size_t kBoxVertices = 36; // six quads, two triangles each, three vertices each

struct BoltPlace {
    Side side{};
    UnitKind kind{};
    int tile_x{};
    int tile_y{};
};

protekt::sim::Sim sim_of(const std::vector<BoltPlace>& places) {
    protekt::sim::Scenario scenario{};
    scenario.seed = 42;
    for (const BoltPlace& place : places) {
        protekt::sim::Spawn spawn{};
        spawn.side = place.side;
        spawn.kind = place.kind;
        spawn.tile_x = place.tile_x;
        spawn.tile_y = place.tile_y;
        scenario.spawns.push_back(spawn);
    }
    return protekt::sim::Sim::create(scenario);
}

/// Only the player's tank is armed: after it fires there is exactly one bolt, and it is the
/// viewer's.
protekt::sim::Sim one_player_bolt_sim() {
    return sim_of(
        {{Side::Player, UnitKind::Tank, 100, 100}, {Side::Enemy, UnitKind::Factory, 106, 100}});
}

/// Only the enemy's tank is armed: exactly one bolt, and it is not the viewer's.
protekt::sim::Sim one_enemy_bolt_sim() {
    return sim_of(
        {{Side::Player, UnitKind::Factory, 100, 100}, {Side::Enemy, UnitKind::Tank, 106, 100}});
}

/// Both tanks are armed and in range of each other: after the volley there is one bolt per side.
protekt::sim::Sim two_bolt_sim() {
    return sim_of(
        {{Side::Player, UnitKind::Tank, 100, 100}, {Side::Enemy, UnitKind::Tank, 106, 100}});
}

/// Steps until a bolt is in the air (the units are stationary, so nothing else moves meanwhile).
void step_until_bolt(protekt::sim::Sim& sim) {
    for (int step = 0; step < 200 && sim.projectiles().empty(); ++step) {
        sim.step();
    }
    ASSERT_FALSE(sim.projectiles().empty()) << "no gun fired within 200 ticks";
}

SceneOptions bolt_options(const protekt::sim::Sim& sim) {
    SceneOptions o = options_at(0.0F, 0.0F, 42, 20);
    o.camera.x = static_cast<float>(sim.units().front().x.raw) / 65536.0F * kMetresPerTile;
    o.camera.z = static_cast<float>(sim.units().front().y.raw) / 65536.0F * kMetresPerTile;
    return o;
}

/// The vertices a scene gained over the same scene with no bolts. Bolts are appended after the
/// units and there is no radar or reticle in these options, so they are the tail.
std::vector<protekt::render::SceneVertex> bolt_vertices(const Scene& with, const Scene& without) {
    if (with.vertices.size() < without.vertices.size()) {
        return {};
    }
    return {with.vertices.begin() + static_cast<std::ptrdiff_t>(without.vertices.size()),
            with.vertices.end()};
}

} // namespace

TEST(SceneBolts, ABoltDrawsExactlyOneBox) {
    auto sim = one_player_bolt_sim();
    const auto none = protekt::game::build_scene(sim, bolt_options(sim));
    step_until_bolt(sim);
    ASSERT_EQ(sim.projectiles().size(), 1U);
    const auto with = protekt::game::build_scene(sim, bolt_options(sim));
    EXPECT_GT(with.vertices.size(), none.vertices.size());
    EXPECT_EQ(with.vertices.size() - none.vertices.size(), kBoxVertices);
}

TEST(SceneBolts, TheBoltIsWhereTheSimulationSaysItIs) {
    auto sim = one_player_bolt_sim();
    const auto none = protekt::game::build_scene(sim, bolt_options(sim));
    step_until_bolt(sim);
    ASSERT_EQ(sim.projectiles().size(), 1U);
    const auto& bolt = sim.projectiles().front();
    const auto drawn = bolt_vertices(protekt::game::build_scene(sim, bolt_options(sim)), none);
    ASSERT_EQ(drawn.size(), kBoxVertices);

    std::array<float, 3> low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max()};
    std::array<float, 3> high{std::numeric_limits<float>::lowest(),
                              std::numeric_limits<float>::lowest(),
                              std::numeric_limits<float>::lowest()};
    for (const auto& v : drawn) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            low.at(axis) = std::min(low.at(axis), v.position.at(axis));
            high.at(axis) = std::max(high.at(axis), v.position.at(axis));
        }
    }
    const float want_x = static_cast<float>(bolt.x.raw) / 65536.0F * kMetresPerTile;
    const float want_z = static_cast<float>(bolt.y.raw) / 65536.0F * kMetresPerTile;
    EXPECT_NEAR((low[0] + high[0]) * 0.5F, want_x, kTolerance);
    EXPECT_NEAR((low[2] + high[2]) * 0.5F, want_z, kTolerance);
    // And it rides above the ground at its own position, at barrel height.
    const float ground = protekt::game::ground_metres(42, bolt.x, bolt.y);
    EXPECT_NEAR((low[1] + high[1]) * 0.5F, ground + 1.2F, 0.2F);
}

TEST(SceneBolts, NothingInTheAirAddsNothing) {
    // A sim with no armed unit never has a bolt, however long it runs: its scene never grows.
    auto sim = sim_of(
        {{Side::Player, UnitKind::Factory, 100, 100}, {Side::Enemy, UnitKind::Factory, 106, 100}});
    const auto before = protekt::game::build_scene(sim, bolt_options(sim));
    for (int step = 0; step < 30; ++step) {
        sim.step();
    }
    ASSERT_TRUE(sim.projectiles().empty());
    const auto after = protekt::game::build_scene(sim, bolt_options(sim));
    EXPECT_EQ(after.vertices.size(), before.vertices.size());
    EXPECT_EQ(before.vertices.size(),
              protekt::game::build_scene(sim, bolt_options(sim)).vertices.size());
}

TEST(SceneBolts, BothSidesAreDrawn) {
    auto sim = two_bolt_sim();
    const auto none = protekt::game::build_scene(sim, bolt_options(sim));
    step_until_bolt(sim);
    ASSERT_EQ(sim.projectiles().size(), 2U);
    const auto with = protekt::game::build_scene(sim, bolt_options(sim));
    EXPECT_EQ(with.vertices.size() - none.vertices.size(), 2U * kBoxVertices);
}

TEST(SceneBolts, TheColourSaysWhoseBoltItIs) {
    auto sim = two_bolt_sim();
    const auto none = protekt::game::build_scene(sim, bolt_options(sim));
    step_until_bolt(sim);
    ASSERT_EQ(sim.projectiles().size(), 2U);
    const auto drawn = bolt_vertices(protekt::game::build_scene(sim, bolt_options(sim)), none);
    ASSERT_EQ(drawn.size(), 2U * kBoxVertices);
    // Each box is one flat colour, and the two boxes do not share it.
    for (std::size_t i = 1; i < kBoxVertices; ++i) {
        EXPECT_EQ(drawn.at(i).colour, drawn.at(0).colour);
        EXPECT_EQ(drawn.at(kBoxVertices + i).colour, drawn.at(kBoxVertices).colour);
    }
    EXPECT_NE(drawn.at(0).colour, drawn.at(kBoxVertices).colour);

    // Viewer-relative: seen from the other side, the same two bolts swap colours.
    SceneOptions flipped = bolt_options(sim);
    flipped.viewer_side = Side::Enemy;
    SceneOptions plain = bolt_options(sim);
    const auto mine = protekt::game::build_scene(sim, plain);
    const auto theirs = protekt::game::build_scene(sim, flipped);
    ASSERT_EQ(mine.vertices.size(), theirs.vertices.size());
    const std::size_t first = mine.vertices.size() - 2U * kBoxVertices;
    EXPECT_EQ(mine.vertices.at(first).colour, theirs.vertices.at(first + kBoxVertices).colour);
    EXPECT_EQ(mine.vertices.at(first + kBoxVertices).colour, theirs.vertices.at(first).colour);
}

TEST(SceneBolts, AHostileBoltDrawsToo) {
    auto sim = one_enemy_bolt_sim();
    const auto none = protekt::game::build_scene(sim, bolt_options(sim));
    step_until_bolt(sim);
    ASSERT_EQ(sim.projectiles().size(), 1U);
    const auto with = protekt::game::build_scene(sim, bolt_options(sim));
    EXPECT_EQ(with.vertices.size() - none.vertices.size(), kBoxVertices);
}

// The viewer's tank, drawn with a hull, a turret and a barrel so the steering model can be seen.

namespace {

using protekt::sim::UnitId;

constexpr std::size_t kHullVertices = 36;
constexpr std::size_t kTurretVertices = 36;
constexpr std::size_t kBarrelVertices = 36;
constexpr std::size_t kFacingTankVertices = kHullVertices + kTurretVertices + kBarrelVertices;

/// The enemy factory first and the viewer's tank LAST, so the viewer's boxes are the tail of the
/// scene: these options have no radar, reticle or bolts after the units.
protekt::sim::Sim facing_sim() {
    return sim_of(
        {{Side::Enemy, UnitKind::Factory, 106, 100}, {Side::Player, UnitKind::Tank, 100, 100}});
}

const protekt::sim::Unit& viewer_of(const protekt::sim::Sim& sim) { return sim.units().back(); }

SceneOptions facing_options(const protekt::sim::Sim& sim) {
    SceneOptions o = options_at(0.0F, 0.0F, 42, 20);
    o.camera.x = static_cast<float>(viewer_of(sim).x.raw) / 65536.0F * kMetresPerTile;
    o.camera.z = static_cast<float>(viewer_of(sim).y.raw) / 65536.0F * kMetresPerTile;
    return o;
}

Scene facing_scene(const protekt::sim::Sim& sim, float heading, float turret) {
    SceneOptions o = facing_options(sim);
    o.facing = SceneOptions::Facing{viewer_of(sim).id, heading, turret};
    return protekt::game::build_scene(sim, o);
}

/// The barrel is the last box of the viewer's tank, and the viewer's tank is the end of the scene.
std::vector<protekt::render::SceneVertex> barrel_of(const Scene& scene) {
    const auto first = scene.vertices.end() - static_cast<std::ptrdiff_t>(kBarrelVertices);
    return {first, scene.vertices.end()};
}

/// Where the barrel lies from the tank's own centre, on the map plane: the mean of its vertices.
/// The barrel is symmetric about its own axis, so this points along it.
std::array<float, 2> barrel_direction(const Scene& scene, const protekt::sim::Unit& unit) {
    const auto barrel = barrel_of(scene);
    float x = 0.0F;
    float z = 0.0F;
    for (const auto& v : barrel) {
        x += v.position[0];
        z += v.position[2];
    }
    x /= static_cast<float>(barrel.size());
    z /= static_cast<float>(barrel.size());
    x -= static_cast<float>(unit.x.raw) / 65536.0F * kMetresPerTile;
    z -= static_cast<float>(unit.y.raw) / 65536.0F * kMetresPerTile;
    const float length = std::hypot(x, z);
    return {x / length, z / length};
}

/// How far the barrel's far end is from the tank's centre, along the map plane.
float barrel_reach(const Scene& scene, const protekt::sim::Unit& unit) {
    const float ux = static_cast<float>(unit.x.raw) / 65536.0F * kMetresPerTile;
    const float uz = static_cast<float>(unit.y.raw) / 65536.0F * kMetresPerTile;
    float reach = 0.0F;
    for (const auto& v : barrel_of(scene)) {
        reach = std::max(reach, std::hypot(v.position[0] - ux, v.position[2] - uz));
    }
    return reach;
}

bool same_vertices(const std::vector<protekt::render::SceneVertex>& a,
                   const std::vector<protekt::render::SceneVertex>& b) {
    return a.size() == b.size()
           && std::equal(a.begin(), a.end(), b.begin(), [](const auto& l, const auto& r) {
                  return l.position == r.position && l.colour == r.colour;
              });
}

} // namespace

TEST(SceneFacing, TheTurretFollowsTheFacingNotTheBody) {
    const auto sim = facing_sim();
    const auto base = barrel_of(facing_scene(sim, 0.3F, 0.4F));
    const auto other_turret = barrel_of(facing_scene(sim, 0.3F, 1.1F));
    const auto other_heading = barrel_of(facing_scene(sim, 0.9F, 0.4F));
    EXPECT_FALSE(same_vertices(base, other_turret)) << "the turret angle must move the barrel";
    EXPECT_FALSE(same_vertices(base, other_heading)) << "the body heading must move the barrel";
    // The turret rides the body: the same sum of the two points the barrel the same way.
    const auto same_sum = barrel_of(facing_scene(sim, 0.7F, 0.0F));
    const auto reference = barrel_of(facing_scene(sim, 0.0F, 0.7F));
    ASSERT_EQ(same_sum.size(), reference.size());
    for (std::size_t i = 0; i < same_sum.size(); ++i) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            EXPECT_NEAR(same_sum.at(i).position.at(axis), reference.at(i).position.at(axis), 1e-4F);
        }
    }
}

// The convention, derived from the camera and not from a belief about it: the camera looks along
// `math::basis_from(yaw, 0).forward`, which is (sin yaw, 0, cos yaw) -- yaw 0 is +Z, and a POSITIVE
// yaw turns toward +X, which on the (mirrored) screen is the LEFT. The turret's world yaw is
// `heading + turret`, the same sum `PlayerCommander::view_yaw` hands the camera, so the barrel must
// lie along the camera's forward at that yaw. If the rotation sign were flipped the barrel would
// point along (-sin, cos): the x component changes sign for every non-zero angle, so every case
// below that has a non-zero yaw fails (and the cases are chosen so none is its own mirror image).
TEST(SceneFacing, TheBarrelPointsWhereTheTurretLooks) {
    const auto sim = facing_sim();
    const auto& viewer = viewer_of(sim);
    struct Case {
        float heading;
        float turret;
    };
    for (const Case c : {Case{0.0F, 0.5F}, Case{0.0F, -0.5F}, Case{0.6F, 0.0F}, Case{0.3F, 0.4F},
                         Case{-0.8F, 0.3F}, Case{2.0F, -0.7F}}) {
        const auto camera = protekt::math::basis_from(c.heading + c.turret, 0.0F).forward;
        const auto got = barrel_direction(facing_scene(sim, c.heading, c.turret), viewer);
        EXPECT_NEAR(got.at(0), camera.at(0), 1e-3F) << c.heading << " / " << c.turret;
        EXPECT_NEAR(got.at(1), camera.at(2), 1e-3F) << c.heading << " / " << c.turret;
    }
    // And the absolute reading, so the test does not lean on basis_from alone: yaw +0.5 lies toward
    // +X and +Z.
    const auto turned = barrel_direction(facing_scene(sim, 0.0F, 0.5F), viewer);
    EXPECT_GT(turned.at(0), 0.3F);
    EXPECT_GT(turned.at(1), 0.3F);
    // A barrel you can read: well clear of the hull, which is 0.3 tiles from the centre.
    EXPECT_GT(barrel_reach(facing_scene(sim, 0.0F, 0.0F), viewer), kMetresPerTile * 0.3F * 1.4F);
}

TEST(SceneFacing, OnlyTheViewersTankChanges) {
    const auto sim = facing_sim();
    const SceneOptions plain = facing_options(sim);
    const Scene without = protekt::game::build_scene(sim, plain);
    const Scene with = facing_scene(sim, 0.7F, -0.4F);
    EXPECT_EQ(with.vertices.size() - without.vertices.size(), kFacingTankVertices - kBoxVertices);
    // Everything before the viewer's tank -- the terrain and the factory -- is untouched.
    const auto shared = static_cast<std::ptrdiff_t>(without.vertices.size() - kBoxVertices);
    EXPECT_TRUE(same_vertices({without.vertices.begin(), without.vertices.begin() + shared},
                              {with.vertices.begin(), with.vertices.begin() + shared}));
    // A facing that names someone else rotates THAT unit and leaves the viewer a plain box.
    SceneOptions elsewhere = plain;
    elsewhere.facing = SceneOptions::Facing{sim.units().front().id, 0.7F, -0.4F};
    const Scene other = protekt::game::build_scene(sim, elsewhere);
    EXPECT_GT(other.vertices.size(), without.vertices.size());
    EXPECT_TRUE(same_vertices(
        {without.vertices.end() - static_cast<std::ptrdiff_t>(kBoxVertices),
         without.vertices.end()},
        {other.vertices.end() - static_cast<std::ptrdiff_t>(kBoxVertices), other.vertices.end()}));
}

TEST(SceneFacing, AFrameWithoutFacingIsByteIdentical) {
    const auto sim = facing_sim();
    SceneOptions absent = facing_options(sim);
    absent.facing = std::nullopt;
    const SceneOptions never_heard = facing_options(sim);
    const Scene a = protekt::game::build_scene(sim, absent);
    const Scene b = protekt::game::build_scene(sim, never_heard);
    EXPECT_TRUE(same_vertices(a.vertices, b.vertices));
    // The viewer's tank is still one axis-aligned box of the size it always was.
    const auto& viewer = viewer_of(sim);
    const float ux = static_cast<float>(viewer.x.raw) / 65536.0F * kMetresPerTile;
    const auto tail = a.vertices.end() - static_cast<std::ptrdiff_t>(kBoxVertices);
    for (auto v = tail; v != a.vertices.end(); ++v) {
        EXPECT_NEAR(std::abs(v->position[0] - ux), kMetresPerTile * 0.3F, 1e-4F);
    }
}
