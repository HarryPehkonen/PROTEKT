// The world as the player sees it: the terrain mesh and the unit boxes.
//
// These tests run on the gate box, which has no display -- because building a scene is pure
// geometry, and only PRESENTING it needs a screen.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "haze.hpp"
#include "protekt/game/atlas.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/radar.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/weapon.hpp"
#include "protekt/input/bindings.hpp"
#include "protekt/render/renderer.hpp"
#include "protekt/sim/orders.hpp"
#include "protekt/sim/outcome.hpp"
#include "protekt/sim/projectile.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/world/world.hpp"

namespace {

// A tank is eight shaded prisms (12 triangles each) and nothing else: its camo and track links are
// in the atlas tiles (D2e), so the 7 camo quads and 16 track-link quads that used to follow the
// prisms -- 138 vertices -- are gone. The eighth prism is the turret's team-colour roof panel
// (D2h).
constexpr std::size_t kTankVertices = std::size_t{12} * 36;

using protekt::game::Camera;
using protekt::game::kHullHalfMetres;
using protekt::game::kMetresPerHeightUnit;
using protekt::game::kMetresPerTile;
using protekt::game::Scene;
using protekt::game::SceneOptions;

constexpr float kTolerance = 0.01F;

SceneOptions options_at(float x, float z, std::uint64_t seed = 42, int radius = 8) {
    SceneOptions o;
    o.seed = seed;
    o.radius_tiles = radius;
    o.camera.x = x;
    o.camera.z = z;
    o.camera.y = 8.0F;
    // A test rig builds a WORLD scene: the ship instrument is on in the game (its own test turns it
    // on), but here its HUD vertices are the tail of the vertex list in every scene with no radar,
    // no reticle and no roster, and these tests read that tail as a tank, a bolt or the reticle.
    o.show_ship_gauge = false;
    // The same for the ledger (C6): the game draws it, the rigs must not, or every tail read here
    // would be reading a scrap count.
    o.show_ledger = false;
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

/// The terrain mesh alone: (2r+2)^2 tiles of six vertices, then the sky, then everything else.
std::size_t terrain_vertex_count(int radius) {
    const auto side = static_cast<std::size_t>(radius) * 2U + 2U;
    return side * side * 6U;
}

/// The scene with its sky cut out, for tests about the ground that the sky's far-away vertices
/// would otherwise swamp.
Scene without_sky(Scene scene, int radius) {
    const auto first
        = scene.vertices.begin() + static_cast<std::ptrdiff_t>(terrain_vertex_count(radius));
    scene.vertices.erase(first,
                         first + static_cast<std::ptrdiff_t>(protekt::game::kSkyVertexCount));
    return scene;
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

    // World +x lands on the RIGHT of the screen for a camera looking down +z (the oracle in
    // test_math), the two sides disagree in sign, and a point further off-axis projects further
    // from the centre.
    const auto plus_x = project(20.0F, 2.0F, 40.0F);
    const auto minus_x = project(0.0F, 2.0F, 40.0F);
    EXPECT_GT(plus_x[3], 0.0F);
    EXPECT_GT(minus_x[3], 0.0F);
    EXPECT_GT(plus_x[0], 0.0F) << "+x is right of centre";
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

// ---------------------------------------------------------------- the ground is lit by its slope
//
// A terrain quad's vertex colour is the light on its own surface, relative to a FLAT tile: the same
// half-Lambert ramp the solids use (`face_light`), divided by what that ramp gives for straight up
// (0.958, not 1.0), so a flat tile keeps exactly the colour it always had. The oracle here is
// written out independently of the scene's code: the quad's mean gradient, the normal it implies,
// and the ramp, from `kSunDirection`.

namespace {

constexpr std::uint64_t kSlopeSeedForLight = 42; // the sloped world, see "smooth elevation" below

struct ShadedTile {
    protekt::math::Vec3 colour;
    float h00;
    float h10;
    float h01;
    float h11;
    std::size_t vertices; ///< the scene's own vertex count, for the count guard
};

/// The colour of tile (tx, ty)'s quad, read from a scene whose camera hangs just above it, so the
/// fog (which leaves a vertex within 12 m alone) has not touched it.
ShadedTile shaded_tile(std::uint64_t seed, int tx, int ty) {
    constexpr int kRadius = 2;
    const float centre_x = (static_cast<float>(tx) + 0.5F) * kMetresPerTile;
    const float centre_z = (static_cast<float>(ty) + 0.5F) * kMetresPerTile;
    SceneOptions o = options_at(centre_x, centre_z, seed, kRadius);
    o.camera.y = protekt::game::height_metres(seed, tx, ty) + 1.0F;
    // Terrain does not depend on the sim, so one sim serves every tile (all on the one seed).
    static const protekt::sim::Sim sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    const Scene scene = protekt::game::build_scene(sim, o);
    // Quads are emitted row by row from (centre - radius - 1); the camera's tile is tx, ty.
    const auto side = static_cast<std::size_t>(kRadius) * 2U + 2U;
    const std::size_t quad = ((static_cast<std::size_t>(kRadius) + 1U) * side)
                             + static_cast<std::size_t>(kRadius) + 1U;
    return ShadedTile{scene.vertices.at(quad * 6U).colour,
                      protekt::game::height_metres(seed, tx, ty),
                      protekt::game::height_metres(seed, tx + 1, ty),
                      protekt::game::height_metres(seed, tx, ty + 1),
                      protekt::game::height_metres(seed, tx + 1, ty + 1),
                      scene.vertices.size()};
}

float oracle_ramp(const protekt::math::Vec3& normal) {
    const protekt::math::Vec3 sun = protekt::math::normalise(protekt::game::kSunDirection);
    const float facing = std::clamp(protekt::math::dot(normal, sun), -1.0F, 1.0F);
    return 0.45F + (0.55F * (0.5F + (0.5F * facing)));
}

/// What the colour of this tile must be: the ramp on the mean-gradient normal over the ramp on up.
float oracle_light(const ShadedTile& t) {
    const float dhdx = ((t.h10 + t.h11) - (t.h00 + t.h01)) / (2.0F * kMetresPerTile);
    const float dhdz = ((t.h01 + t.h11) - (t.h00 + t.h10)) / (2.0F * kMetresPerTile);
    const protekt::math::Vec3 normal = protekt::math::normalise({-dhdx, 1.0F, -dhdz});
    return oracle_ramp(normal) / oracle_ramp({0.0F, 1.0F, 0.0F});
}

} // namespace

TEST(SceneTerrainLight, ASlopedQuadIsShadedByTheSolidsRampAndTheDirectionIsRight) {
    float brightest = 0.0F;
    float darkest = 2.0F;
    for (int ty = 90; ty < 120; ++ty) {
        for (int tx = 100; tx < 130; ++tx) {
            const ShadedTile t = shaded_tile(kSlopeSeedForLight, tx, ty);
            const float want = oracle_light(t);
            for (std::size_t c = 0; c < 3; ++c) {
                EXPECT_NEAR(t.colour.at(c), want, 2e-4F) << "tile " << tx << "," << ty;
            }
            brightest = std::max(brightest, t.colour[0]);
            darkest = std::min(darkest, t.colour[0]);
        }
    }
    EXPECT_GT(brightest, 1.02F) << "a slope toward the sun is brighter than flat ground";
    EXPECT_LT(darkest, 0.90F) << "a slope away from the sun is darker than flat ground";
}

TEST(SceneTerrainLight, TwoTilesWithTheSameCornerHeightsGetTheSameColour) {
    // Heights relative to the tile's own p00: the colour is a function of the corners' shape, not
    // of where the tile is or how high it sits.
    using Shape = std::array<int, 3>;
    std::map<Shape, std::pair<int, int>> seen;
    bool compared = false;
    for (int ty = 90; ty < 110 && !compared; ++ty) {
        for (int tx = 100; tx < 120 && !compared; ++tx) {
            const ShadedTile t = shaded_tile(kSlopeSeedForLight, tx, ty);
            const auto units = [&](float h) {
                return static_cast<int>(std::lround((h - t.h00) / kMetresPerHeightUnit));
            };
            const Shape shape{units(t.h10), units(t.h01), units(t.h11)};
            if (shape == Shape{0, 0, 0}) {
                continue;
            }
            const auto [it, inserted] = seen.emplace(shape, std::pair{tx, ty});
            if (!inserted) {
                const ShadedTile other
                    = shaded_tile(kSlopeSeedForLight, it->second.first, it->second.second);
                EXPECT_EQ(t.colour, other.colour);
                EXPECT_NE(t.colour, protekt::game::kTerrainLight) << "the pair must be sloped";
                compared = true;
            }
        }
    }
    EXPECT_TRUE(compared) << "the scan found no two sloped tiles of one shape";
}

TEST(SceneTerrainLight, AFlatTileKeepsExactlyTheColourItHadBefore) {
    // (32,13), (33,13), (32,14), (33,14) are one height on seed 42.
    const ShadedTile t = shaded_tile(kSlopeSeedForLight, 32, 13);
    ASSERT_EQ(t.h00, t.h10);
    ASSERT_EQ(t.h00, t.h01);
    ASSERT_EQ(t.h00, t.h11);
    EXPECT_EQ(t.colour, protekt::game::kTerrainLight);
}

TEST(SceneTerrainLight, TheVertexCountIsUnchanged) {
    // Two triangles a tile, (2r+2)^2 tiles, then the sky dome starts: nothing was added or split.
    constexpr int kRadius = 8;
    const Scene scene
        = protekt::game::build_scene(protekt::sim::Sim::create(protekt::sim::skirmish(42)),
                                     options_at(60.0F, 60.0F, 42, kRadius));
    EXPECT_EQ(scene.vertices.at(terrain_vertex_count(kRadius)).colour,
              protekt::game::kSkyRingColours.at(0))
        << "the sky must start exactly where an unlit, unsplit terrain mesh ends";
    EXPECT_EQ(scene.vertices.at(terrain_vertex_count(kRadius) - 1).alpha, 1.0F);
}

TEST(SceneTerrain, StaysWhereItWasAsked) {
    const int radius = 6;
    const Scene scene = without_sky(
        protekt::game::build_scene(protekt::sim::Sim::create(protekt::sim::skirmish(7)),
                                   options_at(100.0F, 60.0F, 7, radius)),
        radius);
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
    const Scene scene = without_sky(
        protekt::game::build_scene(protekt::sim::Sim::create(protekt::sim::skirmish(9)),
                                   options_at(90.0F, 90.0F, 9, radius)),
        radius);
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

TEST(SceneTerrain, TheTileIsAPureFunctionOfTheTerrainTypePinnedPerType) {
    using protekt::game::terrain_tile_name;
    using protekt::world::Terrain;
    EXPECT_EQ(terrain_tile_name(Terrain::Flat), "terrain_grass");
    EXPECT_EQ(terrain_tile_name(Terrain::Rough), "terrain_dirt");
    EXPECT_EQ(terrain_tile_name(Terrain::Ridge), "terrain_rock");
    EXPECT_EQ(terrain_tile_name(Terrain::Crater), "terrain_crater");
    const auto& atlas = protekt::game::atlas_manifest();
    std::set<int> tiles;
    for (int t = 0; t < protekt::world::kTerrainCount; ++t) {
        const int tile = atlas.tile(terrain_tile_name(static_cast<Terrain>(t)));
        EXPECT_NE(tile, protekt::game::kFallbackTile) << "terrain " << t << " has a listed tile";
        tiles.insert(tile);
    }
    EXPECT_EQ(tiles.size(), std::size_t{4}) << "every terrain type has its own tile";
}

TEST(SceneTerrain, EveryQuadSamplesTheWholeTileOfItsTypeTheSameWayRound) {
    // A view with all four types inside the mesh. Each quad is six vertices p00, p10, p11, p00,
    // p11, p01, and p00 sits on its tile's own lattice corner (tx * 2 m, ty * 2 m).
    const std::uint64_t seed = 42;
    constexpr int kRadius = 10;
    int centre = 0;
    for (int c = 0; c < 400 && centre == 0; c += 4) {
        std::set<protekt::world::Terrain> types;
        for (int y = c - kRadius; y <= c + kRadius; ++y) {
            for (int x = c - kRadius; x <= c + kRadius; ++x) {
                types.insert(protekt::world::sample(seed, x, y).terrain);
            }
        }
        centre = types.size() == 4U ? c : 0;
    }
    ASSERT_NE(centre, 0) << "no view on the diagonal holds all four terrain types";
    const float metres = static_cast<float>(centre) * kMetresPerTile;
    const SceneOptions view = options_at(metres, metres, seed, kRadius);
    const auto scene
        = protekt::game::build_scene(protekt::sim::Sim::create(protekt::sim::skirmish(seed)), view);
    const auto& atlas = protekt::game::atlas_manifest();
    std::set<protekt::world::Terrain> seen;
    for (std::size_t q = 0; q < terrain_vertex_count(kRadius); q += 6) {
        const auto& p00 = scene.vertices.at(q);
        const int tx = static_cast<int>(std::lround(p00.position[0] / kMetresPerTile));
        const int ty = static_cast<int>(std::lround(p00.position[2] / kMetresPerTile));
        const auto terrain = protekt::world::sample(seed, tx, ty).terrain;
        seen.insert(terrain);
        // D6: the type picks the family, the tile's hash the variant inside it.
        const auto r = atlas.rect(protekt::game::terrain_variant_name(
            terrain, protekt::game::variant_index(terrain, tx, ty)));
        // D7: the tile's orientation turns/mirrors the corners (p00, p10, p11, p01).
        const int o = protekt::game::orientation_index(terrain, tx, ty);
        const auto c0 = protekt::game::oriented_corner_uv(r, o, 0);
        const auto c1 = protekt::game::oriented_corner_uv(r, o, 1);
        const auto c2 = protekt::game::oriented_corner_uv(r, o, 2);
        const auto c3 = protekt::game::oriented_corner_uv(r, o, 3);
        const std::array<std::array<float, 2>, 6> want{{c0, c1, c2, c0, c2, c3}};
        for (std::size_t i = 0; i < 6; ++i) {
            const auto& v = scene.vertices.at(q + i);
            ASSERT_EQ(v.u, want.at(i).at(0)) << "tile " << tx << "," << ty << " vertex " << i;
            ASSERT_EQ(v.v, want.at(i).at(1)) << "tile " << tx << "," << ty << " vertex " << i;
            const ShadedTile corners{{},
                                     protekt::game::height_metres(seed, tx, ty),
                                     protekt::game::height_metres(seed, tx + 1, ty),
                                     protekt::game::height_metres(seed, tx, ty + 1),
                                     protekt::game::height_metres(seed, tx + 1, ty + 1),
                                     0};
            const float light = oracle_light(corners);
            const auto want_colour
                = protekt::game::haze::seen(view, v, protekt::math::Vec3{light, light, light});
            for (std::size_t c = 0; c < 3; ++c) {
                ASSERT_NEAR(v.colour.at(c), want_colour.at(c), 2e-4F)
                    << "the tile carries the colour; the light is the slope's, grey, then hazed";
            }
        }
    }
    EXPECT_EQ(seen.size(), std::size_t{4}) << "this view should hold every terrain type";
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
    EXPECT_EQ(different, kTankVertices) << "exactly one tank changes colour";

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
std::vector<protekt::game::SceneVertex> reticle_of(const Scene& scene) {
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
    const float mid = ground + 0.9F; // half of the 1.8 m box a tank once was
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
    const std::vector<protekt::game::SceneVertex> red = reticle_of(build_scene(sim, options));
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
    const std::vector<protekt::game::SceneVertex> green = reticle_of(build_scene(sim, options));
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

/// The reticle's vertices in its own plane: (right, up) metres from its centre, which is the middle
/// of the dot (the last quad). Ticks first, dot last, as `kReticleVertexCount` documents.
struct LocalPoint {
    float right{};
    float up{};
};

std::vector<LocalPoint> reticle_local(const Scene& scene, const SceneOptions& options) {
    const std::vector<protekt::game::SceneVertex> mark = reticle_of(scene);
    const protekt::math::Basis basis
        = protekt::math::basis_from(options.camera.yaw, options.camera.pitch);
    std::array<float, 3> centre{};
    for (std::size_t i = mark.size() - 6; i < mark.size(); ++i) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            centre.at(axis) += mark.at(i).position.at(axis) / 6.0F;
        }
    }
    std::vector<LocalPoint> out;
    for (const auto& v : mark) {
        LocalPoint p;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const float d = v.position.at(axis) - centre.at(axis);
            p.right += d * basis.right.at(axis);
            p.up += d * basis.up.at(axis);
        }
        out.push_back(p);
    }
    return out;
}

/// The reticle's half-size at rest, in metres: what the old box measured from its centre to an
/// edge.
float reticle_half() { return 1.5F * std::tan(protekt::math::kFovRadians * 0.5F) * 0.055F; }

Scene reticle_scene(float pulse, SceneOptions& options) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    options = options_at(60.0F, 60.0F);
    options.aim = Aim{640.0F, 360.0F};
    options.reticle_pulse = pulse;
    return build_scene(sim, options);
}

TEST(Reticle, IsFourTicksAndADot) {
    SceneOptions options;
    const std::vector<LocalPoint> p = reticle_local(reticle_scene(0.0F, options), options);
    ASSERT_EQ(p.size(), std::size_t{30});
    // Each of the first four quads sits on one axis, and between them they cover
    // up/down/left/right.
    int up = 0;
    int down = 0;
    int left = 0;
    int right = 0;
    for (std::size_t quad = 0; quad < 4; ++quad) {
        float cr = 0.0F;
        float cu = 0.0F;
        for (std::size_t i = quad * 6; i < (quad * 6) + 6; ++i) {
            cr += p.at(i).right / 6.0F;
            cu += p.at(i).up / 6.0F;
        }
        const float eps = reticle_half() * 0.01F;
        if (std::abs(cr) < eps && cu > 0.0F) {
            ++up;
        } else if (std::abs(cr) < eps && cu < 0.0F) {
            ++down;
        } else if (std::abs(cu) < eps && cr < 0.0F) {
            ++left;
        } else if (std::abs(cu) < eps && cr > 0.0F) {
            ++right;
        }
    }
    EXPECT_EQ(up, 1);
    EXPECT_EQ(down, 1);
    EXPECT_EQ(left, 1);
    EXPECT_EQ(right, 1);
}

TEST(Reticle, TheCentreIsOpenExceptForTheDot) {
    SceneOptions options;
    const std::vector<LocalPoint> p = reticle_local(reticle_scene(0.0F, options), options);
    const float half = reticle_half();
    for (std::size_t i = 0; i < 24; ++i) {
        EXPECT_GE(std::max(std::abs(p.at(i).right), std::abs(p.at(i).up)), half * 0.25F)
            << "tick vertex " << i << " intrudes on the open centre";
    }
    for (std::size_t i = 24; i < 30; ++i) {
        EXPECT_LE(std::max(std::abs(p.at(i).right), std::abs(p.at(i).up)), half * 0.15F)
            << "the dot is small";
    }
}

TEST(Reticle, IsNoLongerABox) {
    // The retired square outline had a vertex at every corner (+-half, +-half), and a bar as wide
    // as the whole mark. Nothing of the sort is drawn now, and the size is the same.
    SceneOptions options;
    const std::vector<LocalPoint> p = reticle_local(reticle_scene(0.0F, options), options);
    const float half = reticle_half();
    float widest = 0.0F;
    for (const LocalPoint& v : p) {
        EXPECT_FALSE(std::abs(v.right) > half * 0.5F && std::abs(v.up) > half * 0.5F)
            << "a vertex out at a corner is the old box";
        widest = std::max(widest, std::max(std::abs(v.right), std::abs(v.up)));
    }
    EXPECT_NEAR(widest, half, half * 0.01F) << "the size is comparable to the box it replaced";
}

TEST(Reticle, APulseSpreadsTheTicksAndRestIsUntouched) {
    SceneOptions rest_options;
    SceneOptions pulse_options;
    SceneOptions mid_options;
    const Scene rest = reticle_scene(0.0F, rest_options);
    const Scene mid = reticle_scene(0.5F, mid_options);
    const Scene full = reticle_scene(1.0F, pulse_options);
    ASSERT_EQ(rest.vertices.size(), full.vertices.size());
    const auto reach = [](const std::vector<LocalPoint>& pts) {
        float far = 0.0F;
        for (std::size_t i = 0; i < 24; ++i) {
            far = std::max(far, std::max(std::abs(pts.at(i).right), std::abs(pts.at(i).up)));
        }
        return far;
    };
    const float r0 = reach(reticle_local(rest, rest_options));
    const float r1 = reach(reticle_local(mid, mid_options));
    const float r2 = reach(reticle_local(full, pulse_options));
    EXPECT_LT(r0, r1);
    EXPECT_LT(r1, r2);
    // A pulse of exactly zero is the same bytes as an options struct that never set it.
    SceneOptions plain;
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    plain = options_at(60.0F, 60.0F);
    plain.aim = Aim{640.0F, 360.0F};
    const Scene never = build_scene(sim, plain);
    ASSERT_EQ(never.vertices.size(), rest.vertices.size());
    for (std::size_t i = 0; i < never.vertices.size(); ++i) {
        EXPECT_EQ(never.vertices.at(i).position, rest.vertices.at(i).position);
    }
}

// ---------------------------------------------------------------- the weapon's reticle (M1)

using protekt::game::kMortarRungCount;
using protekt::game::kMortarRungPitches;
using protekt::game::mortar_landing_tiles;
using protekt::game::mortar_rung;
using protekt::game::reticle_for;
using protekt::game::ReticleKind;
using protekt::game::Weapon;

TEST(WeaponReticle, TheChoiceIsAPureFunctionOfWeaponAndPitch) {
    for (const float pitch : {-1.0F, -0.12F, 0.0F, 0.45F, 1.0F}) {
        EXPECT_EQ(reticle_for(Weapon::Gun, pitch).kind, ReticleKind::FourTicks);
        EXPECT_EQ(reticle_for(Weapon::Rocket, pitch).kind, ReticleKind::Heavy);
        EXPECT_EQ(reticle_for(Weapon::Mortar, pitch).kind, ReticleKind::Ladder);
        EXPECT_EQ(reticle_for(Weapon::Mortar, pitch).rung, reticle_for(Weapon::Mortar, pitch).rung);
    }
    // The lit rung follows the pitch: nearest band, clamped at both ends.
    EXPECT_EQ(reticle_for(Weapon::Mortar, -1.0F).rung, 0U);
    EXPECT_EQ(reticle_for(Weapon::Mortar, -0.12F).rung, 0U) << "the starting view";
    EXPECT_EQ(reticle_for(Weapon::Mortar, 0.0F).rung, 0U);
    EXPECT_EQ(reticle_for(Weapon::Mortar, 0.25F).rung, 1U);
    EXPECT_EQ(reticle_for(Weapon::Mortar, 0.45F).rung, 2U);
    EXPECT_EQ(reticle_for(Weapon::Mortar, 0.62F).rung, 3U);
    EXPECT_EQ(reticle_for(Weapon::Mortar, 0.79F).rung, 4U);
    EXPECT_EQ(reticle_for(Weapon::Mortar, 1.0F).rung, 5U);
    EXPECT_EQ(reticle_for(Weapon::Mortar, 1.3F).rung, kMortarRungCount - 1)
        << "the limit lights the last";
    for (std::size_t rung = 0; rung < kMortarRungCount; ++rung) {
        EXPECT_EQ(mortar_rung(kMortarRungPitches.at(rung)), rung);
    }
}

/// A row of `length` + 1 tiles at seed 42 within six height units of each other, none a crater:
/// level ground, as tests/test_mortar.cpp finds it.
struct LevelRow {
    int x{};
    int y{};
    bool found{};
};

LevelRow find_level_row(int length) {
    constexpr int kSlack = 6;
    constexpr int kTiles = protekt::sim::kMapTiles;
    for (int y = 5; y < kTiles - 5; ++y) {
        for (int x = 5; x + length < kTiles - 5; ++x) {
            const int h0 = protekt::world::sample(42, x, y).height;
            bool level = true;
            for (int i = 0; i <= length && level; ++i) {
                const protekt::world::Tile t = protekt::world::sample(42, x + i, y);
                const protekt::world::Tile below = protekt::world::sample(42, x + i, y + 1);
                level = t.terrain != protekt::world::Terrain::Crater
                        && std::abs(t.height - h0) <= kSlack
                        && std::abs(below.height - h0) <= kSlack;
            }
            if (level) {
                return LevelRow{x, y, true};
            }
        }
    }
    return LevelRow{};
}

TEST(MortarLadder, RungDistancesAreTheSimsOwnFlightOnLevelGround) {
    // ONE source of truth: the shell is built from the mortar's spec row and flown by the sim's
    // own `advance_projectiles`; the ladder's distance must fall inside the last step it took.
    namespace sim = protekt::sim;
    const sim::UnitSpec& mortar = sim::spec_of(sim::UnitKind::MortarBike);
    const LevelRow row = find_level_row(16);
    ASSERT_TRUE(row.found);
    const sim::Fixed start_x = sim::Fixed::from_int(row.x);
    const sim::Fixed start_y = sim::Fixed::from_raw((row.y * sim::Fixed::kOne) + 32768);
    for (std::size_t rung = 0; rung < kMortarRungCount; ++rung) {
        const float pitch = kMortarRungPitches.at(rung);
        sim::Projectile shell{};
        shell.x = start_x;
        shell.y = start_y;
        shell.z = sim::Fixed::from_raw(sim::ground_height(42, start_x, start_y).raw
                                       + sim::kMuzzleHeight.raw);
        shell.vx = mortar.bolt_speed;
        shell.vz = sim::Fixed::from_raw(static_cast<std::int32_t>(
            std::tan(pitch) * static_cast<float>(mortar.bolt_speed.raw * 64)));
        shell.gravity = mortar.gravity;
        shell.max_travel = mortar.max_travel;
        std::vector<sim::Projectile> bolts{shell};
        std::vector<sim::Unit> nobody;
        std::vector<sim::Impact> impacts;
        sim::Fixed last_x = shell.x;
        for (int tick = 0; tick < 200 && !bolts.empty(); ++tick) {
            last_x = bolts.front().x;
            sim::advance_projectiles(bolts, nobody, impacts, 42);
        }
        ASSERT_TRUE(bolts.empty());
        const float one = static_cast<float>(sim::Fixed::kOne);
        const float before = static_cast<float>(last_x.raw - start_x.raw) / one;
        const float after = before + (static_cast<float>(mortar.bolt_speed.raw) / one);
        const float ladder = mortar_landing_tiles(pitch);
        // Six height units of unevenness in the row move the touchdown a little either way.
        EXPECT_GE(ladder, before - 0.25F) << "rung " << rung;
        EXPECT_LE(ladder, after + 0.25F) << "rung " << rung;
    }
}

TEST(MortarLadder, TheRungsArePinnedAndClimbWithThePitch) {
    // From the spec: 17476 raw tiles a tick across, a 36.8-unit muzzle, 2.25 units a tick^2.
    // M2 re-record: M1's six rungs are unchanged; 1.15 and 1.28 rad are new, the last the full 14.
    const std::array<float, kMortarRungCount> tiles{1.66F, 2.16F, 2.80F, 3.67F,
                                                    4.90F, 6.90F, 9.55F, 13.95F};
    for (std::size_t rung = 0; rung < kMortarRungCount; ++rung) {
        EXPECT_NEAR(mortar_landing_tiles(kMortarRungPitches.at(rung)), tiles.at(rung), 0.01F)
            << "rung " << rung;
    }
    EXPECT_FLOAT_EQ(mortar_landing_tiles(0.6F), mortar_landing_tiles(0.6F));
}

/// The mortar's and the rocket's scenes, at the reticle test's own place.
Scene weapon_scene(Weapon weapon, SceneOptions& options, float pitch = 0.0F) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    options = options_at(60.0F, 60.0F);
    options.aim = Aim{640.0F, 360.0F};
    options.camera.pitch = pitch;
    options.weapon = weapon;
    return build_scene(sim, options);
}

TEST(WeaponReticle, TheGunIsE12sReticleByteForByte) {
    SceneOptions plain;
    const Scene before = reticle_scene(0.0F, plain);
    SceneOptions gun;
    const Scene after = weapon_scene(Weapon::Gun, gun);
    ASSERT_EQ(after.vertices.size(), before.vertices.size());
    EXPECT_EQ(std::memcmp(after.vertices.data(), before.vertices.data(),
                          after.vertices.size() * sizeof(protekt::game::SceneVertex)),
              0);
}

TEST(WeaponReticle, TheRocketHasABiggerDotAndShorterTicks) {
    SceneOptions gun_options;
    SceneOptions rocket_options;
    const std::vector<LocalPoint> gun
        = reticle_local(weapon_scene(Weapon::Gun, gun_options), gun_options);
    const std::vector<LocalPoint> rocket
        = reticle_local(weapon_scene(Weapon::Rocket, rocket_options), rocket_options);
    ASSERT_EQ(rocket.size(), kReticleVertexCount);
    const auto reach = [](const std::vector<LocalPoint>& pts, std::size_t from, std::size_t to) {
        float far = 0.0F;
        for (std::size_t i = from; i < to; ++i) {
            far = std::max(far, std::max(std::abs(pts.at(i).right), std::abs(pts.at(i).up)));
        }
        return far;
    };
    EXPECT_GT(reach(rocket, 24, 30), reach(gun, 24, 30) * 2.0F) << "the dot reads heavier";
    EXPECT_LT(reach(rocket, 0, 24), reach(gun, 0, 24) * 0.8F) << "the ticks are shorter";
}

TEST(WeaponReticle, TheMortarAddsALadderAndItsLitRungMovesWithThePitch) {
    SceneOptions gun_options;
    SceneOptions mortar_options;
    const Scene gun = weapon_scene(Weapon::Gun, gun_options);
    const Scene mortar = weapon_scene(Weapon::Mortar, mortar_options);
    EXPECT_GT(mortar.vertices.size(), gun.vertices.size()) << "rungs and their labels";
    SceneOptions again_options;
    const Scene again = weapon_scene(Weapon::Mortar, again_options);
    ASSERT_EQ(again.vertices.size(), mortar.vertices.size());
    EXPECT_EQ(std::memcmp(again.vertices.data(), mortar.vertices.data(),
                          again.vertices.size() * sizeof(protekt::game::SceneVertex)),
              0)
        << "the same weapon and pitch draw the same bytes";
}

TEST(WeaponReticle, TheLabelIsDrawnOnlyWhenAsked) {
    SceneOptions options;
    const Scene quiet = weapon_scene(Weapon::Rocket, options);
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    options.show_weapon = true;
    const Scene labelled = build_scene(sim, options);
    EXPECT_GT(labelled.vertices.size(), quiet.vertices.size());
    // The label goes in BEFORE the reticle, so the reticle is still the tail.
    const std::vector<protekt::game::SceneVertex> a = reticle_of(quiet);
    const std::vector<protekt::game::SceneVertex> b = reticle_of(labelled);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a.at(i).position, b.at(i).position);
    }
}

// ---------------------------------------------------------------- the aim cue on the unit

/// The colour of the first vertex of `unit`'s body in `scene`: it is the only thing at the unit's
/// own position that is not terrain. A factory is found by a top corner of its box; a tank by the
/// rear corner of its turret's ROOF PANEL (the full side colour: hull, turret and bustle are only a
/// pale tint of it under their camo since D2h, and the panel is where the team and the mark read),
/// where it faces +Y by default.
std::array<float, 3> box_colour(const Scene& scene, const protekt::sim::Unit& unit) {
    constexpr float kFixedOne = static_cast<float>(protekt::sim::Fixed::kOne);
    const float ux = static_cast<float>(unit.x.raw) / kFixedOne * kMetresPerTile;
    const float uz = static_cast<float>(unit.y.raw) / kFixedOne * kMetresPerTile;
    const bool tank = unit.kind == protekt::sim::UnitKind::Tank;
    const float half = kHullHalfMetres;
    const float want_x = tank ? ux - 0.22F : ux - half; // the panel's half-width across
    // The turret's centre (0.15 m aft since D5b moved it up the hull), less the panel's back.
    const float want_z = tank ? uz - 0.15F - 0.4F : uz - half;
    for (const auto& v : scene.vertices) {
        if (std::abs(v.position[0] - want_x) < 1e-3F && std::abs(v.position[2] - want_z) < 1e-3F
            && v.position[1]
                   > protekt::game::height_metres(42, unit.x.floor_int(), unit.y.floor_int())
                         + 0.5F) {
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
    // The probed corner is shaded (the sun is not square to it), so the highlight is read against
    // the same corner of the unselected unit, not against an absolute white.
    const auto unselected = box_colour(scenes_for(sim, options, *own, {}).elsewhere, *own);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_GT(selected.at(i), unselected.at(i)) << "still the white-mixed blue of a selection";
    }
    EXPECT_GT(selected[0] / selected[2], unselected[0] / unselected[2]);
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
std::vector<protekt::game::SceneVertex> radar_vertices(const Scene& with_viewer,
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
    const std::vector<protekt::game::SceneVertex> radar = radar_vertices(
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
        EXPECT_LE(p.x, panel.x + panel.width + slack);
        // Nothing sits above the panel any more: the hull bar is retired, so the top edge of the
        // ring is the highest thing the radar draws.
        EXPECT_GE(p.y, panel.y - slack);
        EXPECT_LE(p.y, panel.y + panel.height + slack);
        // ...and the oval, not just its rectangle: nothing is drawn in the corners the oval leaves.
        if (p.y > panel.y - slack) {
            const float ex
                = (p.x - (panel.x + (panel.width * 0.5F))) / ((panel.width * 0.5F) + slack);
            const float ey
                = (p.y - (panel.y + (panel.height * 0.5F))) / ((panel.height * 0.5F) + slack);
            EXPECT_LE((ex * ex) + (ey * ey), 1.0F);
        }
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
    // The panel and the ground under it are counted below, and each mark is 6, except the viewer's
    // triangle and an arrow, which are 3.
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
    const std::vector<protekt::game::SceneVertex> radar = radar_vertices(
        protekt::game::build_scene(sim, watched), protekt::game::build_scene(sim, plain));
    // The panel itself: a 48-slice ring (a quad a slice) and a 48-slice disc (a triangle a slice).
    // The hull bar used to add two quads here; the ship instrument states the hull now.
    std::size_t expected = (48 * 6) + (48 * 3);
    // The compass: a plate (a quad) per letter, and a quad per lit cell of its 5x5 glyph:
    // N 13, E 13, S 13, W 12.
    expected += (4 * 6) + ((13 + 13 + 13 + 12) * 6);
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
    // The ground under the marks (RADAR3D-2): a thin quad a segment, from the viewer's own tile
    // through the same default radius and grid step the drawing uses.
    const protekt::sim::Unit* viewer = sim.find(player);
    ASSERT_NE(viewer, nullptr);
    const float kOne = static_cast<float>(protekt::sim::Fixed::kOne);
    const float viewer_x = static_cast<float>(viewer->x.raw) / kOne;
    const float viewer_z = static_cast<float>(viewer->y.raw) / kOne;
    const std::size_t mesh_segments
        = protekt::game::radar_mesh(watched.seed, viewer_x, viewer_z, watched.camera.yaw,
                                    protekt::game::radar_panel(watched.extent))
              .size();
    EXPECT_GT(mesh_segments, 0U) << "the frame is drawn with the wireframe on";
    expected += mesh_segments * 6U;
    EXPECT_EQ(radar.size(), expected);
}

TEST(RadarDraws, TheWireframeCanBeSwitchedOffAndChangesNothingElse) {
    // The flag the plan's pixel acceptance needs: off removes the mesh's vertices and NOTHING else,
    // so the two frames differ only where the ground is drawn.
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    protekt::sim::UnitId player{};
    for (const auto& unit : sim.units()) {
        if (unit.side == protekt::sim::Side::Player && unit.kind == protekt::sim::UnitKind::Tank) {
            player = unit.id;
            break;
        }
    }
    SceneOptions watched = options_at(60.0F, 60.0F);
    watched.viewer_unit = player;
    SceneOptions no_mesh = watched;
    no_mesh.show_radar_mesh = false;
    const Scene with = protekt::game::build_scene(sim, watched);
    const Scene without = protekt::game::build_scene(sim, no_mesh);
    ASSERT_GT(with.vertices.size(), without.vertices.size());
    const std::size_t mesh_vertices = with.vertices.size() - without.vertices.size();
    EXPECT_EQ(mesh_vertices % 6U, 0U) << "the mesh is quads, six vertices each";
    // And a frame with no radar at all is untouched by the flag: there is no ground to draw.
    SceneOptions blind = options_at(60.0F, 60.0F);
    blind.show_radar_mesh = false;
    SceneOptions unset = blind;
    unset.show_radar_mesh = true;
    const Scene blind_plain = protekt::game::build_scene(sim, blind);
    const Scene blind_mesh = protekt::game::build_scene(sim, unset);
    ASSERT_EQ(blind_plain.vertices.size(), blind_mesh.vertices.size());
    EXPECT_EQ(std::memcmp(blind_plain.vertices.data(), blind_mesh.vertices.data(),
                          blind_plain.vertices.size() * sizeof(protekt::game::SceneVertex)),
              0)
        << "with no viewer unit there is no mesh either way";
}

TEST(RadarDraws, TheMeshCoarsensWithItsStepDialAndNothingElseMoves) {
    // RADAR3D-2's cost dial, as a unit test: a coarser grid draws fewer quads, only the wireframe
    // changes (everything up to it is byte-identical), and the sim cannot see the dial at all --
    // the mesh is a READING of the world, never an input to it.
    auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    const std::uint64_t hash_before = sim.state_hash();
    protekt::sim::UnitId player{};
    for (const auto& unit : sim.units()) {
        if (unit.side == protekt::sim::Side::Player && unit.kind == protekt::sim::UnitKind::Tank) {
            player = unit.id;
            break;
        }
    }
    SceneOptions fine = options_at(60.0F, 60.0F);
    fine.viewer_unit = player;
    SceneOptions coarse = fine;
    coarse.radar_mesh_step_tiles = fine.radar_mesh_step_tiles * 2;
    SceneOptions none = fine;
    none.show_radar_mesh = false;
    const Scene at_default = protekt::game::build_scene(sim, fine);
    const Scene at_coarse = protekt::game::build_scene(sim, coarse);
    const Scene at_none = protekt::game::build_scene(sim, none);
    const std::size_t default_mesh = at_default.vertices.size() - at_none.vertices.size();
    const std::size_t coarse_mesh = at_coarse.vertices.size() - at_none.vertices.size();
    EXPECT_GT(default_mesh, coarse_mesh) << "a coarser grid draws fewer quads";
    EXPECT_GT(coarse_mesh, 0U) << "and still draws a mesh";
    EXPECT_EQ(coarse_mesh % 6U, 0U) << "the mesh is quads, six vertices each";
    // The dial is inert when the mesh is off: it prices the wireframe, it does not touch the frame
    // the wireframe is drawn into. (That the wireframe adds ONLY wireframe vertices is
    // TheWireframeCanBeSwitchedOffAndChangesNothingElse's claim, above.)
    SceneOptions none_coarse = none;
    none_coarse.radar_mesh_step_tiles = 8;
    const Scene at_none_coarse = protekt::game::build_scene(sim, none_coarse);
    ASSERT_EQ(at_none.vertices.size(), at_none_coarse.vertices.size());
    EXPECT_EQ(std::memcmp(at_none.vertices.data(), at_none_coarse.vertices.data(),
                          at_none.vertices.size() * sizeof(protekt::game::SceneVertex)),
              0)
        << "with no mesh the step did something";
    EXPECT_EQ(sim.state_hash(), hash_before) << "and the sim never saw the dial";
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
        // The radar is found as "everything past the common prefix", so nothing but the radar
        // may follow it here: the ship instrument is off, as in the other radar rigs.
        plain.show_ship_gauge = false;
        plain.show_ledger = false;
        SceneOptions watched = plain;
        watched.viewer_unit = player;
        return radar_vertices(protekt::game::build_scene(sim, watched),
                              protekt::game::build_scene(sim, plain));
    };
    const auto is_red = [](const protekt::game::SceneVertex& v) {
        return v.colour.at(0) > (v.colour.at(1) + 0.3F) && v.colour.at(0) > (v.colour.at(2) + 0.3F);
    };

    // Five tiles away: inside the radar's radius, so the enemy is on the panel, and red.
    const std::vector<protekt::game::SceneVertex> near_enemy = scene_of(55);
    ASSERT_FALSE(near_enemy.empty());
    std::size_t reds = 0;
    for (const auto& v : near_enemy) {
        if (is_red(v)) {
            ++reds;
        }
    }
    EXPECT_EQ(reds, 6U) << "one red square per hostile in range";

    // Three hundred tiles away: the same frame, but the enemy is not on the panel and nothing
    // about him is drawn -- not a dot, not a hint at the rim.
    const std::vector<protekt::game::SceneVertex> far_enemy = scene_of(350);
    ASSERT_FALSE(far_enemy.empty());
    for (const auto& v : far_enemy) {
        EXPECT_FALSE(is_red(v)) << "a hostile outside the radius must not be visible on the radar";
    }
}

// ---------------------------------------------------------------- the radar, its marks and the
// round radar
//
// Colours are asserted as literals on purpose: a test that compared against the constant the
// code draws with would pass whatever the constant was.

constexpr std::array<float, 3> kBorderRgb{0.80F, 0.82F, 0.85F};
constexpr std::array<float, 3> kFaceRgb{0.04F, 0.05F, 0.06F};
constexpr std::array<float, 3> kGreenRgb{0.30F, 0.85F, 0.35F};
constexpr std::array<float, 3> kAmberRgb{1.0F, 0.75F, 0.20F};
constexpr std::array<float, 3> kRedRgb{1.0F, 0.30F, 0.25F};
constexpr float kPixelEps = 0.1F;

/// The HUD of one frame, sorted by what each vertex is. Sorting is by colour and by WHERE the
/// vertex lands in pixels, never by position in the list, so it holds however the drawing is
/// built.
struct Hud {
    protekt::game::RadarPanel panel{};
    std::vector<std::array<float, 2>> face;
    std::vector<std::array<float, 2>> border;
    std::vector<std::array<float, 2>> marks;
};

Hud hud_of(const protekt::sim::Sim& sim, const SceneOptions& watched) {
    SceneOptions plain = watched;
    plain.viewer_unit = protekt::sim::UnitId{};
    Hud hud;
    hud.panel = protekt::game::radar_panel(watched.extent);
    for (const auto& v : radar_vertices(protekt::game::build_scene(sim, watched),
                                        protekt::game::build_scene(sim, plain))) {
        const protekt::game::Projected p = protekt::game::project(
            watched.camera, watched.extent, v.position[0], v.position[1], v.position[2]);
        const std::array<float, 2> at{p.x, p.y};
        if (v.colour == kBorderRgb) {
            hud.border.push_back(at);
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
        // Never deref an empty list: a broken HUD must FAIL a test, not crash the run before
        // the later tests get their turn. NaN compares false against everything, so every
        // EXPECT built on this value fails, which is what the reader wants to see.
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
        // Never deref an empty list: a broken HUD must FAIL a test, not crash the run before
        // the later tests get their turn. NaN compares false against everything, so every
        // EXPECT built on this value fails, which is what the reader wants to see.
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
        // Never deref an empty list: a broken HUD must FAIL a test, not crash the run before
        // the later tests get their turn. NaN compares false against everything, so every
        // EXPECT built on this value fails, which is what the reader wants to see.
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
        // Never deref an empty list: a broken HUD must FAIL a test, not crash the run before
        // the later tests get their turn. NaN compares false against everything, so every
        // EXPECT built on this value fails, which is what the reader wants to see.
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
    // The radar tests find the radar as "everything past the common prefix", which is only the
    // radar while nothing after it is drawn from these options: the ship instrument is off here
    // for the same reason it is off in `options_at`.
    plain.show_ship_gauge = false;
    plain.show_ledger = false;
    return plain;
}

/// A factory (400 hp, unarmed) shot at by a tank (8 hp a hit) until the factory is down to
/// exactly `hp`. The sim has no setter and must not get one, and the damage is exact, so this
/// is the honest way to get a viewer at a chosen health. `hp == 0` runs it until the factory is
/// dead.
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

// Round radar -------------------------------------------------------------------------------

/// Where a point is against the oval whose semi-axes are the panel's half-width and
/// half-height, each grown by `grow`: 1 on that ellipse, less inside it.
float ellipse_of(const Hud& hud, const std::array<float, 2>& p, float grow = 0.0F) {
    const float a = (hud.panel.width * 0.5F) + grow;
    const float b = (hud.panel.height * 0.5F) + grow;
    const float x = (p.at(0) - (hud.panel.x + (hud.panel.width * 0.5F))) / a;
    const float y = (p.at(1) - (hud.panel.y + (hud.panel.height * 0.5F))) / b;
    return (x * x) + (y * y);
}

constexpr float kEllipseEps = 1.0e-3F;

TEST(Radar, TheFaceIsAnOvalNotARectangle) {
    const Damaged d = factory_down_to(400);
    const Hud hud = hud_of(d.sim, watching(d));
    ASSERT_FALSE(hud.face.empty());
    for (const auto& p : hud.face) {
        EXPECT_LE(ellipse_of(hud, p), 1.0F + kEllipseEps);
    }
    EXPECT_NEAR(min_x(hud.face), hud.panel.x, kPixelEps);
    EXPECT_NEAR(max_x(hud.face), hud.panel.x + hud.panel.width, kPixelEps);
    EXPECT_NEAR(min_y(hud.face), hud.panel.y, kPixelEps);
    EXPECT_NEAR(max_y(hud.face), hud.panel.y + hud.panel.height, kPixelEps);
}

TEST(Radar, TheBorderIsARing) {
    const Damaged d = factory_down_to(400);
    const Hud hud = hud_of(d.sim, watching(d));
    ASSERT_FALSE(hud.border.empty());
    for (const auto& p : hud.border) {
        EXPECT_GE(ellipse_of(hud, p), 1.0F - kEllipseEps);
        EXPECT_LE(ellipse_of(hud, p, protekt::game::kRadarBorderPixels), 1.0F + kEllipseEps);
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

TEST(Radar, EveryMarkLiesWithinTheOval) {
    // A mark at the maximum range is on the rim of the oval in the projection and is pulled in
    // by its own size, so no centre is outside the face. (An out-of-range base's ARROW is drawn
    // at the rim by design and is held to the outer ellipse below instead.)
    const auto sim = ring_of_units();
    protekt::sim::UnitId viewer{};
    for (const auto& unit : sim.units()) {
        if (unit.kind == protekt::sim::UnitKind::Tank && unit.side == protekt::sim::Side::Player) {
            viewer = unit.id;
        }
    }
    ASSERT_TRUE(viewer.valid());
    const protekt::game::RadarPanel panel = protekt::game::radar_panel({1280, 720});
    const float middle = panel.width * 0.5F;
    const float middle_y = panel.height * 0.5F;
    float furthest = 0.0F;
    for (const float yaw : {0.0F, 0.7F, 2.0F, -1.3F, 3.1F}) {
        std::size_t in_range = 0;
        for (const auto& mark : protekt::game::radar_marks(sim.units(), viewer, yaw, panel)) {
            if (mark.shape == protekt::game::RadarShape::Arrow) {
                continue;
            }
            ++in_range;
            const float ex = (mark.px - middle) / middle;
            const float ey = (mark.py - middle_y) / middle_y;
            const float distance = (ex * ex) + (ey * ey);
            EXPECT_LE(distance, 1.0F + kEllipseEps) << "yaw " << yaw;
            furthest = std::max(furthest, distance);
        }
        EXPECT_EQ(in_range, 17U) << "the viewer and sixteen hostiles, all in range";
    }
    EXPECT_GT(furthest, 0.6F) << "the units at the maximum range are out near the rim";
}

TEST(Radar, TwoBuildsOfTheSameFrameAreByteIdentical) {
    const auto sim = ring_of_units();
    protekt::sim::UnitId viewer{};
    for (const auto& unit : sim.units()) {
        if (unit.kind == protekt::sim::UnitKind::Tank && unit.side == protekt::sim::Side::Player) {
            viewer = unit.id;
        }
    }
    SceneOptions watched = battle_options();
    watched.viewer_unit = viewer;
    watched.camera.yaw = 0.7F;
    const auto first = protekt::game::build_scene(sim, watched).vertices;
    const auto second = protekt::game::build_scene(sim, watched).vertices;
    ASSERT_EQ(first.size(), second.size());
    EXPECT_EQ(std::memcmp(first.data(), second.data(), first.size() * sizeof(first.front())), 0);
}

TEST(Radar, NothingIsDrawnOutsideTheOuterEllipse) {
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
        for (const auto* group : {&hud.face, &hud.border, &hud.marks}) {
            for (const auto& p : *group) {
                EXPECT_LE(ellipse_of(hud, p, protekt::game::kRadarBorderPixels), 1.0F + kEllipseEps)
                    << "yaw " << yaw;
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
        const std::vector<protekt::game::SceneVertex> v
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

SceneOptions at_tank(const protekt::sim::Unit& tank) {
    return options_at(static_cast<float>(tank.x.raw) / static_cast<float>(protekt::sim::Fixed::kOne)
                          * kMetresPerTile,
                      static_cast<float>(tank.y.raw) / static_cast<float>(protekt::sim::Fixed::kOne)
                          * kMetresPerTile);
}

/// Exactly one tank's vertices: those that move when the tank is hovered far above the ground.
std::vector<protekt::game::SceneVertex> tank_only(const protekt::sim::Sim& sim,
                                                  const protekt::sim::Unit& tank,
                                                  std::span<const protekt::sim::UnitId> selected) {
    SceneOptions options = at_tank(tank);
    const auto base = protekt::game::build_scene(sim, options, selected);
    options.hover = SceneOptions::Hovering{tank.id, 1000.0F};
    const auto raised = protekt::game::build_scene(sim, options, selected);
    std::vector<protekt::game::SceneVertex> out;
    for (std::size_t i = 0; i < base.vertices.size() && i < raised.vertices.size(); ++i) {
        if (base.vertices.at(i).position != raised.vertices.at(i).position) {
            out.push_back(base.vertices.at(i));
        }
    }
    return out;
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
    EXPECT_EQ(moved, kTankVertices) << "exactly one tank moves";
}

// Re-pinned DELIBERATELY (D5b): the hull gained an engine deck, a grille band and two fenders.
TEST(TankShape, TheTankIsExactlyItsTwelvePrisms) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    const auto tank = tank_only(sim, sim.units().at(0), {});
    EXPECT_EQ(tank.size(), std::size_t{432}) << "twelve prisms of 36; eight (288) before D5b";
    EXPECT_EQ(kTankVertices, std::size_t{432});
}

namespace {

/// The [lo, hi] of `axis` (0 across at heading 0, 1 up, 2 along) over prism `prism` of `tank`.
std::array<float, 2> prism_span(const std::vector<protekt::game::SceneVertex>& tank,
                                std::size_t prism, std::size_t axis) {
    float lo = 1e9F;
    float hi = -1e9F;
    for (std::size_t i = prism * 36U; i < (prism + 1U) * 36U; ++i) {
        lo = std::min(lo, tank.at(i).position.at(axis));
        hi = std::max(hi, tank.at(i).position.at(axis));
    }
    return {lo, hi};
}

/// The skirmish's first tank at tick 0, never seen moving, so it faces +Y: across is world X,
/// along is world Z.
std::vector<protekt::game::SceneVertex> lone_tank() {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    return tank_only(sim, sim.units().at(0), {});
}

constexpr std::size_t kHullPart = 2;
constexpr std::size_t kEngineDeckPart = 3;
constexpr std::size_t kGrillePart = 4;
constexpr std::size_t kFenderParts = 5; ///< and 6: left, then right

} // namespace

TEST(TankHull, TheGlacisLeansBackFromItsFootToTheCompartmentRoof) {
    const auto tank = lone_tank();
    const auto height = prism_span(tank, kHullPart, 1);
    float foot = -1e9F;
    float crest = -1e9F;
    for (std::size_t i = kHullPart * 36U; i < (kHullPart + 1U) * 36U; ++i) {
        const auto& p = tank.at(i).position;
        if (std::abs(p[1] - height[0]) < kTolerance) {
            foot = std::max(foot, p[2]);
        }
        if (std::abs(p[1] - height[1]) < kTolerance) {
            crest = std::max(crest, p[2]);
        }
    }
    // Run 0.65 m over a rise of 0.85 m: a plate sloped well off the vertical.
    EXPECT_GT(foot - crest, 0.5F) << "the front face is a sloped plate, not a wall";
}

TEST(TankHull, TheDeckStepsDownFromTheCompartmentToTheEngineDeckWithAGrilleOnIt) {
    const auto tank = lone_tank();
    const auto compartment = prism_span(tank, kHullPart, 1);
    const auto deck = prism_span(tank, kEngineDeckPart, 1);
    EXPECT_GT(compartment[1] - deck[1], 0.15F) << "two distinct top heights";
    EXPECT_LT(prism_span(tank, kEngineDeckPart, 2)[0], prism_span(tank, kHullPart, 2)[0])
        << "the engine deck is aft of the compartment";
    const auto grille = prism_span(tank, kGrillePart, 1);
    EXPECT_NEAR(grille[0], deck[1], kTolerance) << "the grille lies on the engine deck";
    EXPECT_LT(grille[1], compartment[1]);
}

TEST(TankHull, EachFenderIsAShelfWiderThanTheTrackUnderIt) {
    const auto tank = lone_tank();
    for (std::size_t side = 0; side < 2; ++side) {
        const auto track = prism_span(tank, side, 0);
        const auto fender = prism_span(tank, kFenderParts + side, 0);
        EXPECT_GT(fender[1] - fender[0], track[1] - track[0]) << "side " << side;
        EXPECT_LT(fender[0], track[0]) << "side " << side << ": it overhangs the track inboard";
        EXPECT_GT(fender[1], track[1]) << "side " << side << ": and outboard";
        EXPECT_LE(prism_span(tank, kFenderParts + side, 1)[0], prism_span(tank, side, 1)[1])
            << "side " << side << ": it sits on the track";
        EXPECT_LT(prism_span(tank, kFenderParts + side, 1)[1] - prism_span(tank, side, 1)[1], 0.1F)
            << "side " << side << ": a thin plate";
    }
}

TEST(HullVariant, TheSameSeedAndIdAlwaysPickTheSameVariant) {
    for (std::uint32_t id = 1; id < 64; ++id) {
        const int first = protekt::game::hull_variant(42, id);
        for (int run = 0; run < 3; ++run) {
            EXPECT_EQ(protekt::game::hull_variant(42, id), first) << "id " << id;
        }
    }
}

TEST(HullVariant, DifferentIdsSpreadAcrossAllThreeAndEveryOneIsValid) {
    for (const std::uint64_t seed : {std::uint64_t{1}, std::uint64_t{42}, std::uint64_t{99}}) {
        std::array<int, protekt::game::kHullVariantCount> counts{};
        for (std::uint32_t id = 1; id <= 30; ++id) {
            const int variant = protekt::game::hull_variant(seed, id);
            ASSERT_GE(variant, 0);
            ASSERT_LT(variant, protekt::game::kHullVariantCount);
            ++counts.at(static_cast<std::size_t>(variant));
        }
        for (const int count : counts) {
            EXPECT_GE(count, 5) << "seed " << seed << ": thirty tanks, every pattern worn by some";
        }
    }
    // The seed matters too: the same id is not bound to the same pattern in every world.
    bool seed_matters = false;
    for (std::uint32_t id = 1; id <= 30; ++id) {
        seed_matters = seed_matters
                       || protekt::game::hull_variant(1, id) != protekt::game::hull_variant(42, id);
    }
    EXPECT_TRUE(seed_matters);
}

/// The atlas tile a drawn tank's hull samples: the hull is the third prism, after two skirts.
int hull_tile_of(const std::vector<protekt::game::SceneVertex>& tank) {
    const protekt::game::SceneVertex& v = tank.at(std::size_t{2} * 36U);
    for (int index = 0; index < protekt::game::kTileCount; ++index) {
        const protekt::game::UvRect r = protekt::game::tile_rect(index);
        if ((v.u == r.u0 || v.u == r.u1) && (v.v == r.v0 || v.v == r.v1)) {
            return index;
        }
    }
    return -1;
}

TEST(HullVariant, EveryTankWearsItsOwnValidVariantTileAndTheSquadIsNotUniform) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    const protekt::game::AtlasManifest& atlas = protekt::game::atlas_manifest();
    std::set<int> worn;
    for (const auto& unit : sim.units()) {
        if (unit.kind != protekt::sim::UnitKind::Tank) {
            continue;
        }
        const auto tank = tank_only(sim, unit, {});
        ASSERT_EQ(tank.size(), kTankVertices);
        const int tile = hull_tile_of(tank);
        const auto name
            = protekt::game::kTeamHullNames.at(static_cast<std::size_t>(unit.side))
                  .at(static_cast<std::size_t>(protekt::game::hull_variant(42, unit.id.value)));
        EXPECT_EQ(tile, atlas.tile(name)) << "unit " << unit.id.value;
        // The player's set is tiles 10-12, the enemy's 13-15: each tank wears its own team's camo.
        const int first = unit.side == protekt::sim::Side::Player ? 10 : 13;
        EXPECT_GE(tile, first);
        EXPECT_LE(tile, first + 2);
        // Twice, the same: a tank wears the same pattern every time it is drawn.
        EXPECT_EQ(hull_tile_of(tank_only(sim, unit, {})), tile);
        worn.insert(tile);
    }
    EXPECT_GE(worn.size(), std::size_t{2}) << "the tanks of a skirmish do not all look alike";
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
std::vector<std::vector<protekt::game::SceneVertex>> colour_groups(const Scene& scene) {
    std::vector<std::vector<protekt::game::SceneVertex>> groups;
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

// ---------------------------------------------------------------- the hit mark

using protekt::game::HitMark;
using protekt::game::kHitMarkTicks;
using protekt::game::shooter_connected;

TEST(HitMark, IsOffUntilTheFirstConnection) {
    const HitMark mark;
    EXPECT_FLOAT_EQ(mark.level(0), 0.0F);
    EXPECT_FLOAT_EQ(mark.level(1000), 0.0F);
}

TEST(HitMark, PulsesOnTheTickOfTheHitAndDecaysToRestWithinSixTicks) {
    EXPECT_EQ(kHitMarkTicks, 6U) << "N, the ticks a pulse lasts";
    HitMark mark;
    mark.hit(100);
    EXPECT_FLOAT_EQ(mark.level(100), 1.0F);
    float previous = 1.0F;
    for (std::uint32_t t = 101; t < 100 + kHitMarkTicks; ++t) {
        EXPECT_LT(mark.level(t), previous) << "tick " << t;
        previous = mark.level(t);
    }
    EXPECT_FLOAT_EQ(mark.level(100 + kHitMarkTicks), 0.0F);
    EXPECT_FLOAT_EQ(mark.level(100 + kHitMarkTicks + 500), 0.0F);
}

TEST(HitMark, OnlyABoltFromTheShooterOntoTheOtherSideCounts) {
    using protekt::sim::BattleEvent;
    using protekt::sim::EventKind;
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    protekt::sim::UnitId mine{};
    protekt::sim::UnitId ally{};
    protekt::sim::UnitId foe{};
    for (const protekt::sim::Unit& unit : sim.units()) {
        if (unit.side == protekt::sim::Side::Enemy && !foe.valid()) {
            foe = unit.id;
        } else if (unit.side == protekt::sim::Side::Player) {
            if (!mine.valid()) {
                mine = unit.id;
            } else if (!ally.valid()) {
                ally = unit.id;
            }
        }
    }
    ASSERT_TRUE(mine.valid() && ally.valid() && foe.valid());
    const auto hit = [](protekt::sim::UnitId by, protekt::sim::UnitId victim) {
        return BattleEvent{5, EventKind::HitTaken, by, victim, 90};
    };
    const auto fired = [](protekt::sim::UnitId by, protekt::sim::UnitId at) {
        return BattleEvent{5, EventKind::Fired, by, at, 100};
    };
    const std::span<const protekt::sim::Unit> units = sim.units();
    EXPECT_TRUE(shooter_connected(std::vector{hit(mine, foe)}, mine, units));
    EXPECT_FALSE(shooter_connected(std::vector{hit(foe, mine)}, mine, units))
        << "getting hit is hit_flash_'s job, not the reticle's";
    EXPECT_FALSE(shooter_connected(std::vector{hit(ally, foe)}, mine, units)) << "someone else's";
    EXPECT_FALSE(shooter_connected(std::vector{hit(mine, ally)}, mine, units)) << "friendly fire";
    EXPECT_FALSE(shooter_connected(std::vector{fired(mine, foe)}, mine, units))
        << "a shot leaving the gun is not a connection";
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
std::vector<protekt::game::SceneVertex> bolt_vertices(const Scene& with, const Scene& without) {
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

/// How far a flat colour may drift across one bolt's box once the haze takes it: the box is under
/// half a metre across and the ramp falls by well under 0.01 of a channel per metre.
constexpr float kHazeAcrossABox = 0.01F;

TEST(SceneBolts, TheColourSaysWhoseBoltItIs) {
    auto sim = two_bolt_sim();
    const auto none = protekt::game::build_scene(sim, bolt_options(sim));
    step_until_bolt(sim);
    ASSERT_EQ(sim.projectiles().size(), 2U);
    const auto drawn = bolt_vertices(protekt::game::build_scene(sim, bolt_options(sim)), none);
    ASSERT_EQ(drawn.size(), 2U * kBoxVertices);
    // Each box is one flat colour, and the two boxes do not share it.
    for (std::size_t i = 1; i < kBoxVertices; ++i) {
        // Each corner is fogged by its OWN distance, so a 0.4 m box differs by a hair across it.
        for (std::size_t c = 0; c < 3; ++c) {
            EXPECT_NEAR(drawn.at(i).colour.at(c), drawn.at(0).colour.at(c), kHazeAcrossABox);
            EXPECT_NEAR(drawn.at(kBoxVertices + i).colour.at(c),
                        drawn.at(kBoxVertices).colour.at(c), kHazeAcrossABox);
        }
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
    // The swap is of INK; each bolt is hazed by its own distance, so the swapped colours agree to
    // the haze's difference between the two bolts' places, not bit for bit.
    for (std::size_t c = 0; c < 3; ++c) {
        EXPECT_NEAR(mine.vertices.at(first).colour.at(c),
                    theirs.vertices.at(first + kBoxVertices).colour.at(c), kHazeAcrossABox);
        EXPECT_NEAR(mine.vertices.at(first + kBoxVertices).colour.at(c),
                    theirs.vertices.at(first).colour.at(c), kHazeAcrossABox);
    }
}

TEST(SceneBolts, AHostileBoltDrawsToo) {
    auto sim = one_enemy_bolt_sim();
    const auto none = protekt::game::build_scene(sim, bolt_options(sim));
    step_until_bolt(sim);
    ASSERT_EQ(sim.projectiles().size(), 1U);
    const auto with = protekt::game::build_scene(sim, bolt_options(sim));
    EXPECT_EQ(with.vertices.size() - none.vertices.size(), kBoxVertices);
}

// A bolt is drawn at ITS height: the sim says how high it is, and the picture does not re-derive
// it from the ground the bolt happens to be over. Harri's report: shoot across a valley and the
// bolt "follows the contour of the valley, and starts going UP at the opposite side".

namespace {

struct Valley {
    int x{};
    int y{};
};

int height_at_tile(int x, int y) { return protekt::world::sample(42, x, y).height; }

/// The start of a row that dips into a valley and where a LEVEL shot from the first tile clears
/// the ground all the way along: nothing along twelve tiles rises more than 30 units above the
/// start (the muzzle is 36.8 up), and the middle of the row is at least 60 units below it.
std::optional<Valley> find_valley() {
    for (int y = 5; y < 250; ++y) {
        for (int x = 5; x < 235; ++x) {
            if (protekt::world::sample(42, x, y).terrain == protekt::world::Terrain::Crater) {
                continue;
            }
            const int h0 = height_at_tile(x, y);
            bool clear = true;
            for (int i = 0; i <= 12 && clear; ++i) {
                clear = height_at_tile(x + i, y) <= h0 + 30;
            }
            bool deep = true;
            for (int i = 6; i <= 9 && deep; ++i) {
                deep = height_at_tile(x + i, y) <= h0 - 60;
            }
            if (clear && deep) {
                return Valley{x, y};
            }
        }
    }
    return std::nullopt;
}

/// A lone tank at the valley's rim, a level shot along the row, and the scene at the moment the
/// bolt is `tiles` along. Everything a test needs to compare the bolt with its picture.
struct Flight {
    protekt::sim::Projectile bolt{};
    float drawn_centre{}; ///< metres, the middle of the bolt's box
    float ground_under{}; ///< metres, the ground at the bolt's own x, y
};

std::vector<Flight> fly_over_the_valley(const std::vector<float>& tiles_along) {
    const std::optional<Valley> valley = find_valley();
    EXPECT_TRUE(valley.has_value()) << "no valley on seed 42";
    const Valley at = valley.value_or(Valley{});
    auto sim = sim_of({{Side::Player, UnitKind::Tank, at.x, at.y}});
    for (int tick = 0; tick < 15; ++tick) {
        sim.step(); // a new gun reloads
    }
    sim.enqueue(protekt::sim::Order{
        protekt::sim::UnitId{1}, protekt::sim::orders::FireTowards{protekt::sim::Fixed::from_int(1),
                                                                   protekt::sim::Fixed::zero()}});
    const auto none = protekt::game::build_scene(sim, bolt_options(sim));
    std::vector<Flight> flights;
    std::size_t next = 0;
    for (int tick = 0; tick < 60 && next < tiles_along.size(); ++tick) {
        sim.step();
        if (sim.projectiles().empty()) {
            break;
        }
        const protekt::sim::Projectile& bolt = sim.projectiles().front();
        const float along = static_cast<float>(bolt.x.raw) / 65536.0F - static_cast<float>(at.x);
        if (along < tiles_along.at(next)) {
            continue;
        }
        ++next;
        const auto drawn = bolt_vertices(protekt::game::build_scene(sim, bolt_options(sim)), none);
        EXPECT_EQ(drawn.size(), kBoxVertices);
        float low = std::numeric_limits<float>::max();
        float high = std::numeric_limits<float>::lowest();
        for (const auto& v : drawn) {
            low = std::min(low, v.position[1]);
            high = std::max(high, v.position[1]);
        }
        flights.push_back(
            Flight{bolt, (low + high) * 0.5F, protekt::game::ground_metres(42, bolt.x, bolt.y)});
    }
    return flights;
}

} // namespace

TEST(SceneBolts, ALevelShotFliesLevelOverAValley) {
    const std::vector<Flight> flights = fly_over_the_valley({1.0F, 4.0F, 7.0F, 9.0F});
    ASSERT_EQ(flights.size(), 4U) << "the bolt did not fly across the valley";
    // In the simulation: a level shot keeps the height it left the gun at, exactly.
    for (const Flight& flight : flights) {
        EXPECT_EQ(flight.bolt.z.raw, flights.front().bolt.z.raw);
        EXPECT_EQ(flight.bolt.vz.raw, 0);
    }
    // On the screen: the ground under it fell away, and the bolt was drawn at the same height all
    // the way across -- it did not dip into the valley and climb the far wall.
    float lowest_ground = flights.front().ground_under;
    for (const Flight& flight : flights) {
        lowest_ground = std::min(lowest_ground, flight.ground_under);
        EXPECT_NEAR(flight.drawn_centre, flights.front().drawn_centre, kTolerance)
            << "the drawn bolt followed the ground";
    }
    EXPECT_LT(lowest_ground, flights.front().ground_under - 1.5F)
        << "the valley is not deep enough for this to prove anything";
}

TEST(SceneBolts, ABoltIsDrawnAtItsOwnHeight) {
    const std::vector<Flight> flights = fly_over_the_valley({2.0F, 8.0F});
    ASSERT_EQ(flights.size(), 2U);
    for (const Flight& flight : flights) {
        const float want = static_cast<float>(flight.bolt.z.raw) / 65536.0F * kMetresPerHeightUnit;
        EXPECT_NEAR(flight.drawn_centre, want, 0.05F) << "not at the bolt's own height";
    }
    // Over the deep part the old picture put it 1.2 m above the ground; the bolt's real height is
    // metres above that.
    EXPECT_GT(flights.at(1).drawn_centre, flights.at(1).ground_under + 1.2F + 1.5F);
}

TEST(SceneBolts, TheSimsGroundIsTheGroundTheSceneDraws) {
    // A bolt that meets the ground in the sim has to meet the ground the player sees.
    for (const auto& [x, y] : {std::pair{20.25F, 60.5F}, std::pair{100.9F, 100.1F},
                               std::pair{33.5F, 33.5F}, std::pair{47.75F, 90.25F}}) {
        const auto fx = protekt::sim::Fixed::from_raw(static_cast<std::int32_t>(x * 65536.0F));
        const auto fy = protekt::sim::Fixed::from_raw(static_cast<std::int32_t>(y * 65536.0F));
        const float sim_metres = static_cast<float>(protekt::sim::ground_height(42, fx, fy).raw)
                                 / 65536.0F * kMetresPerHeightUnit;
        EXPECT_NEAR(sim_metres, protekt::game::ground_metres(42, fx, fy), 0.001F) << x << "," << y;
    }
}

// The viewer's tank, drawn with a hull, a turret and a barrel so the steering model can be seen.

namespace {

using protekt::sim::UnitId;

constexpr std::size_t kPrismVertices = 36;
constexpr std::size_t kBarrelVertices = kPrismVertices;
/// Two tracks, hull, turret, bustle, roof panel, muzzle collar, barrel -- the barrel is the last
/// prism -- and
/// which is the end of the tank.
constexpr std::size_t kFacingTankVertices = kTankVertices;

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

/// The barrel is the last prism of the viewer's tank, and the viewer's tank is the end of the
/// scene.
std::vector<protekt::game::SceneVertex> barrel_of(const Scene& scene) {
    const auto first = scene.vertices.end() - static_cast<std::ptrdiff_t>(kBarrelVertices);
    return {first, first + static_cast<std::ptrdiff_t>(kBarrelVertices)};
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

bool same_vertices(const std::vector<protekt::game::SceneVertex>& a,
                   const std::vector<protekt::game::SceneVertex>& b) {
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
// yaw turns toward +X, the RIGHT of the screen. The turret's world yaw is
// `heading + turret`, the same sum `PlayerCommander::view_yaw` hands the camera, so the barrel must
// lie along the camera's forward at that yaw. If the rotation sign were flipped the barrel would
// point along (-sin, cos): the x component changes sign for every non-zero angle, so every case
// below that has a non-zero yaw fails (and the cases are chosen so none is its own reflection).
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
    EXPECT_GT(barrel_reach(facing_scene(sim, 0.0F, 0.0F), viewer), kHullHalfMetres * 1.4F);
}

TEST(SceneFacing, OnlyTheViewersTankChanges) {
    const auto sim = facing_sim();
    const SceneOptions plain = facing_options(sim);
    const Scene without = protekt::game::build_scene(sim, plain);
    const Scene with = facing_scene(sim, 0.7F, -0.4F);
    // Every tank is a hull, turret and barrel now, so a facing changes where they point, not how
    // many vertices there are.
    EXPECT_EQ(with.vertices.size(), without.vertices.size());
    // Everything before the viewer's tank -- the terrain and the factory -- is untouched.
    const auto shared = static_cast<std::ptrdiff_t>(without.vertices.size() - kFacingTankVertices);
    EXPECT_TRUE(same_vertices({without.vertices.begin(), without.vertices.begin() + shared},
                              {with.vertices.begin(), with.vertices.begin() + shared}));
    EXPECT_FALSE(same_vertices({without.vertices.begin() + shared, without.vertices.end()},
                               {with.vertices.begin() + shared, with.vertices.end()}));
    // A facing that names someone else rotates THAT unit and leaves the viewer's tank at its
    // default facing.
    SceneOptions elsewhere = plain;
    elsewhere.facing = SceneOptions::Facing{sim.units().front().id, 0.7F, -0.4F};
    const Scene other = protekt::game::build_scene(sim, elsewhere);
    EXPECT_GT(other.vertices.size(), without.vertices.size());
    EXPECT_TRUE(
        same_vertices({without.vertices.end() - static_cast<std::ptrdiff_t>(kFacingTankVertices),
                       without.vertices.end()},
                      {other.vertices.end() - static_cast<std::ptrdiff_t>(kFacingTankVertices),
                       other.vertices.end()}));
}

TEST(SceneFacing, AFrameWithoutFacingIsByteIdentical) {
    const auto sim = facing_sim();
    SceneOptions absent = facing_options(sim);
    absent.facing = std::nullopt;
    const SceneOptions never_heard = facing_options(sim);
    const Scene a = protekt::game::build_scene(sim, absent);
    const Scene b = protekt::game::build_scene(sim, never_heard);
    EXPECT_TRUE(same_vertices(a.vertices, b.vertices));
    // A tank with no facing and no velocity is the default one: it is axis-aligned and no wider
    // than it always was (the outer edge of a fender), because heading 0 is the world's +Y.
    const auto& viewer = viewer_of(sim);
    const float ux = static_cast<float>(viewer.x.raw) / 65536.0F * kMetresPerTile;
    const auto tail = a.vertices.end() - static_cast<std::ptrdiff_t>(kFacingTankVertices);
    float widest = 0.0F;
    for (auto v = tail; v != a.vertices.end(); ++v) {
        widest = std::max(widest, std::abs(v->position[0] - ux));
    }
    EXPECT_NEAR(widest, kHullHalfMetres, 1e-4F);
}

// ---------------------------------------------------------------- shading

namespace {

/// Face `face` (0 near, 1 far, 2 left, 3 right, 4 top, 5 bottom) of prism `prism` of the viewer's
/// tank (0, 1 tracks; 2 hull), as the colour of its first vertex.
std::array<float, 3> tank_face_colour(const Scene& scene, std::size_t prism, std::size_t face) {
    const auto tail = scene.vertices.end() - static_cast<std::ptrdiff_t>(kFacingTankVertices);
    return (tail + static_cast<std::ptrdiff_t>((prism * kPrismVertices) + (face * 6)))->colour;
}

constexpr std::size_t kHull = 2;
constexpr std::size_t kTurretPrism = 7;
constexpr std::size_t kBustlePrism = 8;
constexpr std::size_t kRoofPanel = 9;
constexpr std::size_t kBarrelPrism = 11;

/// `c` mixed toward white by `kHullPaleMix`: what the scene does to a hull's colour.
float pale(float c) { return c + ((1.0F - c) * protekt::game::kHullPaleMix); }
constexpr std::size_t kLeftFace = 2;
constexpr std::size_t kRightFace = 3;
constexpr std::size_t kTopFace = 4;
constexpr std::size_t kBottomFace = 5;

} // namespace

// Heading 0 leaves the hull's left face looking toward -x (the sun's side, see kSunDirection) and
// its right face toward +x (away from it).
TEST(SceneShading, TheTopIsBrighterThanTheSideAwayFromTheSunAndKeepsTheHue) {
    const auto sim = facing_sim();
    const Scene scene = facing_scene(sim, 0.0F, 0.0F);
    const auto top = tank_face_colour(scene, kHull, kTopFace);
    const auto lit = tank_face_colour(scene, kHull, kLeftFace);
    const auto shade = tank_face_colour(scene, kHull, kRightFace);
    const auto under = tank_face_colour(scene, kHull, kBottomFace);
    EXPECT_GT(top[2], lit[2]);
    EXPECT_GT(lit[2], shade[2]);
    EXPECT_GT(shade[2], under[2]);
    // Every face is the hull's pale tint of the side's blue, scaled, never greyed: the channel
    // ratios are the base's.
    const protekt::math::Vec3 base{pale(0.15F), pale(0.35F), pale(0.85F)};
    for (const auto& face : {top, lit, shade, under}) {
        EXPECT_GT(face[2], 0.0F);
        const float scale = face[2] / base[2];
        EXPECT_GE(scale, 0.45F - 1e-4F);
        EXPECT_LE(scale, 1.0F + 1e-4F);
        EXPECT_NEAR(face[0], base[0] * scale, 1e-5F);
        EXPECT_NEAR(face[1], base[1] * scale, 1e-5F);
    }
}

TEST(SceneShading, TheSunFollowsTheWorldAndNotTheTanksHeading) {
    const auto sim = facing_sim();
    // Turned half a turn, the hull's RIGHT face now looks toward -x, so it is the lit one.
    const Scene turned = facing_scene(sim, protekt::math::kPi, 0.0F);
    EXPECT_GT(tank_face_colour(turned, kHull, kRightFace)[2],
              tank_face_colour(turned, kHull, kLeftFace)[2]);
}

TEST(SceneShading, ASelectedHullStillReadsAsTheHighlight) {
    const auto sim = facing_sim();
    SceneOptions o = facing_options(sim);
    o.facing = SceneOptions::Facing{viewer_of(sim).id, 0.0F, 0.0F};
    const Scene plain = protekt::game::build_scene(sim, o);
    const std::vector<protekt::sim::UnitId> chosen{viewer_of(sim).id};
    const Scene picked = protekt::game::build_scene(sim, o, chosen);
    const auto before = tank_face_colour(plain, kHull, kTopFace);
    const auto after = tank_face_colour(picked, kHull, kTopFace);
    // Brighter in every channel, and bluer-white: the mix toward white lifts red most.
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_GT(after.at(i), before.at(i));
    }
    EXPECT_GT(after[0] / after[2], before[0] / before[2]);
    // The shaded highlight is the hull's pale tint of the highlight colour times the same light
    // the unmarked top got.
    const float light = before[2] / pale(0.85F);
    const float mix = 0.55F;
    EXPECT_NEAR(after[0], pale(0.15F + ((1.0F - 0.15F) * mix)) * light, 1e-5F);
    EXPECT_NEAR(after[2], pale(0.85F + ((1.0F - 0.85F) * mix)) * light, 1e-5F);
}

/// D2f: the hull takes only a PALE tint of the side colour, so its team camo tile dominates, and
/// the turret keeps the full saturated side colour: the turret is the team-identity block.
// From the driver's seat the tank is mostly turret, so the turret and bustle wear the same camo
// tile and pale tint as the hull, and the team reads from two saturated accents: the roof panel and
// the barrel.
TEST(SceneShading, TheTurretWearsTheCamoAndTheRoofPanelAndBarrelCarryTheSideColour) {
    const auto sim = facing_sim();
    const Scene scene = facing_scene(sim, 0.0F, 0.0F);
    const auto tail = scene.vertices.end() - static_cast<std::ptrdiff_t>(kFacingTankVertices);
    const auto first_of = [&](std::size_t prism) {
        return *(tail + static_cast<std::ptrdiff_t>(prism * kPrismVertices));
    };
    // One pattern per tank: turret and bustle sample exactly the hull's camo tile.
    for (const std::size_t part : {kTurretPrism, kBustlePrism}) {
        EXPECT_EQ(first_of(part).u, first_of(kHull).u) << "prism " << part;
        EXPECT_EQ(first_of(part).v, first_of(kHull).v) << "prism " << part;
    }
    // Their colour is the hull's pale tint: the same light-free ratio on the top face.
    for (const std::size_t part : {kTurretPrism, kBustlePrism}) {
        const auto top = tank_face_colour(scene, part, kTopFace);
        const float light = top[2] / pale(0.85F);
        EXPECT_NEAR(top[0], pale(0.15F) * light, 1e-5F) << "prism " << part;
        EXPECT_NEAR(top[1], pale(0.35F) * light, 1e-5F) << "prism " << part;
    }
    // The roof panel and the barrel are the side's own saturated blue.
    for (const std::size_t part : {kRoofPanel, kBarrelPrism}) {
        const auto top = tank_face_colour(scene, part, kTopFace);
        const float light = top[2] / 0.85F;
        EXPECT_NEAR(top[0], 0.15F * light, 1e-5F) << "prism " << part;
        EXPECT_NEAR(top[1], 0.35F * light, 1e-5F) << "prism " << part;
    }
    const auto saturation = [](const std::array<float, 3>& c) {
        const float hi = std::max({c[0], c[1], c[2]});
        const float lo = std::min({c[0], c[1], c[2]});
        return (hi - lo) / hi;
    };
    EXPECT_GT(saturation(tank_face_colour(scene, kRoofPanel, kTopFace)), 0.7F);
    EXPECT_GT(saturation(tank_face_colour(scene, kBarrelPrism, kTopFace)), 0.7F);
    EXPECT_LT(saturation(tank_face_colour(scene, kTurretPrism, kTopFace)), 0.15F);
    // The panel sits on the roof, within it, and is big enough to read: over a quarter of the
    // roof's area, with camo still showing round it.
    const auto span = [&](std::size_t prism, std::size_t axis) {
        float lo = 1e9F;
        float hi = -1e9F;
        for (std::size_t i = 0; i < kPrismVertices; ++i) {
            const auto at = static_cast<std::ptrdiff_t>((prism * kPrismVertices) + i);
            lo = std::min(lo, (tail + at)->position.at(axis));
            hi = std::max(hi, (tail + at)->position.at(axis));
        }
        return std::array<float, 2>{lo, hi};
    };
    const auto extent = [&](std::size_t prism, std::size_t axis) {
        const auto [lo, hi] = span(prism, axis);
        return hi - lo;
    };
    EXPECT_NEAR(span(kRoofPanel, 1)[0], span(kTurretPrism, 1)[1], 1e-4F)
        << "the panel stands on the roof";
    const float panel_area = extent(kRoofPanel, 0) * extent(kRoofPanel, 2);
    const float roof_area = extent(kTurretPrism, 0) * extent(kTurretPrism, 2);
    EXPECT_GT(panel_area, 0.25F * roof_area);
    EXPECT_LT(extent(kRoofPanel, 0), extent(kTurretPrism, 0));
    EXPECT_LT(extent(kRoofPanel, 2), extent(kTurretPrism, 2));
}

TEST(SceneShading, TerrainIsNotShaded) {
    // D3: the terrain's colour is its tile's; the vertex light is a flat white, never sun-shaded.
    const auto sim = facing_sim();
    const Scene scene = facing_scene(sim, 0.0F, 0.0F);
    EXPECT_EQ(
        scene.vertices.front().colour,
        protekt::game::haze::seen(facing_options(sim), scene.vertices.front(), {1.0F, 1.0F, 1.0F}))
        << "white light, however far the haze takes it";
}

// ---------------------------------------------------------------- which way a tank faces

namespace {

using protekt::game::Headings;
using protekt::sim::Fixed;

/// A lone player tank, ordered to drive along +X.
protekt::sim::Sim driving_east() {
    auto sim = sim_of({{Side::Player, UnitKind::Tank, 100, 100}});
    sim.enqueue(protekt::sim::Order{
        sim.units().front().id,
        protekt::sim::orders::MoveTo{Fixed::from_int(104), Fixed::from_int(100)}});
    return sim;
}

bool standing(const protekt::sim::Unit& unit) { return unit.vx.raw == 0 && unit.vy.raw == 0; }

/// How far the tank's forward-most vertex is along +X from its centre.
float forward_reach_x(const Scene& scene, const protekt::sim::Unit& unit) {
    const float ux = static_cast<float>(unit.x.raw) / 65536.0F * kMetresPerTile;
    float reach = -1e9F;
    for (auto v = scene.vertices.end() - static_cast<std::ptrdiff_t>(kFacingTankVertices);
         v != scene.vertices.end(); ++v) {
        reach = std::max(reach, v->position[0] - ux);
    }
    return reach;
}

/// The scene with the tank oriented by `headings`, camera on the tank.
Scene scene_with(const protekt::sim::Sim& sim, const Headings& headings) {
    SceneOptions o = options_at(0.0F, 0.0F, 42, 20);
    const auto& unit = sim.units().front();
    o.camera.x = static_cast<float>(unit.x.raw) / 65536.0F * kMetresPerTile;
    o.camera.z = static_cast<float>(unit.y.raw) / 65536.0F * kMetresPerTile;
    o.headings = &headings;
    return protekt::game::build_scene(sim, o);
}

} // namespace

TEST(SceneHeading, AStandingTankNeverSeenMovingFacesPlusY) {
    const auto sim = sim_of({{Side::Player, UnitKind::Tank, 100, 100}});
    ASSERT_TRUE(standing(sim.units().front()));
    EXPECT_FALSE(Headings::heading_of(sim.units().front()).has_value());
    Headings headings;
    headings.observe(sim);
    EXPECT_EQ(headings.of(sim.units().front()), 0.0F);
    // Barrel along +Y (+Z in metres): no reach worth the name along X.
    EXPECT_LT(forward_reach_x(scene_with(sim, headings), sim.units().front()),
              kHullHalfMetres + 1e-3F);
}

TEST(SceneHeading, ATankMovingTowardPlusXPointsItsBarrelAlongPlusX) {
    auto sim = driving_east();
    Headings headings;
    bool moved = false;
    for (int tick = 0; tick < 200 && !moved; ++tick) {
        sim.step();
        moved = !standing(sim.units().front());
    }
    ASSERT_TRUE(moved) << "the tank must start driving";
    headings.observe(sim);
    EXPECT_NEAR(headings.of(sim.units().front()), protekt::math::kPi * 0.5F, 1e-4F);
    // The barrel tip is the forward-most vertex, well beyond the hull's own half-width.
    EXPECT_GT(forward_reach_x(scene_with(sim, headings), sim.units().front()),
              kHullHalfMetres * 1.5F);
}

TEST(SceneHeading, AStoppedTankKeepsTheFacingItHadWhileMoving) {
    auto sim = driving_east();
    Headings headings;
    bool moved = false;
    bool stopped_after = false;
    for (int tick = 0; tick < 2000 && !stopped_after; ++tick) {
        sim.step();
        headings.observe(sim);
        if (!standing(sim.units().front())) {
            moved = true;
        } else if (moved) {
            stopped_after = true;
        }
    }
    ASSERT_TRUE(moved);
    ASSERT_TRUE(stopped_after) << "the tank must arrive and stop";
    EXPECT_NEAR(headings.of(sim.units().front()), protekt::math::kPi * 0.5F, 1e-4F)
        << "stopped, it keeps facing +X";
    EXPECT_GT(forward_reach_x(scene_with(sim, headings), sim.units().front()),
              kHullHalfMetres * 1.5F);
    // Without the memory the same stopped tank falls back to +Y: the memory is what holds it.
    EXPECT_EQ(Headings{}.of(sim.units().front()), 0.0F);
}

// ---------------------------------------------------------------- the bars over the tanks
//
// Every living tank in view, the driven one excepted, wears a small health bar above its projected
// top: a 20x4 px track at 800x450 inside a 1 px outline, green fill for yours and red for hostiles.
// The bars follow the radar in the vertex list; these scenes have no aim, so they are the tail.
// Colours are literals on purpose, as above.

using protekt::game::kUnitBarVertexCount;

constexpr std::array<float, 3> kOwnBarRgb{0.45F, 1.0F, 0.45F};
constexpr std::array<float, 3> kHostileBarRgb{1.0F, 0.25F, 0.25F};
constexpr std::array<float, 3> kBarOutlineRgb{0.0F, 0.0F, 0.0F};
constexpr std::array<float, 3> kBarPlateRgb{0.10F, 0.10F, 0.12F};
constexpr protekt::game::Extent kBarFrame{800, 450};

/// A pixel rectangle, from the projected corners of one quad.
struct PixelRect {
    float left{};
    float right{};
    float top{};
    float bottom{};
    float width() const { return right - left; }
    float height() const { return bottom - top; }
    float centre_x() const { return (left + right) * 0.5F; }
};

struct UnitBar {
    PixelRect outline;
    PixelRect track;
    PixelRect fill;
    std::array<float, 3> fill_colour{};
    float distance_from_eye{};
};

PixelRect rect_of(const SceneOptions& options, const Scene& scene, std::size_t first) {
    PixelRect r{1e9F, -1e9F, 1e9F, -1e9F};
    for (std::size_t i = 0; i < 6; ++i) {
        const protekt::game::SceneVertex& v = scene.vertices.at(first + i);
        const protekt::game::Projected p
            = project(options.camera, options.extent, v.position[0], v.position[1], v.position[2]);
        r = PixelRect{std::min(r.left, p.x), std::max(r.right, p.x), std::min(r.top, p.y),
                      std::max(r.bottom, p.y)};
    }
    return r;
}

/// The bars at the tail of the scene, in vertex order: groups of `kUnitBarVertexCount` read back
/// from the end while they still look like a bar (an outline quad, a plate quad, a fill quad).
std::vector<UnitBar> unit_bars_of(const SceneOptions& options, const Scene& scene) {
    std::vector<UnitBar> bars;
    for (std::size_t end = scene.vertices.size(); end >= kUnitBarVertexCount;
         end -= kUnitBarVertexCount) {
        const std::size_t first = end - kUnitBarVertexCount;
        const auto& v = scene.vertices;
        if (v.at(first).colour != kBarOutlineRgb || v.at(first + 6).colour != kBarPlateRgb) {
            break;
        }
        const float dx = v.at(first + 12).position[0] - options.camera.x;
        const float dy = v.at(first + 12).position[1] - options.camera.y;
        const float dz = v.at(first + 12).position[2] - options.camera.z;
        bars.insert(bars.begin(),
                    UnitBar{rect_of(options, scene, first), rect_of(options, scene, first + 6),
                            rect_of(options, scene, first + 12), v.at(first + 12).colour,
                            std::sqrt((dx * dx) + (dy * dy) + (dz * dz))});
    }
    return bars;
}

/// A viewer tank, one more of yours and two hostiles, in a row about ten tiles in front of the
/// camera, which looks down +Z from above and behind them.
struct BarBattle {
    protekt::sim::Sim sim;
    protekt::sim::UnitId viewer;
    SceneOptions options;
};
BarBattle bar_battle() {
    using protekt::sim::Side;
    using protekt::sim::Spawn;
    using protekt::sim::UnitKind;
    protekt::sim::Scenario scenario;
    scenario.seed = 42;
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Player, 50, 50});
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Player, 54, 51});
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Enemy, 46, 51});
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Enemy, 52, 53});
    auto sim = protekt::sim::Sim::create(scenario);
    const protekt::sim::UnitId viewer = sim.units().front().id;
    SceneOptions options;
    options.seed = 42;
    options.radius_tiles = 20;
    options.extent = kBarFrame;
    options.viewer_unit = viewer;
    options.viewer_side = Side::Player;
    options.camera.x = 50.0F * kMetresPerTile;
    options.camera.z = 38.0F * kMetresPerTile;
    options.camera.y = 14.0F;
    options.camera.yaw = 0.0F;
    options.camera.pitch = -0.4F;
    // The bars are found by walking BACK from the end of the vertex list, so nothing else may be
    // drawn after them: the ledger (C6) is off here for the same reason it is off in `options_at`.
    options.show_ledger = false;
    return BarBattle{std::move(sim), viewer, options};
}

/// Where a tank's projected top is: the turret roof, 1.2 m above its ground.
protekt::game::Projected top_of(const SceneOptions& options, const protekt::sim::Unit& unit) {
    constexpr float kFixedOne = static_cast<float>(protekt::sim::Fixed::kOne);
    return project(
        options.camera, options.extent, static_cast<float>(unit.x.raw) / kFixedOne * kMetresPerTile,
        protekt::game::height_metres(options.seed, unit.x.floor_int(), unit.y.floor_int()) + 1.2F,
        static_cast<float>(unit.y.raw) / kFixedOne * kMetresPerTile);
}

TEST(UnitBars, EveryVisibleTankButTheDrivenOneGetsABar) {
    const BarBattle b = bar_battle();
    const std::vector<UnitBar> bars = unit_bars_of(b.options, build_scene(b.sim, b.options));
    ASSERT_EQ(bars.size(), 3U) << "four tanks in view, one of them driven";
    for (const protekt::sim::Unit& unit : b.sim.units()) {
        const protekt::game::Projected top = top_of(b.options, unit);
        ASSERT_GT(top.depth, 0.0F);
        ASSERT_GE(top.x, 0.0F);
        ASSERT_LE(top.x, 800.0F);
        ASSERT_GE(top.y, 0.0F);
        ASSERT_LE(top.y, 450.0F);
    }
}

TEST(UnitBars, TheDrivenTankHasNoBarOverIt) {
    const BarBattle b = bar_battle();
    const std::vector<UnitBar> bars = unit_bars_of(b.options, build_scene(b.sim, b.options));
    const protekt::game::Projected driven = top_of(b.options, *b.sim.find(b.viewer));
    for (const UnitBar& bar : bars) {
        const bool over_it = std::abs(bar.track.centre_x() - driven.x) < 3.0F
                             && bar.track.bottom > driven.y - 12.0F && bar.track.bottom < driven.y;
        EXPECT_FALSE(over_it) << "the driven tank's hull is in the HUD panel";
    }
}

TEST(UnitBars, ATrackIs20By4AtEightHundredByFourFifty) {
    const BarBattle b = bar_battle();
    for (const UnitBar& bar : unit_bars_of(b.options, build_scene(b.sim, b.options))) {
        EXPECT_NEAR(bar.track.width(), 20.0F, 0.1F);
        EXPECT_NEAR(bar.track.height(), 4.0F, 0.1F);
        EXPECT_NEAR(bar.outline.width(), 22.0F, 0.1F) << "a 1 px outline all round";
        EXPECT_NEAR(bar.outline.height(), 6.0F, 0.1F);
        EXPECT_NEAR(bar.outline.left, bar.track.left - 1.0F, 0.1F);
        EXPECT_NEAR(bar.outline.top, bar.track.top - 1.0F, 0.1F);
    }
}

TEST(UnitBars, EachBarIsCentredAboveItsTanksProjectedTop) {
    const BarBattle b = bar_battle();
    const std::vector<UnitBar> bars = unit_bars_of(b.options, build_scene(b.sim, b.options));
    ASSERT_EQ(bars.size(), 3U);
    std::size_t next = 0;
    for (const protekt::sim::Unit& unit : b.sim.units()) {
        if (unit.id == b.viewer) {
            continue;
        }
        const protekt::game::Projected top = top_of(b.options, unit);
        const UnitBar& bar = bars.at(next++);
        EXPECT_NEAR(bar.track.centre_x(), top.x, 1.0F);
        EXPECT_LT(bar.outline.bottom, top.y) << "above the top edge, not on it";
        EXPECT_GT(bar.outline.bottom, top.y - 8.0F) << "a few pixels above, not floating off";
    }
}

TEST(UnitBars, TheSizeDoesNotChangeWithRange) {
    BarBattle b = bar_battle();
    const std::vector<UnitBar> near_bars = unit_bars_of(b.options, build_scene(b.sim, b.options));
    b.options.camera.z -= 4.0F * kMetresPerTile;
    const std::vector<UnitBar> far_bars = unit_bars_of(b.options, build_scene(b.sim, b.options));
    ASSERT_FALSE(near_bars.empty());
    ASSERT_FALSE(far_bars.empty());
    EXPECT_NEAR(far_bars.front().track.width(), near_bars.front().track.width(), 0.1F);
    EXPECT_NEAR(far_bars.front().track.height(), near_bars.front().track.height(), 0.1F);
}

TEST(UnitBars, GreenForYoursAndRedForHostiles) {
    const BarBattle b = bar_battle();
    const std::vector<UnitBar> bars = unit_bars_of(b.options, build_scene(b.sim, b.options));
    ASSERT_EQ(bars.size(), 3U);
    std::size_t next = 0;
    for (const protekt::sim::Unit& unit : b.sim.units()) {
        if (unit.id == b.viewer) {
            continue;
        }
        EXPECT_EQ(bars.at(next++).fill_colour,
                  unit.side == protekt::sim::Side::Player ? kOwnBarRgb : kHostileBarRgb);
    }
}

TEST(UnitBars, AFullHullFillsTheTrackAndAHurtOneFillsItsShare) {
    BarBattle b = bar_battle();
    const std::vector<UnitBar> full = unit_bars_of(b.options, build_scene(b.sim, b.options));
    ASSERT_EQ(full.size(), 3U);
    for (const UnitBar& bar : full) {
        EXPECT_NEAR(bar.fill.width(), 20.0F, 0.1F);
        EXPECT_NEAR(bar.fill.left, bar.track.left, 0.1F) << "the fill grows from the left";
    }
    // The tanks shoot each other: step until somebody is hurt, then every fill must be its hp
    // share.
    bool hurt = false;
    for (int guard = 0; guard < 4000 && !hurt; ++guard) {
        b.sim.step();
        for (const protekt::sim::Unit& unit : b.sim.units()) {
            hurt = hurt || (unit.alive && unit.hp < protekt::sim::spec_of(unit.kind).hp);
        }
    }
    ASSERT_TRUE(hurt) << "the scenario must produce damage";
    const std::vector<UnitBar> after = unit_bars_of(b.options, build_scene(b.sim, b.options));
    std::vector<const protekt::sim::Unit*> drawn;
    for (const protekt::sim::Unit& unit : b.sim.units()) {
        if (unit.alive && unit.id != b.viewer) {
            drawn.push_back(&unit);
        }
    }
    ASSERT_EQ(after.size(), drawn.size());
    bool saw_partial = false;
    for (std::size_t i = 0; i < after.size(); ++i) {
        const float share = static_cast<float>(drawn.at(i)->hp)
                            / static_cast<float>(protekt::sim::spec_of(drawn.at(i)->kind).hp);
        EXPECT_NEAR(after.at(i).fill.width(), 20.0F * share, 0.1F);
        saw_partial = saw_partial || share < 1.0F;
    }
    EXPECT_TRUE(saw_partial);
}

TEST(UnitBars, SitOnTheHudLayerNearerThanTheReticle) {
    const BarBattle b = bar_battle();
    for (const UnitBar& bar : unit_bars_of(b.options, build_scene(b.sim, b.options))) {
        EXPECT_GT(bar.distance_from_eye, 0.1F) << "past the near plane";
        EXPECT_LT(bar.distance_from_eye, 1.0F) << "nearer than the reticle's 1.5 m";
    }
}

TEST(UnitBars, NoViewerMeansNoBarsAndATankBehindTheEyeGetsNone) {
    BarBattle b = bar_battle();
    SceneOptions blind = b.options;
    blind.viewer_unit = protekt::sim::UnitId{};
    EXPECT_TRUE(unit_bars_of(blind, build_scene(b.sim, blind)).empty());
    b.options.camera.yaw = protekt::math::kPi; // looking away from every tank
    EXPECT_TRUE(unit_bars_of(b.options, build_scene(b.sim, b.options)).empty());
}

// ---- the sky: a gradient dome and a sun disc, raw colours, centred on the camera ----

namespace {

using protekt::game::kSkyVertexCount;
using protekt::game::SceneVertex;

/// The sky's vertices as build_scene lays them out: right after the terrain.
std::vector<SceneVertex> sky_of(const Scene& scene, int radius) {
    const auto first
        = scene.vertices.begin() + static_cast<std::ptrdiff_t>(terrain_vertex_count(radius));
    return {first, first + static_cast<std::ptrdiff_t>(kSkyVertexCount)};
}

Scene sky_scene(const SceneOptions& options) {
    return protekt::game::build_scene(protekt::sim::Sim::create(protekt::sim::skirmish(5)),
                                      options);
}

} // namespace

TEST(Sky, AddsExactlyItsGeometryToTheScene) {
    // Twelve quad bands (16 x 6) + a zenith fan (16 x 3) + the sun disc (16 x 3) = 1248 vertices,
    // then the moon disc: exactly its own count more.
    EXPECT_EQ(protekt::game::kSkyRings, std::size_t{14});
    EXPECT_EQ(protekt::game::kMoonDiscVertexCount, std::size_t{72});
    EXPECT_EQ(kSkyVertexCount, std::size_t{1248} + protekt::game::kMoonDiscVertexCount);
    const SceneOptions options = options_at(60.0F, 60.0F, 5, 4);
    const Scene scene = sky_scene(options);
    const std::size_t terrain = terrain_vertex_count(4);
    ASSERT_GE(scene.vertices.size(), terrain + kSkyVertexCount);
    // The terrain is first and the sky follows: the first sky vertex is on the dome, 1000 m out.
    const auto& v = scene.vertices.at(terrain);
    const float dx = v.position[0] - options.camera.x;
    const float dy = v.position[1] - options.camera.y;
    const float dz = v.position[2] - options.camera.z;
    EXPECT_NEAR(std::sqrt((dx * dx) + (dy * dy) + (dz * dz)), protekt::game::kSkyRadiusMetres,
                0.1F);
}

TEST(Sky, SunDiscSitsExactlyOnTheDirectionThePrismsAreLitFrom) {
    const SceneOptions options = options_at(60.0F, 60.0F, 5, 4);
    const std::vector<SceneVertex> sky = sky_of(sky_scene(options), 4);
    const std::size_t disc = protekt::game::kSunDiscVertexCount;
    const std::size_t first = sky.size() - protekt::game::kMoonDiscVertexCount - disc;
    std::array<double, 3> sum{};
    for (std::size_t i = first; i < first + disc; ++i) {
        EXPECT_EQ(sky.at(i).colour, protekt::game::kSunDiscColour);
        sum[0] += static_cast<double>(sky.at(i).position[0] - options.camera.x);
        sum[1] += static_cast<double>(sky.at(i).position[1] - options.camera.y);
        sum[2] += static_cast<double>(sky.at(i).position[2] - options.camera.z);
    }
    const double n = static_cast<double>(disc);
    const protekt::math::Vec3 centroid{static_cast<float>(sum[0] / n),
                                       static_cast<float>(sum[1] / n),
                                       static_cast<float>(sum[2] / n)};
    const protekt::math::Vec3 seen = protekt::math::normalise(centroid);
    const protekt::math::Vec3 sun = protekt::math::normalise(protekt::game::kSunDirection);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        EXPECT_NEAR(seen.at(axis), sun.at(axis), 1e-4F) << "axis " << axis;
    }
    EXPECT_GT(seen[1], 0.5F) << "the sun is well above the horizon";
}

TEST(Sky, CarriesTheExactGradientColoursAndIsNeverShaded) {
    const SceneOptions options = options_at(60.0F, 60.0F, 5, 4);
    const std::vector<SceneVertex> sky = sky_of(sky_scene(options), 4);
    const std::size_t dome
        = sky.size() - protekt::game::kSunDiscVertexCount - protekt::game::kMoonDiscVertexCount;
    // Every dome vertex's colour is one of the ring colours, chosen by its elevation.
    for (std::size_t i = 0; i < dome; ++i) {
        const float dy = sky.at(i).position[1] - options.camera.y;
        const float elevation = std::asin(std::min(1.0F, dy / protekt::game::kSkyRadiusMetres))
                                * 180.0F / 3.14159265F;
        std::size_t ring = 0;
        for (std::size_t r = 0; r < protekt::game::kSkyRings; ++r) {
            if (std::abs(elevation - protekt::game::kSkyRingElevationDegrees.at(r)) < 0.2F) {
                ring = r;
            }
        }
        EXPECT_NEAR(elevation, protekt::game::kSkyRingElevationDegrees.at(ring), 0.2F);
        EXPECT_EQ(sky.at(i).colour, protekt::game::kSkyRingColours.at(ring));
    }
    // Horizon haze is lighter than the zenith, and the old flat clear colour is in the gradient.
    EXPECT_GT(protekt::game::kSkyRingColours.front()[0], protekt::game::kSkyRingColours.back()[0]);
    EXPECT_EQ(protekt::game::kSkyRingColours.at(protekt::game::kSkyThirtyDegreeRing),
              (protekt::math::Vec3{0.53F, 0.81F, 0.92F}));
}

namespace {

/// The moon's vertices: the last kMoonDiscVertexCount of the sky.
std::vector<SceneVertex> moon_of(const SceneOptions& options) {
    const std::vector<SceneVertex> sky = sky_of(sky_scene(options), 4);
    return {sky.end() - static_cast<std::ptrdiff_t>(protekt::game::kMoonDiscVertexCount),
            sky.end()};
}

float degrees_between(const protekt::math::Vec3& a, const protekt::math::Vec3& b) {
    const float d = std::clamp(
        protekt::math::dot(protekt::math::normalise(a), protekt::math::normalise(b)), -1.0F, 1.0F);
    return std::acos(d) * 180.0F / 3.14159265F;
}

protekt::math::Vec3 centroid_direction(const std::vector<SceneVertex>& disc,
                                       const SceneOptions& options) {
    protekt::math::Vec3 sum{0.0F, 0.0F, 0.0F};
    for (const SceneVertex& v : disc) {
        sum[0] += v.position[0] - options.camera.x;
        sum[1] += v.position[1] - options.camera.y;
        sum[2] += v.position[2] - options.camera.z;
    }
    return protekt::math::normalise(sum);
}

} // namespace

TEST(Moon, AddsExactlyItsDiscAndSitsOnTheDirectionTheConstantNames) {
    const SceneOptions options = options_at(60.0F, 60.0F, 5, 4);
    const std::vector<SceneVertex> moon = moon_of(options);
    ASSERT_EQ(moon.size(), protekt::game::kMoonDiscVertexCount);
    EXPECT_EQ(protekt::game::kMoonDiscVertexCount % 3, std::size_t{0});
    EXPECT_LT(degrees_between(centroid_direction(moon, options), protekt::game::kMoonDirection),
              0.1F);
    // On the sky's own surface (a hair inside, like the sun), above the horizon, clear of the sun.
    for (const SceneVertex& v : moon) {
        const protekt::math::Vec3 d{v.position[0] - options.camera.x,
                                    v.position[1] - options.camera.y,
                                    v.position[2] - options.camera.z};
        const float r = std::sqrt((d[0] * d[0]) + (d[1] * d[1]) + (d[2] * d[2]));
        EXPECT_LE(r, protekt::game::kSkyRadiusMetres);
        EXPECT_GT(r, 0.9F * protekt::game::kSkyRadiusMetres);
        EXPECT_GT(d[1], 0.0F) << "every rim vertex is above the horizon";
    }
    EXPECT_GT(degrees_between(protekt::game::kMoonDirection, protekt::game::kSunDirection), 30.0F);
}

TEST(Moon, IsItsOwnColourAndTheSunDoesNotShadeIt) {
    const SceneOptions options = options_at(60.0F, 60.0F, 5, 4);
    for (const SceneVertex& v : moon_of(options)) {
        EXPECT_EQ(v.colour, protekt::game::kMoonDiscColour);
    }
    // The sky is its own emitter: nothing the sun does reaches it. The sun is a constant, so the
    // fixture turns the CAMERA instead -- the moon's colour must not move with the view either.
    SceneOptions turned = options;
    turned.camera.yaw = 2.0F;
    turned.camera.pitch = 0.3F;
    for (const SceneVertex& v : moon_of(turned)) {
        EXPECT_EQ(v.colour, protekt::game::kMoonDiscColour);
    }
}

TEST(Moon, IsAPureFunctionOfTheCamera) {
    const SceneOptions options = options_at(60.0F, 60.0F, 5, 4);
    const std::vector<SceneVertex> a = moon_of(options);
    const std::vector<SceneVertex> b = moon_of(options);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a.at(i).position, b.at(i).position);
    }
    SceneOptions moved = options;
    moved.camera.x += 40.0F;
    const std::vector<SceneVertex> c = moon_of(moved);
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_NEAR(c.at(i).position[0] - a.at(i).position[0], 40.0F, 1e-3F);
        EXPECT_NEAR(c.at(i).position[1], a.at(i).position[1], 1e-3F);
    }
}

TEST(Moon, IsInFrameWhenTheCameraFacesIt) {
    // Facing the moon's bearing, its centre projects inside the frame (clip x, y within +-1).
    SceneOptions options = options_at(60.0F, 60.0F, 5, 4);
    options.camera.yaw
        = std::atan2(protekt::game::kMoonDirection[0], protekt::game::kMoonDirection[2]);
    const Scene scene = sky_scene(options);
    const auto clip = protekt::math::transform(
        scene.view_projection,
        protekt::math::Vec3{options.camera.x + (protekt::game::kMoonDirection[0] * 980.0F),
                            options.camera.y + (protekt::game::kMoonDirection[1] * 980.0F),
                            options.camera.z + (protekt::game::kMoonDirection[2] * 980.0F)});
    ASSERT_GT(clip[3], 0.0F);
    EXPECT_LT(std::abs(clip[0] / clip[3]), 1.0F);
    EXPECT_LT(std::abs(clip[1] / clip[3]), 1.0F);
}

TEST(Sky, RingsAreDenseEnoughNearTheHorizonToNotBand) {
    const auto& degrees = protekt::game::kSkyRingElevationDegrees;
    EXPECT_EQ(degrees.front(), -30.0F);
    EXPECT_EQ(degrees.at(protekt::game::kSkyHorizonRing), 0.0F);
    EXPECT_EQ(degrees.back(), 90.0F);
    EXPECT_EQ(degrees.at(protekt::game::kSkyThirtyDegreeRing), 30.0F);
    for (std::size_t r = 0; r + 1 < degrees.size(); ++r) {
        EXPECT_GT(degrees.at(r + 1), degrees.at(r)) << "ring " << r;
        if (degrees.at(r) >= 0.0F && degrees.at(r + 1) <= 30.0F) {
            EXPECT_LE(degrees.at(r + 1) - degrees.at(r), 6.0F) << "gap above ring " << r;
        }
    }
}

TEST(Sky, RingColoursBlendMonotonicallyFromHazeToZenith) {
    const auto& colours = protekt::game::kSkyRingColours;
    EXPECT_EQ(colours.front(), (protekt::math::Vec3{0.80F, 0.90F, 0.95F}));
    for (std::size_t r = protekt::game::kSkyHorizonRing + 1; r + 1 < colours.size(); ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            EXPECT_LT(colours.at(r).at(c), colours.at(r - 1).at(c))
                << "ring " << r << " channel " << c;
            EXPECT_GT(colours.at(r).at(c), colours.at(r + 1).at(c))
                << "ring " << r << " channel " << c;
        }
    }
    EXPECT_EQ(colours.back(), (protekt::math::Vec3{0.25F, 0.50F, 0.82F}));
}

TEST(Sky, TheSkirtReachesMinusThirtyDegreesInExactlyTheHazeColour) {
    // The regression pin for the "ocean" band: the clear colour used to show between the dome's
    // horizon ring and the terrain silhouette. The dome now covers -30 to 90 degrees, and its
    // lowest ring (and the three above it, up to the horizon) is exactly haze.
    const auto& degrees = protekt::game::kSkyRingElevationDegrees;
    const auto& colours = protekt::game::kSkyRingColours;
    EXPECT_EQ(degrees.front(), -30.0F);
    for (std::size_t r = 0; r <= protekt::game::kSkyHorizonRing; ++r) {
        EXPECT_EQ(colours.at(r), (protekt::math::Vec3{0.80F, 0.90F, 0.95F})) << "ring " << r;
    }
    SceneOptions options = options_at(60.0F, 60.0F, 5, 4);
    options.camera.y = 11.5F;
    float lowest = 1e9F;
    float highest = -1e9F;
    const std::vector<SceneVertex> sky = sky_of(sky_scene(options), 4);
    const std::size_t dome = sky.size() - protekt::game::kSunDiscVertexCount;
    for (std::size_t i = 0; i < dome; ++i) {
        lowest = std::min(lowest, sky.at(i).position[1] - options.camera.y);
        highest = std::max(highest, sky.at(i).position[1] - options.camera.y);
    }
    EXPECT_NEAR(lowest, -500.0F, 0.5F) << "sin(-30 degrees) * 1000 m";
    EXPECT_NEAR(highest, protekt::game::kSkyRadiusMetres, 0.5F);
}

TEST(Sky, FollowsTheCameraButNeverTurnsWithIt) {
    SceneOptions a = options_at(60.0F, 60.0F, 5, 4);
    SceneOptions b = a;
    b.camera.yaw = 1.3F;
    b.camera.pitch = 0.2F;
    const auto sky_a = sky_of(sky_scene(a), 4);
    const auto sky_b = sky_of(sky_scene(b), 4);
    ASSERT_EQ(sky_a.size(), sky_b.size());
    for (std::size_t i = 0; i < sky_a.size(); ++i) {
        EXPECT_EQ(sky_a.at(i).position, sky_b.at(i).position) << "world-fixed orientation";
    }
    SceneOptions moved = a;
    moved.camera.x += 2.0F;
    const auto sky_m = sky_of(sky_scene(moved), 4);
    EXPECT_NEAR(sky_m.front().position[0] - sky_a.front().position[0], 2.0F, 1e-3F);
}

TEST(Sky, FitsInsideTheFarPlane) {
    SceneOptions options = options_at(60.0F, 60.0F, 5, 4);
    for (const SceneVertex& v : sky_of(sky_scene(options), 4)) {
        const float dx = v.position[0] - options.camera.x;
        const float dy = v.position[1] - options.camera.y;
        const float dz = v.position[2] - options.camera.z;
        EXPECT_LT(std::sqrt((dx * dx) + (dy * dy) + (dz * dz)), protekt::math::kFarPlane);
    }
}

TEST(Sky, TwoBuildsOfTheSameFrameAreByteIdentical) {
    const SceneOptions options = options_at(60.0F, 60.0F, 5, 4);
    const Scene a = sky_scene(options);
    const Scene b = sky_scene(options);
    ASSERT_EQ(a.vertices.size(), b.vertices.size());
    EXPECT_EQ(
        std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(SceneVertex)),
        0);
}

TEST(Sky, IsAbsentFromTheViewlessCalibrationScene) {
    const Scene scene = protekt::game::build_calibration_scene(options_at(60.0F, 60.0F, 5, 4));
    EXPECT_EQ(scene.vertices.size(), std::size_t{18}) << "three bars, and no sky";
}

// ---- texture plumbing (D2a): the vertex carries a UV that nothing sets yet ----

namespace {

using protekt::game::SceneVertex;

TEST(SceneUv, TerrainSamplesAGroundTileAndNothingLeavesTheAtlas) {
    // A viewer (radar, ship instrument), an aim (reticle) and a hit flash (the final wash) between
    // them reach every emitter in build_scene: terrain, sky, units, projectiles and the HUD.
    const Damaged d = factory_down_to(200);
    SceneOptions options = watching(d);
    options.aim = protekt::game::Aim{640.0F, 360.0F};
    options.hit_flash = 0.5F;
    const Scene scene = protekt::game::build_scene(d.sim, options);
    ASSERT_GT(scene.vertices.size(), terrain_vertex_count(options.radius_tiles));
    // Units carry atlas UVs (tanks since D2b, buildings since D2e); since D3 the terrain samples
    // one of the ground tiles (since D6 one of each type's three variants), never anything else.
    std::vector<protekt::game::UvRect> ground;
    for (int t = 0; t < protekt::world::kTerrainCount; ++t) {
        for (int v = 0; v < protekt::game::kGroundVariantCount; ++v) {
            ground.push_back(protekt::game::atlas_manifest().rect(
                protekt::game::terrain_variant_name(static_cast<protekt::world::Terrain>(t), v)));
        }
    }
    for (std::size_t i = 0; i < scene.vertices.size(); ++i) {
        const SceneVertex& v = scene.vertices.at(i);
        ASSERT_GE(v.u, 0.0F);
        ASSERT_LE(v.u, 1.0F);
        ASSERT_GE(v.v, 0.0F);
        ASSERT_LE(v.v, 1.0F);
        if (i < terrain_vertex_count(options.radius_tiles)) {
            ASSERT_TRUE(std::any_of(ground.begin(), ground.end(),
                                    [&](const auto& r) {
                                        return (v.u == r.u0 || v.u == r.u1)
                                               && (v.v == r.v0 || v.v == r.v1);
                                    }))
                << "terrain vertex " << i;
        }
    }
}

TEST(SceneUv, ASkirmishPastItsTerrainAndTheCalibrationSceneLeaveTheUvAtZeroToo) {
    // D3: the terrain samples its ground tiles (above); everything after it here sets no UV.
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    const SceneOptions options = options_at(60.0F, 60.0F);
    const std::size_t terrain = terrain_vertex_count(options.radius_tiles);
    for (const auto& [scene, first] :
         {std::pair{protekt::game::build_scene(sim, options), terrain},
          std::pair{protekt::game::build_calibration_scene(options), std::size_t{0}}}) {
        ASSERT_GT(scene.vertices.size(), first);
        for (std::size_t i = first; i < scene.vertices.size(); ++i) {
            ASSERT_EQ(scene.vertices.at(i).u, 0.0F);
            ASSERT_EQ(scene.vertices.at(i).v, 0.0F);
        }
    }
}

TEST(SceneUv, TheVertexIsNineFloatsWithNoPaddingBecauseItIsUploadedVerbatim) {
    EXPECT_EQ(sizeof(SceneVertex), std::size_t{9} * sizeof(float));
    EXPECT_EQ(offsetof(SceneVertex, alpha), std::size_t{6} * sizeof(float));
    EXPECT_EQ(offsetof(SceneVertex, u), std::size_t{7} * sizeof(float));
    EXPECT_EQ(offsetof(SceneVertex, v), std::size_t{8} * sizeof(float));
}

TEST(SceneUv, PinnedVertexCountsSurviveTheWiderVertex) {
    const Scene scene = protekt::game::build_calibration_scene(options_at(60.0F, 60.0F, 5, 4));
    EXPECT_EQ(scene.vertices.size(), std::size_t{18});
    EXPECT_EQ(scene.vertices.size() * sizeof(SceneVertex), std::size_t{18} * 36U);
}

} // namespace

// ---- real UVs on the tank (D2b): every part addresses its own atlas tile ----

namespace {

using protekt::game::kHalfTexel;
using protekt::game::SceneVertex;
using protekt::game::tile_rect;
using protekt::game::UvRect;

/// A whole 0.125-wide cell of the atlas pulled in by the half-texel inset.
UvRect cell(float u0, float v0, float u1, float v1) {
    return {u0 + kHalfTexel, v0 + kHalfTexel, u1 - kHalfTexel, v1 - kHalfTexel};
}

/// The viewer's drawn tank: the last kTankVertices of a facing scene, in emission order -- two
/// tracks, hull, engine deck, grille, two fenders, turret, bustle, roof panel, muzzle collar,
/// barrel.
std::vector<SceneVertex> drawn_tank(float heading, float turret) {
    const auto sim = facing_sim();
    const Scene scene = facing_scene(sim, heading, turret);
    return {scene.vertices.end() - static_cast<std::ptrdiff_t>(kTankVertices),
            scene.vertices.end()};
}

/// Every vertex in [first, last) lies on the corners of `rect`, and all four corners are used.
void expect_corners_of(const std::vector<SceneVertex>& tank, std::size_t first, std::size_t last,
                       const UvRect& rect, const char* what) {
    std::set<std::pair<float, float>> seen;
    for (std::size_t i = first; i < last; ++i) {
        const SceneVertex& v = tank.at(i);
        const bool on_u = v.u == rect.u0 || v.u == rect.u1;
        const bool on_v = v.v == rect.v0 || v.v == rect.v1;
        ASSERT_TRUE(on_u && on_v) << what << " vertex " << i << " has uv (" << v.u << ", " << v.v
                                  << ")";
        seen.insert({v.u, v.v});
    }
    EXPECT_EQ(seen.size(), std::size_t{4}) << what << " uses all four corners of its tile";
}

/// The tile of the camo variant the facing scene's tank wears.
UvRect viewer_hull_rect() {
    const auto sim = facing_sim();
    const auto name
        = protekt::game::kTeamHullNames.at(static_cast<std::size_t>(viewer_of(sim).side))
              .at(static_cast<std::size_t>(
                  protekt::game::hull_variant(42, viewer_of(sim).id.value)));
    return tile_rect(protekt::game::atlas_manifest().tile(name));
}

TEST(TankUv, EachPartAddressesItsTilesExactRect) {
    const auto tank = drawn_tank(0.3F, 0.4F);
    ASSERT_EQ(tank.size(), kTankVertices);
    // Pinned: tracks and grille are tile 1, (0.125, 0) to (0.25, 0.125); the hull, engine deck,
    // fenders, turret and bustle the same one of its team's camo tiles; the roof panel 2; barrel
    // and collar 3.
    const UvRect tracks = tile_rect(1);
    EXPECT_EQ(tracks.u0, 0.125F + kHalfTexel);
    EXPECT_EQ(tracks.u1, 0.25F - kHalfTexel);
    expect_corners_of(tank, 0, 72, cell(0.125F, 0.0F, 0.25F, 0.125F), "skirts");
    const UvRect hull = viewer_hull_rect();
    EXPECT_GE(hull.v0, 0.125F) << "the camo tiles are on the second and third rows";
    expect_corners_of(tank, 72, 108, hull, "hull");
    expect_corners_of(tank, 108, 144, hull, "engine deck");
    expect_corners_of(tank, 144, 180, cell(0.125F, 0.0F, 0.25F, 0.125F), "grille");
    expect_corners_of(tank, 180, 252, hull, "fenders");
    expect_corners_of(tank, 252, 288, hull, "turret");
    expect_corners_of(tank, 288, 324, hull, "bustle");
    expect_corners_of(tank, 324, 360, cell(0.25F, 0.0F, 0.375F, 0.125F), "roof panel");
    expect_corners_of(tank, 360, 396, cell(0.375F, 0.0F, 0.5F, 0.125F), "collar");
    expect_corners_of(tank, 396, 432, cell(0.375F, 0.0F, 0.5F, 0.125F), "barrel");
}

TEST(TankUv, EveryHullFaceCarriesTheHullTilesCorners) {
    const auto tank = drawn_tank(0.0F, 0.0F);
    for (std::size_t face = 0; face < 6; ++face) {
        std::set<std::pair<float, float>> seen;
        for (std::size_t i = 72 + (face * 6); i < 72 + (face * 6) + 6; ++i) {
            seen.insert({tank.at(i).u, tank.at(i).v});
        }
        const UvRect hull = viewer_hull_rect();
        const std::set<std::pair<float, float>> corners{
            {hull.u0, hull.v0}, {hull.u1, hull.v0}, {hull.u1, hull.v1}, {hull.u0, hull.v1}};
        EXPECT_EQ(seen, corners) << "hull face " << face;
    }
}

TEST(TankUv, NoTankUvIsOutsideTheAtlasOrLeftAtThePlaceholder) {
    for (const float heading : {0.0F, 1.2F}) {
        const auto tank = drawn_tank(heading, 0.5F);
        for (std::size_t tri = 0; tri < tank.size(); tri += 3) {
            bool moved = false;
            for (std::size_t k = 0; k < 3; ++k) {
                const SceneVertex& v = tank.at(tri + k);
                ASSERT_GE(v.u, 0.0F);
                ASSERT_LE(v.u, 1.0F);
                ASSERT_GE(v.v, 0.0F);
                ASSERT_LE(v.v, 1.0F);
                moved = moved || v.u != tank.at(tri).u || v.v != tank.at(tri).v;
            }
            EXPECT_TRUE(moved) << "triangle " << tri / 3 << " has the same uv on all three corners";
        }
    }
}

/// One unit's box, alone: the vertices that move when it is hovered far above the ground.
std::vector<SceneVertex> unit_box_of(protekt::sim::UnitKind kind) {
    const auto sim = sim_of({{Side::Player, kind, 100, 100}});
    return tank_only(sim, sim.units().at(0), {});
}

/// The factory's prisms, in emission order. The player's factory is not turned, so its local
/// across is world X and its door's side is world +Z.
constexpr std::size_t kHallPart = 0;
constexpr std::size_t kJambParts = 1; ///< and 2
constexpr std::size_t kLintelPart = 3;
constexpr std::size_t kAnnexPart = 4;
constexpr std::size_t kStackPart = 5;
constexpr std::size_t kBandPart = 6;

/// The [lo, hi] of `axis` (0 X, 1 up, 2 Z) over prism `prism` of a drawn factory.
std::array<float, 2> factory_span(const std::vector<SceneVertex>& factory, std::size_t prism,
                                  std::size_t axis) {
    float lo = 1e9F;
    float hi = -1e9F;
    for (std::size_t i = prism * 36U; i < (prism + 1U) * 36U; ++i) {
        lo = std::min(lo, factory.at(i).position.at(axis));
        hi = std::max(hi, factory.at(i).position.at(axis));
    }
    return {lo, hi};
}

// Adapted (D5c): the factory is seven prisms, not one box. The hall and annex keep whole tiles per
// face, the walls now stood upright; each portal piece samples the strip of wall it covers.
TEST(UnitBoxUv, AFactorysWallsTakeTheWallTileAndItsRoofTheRoofTile) {
    const auto box = unit_box_of(UnitKind::Factory);
    ASSERT_EQ(box.size(), protekt::game::kFactoryVertices);
    const auto& atlas = protekt::game::atlas_manifest();
    const UvRect walls = tile_rect(atlas.tile("factory_walls"));
    const UvRect roof = tile_rect(atlas.tile("factory_roof"));
    EXPECT_EQ(walls.u0, 0.0F + kHalfTexel) << "factory_walls is tile 4, the second row's first";
    EXPECT_EQ(walls.v0, 0.125F + kHalfTexel);
    // Faces in order: near, far, left, right, TOP, bottom.
    for (const std::size_t solid : {kHallPart, kAnnexPart}) {
        const std::size_t at = solid * 36U;
        expect_corners_of(box, at, at + 24, walls, "walls");
        expect_corners_of(box, at + 24, at + 30, roof, "roof");
        expect_corners_of(box, at + 30, at + 36, walls, "underside");
    }
    expect_corners_of(box, kStackPart * 36U, (kStackPart + 2U) * 36U, roof, "stack and band");
    for (std::size_t i = kHallPart * 36U + 36U; i < kAnnexPart * 36U; ++i) {
        const auto& v = box.at(i);
        const UvRect& tile
            = i >= (kLintelPart * 36U) + 24 && i < (kLintelPart * 36U) + 30 ? roof : walls;
        EXPECT_TRUE(v.u >= tile.u0 && v.u <= tile.u1 && v.v >= tile.v0 && v.v <= tile.v1)
            << "portal vertex " << i << " stays in its tile";
    }
    // UPRIGHT: on the hall's front face the ground edge takes the tile's bottom row, so the
    // painted door stands on the ground and the eaves are at the roof.
    const float ground = factory_span(box, kHallPart, 1)[0];
    for (std::size_t i = (kHallPart * 36U) + 6; i < (kHallPart * 36U) + 12; ++i) {
        const auto& v = box.at(i);
        EXPECT_EQ(v.v, std::abs(v.position[1] - ground) < kTolerance ? walls.v1 : walls.v0)
            << "front face vertex " << i;
    }
}

// Re-pinned DELIBERATELY (D5a-2): the turret was one box, it is four prisms of the metal tile now.
TEST(UnitBoxUv, ATurretIsTurretBodyAllOver) {
    const auto box = unit_box_of(UnitKind::Turret);
    ASSERT_EQ(box.size(), protekt::game::kTurretVertices);
    expect_corners_of(box, 0, protekt::game::kTurretVertices,
                      tile_rect(protekt::game::atlas_manifest().tile("turret_body")),
                      "turret body");
}

TEST(UnitBoxUv, ATintStillMultipliesOnTheBuildingsVertexColourAndNotItsUv) {
    const auto sim = sim_of({{Side::Player, UnitKind::Factory, 100, 100}});
    const auto plain = tank_only(sim, sim.units().at(0), {});
    const std::array<protekt::sim::UnitId, 1> chosen{sim.units().at(0).id};
    const auto picked = tank_only(sim, sim.units().at(0), chosen);
    ASSERT_EQ(plain.size(), picked.size());
    for (std::size_t i = 0; i < plain.size(); ++i) {
        EXPECT_EQ(plain.at(i).u, picked.at(i).u);
        EXPECT_EQ(plain.at(i).v, picked.at(i).v);
    }
    EXPECT_NE(plain.at(0).colour, picked.at(0).colour);
}

// Re-pinned DELIBERATELY (D5c): the factory was one 36-vertex box.
TEST(FactoryShape, TheFactoryIsExactlyItsSevenPrisms) {
    EXPECT_EQ(unit_box_of(UnitKind::Factory).size(), std::size_t{252})
        << "seven prisms of 36; one box (36) before D5c";
    EXPECT_EQ(protekt::game::kFactoryVertices, std::size_t{252});
}

// The model chain's `else` draws a FACTORY, so a kind with no branch of its own is a
// factory-shaped bug. The same spot, drawn as each, must come out different.
TEST(RecyclerShape, ARecyclerIsNotDrawnAsAFactoryNorAsATank) {
    const auto recycler = unit_box_of(UnitKind::Recycler);
    const auto factory = unit_box_of(UnitKind::Factory);
    ASSERT_EQ(factory.size(), protekt::game::kFactoryVertices) << "the anchor: the factory's count";
    EXPECT_FALSE(recycler.empty()) << "a new kind must not be an invisible one";
    EXPECT_NE(recycler.size(), factory.size()) << "the fallback drew a factory";
    EXPECT_NE(recycler.size(), kTankVertices) << "the recycler borrowed the tank's turret model";
}

// VIS: the scout has no art of its own yet and borrows the tank's body, rather than falling to the
// chain's factory `else`.
TEST(ScoutShape, AScoutIsDrawnAsATankAndNotAsAFactory) {
    const auto scout = unit_box_of(UnitKind::Scout);
    EXPECT_EQ(scout.size(), kTankVertices) << "the tank's body, until it has its own";
    EXPECT_NE(scout.size(), protekt::game::kFactoryVertices) << "the fallback drew a factory";
}

/// The recycler's prisms, in emission order. A fresh player recycler is not turned, so its local
/// across is world X and its front world +Z.
constexpr std::size_t kLeftTrackAssembly = 0;  ///< the track, its six wheels, its skirt
constexpr std::size_t kRightTrackAssembly = 8; ///< the same on the right
constexpr std::size_t kTrackAssemblyParts = 8;
constexpr std::size_t kRecyclerCab = 26;

/// The [lo, hi] of `axis` (0 X, 1 up, 2 Z) over the prisms [first, last) of a drawn model.
std::array<float, 2> parts_span(const std::vector<SceneVertex>& model, std::size_t first,
                                std::size_t last, std::size_t axis) {
    float lo = 1e9F;
    float hi = -1e9F;
    for (std::size_t i = first * 36U; i < last * 36U; ++i) {
        lo = std::min(lo, model.at(i).position.at(axis));
        hi = std::max(hi, model.at(i).position.at(axis));
    }
    return {lo, hi};
}

// Re-pinned DELIBERATELY (RECYCLER-ART): the placeholder was the tank's hull and three boxes.
TEST(RecyclerShape, TheRecyclerIsExactlyItsThirtyThreePrisms) {
    EXPECT_EQ(unit_box_of(UnitKind::Recycler).size(), std::size_t{1188})
        << "thirty-three prisms of 36; the placeholder's ten (360) before RECYCLER-ART";
    EXPECT_EQ(protekt::game::kRecyclerVertices, std::size_t{1188});
}

TEST(RecyclerShape, ItIsLongerThanItIsWideAndStaysInsideTheFootprint) {
    const auto recycler = unit_box_of(UnitKind::Recycler);
    ASSERT_EQ(recycler.size(), protekt::game::kRecyclerVertices);
    const std::size_t all = protekt::game::kRecyclerVertices / 36U;
    const auto across = parts_span(recycler, 0, all, 0);
    const auto along = parts_span(recycler, 0, all, 2);
    const auto up = parts_span(recycler, 0, all, 1);
    EXPECT_GT(along[1] - along[0], 1.5F * (across[1] - across[0])) << "a long vehicle, not a block";
    EXPECT_LT(up[1] - up[0], 0.5F * (along[1] - along[0])) << "and a low one";
    // The placeholder's envelope (unit at tile 100, 100): 0.3 tile across, the tank's 1.3 m along,
    // and under the picker's 1.8 m box.
    constexpr float kCentre = 100.0F * kMetresPerTile;
    for (const SceneVertex& v : recycler) {
        EXPECT_LE(std::abs(v.position[0] - kCentre), (0.3F * kMetresPerTile) + kTolerance);
        EXPECT_LE(std::abs(v.position[2] - kCentre), 1.3F + kTolerance);
        EXPECT_LE(v.position[1] - up[0], 1.8F);
    }
}

TEST(RecyclerShape, TheCabIsTheTallestPartAndSitsTowardTheFront) {
    const auto recycler = unit_box_of(UnitKind::Recycler);
    ASSERT_EQ(recycler.size(), protekt::game::kRecyclerVertices);
    const std::size_t all = protekt::game::kRecyclerVertices / 36U;
    const float ground = parts_span(recycler, 0, all, 1)[0];
    const float roof = parts_span(recycler, kRecyclerCab, kRecyclerCab + 1, 1)[1];
    for (std::size_t part = 0; part < all; ++part) {
        if (part != kRecyclerCab) {
            EXPECT_LT(parts_span(recycler, part, part + 1, 1)[1], roof - 0.15F)
                << "part " << part << " stands as tall as the cab";
        }
    }
    // The cab roof is about 1.7 times the fender line, as BZ2's is.
    const float fender = parts_span(recycler, kTrackAssemblyParts - 1, kTrackAssemblyParts, 1)[1];
    EXPECT_NEAR((roof - ground) / (fender - ground), 1.7F, 0.1F);
    constexpr float kCentre = 100.0F * kMetresPerTile;
    const auto cab = parts_span(recycler, kRecyclerCab, kRecyclerCab + 1, 2);
    EXPECT_GT(cab[0], kCentre) << "the whole cab is in the front half";
    EXPECT_GT(cab[1] - cab[0], 0.6F) << "and it is a big block, not a hatch";
}

TEST(RecyclerShape, ItRunsOnTwoSeparateTrackAssemblies) {
    const auto recycler = unit_box_of(UnitKind::Recycler);
    ASSERT_EQ(recycler.size(), protekt::game::kRecyclerVertices);
    constexpr float kCentre = 100.0F * kMetresPerTile;
    const auto left
        = parts_span(recycler, kLeftTrackAssembly, kLeftTrackAssembly + kTrackAssemblyParts, 0);
    const auto right
        = parts_span(recycler, kRightTrackAssembly, kRightTrackAssembly + kTrackAssemblyParts, 0);
    EXPECT_LT(left[1], kCentre - 0.3F) << "the left assembly keeps to its side";
    EXPECT_GT(right[0], kCentre + 0.3F) << "and the right to its own: not one central slab";
    const auto along = parts_span(recycler, 0, protekt::game::kRecyclerVertices / 36U, 2);
    const float length = along[1] - along[0];
    for (const std::size_t first : {kLeftTrackAssembly, kRightTrackAssembly}) {
        // The track itself is the first of its assembly and runs most of the length.
        const auto track = parts_span(recycler, first, first + 1, 2);
        EXPECT_GT(track[1] - track[0], 0.8F * length) << "assembly " << first;
        // Its six road wheels sit under the skirt, and down at the ground.
        const float skirt_bottom = parts_span(recycler, first + 7, first + 8, 1)[0];
        for (std::size_t wheel = first + 1; wheel < first + 7; ++wheel) {
            EXPECT_LE(parts_span(recycler, wheel, wheel + 1, 1)[1], skirt_bottom + kTolerance)
                << "wheel " << wheel;
        }
    }
}

TEST(FactorySkyline, TheStackRisesWellOverTheHallRoofInsideTheFootprint) {
    const auto factory = unit_box_of(UnitKind::Factory);
    const auto hall = factory_span(factory, kHallPart, 1);
    EXPECT_NEAR(hall[1] - hall[0], 4.5F, kTolerance) << "the picker's housing height";
    const auto stack = factory_span(factory, kStackPart, 1);
    EXPECT_GT(stack[1] - hall[1], 1.0F) << "a silhouette against the sky, not a vent";
    EXPECT_LT(factory_span(factory, kStackPart, 2)[1],
              factory_span(factory, kHallPart, 2)[0] + 0.5F)
        << "at the rear";
    const auto band = factory_span(factory, kBandPart, 1);
    EXPECT_GT(band[0], hall[1]) << "the band is up in the skyline";
    EXPECT_LT(band[1], stack[1]);
    // Every part inside the old box's footprint: 0.3 tile either way of the unit (tile 100, 100).
    constexpr float kCentre = 100.0F * kMetresPerTile;
    for (const SceneVertex& v : factory) {
        EXPECT_LE(std::abs(v.position[0] - kCentre), (0.3F * kMetresPerTile) + kTolerance);
        EXPECT_LE(std::abs(v.position[2] - kCentre), (0.3F * kMetresPerTile) + kTolerance);
    }
}

TEST(FactorySkyline, TheAnnexRoofSitsWellUnderTheHallRoof) {
    const auto factory = unit_box_of(UnitKind::Factory);
    EXPECT_GT(factory_span(factory, kHallPart, 1)[1] - factory_span(factory, kAnnexPart, 1)[1],
              1.0F)
        << "the skyline steps";
    EXPECT_NEAR(factory_span(factory, kAnnexPart, 0)[0], factory_span(factory, kHallPart, 0)[1],
                kTolerance)
        << "beside the hall";
    EXPECT_LT(factory_span(factory, kAnnexPart, 2)[1], factory_span(factory, kHallPart, 2)[1])
        << "and set back from its front";
}

TEST(FactorySkyline, TheFrontFaceStepsInAroundTheDoor) {
    const auto factory = unit_box_of(UnitKind::Factory);
    const float face = factory_span(factory, kHallPart, 2)[1];
    const float ground = factory_span(factory, kHallPart, 1)[0];
    for (const std::size_t part : {kJambParts, kJambParts + 1, kLintelPart}) {
        const auto depth = factory_span(factory, part, 2);
        EXPECT_NEAR(depth[0], face, kTolerance) << "part " << part << " stands on the face";
        EXPECT_GT(depth[1] - face, 0.1F) << "part " << part << ": the door is set back";
    }
    // The opening between the jambs and under the lintel is most of the face: it is the door.
    const float opening
        = factory_span(factory, kJambParts, 0)[0] - factory_span(factory, kJambParts + 1, 0)[1];
    const auto across = factory_span(factory, kHallPart, 0);
    EXPECT_GT(opening, 0.6F * (across[1] - across[0]));
    EXPECT_NEAR(factory_span(factory, kJambParts, 1)[1], factory_span(factory, kLintelPart, 1)[0],
                kTolerance);
    EXPECT_GT(factory_span(factory, kLintelPart, 1)[0] - ground, 3.0F) << "a vehicle door";
}

TEST(TankUv, ATintStillMultipliesOnTopOfTheTexel) {
    // The colour that reaches the shader is the tint, lit; the texel multiplies it. A selected tank
    // therefore still differs from an unselected one in its vertex colour.
    const auto sim = facing_sim();
    const SceneOptions o = [&] {
        SceneOptions x = facing_options(sim);
        x.facing = SceneOptions::Facing{viewer_of(sim).id, 0.0F, 0.0F};
        return x;
    }();
    const std::array<protekt::sim::UnitId, 1> chosen{viewer_of(sim).id};
    const Scene plain = protekt::game::build_scene(sim, o);
    const Scene picked = protekt::game::build_scene(sim, o, chosen);
    ASSERT_EQ(plain.vertices.size(), picked.vertices.size());
    const std::size_t hull = plain.vertices.size() - kTankVertices + 72;
    EXPECT_NE(plain.vertices.at(hull).colour, picked.vertices.at(hull).colour);
    EXPECT_EQ(plain.vertices.at(hull).u, picked.vertices.at(hull).u);
}

// ---------------------------------------------------------------- the hover tank (E9b)

/// One hover tank of `side` at tile (100, 100), alone, and its drawn vertices in emission order:
/// the wedge (36), the four skirt quads (24), the turret (36), the barrel (36), the fin (36).
std::vector<SceneVertex> hover_tank_of(Side side, std::span<const protekt::sim::UnitId> selected) {
    const auto sim = sim_of({{side, UnitKind::HoverTank, 100, 100}});
    return tank_only(sim, sim.units().at(0), selected);
}

constexpr std::size_t kWedge = 0;
constexpr std::size_t kSkirt = 36;
constexpr std::size_t kHoverTurret = 60;
constexpr std::size_t kHoverBarrel = 96;
constexpr std::size_t kFin = 132;

bool on_rect(const SceneVertex& v, const UvRect& r) {
    return (v.u == r.u0 || v.u == r.u1) && (v.v == r.v0 || v.v == r.v1);
}

float lowest(const std::vector<SceneVertex>& v, std::size_t first, std::size_t last) {
    float y = 1e9F;
    for (std::size_t i = first; i < last; ++i) {
        y = std::min(y, v.at(i).position[1]);
    }
    return y;
}

float highest(const std::vector<SceneVertex>& v, std::size_t first, std::size_t last) {
    float y = -1e9F;
    for (std::size_t i = first; i < last; ++i) {
        y = std::max(y, v.at(i).position[1]);
    }
    return y;
}

TEST(HoverShape, TheHoverTankIsItsWedgeSkirtTurretBarrelAndFin) {
    const auto hover = hover_tank_of(Side::Player, {});
    EXPECT_EQ(hover.size(), protekt::game::kHoverTankVertices);
    EXPECT_EQ(protekt::game::kHoverTankVertices, std::size_t{168}) << "four solids and four quads";
}

TEST(HoverShape, NoPartOfItIsATrack) {
    const UvRect tracks = tile_rect(protekt::game::atlas_manifest().tile("tank_tracks"));
    for (const Side side : {Side::Player, Side::Enemy}) {
        const auto hover = hover_tank_of(side, {});
        for (const SceneVertex& v : hover) {
            ASSERT_FALSE(on_rect(v, tracks)) << "a hover tank has no track prisms";
        }
    }
}

TEST(HoverShape, TheSkirtIsFourRawBrightQuadsUnderTheHull) {
    const auto hover = hover_tank_of(Side::Player, {});
    const UvRect skirt = tile_rect(protekt::game::atlas_manifest().tile("hover_skirt"));
    ASSERT_NE(protekt::game::atlas_manifest().tile("hover_skirt"), protekt::game::kFallbackTile);
    std::size_t on_skirt = 0;
    for (std::size_t i = 0; i < hover.size(); ++i) {
        if (on_rect(hover.at(i), skirt)) {
            ++on_skirt;
            EXPECT_GE(i, kSkirt);
            EXPECT_LT(i, kHoverTurret);
            // Raw: not lit by the face it is on, so every vertex carries exactly the skirt colour.
            EXPECT_EQ(hover.at(i).colour, protekt::game::kHoverSkirtColour) << i;
        }
    }
    EXPECT_EQ(on_skirt, std::size_t{24}) << "one quad on each of the wedge's four sides";
    EXPECT_LE(highest(hover, kSkirt, kHoverTurret), lowest(hover, kWedge, kSkirt) + kTolerance)
        << "the band is under the hull";
    // The brightest thing on it: brighter than any lit face of the wedge.
    const auto sum = [](const SceneVertex& v) { return v.colour[0] + v.colour[1] + v.colour[2]; };
    for (std::size_t i = kWedge; i < kSkirt; ++i) {
        EXPECT_LT(sum(hover.at(i)), sum(hover.at(kSkirt)));
    }
}

TEST(HoverShape, ASelectionMarksTheHullAndLeavesTheSkirtRaw) {
    const auto sim = sim_of({{Side::Player, UnitKind::HoverTank, 100, 100}});
    const std::array<protekt::sim::UnitId, 1> chosen{sim.units().at(0).id};
    const auto plain = tank_only(sim, sim.units().at(0), {});
    const auto picked = tank_only(sim, sim.units().at(0), chosen);
    ASSERT_EQ(plain.size(), picked.size());
    EXPECT_NE(plain.at(kWedge).colour, picked.at(kWedge).colour);
    EXPECT_EQ(plain.at(kSkirt).colour, picked.at(kSkirt).colour);
}

TEST(HoverShape, TheWedgeIsShallowerAndPointierThanATanksHull) {
    const auto hover = hover_tank_of(Side::Player, {});
    const auto sim = sim_of({{Side::Player, UnitKind::Tank, 100, 100}});
    const auto tank = tank_only(sim, sim.units().at(0), {});
    // Heading 0 (never seen moving): local +z is world +Z, across is world X.
    const auto depth = [](const std::vector<SceneVertex>& v, std::size_t first) {
        return highest(v, first, first + 36) - lowest(v, first, first + 36);
    };
    EXPECT_LT(depth(hover, kWedge), depth(tank, 72)) << "a slab is deeper than the wedge";
    float nose_z = -1e9F;
    float tail_z = 1e9F;
    for (std::size_t i = kWedge; i < kSkirt; ++i) {
        nose_z = std::max(nose_z, hover.at(i).position[2]);
        tail_z = std::min(tail_z, hover.at(i).position[2]);
    }
    float nose_width = 0.0F;
    float tail_width = 0.0F;
    float nose_top = -1e9F;
    float tail_top = -1e9F;
    const float centre_x = static_cast<float>(sim.units().at(0).x.raw)
                           / static_cast<float>(protekt::sim::Fixed::kOne) * kMetresPerTile;
    for (std::size_t i = kWedge; i < kSkirt; ++i) {
        const auto& p = hover.at(i).position;
        const float across = std::abs(p[0] - centre_x);
        if (std::abs(p[2] - nose_z) < kTolerance) {
            nose_width = std::max(nose_width, across);
            nose_top = std::max(nose_top, p[1]);
        }
        if (std::abs(p[2] - tail_z) < kTolerance) {
            tail_width = std::max(tail_width, across);
            tail_top = std::max(tail_top, p[1]);
        }
    }
    EXPECT_LT(nose_width * 4.0F, tail_width) << "it tapers in plan to a narrow nose";
    EXPECT_LT(nose_top + 0.2F, tail_top) << "and slopes down to it in profile";
}

TEST(HoverShape, ItWearsItsTeamsCamoAndTheBarrelAndFinCarryTheSideColour) {
    const auto& atlas = protekt::game::atlas_manifest();
    for (const Side side : {Side::Player, Side::Enemy}) {
        const auto sim = sim_of({{side, UnitKind::HoverTank, 100, 100}});
        const auto hover = tank_only(sim, sim.units().at(0), {});
        const auto name = protekt::game::kTeamHullNames.at(static_cast<std::size_t>(side))
                              .at(static_cast<std::size_t>(
                                  protekt::game::hull_variant(42, sim.units().at(0).id.value)));
        const UvRect camo = tile_rect(atlas.tile(name));
        expect_corners_of(hover, kWedge, kSkirt, camo, "wedge");
        expect_corners_of(hover, kHoverTurret, kHoverBarrel, camo, "turret");
        expect_corners_of(hover, kHoverBarrel, kFin, tile_rect(atlas.tile("tank_barrel")),
                          "barrel");
        expect_corners_of(hover, kFin, hover.size(), tile_rect(atlas.tile("tank_turret")), "fin");
        // The accents are saturated toward the side's hue; the camo parts only a pale tint of it.
        const auto& barrel = hover.at(kHoverBarrel).colour;
        const auto& wedge = hover.at(kWedge).colour;
        const std::size_t hue = side == Side::Player ? 2 : 0;
        const std::size_t other = 1;
        EXPECT_GT(barrel.at(hue) - barrel.at(other), wedge.at(hue) - wedge.at(other));
    }
}

TEST(HoverShape, ATankIsStillATankAndTheFacingTurnsTheHoverTank) {
    // The tank's eight prisms are untouched by the hover tank's arrival.
    const auto sim = sim_of({{Side::Player, UnitKind::Tank, 100, 100}});
    EXPECT_EQ(tank_only(sim, sim.units().at(0), {}).size(), kTankVertices);
    // A facing on a hover tank turns the hover model, never swaps in the tank's.
    const auto hover_sim = sim_of({{Side::Player, UnitKind::HoverTank, 100, 100}});
    const protekt::sim::Unit& unit = hover_sim.units().at(0);
    SceneOptions options = at_tank(unit);
    const Scene still = protekt::game::build_scene(hover_sim, options);
    options.facing = SceneOptions::Facing{unit.id, 1.0F, 0.5F};
    const Scene turned = protekt::game::build_scene(hover_sim, options);
    ASSERT_EQ(still.vertices.size(), turned.vertices.size());
    EXPECT_NE(still.vertices.back().position, turned.vertices.back().position);
}

TEST(HoverShape, TheHealthBarSitsJustOverTheModelsTop) {
    // The bar is drawn over a point 1.2 m above the unit's ground; the hover tank's highest part
    // (the fin) must stand under it, and close enough that the bar does not float off the model.
    const auto hover = hover_tank_of(Side::Player, {});
    const auto sim = sim_of({{Side::Player, UnitKind::HoverTank, 100, 100}});
    const float ground = protekt::game::ground_metres(42, sim.units().at(0).x, sim.units().at(0).y);
    const float top = highest(hover, 0, hover.size()) - ground;
    EXPECT_LE(top, 1.2F);
    EXPECT_GE(top, 0.9F);
}

TEST(HoverShape, AHoverTankInViewGetsItsBarOverIt) {
    using protekt::sim::Spawn;
    protekt::sim::Scenario scenario;
    scenario.seed = 42;
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Player, 50, 50});
    scenario.spawns.push_back(Spawn{UnitKind::HoverTank, Side::Player, 52, 51});
    const auto sim = protekt::sim::Sim::create(scenario);
    BarBattle b = bar_battle();
    b.options.viewer_unit = sim.units().front().id;
    const Scene scene = build_scene(sim, b.options);
    const std::vector<UnitBar> bars = unit_bars_of(b.options, scene);
    ASSERT_EQ(bars.size(), 1U);
    const protekt::game::Projected top = top_of(b.options, sim.units().at(1));
    EXPECT_NEAR(bars.front().track.centre_x(), top.x, 1.0F);
    EXPECT_LT(bars.front().outline.bottom, top.y);
    EXPECT_GT(bars.front().outline.bottom, top.y - 8.0F);
}

// ---------------------------------------------------------------- the DEPLOYING plate
//
// An own turret that is not yet mounted wears a yellow plate reading DEPLOYING over its projected
// top, the E5 bars' anchor; a mounted turret, a hostile one and a tank wear nothing.

constexpr std::array<float, 3> kMountPlateRgb{1.0F, 0.85F, 0.10F};

std::vector<PixelRect> mount_plates_of(const SceneOptions& options, const Scene& scene) {
    std::vector<PixelRect> plates;
    // The plate is the only yellow quad: a run of six vertices, so each one starts a run.
    for (std::size_t first = 0; first + 6 <= scene.vertices.size();) {
        if (scene.vertices.at(first).colour == kMountPlateRgb) {
            plates.push_back(rect_of(options, scene, first));
            first += 6;
        } else {
            ++first;
        }
    }
    return plates;
}

/// The bar battle plus a turret of yours at (54, 53) and a hostile one at (46, 53), both mounted.
BarBattle turret_battle() {
    using protekt::sim::Side;
    using protekt::sim::Spawn;
    using protekt::sim::UnitKind;
    protekt::sim::Scenario scenario;
    scenario.seed = 42;
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Player, 50, 50});
    scenario.spawns.push_back(Spawn{UnitKind::Turret, Side::Player, 54, 53});
    scenario.spawns.push_back(Spawn{UnitKind::Turret, Side::Enemy, 46, 53});
    scenario.spawns.push_back(Spawn{UnitKind::HoverTank, Side::Player, 50, 53});
    BarBattle b = bar_battle();
    b.sim = protekt::sim::Sim::create(scenario);
    b.viewer = b.sim.units().front().id;
    b.options.viewer_unit = b.viewer;
    return b;
}

void unmount(BarBattle& b, std::size_t index, std::int32_t to_tile_x) {
    const protekt::sim::UnitId id = b.sim.units().at(index).id;
    b.sim.enqueue(protekt::sim::Order{
        id, protekt::sim::orders::MoveTo{protekt::sim::Fixed::from_int(to_tile_x),
                                         b.sim.units().at(index).y}});
    for (int t = 0; t < 5; ++t) {
        b.sim.step();
    }
}

TEST(MountPlate, AMountedTurretWearsNothing) {
    const BarBattle b = turret_battle();
    ASSERT_TRUE(protekt::sim::is_mounted(b.sim.units().at(1)));
    EXPECT_TRUE(mount_plates_of(b.options, build_scene(b.sim, b.options)).empty());
}

TEST(MountPlate, AnUnmountedOwnTurretWearsOne) {
    BarBattle b = turret_battle();
    unmount(b, 1, 56);
    ASSERT_FALSE(protekt::sim::is_mounted(b.sim.units().at(1)));
    const std::vector<PixelRect> plates = mount_plates_of(b.options, build_scene(b.sim, b.options));
    ASSERT_EQ(plates.size(), 1U);
    EXPECT_GE(plates.front().height(), 20.0F) << "not tiny";
    EXPECT_GE(plates.front().width(), 150.0F);
}

TEST(MountPlate, AHostileTurretNeverWearsOne) {
    BarBattle b = turret_battle();
    unmount(b, 2, 44);
    ASSERT_FALSE(protekt::sim::is_mounted(b.sim.units().at(2)));
    EXPECT_TRUE(mount_plates_of(b.options, build_scene(b.sim, b.options)).empty());
}

TEST(MountPlate, ATankOrHoverTankNeverWearsOne) {
    BarBattle b = turret_battle();
    unmount(b, 3, 52); // the hover tank moves
    EXPECT_TRUE(mount_plates_of(b.options, build_scene(b.sim, b.options)).empty());
}

TEST(MountPlate, ThePlateFollowsTheTurretLikeABar) {
    BarBattle b = turret_battle();
    unmount(b, 1, 58);
    const auto check = [&]() {
        const std::vector<PixelRect> plates
            = mount_plates_of(b.options, build_scene(b.sim, b.options));
        ASSERT_EQ(plates.size(), 1U);
        const protekt::game::Projected top = top_of(b.options, b.sim.units().at(1));
        EXPECT_NEAR(plates.front().centre_x(), top.x, 1.0F);
        EXPECT_LT(plates.front().bottom, top.y);
        EXPECT_GT(plates.front().bottom, top.y - 12.0F);
    };
    check();
    for (int t = 0; t < 10; ++t) {
        b.sim.step();
    }
    check(); // it has moved on, and the plate with it
}

TEST(MountPlate, TheRemountedTurretLosesItsPlate) {
    BarBattle b = turret_battle();
    unmount(b, 1, 55);
    for (int t = 0; t < 400 && !protekt::sim::is_mounted(b.sim.units().at(1)); ++t) {
        b.sim.step();
    }
    ASSERT_TRUE(protekt::sim::is_mounted(b.sim.units().at(1)));
    EXPECT_TRUE(mount_plates_of(b.options, build_scene(b.sim, b.options)).empty());
}

TEST(MountPlate, TwoSameSceneBuildsAreByteIdentical) {
    BarBattle b = turret_battle();
    unmount(b, 1, 56);
    const Scene one = build_scene(b.sim, b.options);
    const Scene two = build_scene(b.sim, b.options);
    ASSERT_EQ(one.vertices.size(), two.vertices.size());
    for (std::size_t i = 0; i < one.vertices.size(); ++i) {
        EXPECT_EQ(one.vertices.at(i).position, two.vertices.at(i).position);
        EXPECT_EQ(one.vertices.at(i).colour, two.vertices.at(i).colour);
    }
}

} // namespace

// D5a-2: guns point where they shoot. Every gun that is not the driven one is aimed from the sim's
// own persistent target data (`game::gun_yaw`); with none it lies along its heading.

namespace {

using protekt::game::gun_yaw;
using protekt::game::kTurretVertices;

/// The world yaw from one unit to another, in the `Headings` convention.
float bearing(const protekt::sim::Unit& from, const protekt::sim::Unit& to) {
    return std::atan2(static_cast<float>(to.x.raw - from.x.raw),
                      static_cast<float>(to.y.raw - from.y.raw));
}

/// The yaw from a unit's centre to the mean of vertices [first, last) on the map plane. A barrel is
/// symmetric about its own axis, so for a barrel this is the way it points.
float yaw_of_part(const std::vector<SceneVertex>& v, std::size_t first, std::size_t last,
                  const protekt::sim::Unit& unit) {
    float x = 0.0F;
    float z = 0.0F;
    for (std::size_t i = first; i < last; ++i) {
        x += v.at(i).position[0];
        z += v.at(i).position[2];
    }
    const auto n = static_cast<float>(last - first);
    return std::atan2((x / n) - (static_cast<float>(unit.x.raw) / 65536.0F * kMetresPerTile),
                      (z / n) - (static_cast<float>(unit.y.raw) / 65536.0F * kMetresPerTile));
}

/// The barrel is the last prism of every gun model.
float barrel_yaw(const std::vector<SceneVertex>& model, const protekt::sim::Unit& unit) {
    return yaw_of_part(model, model.size() - kPrismVertices, model.size(), unit);
}

/// Steps until some gun has fired, so `aim_target` names what it engaged.
void step_until_shot(protekt::sim::Sim& sim) {
    for (int step = 0; step < 200 && sim.shots().empty(); ++step) {
        sim.step();
    }
    ASSERT_FALSE(sim.shots().empty()) << "no gun fired within 200 ticks";
}

/// The farthest any of vertices [first, last) lies from the unit's centre, and the nearest.
std::array<float, 2> reach_of(const std::vector<SceneVertex>& v, std::size_t first,
                              std::size_t last, const protekt::sim::Unit& unit) {
    const float ux = static_cast<float>(unit.x.raw) / 65536.0F * kMetresPerTile;
    const float uz = static_cast<float>(unit.y.raw) / 65536.0F * kMetresPerTile;
    float near = 1e9F;
    float far = 0.0F;
    for (std::size_t i = first; i < last; ++i) {
        const float d = std::hypot(v.at(i).position[0] - ux, v.at(i).position[2] - uz);
        near = std::min(near, d);
        far = std::max(far, d);
    }
    return {near, far};
}

constexpr float kYawTolerance = 1e-3F;

TEST(GunAim, WithNoTargetTheGunLiesAlongTheHeading) {
    const auto sim = sim_of({{Side::Player, UnitKind::Tank, 100, 100}});
    EXPECT_FLOAT_EQ(gun_yaw(sim, sim.units().at(0), 0.7F), 0.7F);
    EXPECT_FLOAT_EQ(gun_yaw(sim, sim.units().at(0), -2.1F), -2.1F);
}

TEST(GunAim, AnAttackTargetIsAimedAtEvenOutOfRange) {
    // Eighteen tiles off, past a tank's twelve: nothing is engaged, so only the order names it.
    auto sim = sim_of(
        {{Side::Player, UnitKind::Tank, 100, 100}, {Side::Enemy, UnitKind::Factory, 115, 110}});
    sim.enqueue(protekt::sim::Order{sim.units().at(0).id,
                                    protekt::sim::orders::Attack{sim.units().at(1).id}});
    sim.step();
    const auto& tank = sim.units().at(0);
    ASSERT_FALSE(tank.aim_target.valid()) << "the order alone must name the target";
    EXPECT_NEAR(gun_yaw(sim, tank, 3.0F), bearing(tank, sim.units().at(1)), kYawTolerance);
}

TEST(GunAim, TheTriggersFocusOutranksTheAttackTarget) {
    auto sim = sim_of({{Side::Player, UnitKind::Tank, 100, 100},
                       {Side::Enemy, UnitKind::Factory, 115, 110},
                       {Side::Enemy, UnitKind::Factory, 85, 115}});
    const auto tank_id = sim.units().at(0).id;
    sim.enqueue(protekt::sim::Order{tank_id, protekt::sim::orders::Attack{sim.units().at(1).id}});
    sim.enqueue(
        protekt::sim::Order{tank_id, protekt::sim::orders::FocusFire{sim.units().at(2).id}});
    sim.step();
    const auto& tank = sim.units().at(0);
    EXPECT_NEAR(gun_yaw(sim, tank, 0.0F), bearing(tank, sim.units().at(2)), kYawTolerance);
}

TEST(GunAim, AnAutoEngagedGunAimsAtWhatItLastEngaged) {
    // No order at all: the turret picks the factory itself, and the sim remembers it as
    // `aim_target` between shots.
    auto sim = sim_of(
        {{Side::Enemy, UnitKind::Turret, 100, 100}, {Side::Player, UnitKind::Factory, 106, 103}});
    step_until_shot(sim);
    sim.step(); // reloading: no shot this tick, the gun still holds its target
    const auto& turret = sim.units().at(0);
    EXPECT_NEAR(gun_yaw(sim, turret, 1.0F), bearing(turret, sim.units().at(1)), kYawTolerance);
}

TEST(SceneGunAim, AnAiTanksBarrelPointsAtWhatItShoots) {
    // A stopped tank faces +Y; its target is due +X. Before D5a-2 the barrel lay along the hull.
    auto sim = one_player_bolt_sim();
    step_until_shot(sim);
    const auto& tank = sim.units().at(0);
    const auto model = tank_only(sim, tank, {});
    ASSERT_EQ(model.size(), kTankVertices);
    EXPECT_NEAR(barrel_yaw(model, tank), bearing(tank, sim.units().at(1)), kYawTolerance);
    EXPECT_NEAR(barrel_yaw(model, tank), protekt::math::kPi * 0.5F, kYawTolerance);
}

TEST(SceneGunAim, AnAiHoverTanksBarrelPointsAtWhatItShoots) {
    auto sim = sim_of(
        {{Side::Player, UnitKind::HoverTank, 100, 100}, {Side::Enemy, UnitKind::Factory, 94, 100}});
    step_until_shot(sim);
    const auto& hover = sim.units().at(0);
    const auto model = tank_only(sim, hover, {});
    ASSERT_EQ(model.size(), protekt::game::kHoverTankVertices);
    // The barrel is the fourth solid, before the fin.
    EXPECT_NEAR(yaw_of_part(model, kHoverBarrel, kFin, hover), bearing(hover, sim.units().at(1)),
                kYawTolerance);
}

TEST(SceneGunAim, AnIdleTanksGunLiesAlongItsHull) {
    auto sim = sim_of({{Side::Player, UnitKind::Tank, 100, 100}});
    sim.enqueue(protekt::sim::Order{
        sim.units().at(0).id, protekt::sim::orders::MoveTo{protekt::sim::Fixed::from_int(110),
                                                           protekt::sim::Fixed::from_int(100)}});
    sim.step();
    const auto& tank = sim.units().at(0);
    EXPECT_NEAR(barrel_yaw(tank_only(sim, tank, {}), tank), protekt::math::kPi * 0.5F,
                kYawTolerance)
        << "moving east with nothing to shoot: the gun looks the way the hull goes";
}

TEST(SceneGunAim, ADefenseTurretsBarrelPointsAtWhatItShoots) {
    auto sim = sim_of(
        {{Side::Enemy, UnitKind::Turret, 100, 100}, {Side::Player, UnitKind::Factory, 106, 103}});
    step_until_shot(sim);
    const auto& turret = sim.units().at(0);
    const auto model = tank_only(sim, turret, {});
    ASSERT_EQ(model.size(), kTurretVertices);
    EXPECT_NEAR(barrel_yaw(model, turret), bearing(turret, sim.units().at(1)), kYawTolerance);
    // The housing turns with the barrel; the pedestal keeps the turret's own heading (+Y).
    EXPECT_NEAR(yaw_of_part(model, 72, 144, turret), bearing(turret, sim.units().at(1)), 0.05F);
}

TEST(SceneGunAim, AnIdleTurretLooksForwardAndAMovingOneAlongItsCourse) {
    auto sim = sim_of({{Side::Player, UnitKind::Turret, 100, 100}});
    EXPECT_NEAR(barrel_yaw(tank_only(sim, sim.units().at(0), {}), sim.units().at(0)), 0.0F,
                kYawTolerance)
        << "never moved, never fired: forward, +Y";
    sim.enqueue(protekt::sim::Order{
        sim.units().at(0).id, protekt::sim::orders::MoveTo{protekt::sim::Fixed::from_int(90),
                                                           protekt::sim::Fixed::from_int(100)}});
    for (int t = 0; t <= protekt::sim::kUnmountTicks; ++t) {
        sim.step(); // unmount, then one tick of crawl
    }
    const auto& turret = sim.units().at(0);
    const auto model = tank_only(sim, turret, {});
    EXPECT_NEAR(barrel_yaw(model, turret), -protekt::math::kPi * 0.5F, kYawTolerance)
        << "crawling west";
    // The pedestal turns to the course too. It is square, so read one face: the back one.
    const float corner_yaw = yaw_of_part(model, 0, 6, turret);
    EXPECT_NEAR(corner_yaw, protekt::math::kPi * 0.5F, kYawTolerance)
        << "the back face is behind, to the east";
}

TEST(SceneGunAim, TheDrivenTankKeepsThePlayersTurret) {
    // The viewer's tank has engaged the factory (due +X), but the player's turret wins.
    auto sim = facing_sim();
    step_until_shot(sim);
    ASSERT_TRUE(viewer_of(sim).aim_target.valid());
    // The bolt in the air follows the units, so the viewer's tank ends one box before the end.
    const Scene scene = facing_scene(sim, 0.3F, 0.4F);
    const auto bolts = static_cast<std::ptrdiff_t>(sim.projectiles().size() * kBoxVertices);
    const std::vector<SceneVertex> tank{scene.vertices.end() - bolts
                                            - static_cast<std::ptrdiff_t>(kTankVertices),
                                        scene.vertices.end() - bolts};
    EXPECT_NEAR(barrel_yaw(tank, viewer_of(sim)), 0.3F + 0.4F, kYawTolerance);
}

// Re-pinned DELIBERATELY (D5a-2): a defense turret was one 36-vertex box; it is four prisms now.
TEST(TurretModel, ADefenseTurretIsAPedestalAHousingAMuzzleCollarAndABarrel) {
    const auto sim = sim_of({{Side::Player, UnitKind::Turret, 100, 100}});
    const auto& turret = sim.units().at(0);
    const auto model = unit_box_of(UnitKind::Turret);
    ASSERT_EQ(model.size(), kTurretVertices);
    EXPECT_EQ(kTurretVertices, std::size_t{144}) << "four prisms";
    constexpr std::size_t kPedestal = 0;
    constexpr std::size_t kHousing = 36;
    constexpr std::size_t kCollar = 72;
    constexpr std::size_t kBarrel = 108;
    // The housing sits on the pedestal.
    EXPECT_NEAR(lowest(model, kHousing, kCollar), highest(model, kPedestal, kHousing), kTolerance);
    // The barrel runs from the housing's front face; the collar is AT THE TIP, past the barrel's
    // end, and thicker than it.
    const auto housing = reach_of(model, kHousing, kCollar, turret);
    const auto collar = reach_of(model, kCollar, kBarrel, turret);
    const auto barrel = reach_of(model, kBarrel, model.size(), turret);
    EXPECT_LT(barrel[0], housing[1]) << "the barrel starts inside the housing's reach";
    EXPECT_GT(collar[1], barrel[1]) << "the collar is the far end";
    EXPECT_GT(collar[0], barrel[1] - 0.2F) << "the collar starts where the barrel ends";
    EXPECT_GT(highest(model, kCollar, kBarrel) - lowest(model, kCollar, kBarrel),
              highest(model, kBarrel, model.size()) - lowest(model, kBarrel, model.size()))
        << "the collar is thicker than the barrel";
    EXPECT_GT(collar[1], kHullHalfMetres * 2.0F) << "a barrel you can read, past the pedestal";
}

// ---------------------------------------------------------------- the mortar bike and the rocket
// tank (R3b2)
//
// Both are built of the fleet's shaded prisms and wear the team's camo with side-colour accents; no
// new atlas tile. The mortar bike is a low wide box, a mount and a short stubby tube angled up with
// a collar at its mouth (it lobs). The rocket tank is the tank's hull -- the same seven prisms,
// byte for byte -- with a mount and a rectangular launcher rack where the turret and barrel would
// be.

constexpr std::size_t kMortarBase = 0;
constexpr std::size_t kMortarMount = 36;
constexpr std::size_t kMortarTube = 72;
constexpr std::size_t kMortarCollar = 108;
constexpr std::size_t kRocketTankHull = std::size_t{7} * 36U; ///< the tank's tracks .. fenders
constexpr std::size_t kRocketTankRack = kRocketTankHull + 36U;

TEST(MortarBikeModel, TheMortarBikeIsALowWideBoxAMountAStubbyTubeAndACollar) {
    const auto model = unit_box_of(UnitKind::MortarBike);
    ASSERT_EQ(model.size(), protekt::game::kMortarBikeVertices);
    EXPECT_EQ(protekt::game::kMortarBikeVertices, std::size_t{144}) << "four prisms";
    const auto sim = sim_of({{Side::Player, UnitKind::MortarBike, 100, 100}});
    const protekt::sim::Unit& unit = sim.units().at(0);
    const float ground = protekt::game::ground_metres(42, unit.x, unit.y);
    // Low and wide: the base is under half a metre tall and as wide as a tank.
    EXPECT_LT(highest(model, kMortarBase, kMortarMount) - ground, 0.5F);
    const auto base = reach_of(model, kMortarBase, kMortarMount, unit);
    EXPECT_GT(base[1], kHullHalfMetres) << "as wide as a tank";
    // The tube is angled UP: its mouth (the collar) stands well over its breech, and it is short.
    EXPECT_GT(highest(model, kMortarCollar, model.size()),
              lowest(model, kMortarTube, kMortarCollar) + 0.4F);
    const auto tube = reach_of(model, kMortarTube, model.size(), unit);
    EXPECT_LT(tube[1], 1.2F) << "stubby: nothing like the tank's 2 m barrel";
    // And it stays under the health bar's 1.2 m anchor.
    EXPECT_LE(highest(model, 0, model.size()) - ground, 1.2F);
    EXPECT_GE(highest(model, 0, model.size()) - ground, 0.9F);
}

TEST(MortarBikeModel, TheTubeAndCollarCarryTheSideColourAndTheSidesDiffer) {
    const auto mine = unit_box_of(UnitKind::MortarBike);
    const auto theirs = [] {
        const auto sim = sim_of({{Side::Enemy, UnitKind::MortarBike, 100, 100}});
        return tank_only(sim, sim.units().at(0), {});
    }();
    ASSERT_EQ(mine.size(), theirs.size());
    EXPECT_NE(mine.at(kMortarTube).colour, theirs.at(kMortarTube).colour);
    EXPECT_NE(mine.at(kMortarBase).colour, theirs.at(kMortarBase).colour);
    // The base is the pale camo tint; the tube is the saturated accent, so they differ.
    EXPECT_NE(mine.at(kMortarBase).colour, mine.at(kMortarTube).colour);
}

TEST(RocketTankModel, TheRocketTankIsATanksHullWithAMountAndALauncherRack) {
    const auto rocket_tank = unit_box_of(UnitKind::RocketTank);
    ASSERT_EQ(rocket_tank.size(), protekt::game::kRocketTankVertices);
    EXPECT_EQ(protekt::game::kRocketTankVertices, std::size_t{324}) << "nine prisms";
    // The hull is the tank's, vertex for vertex: the same id at the same tile wears the same camo.
    const auto tank = unit_box_of(UnitKind::Tank);
    for (std::size_t i = 0; i < kRocketTankHull; ++i) {
        ASSERT_EQ(rocket_tank.at(i).position, tank.at(i).position) << "vertex " << i;
        ASSERT_EQ(rocket_tank.at(i).colour, tank.at(i).colour) << "vertex " << i;
        ASSERT_EQ(rocket_tank.at(i).u, tank.at(i).u) << "vertex " << i;
    }
    // No barrel: nothing on it addresses the barrel tile.
    const UvRect barrel = tile_rect(protekt::game::atlas_manifest().tile("tank_barrel"));
    for (const SceneVertex& v : rocket_tank) {
        ASSERT_FALSE(on_rect(v, barrel)) << "a rocket tank carries a rack, not a gun";
    }
    // The rack sits on the mount, is a broad slab rather than a barrel, and stays inside the hull's
    // length rather than reaching past the nose as a barrel does.
    const auto sim = sim_of({{Side::Player, UnitKind::RocketTank, 100, 100}});
    const protekt::sim::Unit& unit = sim.units().at(0);
    EXPECT_GE(lowest(rocket_tank, kRocketTankRack, rocket_tank.size()) + kTolerance,
              lowest(rocket_tank, kRocketTankHull, kRocketTankRack));
    const auto rack = reach_of(rocket_tank, kRocketTankRack, rocket_tank.size(), unit);
    EXPECT_LT(rack[1], 1.3F) << "inside the 1.3 m track, no barrel past the nose";
    const float ground = protekt::game::ground_metres(42, unit.x, unit.y);
    EXPECT_LE(highest(rocket_tank, 0, rocket_tank.size()) - ground, 1.2F)
        << "under the bar's anchor";
    EXPECT_NE(rocket_tank.at(kRocketTankRack).colour, rocket_tank.at(kRocketTankHull - 36U).colour)
        << "the rack is the side-colour accent, the fender the pale camo";
}

TEST(MortarBikeModel, AMortarBikeAndARocketTankInViewEachGetABarLikeATank) {
    using protekt::sim::Spawn;
    protekt::sim::Scenario scenario;
    scenario.seed = 42;
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Player, 50, 50});
    scenario.spawns.push_back(Spawn{UnitKind::MortarBike, Side::Player, 54, 51});
    scenario.spawns.push_back(Spawn{UnitKind::RocketTank, Side::Enemy, 46, 51});
    const auto sim = protekt::sim::Sim::create(scenario);
    BarBattle b = bar_battle();
    b.options.viewer_unit = sim.units().front().id;
    const Scene scene = build_scene(sim, b.options);
    const std::vector<UnitBar> bars = unit_bars_of(b.options, scene);
    ASSERT_EQ(bars.size(), 2U);
    EXPECT_EQ(bars.at(0).fill_colour, kOwnBarRgb);
    EXPECT_EQ(bars.at(1).fill_colour, kHostileBarRgb);
}

TEST(MortarBikeModel, TwoBuildsOfASkirmishWithBothInViewAreByteIdentical) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    const protekt::sim::Unit& mortar_bike = sim.units().at(7);
    ASSERT_EQ(mortar_bike.kind, UnitKind::MortarBike);
    const SceneOptions options = at_tank(mortar_bike);
    const Scene a = build_scene(sim, options);
    const Scene b = build_scene(sim, options);
    ASSERT_EQ(a.vertices.size(), b.vertices.size());
    EXPECT_EQ(
        std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(SceneVertex)),
        0);
}

// THE ATTACK LIST (BZ2A-2): the panel the player chooses from. The logic that fills it is tested
// in test_targets.cpp; these tests pin the drawing: asked for or not, deterministic, and a pure
// read of the sim.
namespace {

/// Two rows, near enough to name real units in the skirmish. The panel only reads the numbers and
/// the kind, so the ids do not have to be the ones nearest the camera.
std::vector<protekt::game::TargetRow> two_rows(const protekt::sim::Sim& sim) {
    return {{1, sim.units().at(0).kind, sim.units().at(0).id, 12},
            {2, sim.units().at(1).kind, sim.units().at(1).id, 40}};
}

/// The player's command row over `sim`, as the app would hold it: the strip the panels sit under.
protekt::game::RowState row_of(const protekt::sim::Sim& sim) {
    protekt::game::RowState row;
    row.update(sim, protekt::sim::Side::Player, 0);
    return row;
}

} // namespace

TEST(SceneAttackList, TheListIsDrawnOnlyWhenAskedAndNeverTouchesTheSim) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    SceneOptions options = options_at(60.0F, 60.0F);
    protekt::game::RowState row = row_of(sim);
    options.row = &row;
    const std::uint64_t hash = sim.state_hash();
    const Scene without = build_scene(sim, options);

    const std::vector<protekt::game::TargetRow> rows = two_rows(sim);
    options.attack_list = &rows;
    const Scene with = build_scene(sim, options);
    EXPECT_GT(with.vertices.size(), without.vertices.size()) << "the rows must reach the frame";
    EXPECT_EQ(sim.state_hash(), hash) << "drawing the list orders nothing: it is a view";

    // The field's default draws nothing at all, so a frame that never heard of it is unchanged.
    SceneOptions plain = options_at(60.0F, 60.0F);
    plain.row = &row;
    const Scene untouched = build_scene(sim, plain);
    ASSERT_EQ(untouched.vertices.size(), without.vertices.size());
    EXPECT_EQ(std::memcmp(untouched.vertices.data(), without.vertices.data(),
                          without.vertices.size() * sizeof(SceneVertex)),
              0);
}

TEST(SceneAttackList, AnEmptyListDrawsNothing) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    SceneOptions options = options_at(60.0F, 60.0F);
    protekt::game::RowState row = row_of(sim);
    options.row = &row;
    const Scene without = build_scene(sim, options);
    const std::vector<protekt::game::TargetRow> none;
    options.attack_list = &none;
    const Scene empty = build_scene(sim, options);
    ASSERT_EQ(empty.vertices.size(), without.vertices.size());
    EXPECT_EQ(std::memcmp(empty.vertices.data(), without.vertices.data(),
                          without.vertices.size() * sizeof(SceneVertex)),
              0);
}

TEST(SceneAttackList, TwoSameSeedBuildsWithAListAreByteIdentical) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    SceneOptions options = options_at(60.0F, 60.0F);
    protekt::game::RowState row = row_of(sim);
    options.row = &row;
    const std::vector<protekt::game::TargetRow> rows = two_rows(sim);
    options.attack_list = &rows;
    const Scene a = build_scene(sim, options);
    const Scene b = build_scene(sim, options);
    ASSERT_EQ(a.vertices.size(), b.vertices.size());
    EXPECT_EQ(
        std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(SceneVertex)),
        0);
}

TEST(SceneAttackList, EveryRowIsDrawnSoMoreRowsMakeMoreVertices) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    SceneOptions options = options_at(60.0F, 60.0F);
    protekt::game::RowState row = row_of(sim);
    options.row = &row;
    std::vector<protekt::game::TargetRow> rows = two_rows(sim);
    options.attack_list = &rows;
    const Scene two = build_scene(sim, options);
    // A third row at a distance with more digits than either: the count and the width both grow.
    rows.push_back({3, sim.units().at(2).kind, sim.units().at(2).id, 1234});
    const Scene three = build_scene(sim, options);
    ASSERT_EQ(rows.size(), 3U);
    EXPECT_GT(three.vertices.size(), two.vertices.size()) << "each row must emit its own glyphs";
}

TEST(SceneAttackList, ThePanelSitsBelowTheRosterStrip) {
    const auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    SceneOptions options = options_at(60.0F, 60.0F);
    protekt::game::RowState row = row_of(sim);
    options.row = &row;
    const Scene without = build_scene(sim, options);
    const std::vector<protekt::game::TargetRow> rows = two_rows(sim);
    options.attack_list = &rows;
    const Scene with = build_scene(sim, options);

    // The panel's vertices are the tail the list added: the strip and its notice are drawn before
    // it and are identical in both scenes, so the suffix is exactly the list.
    ASSERT_GT(with.vertices.size(), without.vertices.size());
    const float scale = static_cast<float>(options.extent.height) / 720.0F;
    // The strip's bottom edge: its 6 px top margin and its 34 px plate, in the HUD's own units.
    constexpr float kStripBottomPixels = 40.0F;
    float highest = std::numeric_limits<float>::max();
    for (std::size_t index = without.vertices.size(); index < with.vertices.size(); ++index) {
        const SceneVertex& vertex = with.vertices.at(index);
        const protekt::game::Projected p
            = project(options.camera, options.extent, vertex.position[0], vertex.position[1],
                      vertex.position[2]);
        ASSERT_GT(p.depth, 0.0F) << "a HUD vertex must be in front of the camera";
        highest = std::min(highest, p.y);
    }
    EXPECT_GE(highest, kStripBottomPixels * scale)
        << "the list must start at or below the strip's bottom edge, never over it";
}

// ---------------------------------------------------------------- the ship instrument
// (SHIPGAUGE-1)
//
// BZ2's bottom-right gauge: a segmented C-ring open to the right (the hull's arc green above, the
// magazine's blue below), the two readings in the ring's opening, and the four carried weapons
// beside it with the one he has up on a green bar. It is a PURE VIEW -- the sim is read and never
// written -- and these tests count the vertices it puts on the frame and the bytes a weapon change
// moves.
//
// Colours and geometry are literals on purpose, as everywhere else in this file: a test that
// compared against the constant the drawing used would pass whatever the constant was.

namespace {

using protekt::game::Projected;
using protekt::game::SceneVertex;

constexpr std::array<float, 3> kGaugeHullLit{0.30F, 0.95F, 0.40F};
constexpr std::array<float, 3> kGaugeHullDim{0.12F, 0.32F, 0.14F};
constexpr std::array<float, 3> kGaugeAmmoLit{0.30F, 0.55F, 1.00F};
constexpr std::array<float, 3> kGaugeAmmoDim{0.12F, 0.20F, 0.35F};
constexpr std::array<float, 3> kGaugeActiveBar{0.16F, 0.42F, 0.18F};
// The three inks `health_colour` gives the hull number (green/amber/red, named at the top of this
// file where the radar's own colours live): it is the reading's ink and nothing else in the ring's
// opening, so counting one of them counts the number.

/// The ring, in the same fractions of the frame's height the drawing uses, and the font's cell.
constexpr float kGaugeCentreFromRight = 0.300F;
constexpr float kGaugeCentreFromBottom = 0.100F;
constexpr float kGaugeOuter = 0.060F;
constexpr float kGaugeThickness = 0.016F;
constexpr float kGaugeCell = 3.0F / 720.0F;
constexpr float kGaugeRowPitch = 8.0F * kGaugeCell;
constexpr float kGaugeEps = 0.5F; ///< a pixel of slack, for the projection's own rounding

/// Vertices of one colour whose pixel distance from the ring's centre is in [low, high].
std::size_t at_radius(const SceneOptions& options, const Scene& scene,
                      const std::array<float, 3>& rgb, float low, float high) {
    const float height = static_cast<float>(options.extent.height);
    const float cx = static_cast<float>(options.extent.width) - (kGaugeCentreFromRight * height);
    const float cy = height - (kGaugeCentreFromBottom * height);
    std::size_t count = 0;
    for (const SceneVertex& vertex : scene.vertices) {
        if (vertex.colour != rgb) {
            continue;
        }
        const Projected p = project(options.camera, options.extent, vertex.position[0],
                                    vertex.position[1], vertex.position[2]);
        const float dx = p.x - cx;
        const float dy = p.y - cy;
        const float distance = std::sqrt((dx * dx) + (dy * dy));
        if (distance >= low && distance <= high) {
            ++count;
        }
    }
    return count;
}

/// A ring's OUTER radius, in pixels: where three of a segment's six vertices stand (a segment is a
/// quad; its other three are at the inner radius, and nothing else in the instrument reaches this
/// far out except the weapon rows, which are well past it). So this counts segments, whole ones.
float outer_radius(const SceneOptions& options) {
    return kGaugeOuter * static_cast<float>(options.extent.height);
}

/// The segments of one colour: three vertices each, at the outer radius.
std::size_t arc_segments(const SceneOptions& options, const Scene& scene,
                         const std::array<float, 3>& rgb) {
    const float outer = outer_radius(options);
    return at_radius(options, scene, rgb, outer - kGaugeEps, outer + kGaugeEps);
}

/// Everything INSIDE the ring's opening: where the two readings are.
std::size_t inside_ring(const SceneOptions& options, const Scene& scene,
                        const std::array<float, 3>& rgb) {
    const float height = static_cast<float>(options.extent.height);
    const float inner = (kGaugeOuter - kGaugeThickness) * height;
    return at_radius(options, scene, rgb, 0.0F, inner - kGaugeEps);
}

/// The active row's bar as a pixel rectangle {left, right, top, bottom}. False unless the frame
/// holds EXACTLY one bar -- a bar is one quad of six vertices -- so a highlight that appeared on
/// every row (or on two, or on none) fails here instead of being averaged away.
bool active_bar_rect(const SceneOptions& options, const Scene& scene, std::array<float, 4>& out) {
    std::size_t first = 0;
    std::size_t runs = 0;
    for (std::size_t index = 0; index + 6U <= scene.vertices.size(); ++index) {
        if (scene.vertices.at(index).colour != kGaugeActiveBar) {
            continue;
        }
        bool whole_quad = true;
        for (std::size_t i = 1; i < 6U; ++i) {
            whole_quad = whole_quad && scene.vertices.at(index + i).colour == kGaugeActiveBar;
        }
        if (!whole_quad) {
            continue;
        }
        ++runs;
        first = index;
        index += 5U;
    }
    if (runs != 1U) {
        return false;
    }
    out = std::array<float, 4>{1e9F, -1e9F, 1e9F, -1e9F};
    for (std::size_t i = 0; i < 6U; ++i) {
        const SceneVertex& vertex = scene.vertices.at(first + i);
        const Projected p = project(options.camera, options.extent, vertex.position[0],
                                    vertex.position[1], vertex.position[2]);
        out = std::array<float, 4>{std::min(out.at(0), p.x), std::max(out.at(1), p.x),
                                   std::min(out.at(2), p.y), std::max(out.at(3), p.y)};
    }
    return true;
}

/// The ring's centre line, in pixels: what the rows and the readings are hung off.
std::array<float, 2> gauge_centre(const SceneOptions& options) {
    const float height = static_cast<float>(options.extent.height);
    return std::array<float, 2>{static_cast<float>(options.extent.width)
                                    - (kGaugeCentreFromRight * height),
                                height - (kGaugeCentreFromBottom * height)};
}

/// `test_world_map.cpp`'s byte compare: same vertices, same order, the same bytes.
[[nodiscard]] bool same_bytes(const Scene& a, const Scene& b) {
    return a.vertices.size() == b.vertices.size()
           && (a.vertices.empty()
               || std::memcmp(a.vertices.data(), b.vertices.data(),
                              a.vertices.size() * sizeof(SceneVertex))
                      == 0);
}

/// The shared rig builds a WORLD scene with the instrument off; a ShipGauge test asks for it.
SceneOptions gauge_options(const Damaged& d, protekt::game::Weapon weapon) {
    SceneOptions options = watching(d);
    options.show_ship_gauge = true;
    options.weapon = weapon;
    return options;
}

} // namespace

TEST(ShipGauge, AlwaysDrawsTheRingAndFourRows) {
    // It is a HUD layer, not a reading that appears when something happens: the ring and the four
    // rows are on the frame whatever the sim says. So the count is the DIFFERENCE against a frame
    // with the instrument switched off, which is what that option is for.
    const Damaged d = factory_down_to(400); // full health: the whole upper arc is lit
    SceneOptions shown = gauge_options(d, protekt::game::Weapon::Gun);
    SceneOptions hidden = shown;
    hidden.show_ship_gauge = false;
    const Scene with = build_scene(d.sim, shown);
    const Scene without = build_scene(d.sim, hidden);
    ASSERT_GT(with.vertices.size(), without.vertices.size());

    // The two arcs: twelve segments a half and twenty-four round the ring, drawn as trapezoids in
    // every case, lit or dim -- so 24 segments of three counted vertices each, whatever the
    // readings are.
    EXPECT_EQ(arc_segments(shown, with, kGaugeHullLit) + arc_segments(shown, with, kGaugeHullDim)
                  + arc_segments(shown, with, kGaugeAmmoLit)
                  + arc_segments(shown, with, kGaugeAmmoDim),
              24U * 3U);
    // A full hull over a full magazine: every segment lit, none dim.
    EXPECT_EQ(arc_segments(shown, with, kGaugeHullLit), 12U * 3U);
    EXPECT_EQ(arc_segments(shown, with, kGaugeHullDim), 0U);
    EXPECT_EQ(arc_segments(shown, with, kGaugeAmmoLit), 12U * 3U);
    EXPECT_EQ(arc_segments(shown, with, kGaugeAmmoDim), 0U);
    EXPECT_EQ(arc_segments(shown, without, kGaugeHullLit)
                  + arc_segments(shown, without, kGaugeHullDim),
              0U)
        << "the arcs are what the option turns off";
    // And the four rows are drawn on top of the ring, not instead of it.
    EXPECT_GT(with.vertices.size() - without.vertices.size(), 24U * 6U);

    // AND IT IS NOT THE RADAR'S LAYER. R turns the radar into the world map; the instrument stands
    // in the other corner on a layer of its own, so it is still whole with the map up.
    SceneOptions mapped = shown;
    mapped.show_world_map = true;
    const Scene over_map = build_scene(d.sim, mapped);
    EXPECT_EQ(arc_segments(mapped, over_map, kGaugeHullLit), 12U * 3U)
        << "the hull arc survives the world map";
    EXPECT_EQ(arc_segments(mapped, over_map, kGaugeAmmoLit), 12U * 3U)
        << "and so does the magazine arc";

    // Determinism: the same sim and the same options give the same frame, byte for byte, so the
    // instrument's own size is the same number both times.
    const Scene again = build_scene(d.sim, shown);
    EXPECT_TRUE(same_bytes(with, again));
    EXPECT_EQ(again.vertices.size() - build_scene(d.sim, hidden).vertices.size(),
              with.vertices.size() - without.vertices.size());
}

TEST(ShipGauge, TheActiveWeaponMovesTheHighlightAndNothingElse) {
    // The five rows, in `kDrivenWeapons` order: gun, rocket, mortar, bouncer, shotgun.
    const std::array<protekt::game::Weapon, 5> kRows{
        protekt::game::Weapon::Gun, protekt::game::Weapon::Rocket, protekt::game::Weapon::Mortar,
        protekt::game::Weapon::Bouncer, protekt::game::Weapon::Shotgun};
    const Damaged d = factory_down_to(400);
    const std::array<float, 2> centre = gauge_centre(gauge_options(d, kRows.at(0)));
    std::vector<Scene> frames;
    for (std::size_t row = 0; row < kRows.size(); ++row) {
        const SceneOptions options = gauge_options(d, kRows.at(row));
        const Scene scene = build_scene(d.sim, options);
        frames.push_back(scene);
        const float scale = static_cast<float>(options.extent.height);
        // ONE bar of six vertices, on the row of the weapon that is up: row i of five is pitched
        // (i - 2.0) rows from the ring's centre line, and the bar is six cells of the font tall.
        std::array<float, 4> bar{};
        ASSERT_TRUE(active_bar_rect(options, scene, bar))
            << "the frame must hold exactly one highlight bar, row " << row;
        const float want
            = centre.at(1)
              + ((static_cast<float>(row) - (static_cast<float>(kRows.size()) - 1.0F) * 0.5F)
                 * kGaugeRowPitch * scale);
        EXPECT_NEAR((bar.at(2) + bar.at(3)) * 0.5F, want, 0.5F) << "row " << row;
        EXPECT_NEAR(bar.at(3) - bar.at(2), 6.0F * kGaugeCell * scale, 0.5F)
            << "the bar is six cells tall, row " << row;
        // And the rest of the instrument does not move: the same 24 segments, the same 12 lit a
        // half, whatever weapon is up.
        EXPECT_EQ(arc_segments(options, scene, kGaugeHullLit), 12U * 3U) << "row " << row;
        EXPECT_EQ(arc_segments(options, scene, kGaugeAmmoLit), 12U * 3U) << "row " << row;
    }
    // The frames really do differ: each weapon puts its own ink and its own icon disc in the same
    // places, which a count alone cannot see.
    EXPECT_FALSE(same_bytes(frames.at(0), frames.at(1)));
    EXPECT_FALSE(same_bytes(frames.at(1), frames.at(2)));
    EXPECT_FALSE(same_bytes(frames.at(2), frames.at(3)));
    EXPECT_FALSE(same_bytes(frames.at(3), frames.at(4)));
    EXPECT_FALSE(same_bytes(frames.at(0), frames.at(2)));
}

TEST(ShipGauge, AnInvalidViewerLeavesTheHullArcFull) {
    const Damaged hurt = factory_down_to(160); // two fifths of the factory's 400 hit points
    ASSERT_EQ(hurt.sim.find(hurt.viewer)->hp, 160);
    const SceneOptions watched_options = gauge_options(hurt, protekt::game::Weapon::Gun);
    const Scene watched = build_scene(hurt.sim, watched_options);
    // Two fifths of twelve segments rounds to five lit and seven dim: WHOLE segments, no sliver
    // drawn for the sixth. The hull number stands inside the ring, in the amber two fifths is.
    EXPECT_EQ(arc_segments(watched_options, watched, kGaugeHullLit), 5U * 3U);
    EXPECT_EQ(arc_segments(watched_options, watched, kGaugeHullDim), 7U * 3U);
    EXPECT_GT(inside_ring(watched_options, watched, kAmberRgb), 0U)
        << "the hull number is drawn for a viewer that exists";

    SceneOptions nobody = watched_options;
    nobody.viewer_unit = protekt::sim::UnitId{9999};
    const Scene blind = build_scene(hurt.sim, nobody);
    // With no driven unit there is no hull to report at all, so every upper segment is lit rather
    // than the arc reading empty -- an empty arc would say "dead" about a tank that is not there --
    // and no hull number is invented for it either.
    EXPECT_EQ(arc_segments(nobody, blind, kGaugeHullLit), 12U * 3U);
    EXPECT_EQ(arc_segments(nobody, blind, kGaugeHullDim), 0U);
    EXPECT_EQ(inside_ring(nobody, blind, kAmberRgb), 0U);
    EXPECT_FALSE(same_bytes(watched, blind));
}

TEST(ShipGauge, TheHullNumberChangesInkAtTheHealthThresholds) {
    // The radar's hull bar used to carry this test; the bar is retired and the instrument is what
    // states the hull, so the thresholds are pinned where the hull is stated now -- the NUMBER's
    // ink inside the ring, which is `health_colour` and nothing else there. The sim's damage is
    // exact, so each side of every threshold is reached exactly: a 400 hp factory is at 0.50 at 200
    // hp and 0.20 at 80 hp, and a hit is 8 hp (0.02).
    struct Case {
        std::uint16_t hp;
        std::array<float, 3> ink;
    };
    const std::array<Case, 6> cases{{{208, kGreenRgb}, // 0.52
                                     {200, kGreenRgb}, // 0.50 is the first green
                                     {192, kAmberRgb}, // 0.48
                                     {88, kAmberRgb},  // 0.22
                                     {80, kAmberRgb},  // 0.20 is the last amber
                                     {72, kRedRgb}}};  // 0.18
    const std::array<std::array<float, 3>, 3> inks{kGreenRgb, kAmberRgb, kRedRgb};
    for (const Case& c : cases) {
        const Damaged d = factory_down_to(c.hp);
        ASSERT_EQ(d.sim.find(d.viewer)->hp, c.hp);
        const SceneOptions options = gauge_options(d, protekt::game::Weapon::Gun);
        const Scene scene = build_scene(d.sim, options);
        EXPECT_GT(inside_ring(options, scene, c.ink), 0U)
            << "no hull number in its own ink at hp " << c.hp;
        for (const auto& other : inks) {
            if (other != c.ink) {
                EXPECT_EQ(inside_ring(options, scene, other), 0U)
                    << "a second threshold's ink at hp " << c.hp;
            }
        }
    }
}

TEST(ShipGauge, TheAmmoReadingIsThePlaceholder) {
    // AMMO-2 replaces both halves of this: a fraction the sim owns and a number it produces. Today
    // the sim has no magazine, so the constant is exactly 1 and the string is exactly "100".
    static_assert(protekt::game::kAmmoPlaceholderFraction == 1.0F,
                  "the placeholder is a full magazine, and changing it is AMMO-2's slice");
    static_assert(protekt::game::ammo_placeholder_percent() == "100");
    EXPECT_EQ(protekt::game::ammo_placeholder_percent(), "100");
    // Because the fraction is 1 the ring's lower half is full: twelve lit segments, none dim.
    const Damaged d = factory_down_to(400);
    const SceneOptions options = gauge_options(d, protekt::game::Weapon::Gun);
    const Scene scene = build_scene(d.sim, options);
    EXPECT_EQ(arc_segments(options, scene, kGaugeAmmoLit), 12U * 3U);
    EXPECT_EQ(arc_segments(options, scene, kGaugeAmmoDim), 0U);
    // And "100" is drawn in that same blue inside the ring, so the reading is the number and not
    // only the arc.
    EXPECT_GT(inside_ring(options, scene, kGaugeAmmoLit), 0U);
}

// ---- the terrain memo (the world's per-tile answers for one seed, remembered) -------------------
//
// WHY IT EXISTS: world::sample runs three octaves of value noise per call (measured 0.77 us) and
// the terrain mesh asks it five times a tile, every frame. game::TerrainCache remembers those
// answers, so the two properties worth pinning are that it can never disagree with the world, and
// that a frame drawn through it is the same frame to the byte.

/// The memo's contract: an entry IS the world's own answer, tile for tile, inside the map and out.
TEST(TerrainCache, MatchesTheWorldTileForTile) {
    constexpr std::uint64_t kSeed = 42;
    protekt::game::TerrainCache cache{kSeed};
    ASSERT_EQ(cache.seed(), kSeed);
    ASSERT_EQ(cache.tiles_derived(), std::size_t{0}) << "a fresh cache has derived nothing";
    // The map's corners and edges, two interior tiles, and the fringe the mesh samples one tile
    // past its far edge: off the map the field is still defined, and it is still the same field.
    const std::array<std::pair<int, int>, 10> kTiles{{{0, 0},
                                                      {1, 0},
                                                      {0, 1},
                                                      {255, 255},
                                                      {254, 255},
                                                      {255, 254},
                                                      {128, 64},
                                                      {-1, 0},
                                                      {256, 256},
                                                      {-1, 256}}};
    for (const auto& [tile_x, tile_y] : kTiles) {
        const protekt::world::Tile tile = protekt::world::sample(kSeed, tile_x, tile_y);
        EXPECT_EQ(cache.height_units(tile_x, tile_y), tile.height)
            << "height at " << tile_x << "," << tile_y;
        EXPECT_EQ(cache.terrain(tile_x, tile_y), tile.terrain)
            << "terrain at " << tile_x << "," << tile_y;
    }
    // The seven in-map tiles are remembered and the three off-map ones are not: a table that
    // claimed to hold the off-map field would be claiming a table it does not have.
    EXPECT_EQ(cache.tiles_derived(), std::size_t{7});
    // Asking again derives nothing. This is the whole point of the type: the work is what changes,
    // never the answer.
    for (const auto& [tile_x, tile_y] : kTiles) {
        (void)cache.height_units(tile_x, tile_y);
        (void)cache.terrain(tile_x, tile_y);
    }
    EXPECT_EQ(cache.tiles_derived(), std::size_t{7});
}

/// A frame drawn through the memo is the SAME FRAME, byte for byte -- the terrain vertices carry
/// replicated heights, so a memo off by one height unit would show up as bytes.
TEST(TerrainCache, ChangesNothingAboutAFrame) {
    const protekt::sim::Sim sim = protekt::sim::Sim::create(protekt::sim::skirmish(42));
    // One camera well inside the map and one near the west edge, where the mesh walks off it.
    for (const float camera_x : {100.0F, 6.0F}) {
        protekt::game::TerrainCache cache{42};
        SceneOptions plain = options_at(camera_x, 104.0F);
        SceneOptions cached = plain;
        cached.terrain = &cache;
        EXPECT_TRUE(same_bytes(protekt::game::build_scene(sim, plain),
                               protekt::game::build_scene(sim, cached)))
            << "the memo must not change a byte at x=" << camera_x;
        // The same again with the world map open, which draws the whole map from the same world.
        plain.show_world_map = true;
        cached.show_world_map = true;
        EXPECT_TRUE(same_bytes(protekt::game::build_scene(sim, plain),
                               protekt::game::build_scene(sim, cached)))
            << "the memo must not change a byte with the map open at x=" << camera_x;
        // And the memo really was read: every quad asks for its four corners, so a radius-8 mesh --
        // (2*8+2)^2 quads spanning 2*8+2 tiles -- asks about a (2*8+3)^2 = 19x19 window of tiles.
        // The tiles of that window which fall off the map are answered but not remembered, so the
        // camera near the west edge fills in fewer.
        const int centre = static_cast<int>(std::floor(camera_x / kMetresPerTile));
        const int low = std::max(0, centre - 9);
        const int high = std::min(255, centre + 9);
        EXPECT_EQ(cache.tiles_derived(), static_cast<std::size_t>(high - low + 1) * 19U)
            << "the frame should have filled the memo at x=" << camera_x;
    }
    // A cache built for ANOTHER seed is ignored rather than trusted: the seed is not this frame's,
    // so the frame must still come out right.
    protekt::game::TerrainCache other{7};
    const SceneOptions plain = options_at(100.0F, 104.0F);
    SceneOptions stale = plain;
    stale.terrain = &other;
    EXPECT_TRUE(
        same_bytes(protekt::game::build_scene(sim, plain), protekt::game::build_scene(sim, stale)))
        << "a cache for another seed must be ignored, not believed";
}

// ---- the instrument's placement, measured rather than eyeballed ---------------------------------
//
// The plan's own acceptance for this slice: the instrument stays inside the frame and clear of the
// radar's panel rectangle, and four rows fit at the smallest window the game is run at. Both are
// geometry, so both are asserted from the frame's own fractions and from vertices really emitted.

/// The ring's box in the frame, in pixels. The ring is the instrument's outermost part on the left,
/// the top and the bottom; the rows reach further right, and the row test measures that edge.
struct GaugeBox {
    float left{};
    float right{};
    float top{};
    float bottom{};
};

GaugeBox gauge_box(const SceneOptions& options) {
    const std::array<float, 2> centre = gauge_centre(options);
    const float outer = outer_radius(options);
    return GaugeBox{centre.at(0) - outer, centre.at(0) + outer, centre.at(1) - outer,
                    centre.at(1) + outer};
}

/// The radar's panel rectangle widened by its own border: everything the radar draws is inside it.
GaugeBox radar_box(const SceneOptions& options) {
    const protekt::game::RadarPanel panel = protekt::game::radar_panel(options.extent);
    const float border = protekt::game::kRadarBorderPixels;
    return GaugeBox{panel.x - border, panel.x + panel.width + border, panel.y - border,
                    panel.y + panel.height + border};
}

TEST(ShipGauge, TheInstrumentStaysInsideTheFrameAndClearOfTheRadarPanel) {
    const Damaged d = factory_down_to(400);
    // The smallest window the game is run at, the render CLI's own default, and a 1080p frame: the
    // instrument is a fraction of the frame's HEIGHT, so all three have to hold.
    for (const protekt::game::Extent extent :
         {protekt::game::Extent{640, 400}, protekt::game::Extent{1280, 720},
          protekt::game::Extent{1920, 1080}}) {
        SceneOptions options = gauge_options(d, protekt::game::Weapon::Bouncer);
        options.extent = extent;
        const Scene scene = build_scene(d.sim, options);
        const GaugeBox box = gauge_box(options);
        const float width = static_cast<float>(extent.width);
        const float height = static_cast<float>(extent.height);
        EXPECT_GE(box.left, 0.0F) << "the ring's left edge is off the frame at " << width << "x"
                                  << height;
        EXPECT_GE(box.top, 0.0F) << "the ring's top is off the frame at " << width << "x" << height;
        EXPECT_LE(box.right, width);
        EXPECT_LE(box.bottom, height);

        // Clear of the radar, which is bottom-left: the two must never share a column.
        const GaugeBox radar = radar_box(options);
        EXPECT_GT(box.left, radar.right)
            << "the instrument overlaps the radar's panel at " << width << "x" << height;

        // And the arcs really do land inside the frame: measured from the emitted vertices, not
        // from the fractions that placed them.
        for (const std::array<float, 3>& rgb :
             {kGaugeHullLit, kGaugeHullDim, kGaugeAmmoLit, kGaugeAmmoDim}) {
            for (const SceneVertex& vertex : scene.vertices) {
                if (vertex.colour != rgb) {
                    continue;
                }
                const Projected p = project(options.camera, options.extent, vertex.position[0],
                                            vertex.position[1], vertex.position[2]);
                EXPECT_GE(p.x, 0.0F);
                EXPECT_GE(p.y, 0.0F);
                EXPECT_LE(p.x, width);
                EXPECT_LE(p.y, height);
            }
        }
    }
}

TEST(ShipGauge, FourRowsFitAtTheSmallestWindow) {
    const Damaged d = factory_down_to(400);
    const protekt::game::Extent extent{640, 400}; // the smallest window the game is run at
    const float height = static_cast<float>(extent.height);
    const float cell = kGaugeCell * height;
    const float pitch = kGaugeRowPitch * height;
    // The window comes FIRST: the instrument is placed from the frame's height, so a centre
    // computed from the default 1280x720 rig would measure a different frame than the one being
    // built.
    SceneOptions small = gauge_options(d, protekt::game::Weapon::Gun);
    small.extent = extent;
    const std::array<float, 2> centre = gauge_centre(small);

    // The rows are one pitch apart and the glyphs are five cells tall, so the gap between two
    // neighbouring rows is three cells and they cannot touch at any window, because both scale with
    // the frame's height. What a small window CAN break is the frame's own edges.
    EXPECT_GT(pitch, 5.0F * cell) << "the rows would overlap";
    for (int row = 0; row < 4; ++row) {
        const float row_y = centre.at(1) + ((static_cast<float>(row) - 1.5F) * pitch);
        EXPECT_GE(row_y - (2.5F * cell), 0.0F) << "row " << row << " is off the top";
        EXPECT_LE(row_y + (2.5F * cell), height) << "row " << row << " is off the bottom";
    }

    // The bar spans the icon and the name, so with the LONGEST name active it measures the furthest
    // right the instrument ever reaches. That it lands inside the frame is the "four rows fit"
    // claim, decided by the name that would break it first.
    float widest_right = 0.0F;
    for (const protekt::game::Weapon weapon :
         {protekt::game::Weapon::Gun, protekt::game::Weapon::Rocket, protekt::game::Weapon::Mortar,
          protekt::game::Weapon::Bouncer}) {
        SceneOptions options = gauge_options(d, weapon);
        options.extent = extent;
        const Scene scene = build_scene(d.sim, options);
        std::array<float, 4> bar{};
        ASSERT_TRUE(active_bar_rect(options, scene, bar))
            << "exactly one row is highlighted whatever the weapon is";
        EXPECT_GE(bar.at(0), 0.0F);
        EXPECT_LE(bar.at(1), static_cast<float>(extent.width)) << "a row runs off the right edge";
        EXPECT_GE(bar.at(2), 0.0F);
        EXPECT_LE(bar.at(3), height);
        widest_right = std::max(widest_right, bar.at(1));
    }
    // BOUNCER is the longest name in `kDrivenWeapons`, so its bar is the rightmost thing the
    // instrument draws: this pins that the measured edge really was the rows and not the ring.
    EXPECT_GT(widest_right,
              centre.at(0) + outer_radius(gauge_options(d, protekt::game::Weapon::Gun)))
        << "the longest name should reach past the ring, or this test is measuring the ring";
}

/// C5 -- THE WRECKAGE IS DRAWN. C1 has scattered a dead unit's hull on the ground since the economy
/// landed and nothing ever drew it, so the litter was invisible to the player. The layer is pinned
/// three ways below: one quad per piece and no more, each plate lying ON THE GROUND at the piece's
/// own tile, and NOTHING AT ALL in the wreckage's colour while the field is clean -- which is the
/// property that lets every other frame in this suite (and every frame of a battle that has not yet
/// lost a unit) stay byte-identical to what it drew before this layer existed.

/// The verdict's rig: one player gun against four enemy tanks, close enough that the fight starts
/// at once, and one recycler per side parked well away from it. The war is won and lost on the
/// recyclers (OBJ-a), so the player's gun can die here and decide NOTHING: that is the retirement
/// of C8's attrition rule, as a battle. Tests build this same battle and must agree about it, so
/// the seed is named.
constexpr std::uint64_t kOutcomeSeed = 20261023;

protekt::sim::Scenario an_outcome_battle() {
    protekt::sim::Scenario scenario;
    scenario.seed = kOutcomeSeed;
    scenario.spawns.push_back(
        protekt::sim::Spawn{protekt::sim::UnitKind::Tank, protekt::sim::Side::Player, 60, 50});
    scenario.spawns.push_back(
        protekt::sim::Spawn{protekt::sim::UnitKind::Tank, protekt::sim::Side::Enemy, 62, 50});
    scenario.spawns.push_back(
        protekt::sim::Spawn{protekt::sim::UnitKind::Tank, protekt::sim::Side::Enemy, 62, 51});
    scenario.spawns.push_back(
        protekt::sim::Spawn{protekt::sim::UnitKind::Tank, protekt::sim::Side::Enemy, 61, 51});
    scenario.spawns.push_back(
        protekt::sim::Spawn{protekt::sim::UnitKind::Tank, protekt::sim::Side::Enemy, 61, 50});
    scenario.spawns.push_back(
        protekt::sim::Spawn{protekt::sim::UnitKind::Recycler, protekt::sim::Side::Player, 20, 20});
    scenario.spawns.push_back(
        protekt::sim::Spawn{protekt::sim::UnitKind::Recycler, protekt::sim::Side::Enemy, 120, 120});
    return scenario;
}

/// The living armed units a side has: the count the retired attrition rule looked at.
int armed_alive_in(const protekt::sim::Sim& sim, protekt::sim::Side side) {
    int n = 0;
    for (const protekt::sim::Unit& unit : sim.units()) {
        if (unit.alive && unit.side == side && protekt::sim::spec_of(unit.kind).range.raw > 0) {
            ++n;
        }
    }
    return n;
}

/// A player gun beside the enemy's recycler, with the enemy's one tank far across the map, played
/// until the recycler is dead: the moment the player has won, with an armed enemy still alive.
protekt::sim::Sim a_battle_where_the_enemy_recycler_just_died() {
    protekt::sim::Scenario scenario;
    scenario.seed = kOutcomeSeed;
    scenario.spawns.push_back(
        protekt::sim::Spawn{protekt::sim::UnitKind::Tank, protekt::sim::Side::Player, 60, 50});
    scenario.spawns.push_back(
        protekt::sim::Spawn{protekt::sim::UnitKind::Recycler, protekt::sim::Side::Player, 20, 20});
    scenario.spawns.push_back(
        protekt::sim::Spawn{protekt::sim::UnitKind::Recycler, protekt::sim::Side::Enemy, 62, 50});
    scenario.spawns.push_back(
        protekt::sim::Spawn{protekt::sim::UnitKind::Tank, protekt::sim::Side::Enemy, 120, 120});
    protekt::sim::Sim sim = protekt::sim::Sim::create(scenario);
    const protekt::sim::UnitId target = sim.units().at(2).id;
    sim.enqueue(protekt::sim::Order{sim.units().front().id, protekt::sim::orders::Attack{target}});
    for (int i = 0; i < 3000 && sim.find(target)->alive; ++i) {
        sim.step();
    }
    return sim;
}

/// A fight that ENDS: three tanks of one side against a lone one on open ground, so one side loses
/// a unit and the other does not. The same rig `test_scrap.cpp` uses to get litter on the field.
protekt::sim::Sim a_battle_that_leaves_litter() {
    protekt::sim::Scenario scenario;
    scenario.seed = 20261023;
    scenario.spawns.push_back(
        protekt::sim::Spawn{protekt::sim::UnitKind::Tank, protekt::sim::Side::Player, 57, 41});
    scenario.spawns.push_back(
        protekt::sim::Spawn{protekt::sim::UnitKind::Tank, protekt::sim::Side::Player, 58, 41});
    scenario.spawns.push_back(
        protekt::sim::Spawn{protekt::sim::UnitKind::Tank, protekt::sim::Side::Player, 59, 41});
    scenario.spawns.push_back(
        protekt::sim::Spawn{protekt::sim::UnitKind::Tank, protekt::sim::Side::Enemy, 57, 43});
    protekt::sim::Sim sim = protekt::sim::Sim::create(scenario);
    for (int i = 0; i < 900 && sim.scraps().empty(); ++i) {
        sim.step();
    }
    return sim;
}

/// How many vertices a frame draws in the wreckage's own colour. A colour only this layer emits is
/// how a layer's absence is proved: a vertex count or "the bytes differ" would not say which.
std::size_t wreckage_vertices(const Scene& scene) {
    std::size_t count = 0;
    for (const auto& vertex : scene.vertices) {
        if (std::abs(vertex.colour.at(0) - protekt::game::kScrapColour[0]) < kTolerance
            && std::abs(vertex.colour.at(1) - protekt::game::kScrapColour[1]) < kTolerance
            && std::abs(vertex.colour.at(2) - protekt::game::kScrapColour[2]) < kTolerance) {
            ++count;
        }
    }
    return count;
}

/// C6 -- THE LEDGER ON THE HUD. C3 banks a count of the loads a hauler delivers and C4 spends it;
/// nothing showed it before this layer, so the currency the factory builds on was invisible. The
/// test pins that the reading is drawn from the first frame, that the layer being off means not one
/// vertex of it, that it sits above the bottom-left panel, and -- the assertion that matters --
/// that it is the SIM'S NUMBER and not a constant: two battles with different ledgers draw
/// different readings. Colours here are literals on purpose, as in the radar tests: comparing
/// against the constant the code draws with would pass whatever that constant was.

/// A battle that can bank a load: a factory, a hauler and three tanks of one side, three enemy
/// tanks forward. The enemy wrecks land near the front and the hauler banks them. The same rig
/// `test_scavenger.cpp` uses to get ore on the ledger.
// The player's tanks stand off each other's lines of fire and the factory's: a bolt hits a friendly
// in its way (friendly fire), so the old file along row 45 shot itself and the factory.
protekt::sim::Sim bankable_battle() {
    using protekt::sim::Side;
    using protekt::sim::Spawn;
    using protekt::sim::UnitKind;
    protekt::sim::Scenario scenario;
    scenario.seed = 20261023;
    scenario.spawns = {Spawn{UnitKind::Factory, Side::Player, 52, 45},
                       Spawn{UnitKind::Scavenger, Side::Player, 48, 47},
                       Spawn{UnitKind::Turret, Side::Player, 58, 45},
                       Spawn{UnitKind::Tank, Side::Player, 50, 46},
                       Spawn{UnitKind::Tank, Side::Player, 51, 47},
                       Spawn{UnitKind::Tank, Side::Player, 52, 49},
                       Spawn{UnitKind::Tank, Side::Enemy, 60, 45},
                       Spawn{UnitKind::Tank, Side::Enemy, 60, 46},
                       Spawn{UnitKind::Tank, Side::Enemy, 61, 46}};
    return protekt::sim::Sim::create(scenario);
}

/// How many vertices a frame draws in the ledger's ink. The reading's amber is unlike anything else
/// in a frame with no radar, so counting it counts exactly the layer.
std::size_t ledger_ink_vertices(const Scene& scene) {
    std::size_t count = 0;
    for (const auto& vertex : scene.vertices) {
        if (std::abs(vertex.colour.at(0) - 1.0F) < kTolerance
            && std::abs(vertex.colour.at(1) - 0.85F) < kTolerance
            && std::abs(vertex.colour.at(2) - 0.30F) < kTolerance) {
            ++count;
        }
    }
    return count;
}

/// The world rig with the ledger turned ON: `options_at` turns it off because the tests around it
/// read the vertex list's tail, and this test is the one that wants the reading.
SceneOptions ledger_options() {
    SceneOptions options = options_at(60.0F, 60.0F, 42, 8);
    options.show_ledger = true;
    return options;
}

// DEPLOY: the armed deploy's footprint, drawn lying on the ground as its 2x2 tile grid in green.
TEST(Scene, TheDeployPreviewDrawsTheFootprintsTileGridOnTheGroundAndNothingWhenAbsent) {
    using protekt::sim::Footprint;
    using protekt::sim::kBuildingFootprintTiles;
    const auto sim = protekt::sim::Sim::create(protekt::sim::Scenario{
        42, {{protekt::sim::UnitKind::Tank, protekt::sim::Side::Player, 60, 60}}});
    SceneOptions plain = options_at(60.0F * kMetresPerTile, 60.0F * kMetresPerTile);
    SceneOptions previewed = plain;
    const Footprint site{63, 58};
    previewed.deploy_preview = protekt::game::DeployPreview{site, true};
    const Scene without = protekt::game::build_scene(sim, plain);
    const Scene with = protekt::game::build_scene(sim, previewed);
    ASSERT_EQ(with.vertices.size(),
              without.vertices.size() + protekt::game::kDeployPreviewVertexCount)
        << "three lines each way, two tile edges long: twelve strokes";
    // Everything after the nav beacons is HUD the rig turns off, so the preview is the tail.
    const std::size_t first = without.vertices.size();
    const float stroke = protekt::game::kDeployPreviewHalfStrokeMetres;
    for (std::size_t i = first; i < with.vertices.size(); ++i) {
        const auto& v = with.vertices.at(i);
        EXPECT_EQ(v.colour,
                  protekt::game::haze::seen(previewed, v, protekt::game::kDeployPreviewColour))
            << i;
        EXPECT_GE(v.position[0], static_cast<float>(site.x) * kMetresPerTile - stroke - kTolerance);
        EXPECT_LE(v.position[0],
                  static_cast<float>(site.x + kBuildingFootprintTiles) * kMetresPerTile + stroke
                      + kTolerance);
        EXPECT_GE(v.position[2], static_cast<float>(site.y) * kMetresPerTile - stroke - kTolerance);
        EXPECT_LE(v.position[2],
                  static_cast<float>(site.y + kBuildingFootprintTiles) * kMetresPerTile + stroke
                      + kTolerance);
        const float tile_x = v.position[0] / kMetresPerTile;
        const float tile_y = v.position[2] / kMetresPerTile;
        EXPECT_GT(v.position[1], protekt::game::ground_metres(42, tile_x, tile_y) - 1.0F)
            << "it lies on the ground, not in the sky";
    }
    for (std::size_t i = 0; i < first; ++i) {
        ASSERT_EQ(with.vertices.at(i).position, without.vertices.at(i).position)
            << "the preview adds a layer and changes nothing under it";
    }
}

// DEPLOY: the verdict is the SIM's, carried on the preview and never re-derived in the drawing
// layer -- the same ground and the same footprint, one frame green and one red.
TEST(Scene, TheDeployPreviewIsRedWhereTheDeployWouldBeRefusedAndGreenWhereItWouldNot) {
    using protekt::sim::Footprint;
    const auto sim = protekt::sim::Sim::create(protekt::sim::Scenario{
        42, {{protekt::sim::UnitKind::Tank, protekt::sim::Side::Player, 60, 60}}});
    const SceneOptions options = options_at(60.0F * kMetresPerTile, 60.0F * kMetresPerTile);
    const Footprint site{63, 58};
    const Scene plain = protekt::game::build_scene(sim, options);
    const std::size_t first = plain.vertices.size();

    SceneOptions accepted = options;
    accepted.deploy_preview = protekt::game::DeployPreview{site, true};
    SceneOptions refused = options;
    refused.deploy_preview = protekt::game::DeployPreview{site, false};
    const Scene green = protekt::game::build_scene(sim, accepted);
    const Scene red = protekt::game::build_scene(sim, refused);

    ASSERT_EQ(green.vertices.size(), first + protekt::game::kDeployPreviewVertexCount);
    ASSERT_EQ(red.vertices.size(), first + protekt::game::kDeployPreviewVertexCount);
    for (std::size_t i = first; i < green.vertices.size(); ++i) {
        EXPECT_EQ(green.vertices.at(i).colour,
                  protekt::game::haze::seen(accepted, green.vertices.at(i),
                                            protekt::game::kDeployPreviewColour))
            << i;
        EXPECT_EQ(red.vertices.at(i).colour,
                  protekt::game::haze::seen(refused, red.vertices.at(i),
                                            protekt::game::kDeployPreviewBlockedColour))
            << "the refused site must be red at vertex " << i;
        EXPECT_EQ(green.vertices.at(i).position, red.vertices.at(i).position)
            << "one footprint on one patch of ground: only the verdict differs, at vertex " << i;
    }
}

} // namespace

TEST(Scene, TheLedgerIsDrawnAndItIsTheSimsOwnNumber) {
    const SceneOptions options = ledger_options();

    const protekt::sim::Sim empty = bankable_battle();
    ASSERT_EQ(empty.ore(protekt::sim::Side::Player), 0) << "the rig starts with an empty ledger";
    const Scene first = build_scene(empty, options);
    const std::size_t when_empty = ledger_ink_vertices(first);
    EXPECT_GT(when_empty, 0U)
        << "the ledger is drawn from the first frame, so the player can see the currency exists";

    SceneOptions hidden = options;
    hidden.show_ledger = false;
    EXPECT_EQ(ledger_ink_vertices(build_scene(empty, hidden)), 0U)
        << "with the layer off, not one vertex of the reading";

    // Every stroke of the reading is the same height and sits ABOVE the bottom-left panel, clear of
    // the radar and the map that share that corner.
    const protekt::game::RadarPanel panel = protekt::game::radar_panel(options.extent);
    for (const auto& vertex : first.vertices) {
        if (std::abs(vertex.colour.at(0) - 1.0F) >= kTolerance
            || std::abs(vertex.colour.at(1) - 0.85F) >= kTolerance) {
            continue;
        }
        const protekt::game::Projected p
            = protekt::game::project(options.camera, options.extent, vertex.position[0],
                                     vertex.position[1], vertex.position[2]);
        EXPECT_LT(p.y, panel.y) << "the ledger belongs above the panel, not over it";
        EXPECT_GT(p.y, 0.0F);
    }

    // THE ASSERTION THAT MATTERS: the reading is the sim's number, not a fixed string. A battle
    // whose hauler has banked a load draws a different reading from one that has banked nothing.
    protekt::sim::Sim banked = bankable_battle();
    for (int i = 0; i < 4000 && banked.ore(protekt::sim::Side::Player) == 0; ++i) {
        banked.step();
    }
    ASSERT_GT(banked.ore(protekt::sim::Side::Player), 0)
        << "the rig must bank a load, or this proves nothing about the reading being live";
    const std::size_t when_banked = ledger_ink_vertices(build_scene(banked, options));
    EXPECT_NE(when_banked, when_empty)
        << "the reading must come from Sim::ore(), not from a constant";
}

TEST(Scene, TheWreckageIsDrawnWhereThePiecesLie) {
    const protekt::sim::Sim sim = a_battle_that_leaves_litter();
    ASSERT_FALSE(sim.scraps().empty()) << "the rig must leave litter, or this test proves nothing";

    // Look from just beside the first piece, close enough that the whole field of litter is inside
    // the meshed radius (so no piece is culled and the count below is exact).
    const auto tile_of = [](const protekt::sim::Scrap& piece, float metres) {
        return static_cast<float>(piece.x.raw) / static_cast<float>(protekt::sim::Fixed::kOne)
               * metres;
    };
    const protekt::sim::Scrap& first = sim.scraps().front();
    SceneOptions options
        = options_at(tile_of(first, kMetresPerTile),
                     static_cast<float>(first.y.raw) / static_cast<float>(protekt::sim::Fixed::kOne)
                         * kMetresPerTile,
                     sim.seed(), 40);

    const Scene with = build_scene(sim, options);
    SceneOptions hidden = options;
    hidden.show_scrap = false;
    const Scene without = build_scene(sim, hidden);

    const std::size_t expected = protekt::game::kScrapVertexCount * sim.scraps().size();
    EXPECT_EQ(with.vertices.size() - without.vertices.size(), expected)
        << "one quad per piece of wreckage, and no other vertex moved";
    EXPECT_EQ(wreckage_vertices(with), expected) << "counted by the layer's own colour";
    EXPECT_EQ(wreckage_vertices(without), 0U) << "with the layer off, not one vertex of it";

    // Every plate lies ON THE GROUND at a piece's own tile: within one plate's half-width of a
    // piece's centre horizontally, and at exactly that tile's terrain height plus the lift -- so a
    // plate can never float over the field or bury itself in the ground.
    for (const auto& vertex : with.vertices) {
        const bool is_wreckage
            = std::abs(vertex.colour.at(0) - protekt::game::kScrapColour[0]) < kTolerance
              && std::abs(vertex.colour.at(1) - protekt::game::kScrapColour[1]) < kTolerance
              && std::abs(vertex.colour.at(2) - protekt::game::kScrapColour[2]) < kTolerance;
        if (!is_wreckage) {
            continue;
        }
        bool at_a_piece = false;
        for (const protekt::sim::Scrap& piece : sim.scraps()) {
            const float px = tile_of(piece, kMetresPerTile);
            const float pz = static_cast<float>(piece.y.raw)
                             / static_cast<float>(protekt::sim::Fixed::kOne) * kMetresPerTile;
            const float ground = protekt::game::ground_metres(sim.seed(), px / kMetresPerTile,
                                                              pz / kMetresPerTile);
            const bool inside
                = std::abs(vertex.position[0] - px) <= protekt::game::kScrapHalfMetres + kTolerance
                  && std::abs(vertex.position[2] - pz)
                         <= protekt::game::kScrapHalfMetres + kTolerance;
            if (inside
                && std::abs(vertex.position[1] - (ground + protekt::game::kScrapLiftMetres))
                       < kTolerance) {
                at_a_piece = true;
                break;
            }
        }
        EXPECT_TRUE(at_a_piece) << "a wreckage plate drawn off the ground, or off its piece";
    }
}

TEST(Scene, TheVerdictIsDrawnOnlyWhenARecyclerHasDied) {
    // The verdict, on the screen. Claim one protects every other test in this file: while no
    // recycler has died the layer adds NOT ONE VERTEX, so no rig that reads the vertex tail can be
    // disturbed by it. Claim two is the RETIREMENT of C8's attrition rule: a side that has lost
    // every gun but keeps its recycler has lost nothing, and the HUD says nothing. Claim three is
    // that the line is there once the enemy recycler is dead.
    //
    // The comparison is against the same scene with the layer switched OFF rather than counting a
    // colour, because the banner's ink is a red the radar's hostile marks already use.
    SceneOptions options = options_at(0.0F, 0.0F, kOutcomeSeed, 40);
    options.show_ledger = false;
    const auto banner_adds = [&](const protekt::sim::Sim& sim) {
        options.show_outcome = true;
        const std::size_t with = build_scene(sim, options).vertices.size();
        options.show_outcome = false;
        const std::size_t without = build_scene(sim, options).vertices.size();
        return with - without;
    };

    const protekt::sim::Sim fighting = protekt::sim::Sim::create(an_outcome_battle());
    EXPECT_EQ(banner_adds(fighting), 0U) << "the verdict drew while the war was still being fought";

    // The player's only gun is dead and both recyclers live: C8 called that a defeat, and the
    // ruling retires it.
    protekt::sim::Sim attrited = protekt::sim::Sim::create(an_outcome_battle());
    for (int i = 0; i < 6000 && armed_alive_in(attrited, protekt::sim::Side::Player) > 0; ++i) {
        attrited.step();
    }
    ASSERT_EQ(armed_alive_in(attrited, protekt::sim::Side::Player), 0)
        << "the rig must lose the gun";
    ASSERT_EQ(attrited.war_outcome(), protekt::sim::WarOutcome::Undecided);
    EXPECT_EQ(banner_adds(attrited), 0U) << "a side with no guns but a recycler was declared lost";

    const protekt::sim::Sim won = a_battle_where_the_enemy_recycler_just_died();
    ASSERT_EQ(won.war_outcome(), protekt::sim::WarOutcome::PlayerWon);
    ASSERT_GT(armed_alive_in(won, protekt::sim::Side::Enemy), 0)
        << "a win by attrition proves nothing";
    EXPECT_GT(banner_adds(won), 0U) << "the enemy recycler died and the HUD said nothing";
}

// ---------------------------------------------------------------- D4: the blast and the smoke
//
// The wake a wreck leaves: a burst of hot puffs for the first `kBlastTicks`, then a slow grey
// column for `kSmokeTicks` after that. Both are DERIVED -- the death is a `sim::EventKind::Killed`
// event and the tile is the dead unit's own, still in the sim's table -- so nothing about a puff is
// stored anywhere and a replay stays the whole input.

namespace {

namespace sim = protekt::sim;

using protekt::game::build_scene;
using protekt::game::kBlastAlpha;
using protekt::game::kBlastPuffs;
using protekt::game::kBlastTicks;
using protekt::game::kMetresPerTile;
using protekt::game::kSmokeAlpha;
using protekt::game::kSmokePuffs;
using protekt::game::kSmokeTicks;
using protekt::game::SceneVertex;

/// One puff is a box, and a box is six quads: the same 36 vertices a prism gets.
constexpr std::size_t kPuffVertices = std::size_t{6} * 6;
constexpr float kWakeTolerance = 0.01F;

/// The rig these tests read a wake out of: a world frame with the HUD layers that would otherwise
/// be the tail of the vertex list turned off, as `options_at` above does for its own callers.
[[nodiscard]] protekt::game::SceneOptions wake_options(float x, float z, int radius = 8) {
    protekt::game::SceneOptions options;
    options.seed = 42ULL;
    options.radius_tiles = radius;
    options.camera.x = x;
    options.camera.z = z;
    options.camera.y = 8.0F;
    options.show_ship_gauge = false;
    options.show_ledger = false;
    return options;
}

/// The tick the rig's Killed event is stamped with, and the id the sim gave the unit that died.
struct Wreck {
    bool valid{false};
    sim::UnitId id{};
    sim::Tick stamp{}; ///< the tick the Killed event carries
    std::vector<sim::BattleEvent> events;
    int tile_x{};
    int tile_y{};
};

/// A wreck with its own history: an enemy hauler four tiles from a player tank. The tank is armed
/// and the hauler is not, so the kill is the tank's own and the wreck stands where the hauler was.
[[nodiscard]] Wreck a_wreck_just_made() {
    Wreck wreck;
    sim::Scenario scenario;
    scenario.seed = 7ULL;
    scenario.spawns = {{sim::UnitKind::Tank, sim::Side::Player, 40, 40},
                       {sim::UnitKind::Scavenger, sim::Side::Enemy, 44, 40}};
    sim::Sim sim = sim::Sim::create(scenario);
    sim::UnitId hauler{};
    for (const sim::Unit& unit : sim.units()) {
        if (unit.kind == sim::UnitKind::Scavenger) {
            hauler = unit.id;
        }
    }
    for (int tick = 0; tick < 4000; ++tick) {
        const sim::UnitSnapshot before = sim.units();
        const sim::Tick now = sim.tick();
        sim.step();
        const std::vector<sim::BattleEvent> events = sim::events_between(before, sim, now);
        for (const sim::BattleEvent& event : events) {
            if (event.kind == sim::EventKind::Killed && event.subject == hauler) {
                wreck.valid = true;
                wreck.id = event.subject;
                wreck.stamp = event.tick;
                wreck.events = events;
                const sim::Unit* dead = sim.find(hauler);
                wreck.tile_x = dead->x.floor_int();
                wreck.tile_y = dead->y.floor_int();
                return wreck;
            }
        }
    }
    return wreck; // never: the tests assert `valid` first
}

/// One measurement of the wake at `age` ticks after the death, each from its OWN battle so that two
/// ages cannot contaminate each other: the frame built WITH the death's events, minus the same
/// frame built WITHOUT them. Reading a layer that sits inside a vertex list any other way is a
/// guess about where the list ends.
struct WakeFrame {
    bool wreck_valid{false};
    std::size_t first{};
    int tile_x{};
    int tile_y{};
    std::vector<SceneVertex> wake;
};

[[nodiscard]] WakeFrame wake_at(sim::Tick age) {
    WakeFrame frame;
    const Wreck wreck = a_wreck_just_made();
    frame.wreck_valid = wreck.valid;
    frame.tile_x = wreck.tile_x;
    frame.tile_y = wreck.tile_y;
    if (!wreck.valid) {
        return frame;
    }
    sim::Scenario scenario;
    scenario.seed = 7ULL;
    scenario.spawns = {{sim::UnitKind::Tank, sim::Side::Player, 40, 40},
                       {sim::UnitKind::Scavenger, sim::Side::Enemy, 44, 40}};
    sim::Sim sim = sim::Sim::create(scenario);
    // The Killed event is stamped with the tick BEFORE the step that applied the blow, so the frame
    // at age 0 is the frame one step past the stamp: the wreck is in the table and it is dead.
    for (sim::Tick tick = 0; tick < wreck.stamp + 1; ++tick) {
        sim.step();
    }
    for (sim::Tick tick = 0; tick < age; ++tick) {
        sim.step();
    }
    SceneOptions plain = wake_options(static_cast<float>(wreck.tile_x) * kMetresPerTile,
                                      static_cast<float>(wreck.tile_y) * kMetresPerTile);
    SceneOptions with = plain;
    with.events = wreck.events;
    const Scene before = build_scene(sim, plain);
    const Scene after = build_scene(sim, with);
    frame.first = before.vertices.size();
    if (after.vertices.size() >= before.vertices.size()) {
        frame.wake.assign(after.vertices.begin() + static_cast<std::ptrdiff_t>(frame.first),
                          after.vertices.end());
    }
    return frame;
}

[[nodiscard]] float mean_y(const std::vector<SceneVertex>& vertices) {
    float sum = 0.0F;
    for (const SceneVertex& v : vertices) {
        sum += v.position[1];
    }
    return vertices.empty() ? 0.0F : sum / static_cast<float>(vertices.size());
}

TEST(SceneWake, NothingIsDrawnWithoutADeath) {
    // A frame whose events hold no Killed event draws exactly what a frame with no events at all
    // draws: the layer is free for every frame that has not lost a unit.
    const Wreck wreck = a_wreck_just_made();
    ASSERT_TRUE(wreck.valid);
    sim::Scenario scenario;
    scenario.seed = 7ULL;
    scenario.spawns = {{sim::UnitKind::Tank, sim::Side::Player, 40, 40}};
    const sim::Sim quiet = sim::Sim::create(scenario);
    const SceneOptions plain = wake_options(0.0F, 0.0F);
    // The list is NAMED and lives past the frame: `events` is a span, and a span over a temporary
    // vector is a pointer into memory the statement's end has already freed (ASan said so).
    const std::vector<sim::BattleEvent> hits{
        sim::BattleEvent{wreck.stamp, sim::EventKind::HitTaken, sim::UnitId{1}, sim::UnitId{2}, 0}};
    SceneOptions with = plain;
    with.events = hits;
    EXPECT_EQ(build_scene(quiet, plain).vertices.size(), build_scene(quiet, with).vertices.size());
}

TEST(SceneWake, AKilledEventLaysAHotBurstAtTheWreckAndItBurnsOut) {
    const WakeFrame frame = wake_at(0);
    ASSERT_TRUE(frame.wreck_valid);
    ASSERT_EQ(frame.wake.size(), kBlastPuffs * kPuffVertices);
    // The ground under the wreck, from the rig's own seed: a puff sits ABOVE it, never in the hill.
    const float ground = protekt::game::ground_metres(42ULL, static_cast<float>(frame.tile_x),
                                                      static_cast<float>(frame.tile_y));
    // A Killed event is stamped with the tick before the blow lands, so the freshest burst a frame
    // can show is one tick old: the full burst alpha less one tick's worth of fade.
    const float fresh = kBlastAlpha * (1.0F - (1.0F / static_cast<float>(kBlastTicks)));
    for (const SceneVertex& v : frame.wake) {
        EXPECT_NEAR(v.alpha, fresh, 1e-4F) << "a fresh puff is at the burst's own alpha";
        EXPECT_LT(v.alpha, 1.0F) << "it blends; it is not a solid";
        EXPECT_GT(v.position[1], ground) << "and it is off the ground";
    }
    // And it cools as it burns: the same burst halfway through its window is thinner.
    const WakeFrame fading = wake_at(10);
    ASSERT_FALSE(fading.wake.empty());
    EXPECT_LT(fading.wake.at(0).alpha, frame.wake.at(0).alpha) << "the burst did not fade";
    // The burst is OUT by the end of its window: what is left at `kBlastTicks` is the column.
    const WakeFrame later = wake_at(kBlastTicks);
    ASSERT_TRUE(later.wreck_valid);
    EXPECT_EQ(later.wake.size(), kSmokePuffs * kPuffVertices) << "the burst outlived kBlastTicks";
}

TEST(SceneWake, TheBurstBecomesASlowGreyColumnAndTheColumnGoesInItsTurn) {
    const WakeFrame frame = wake_at(kBlastTicks + 1);
    ASSERT_TRUE(frame.wreck_valid);
    ASSERT_EQ(frame.wake.size(), kSmokePuffs * kPuffVertices);
    for (const SceneVertex& v : frame.wake) {
        EXPECT_LT(v.alpha, 0.5F) << "smoke is thin, not a wall";
        const float spread = std::max({v.colour[0], v.colour[1], v.colour[2]})
                             - std::min({v.colour[0], v.colour[1], v.colour[2]});
        EXPECT_LT(spread, 0.1F) << "smoke is grey, not lit";
    }
    EXPECT_TRUE(wake_at(kSmokeTicks).wake.empty()) << "the column outlived kSmokeTicks";
}

/// The width of the first puff in a wake (36 vertices, one box), in metres.
[[nodiscard]] float first_puff_width(const WakeFrame& frame) {
    float lo = frame.wake.at(0).position[0];
    float hi = lo;
    for (std::size_t i = 0; i < kPuffVertices; ++i) {
        lo = std::min(lo, frame.wake.at(i).position[0]);
        hi = std::max(hi, frame.wake.at(i).position[0]);
    }
    return hi - lo;
}

TEST(SceneWake, TheFireballIsBiggerThanTheHullThatDied) {
    const WakeFrame frame = wake_at(0);
    ASSERT_TRUE(frame.wreck_valid);
    ASSERT_FALSE(frame.wake.empty());
    EXPECT_GT(first_puff_width(frame), 2.0F * protekt::game::kHullHalfMetres * 1.5F)
        << "a puff a fraction of the hull is a spark, not a kill: it must read as an explosion";
}

TEST(SceneWake, TheColumnIsWiderThanTheHullToo) {
    const WakeFrame frame = wake_at(kBlastTicks + 1);
    ASSERT_TRUE(frame.wreck_valid);
    ASSERT_FALSE(frame.wake.empty());
    EXPECT_GT(first_puff_width(frame), 2.0F * protekt::game::kHullHalfMetres)
        << "the smoke that marks a wreck must be at least as wide as the wreck";
}

TEST(SceneWake, TheColumnIsStillThereOnItsLastTickAndGoneOnTheNext) {
    // `wake_at(n)` is a frame whose scene age is n + 1 (the frame after the stamp is age 1).
    const WakeFrame last = wake_at(kSmokeTicks - 2);
    ASSERT_TRUE(last.wreck_valid);
    EXPECT_EQ(last.wake.size(), kSmokePuffs * kPuffVertices)
        << "the wreck went cold before kSmokeTicks";
    EXPECT_TRUE(wake_at(kSmokeTicks - 1).wake.empty()) << "and it must be cold at age kSmokeTicks";
}

TEST(SceneWake, TheColumnRisesAndFadesAsTheTicksPass) {
    const WakeFrame young = wake_at(kBlastTicks + 5);
    const WakeFrame old = wake_at(kBlastTicks + 200);
    ASSERT_TRUE(young.wreck_valid);
    ASSERT_FALSE(young.wake.empty());
    ASSERT_FALSE(old.wake.empty());
    EXPECT_GT(mean_y(old.wake), mean_y(young.wake) + 0.5F) << "the column did not rise";
    EXPECT_LT(old.wake.at(0).alpha, young.wake.at(0).alpha) << "and did not thin out";
}

TEST(SceneWake, AWreckBeyondTheMeshedRadiusLeavesNothingOnScreen) {
    const Wreck wreck = a_wreck_just_made();
    ASSERT_TRUE(wreck.valid);
    // The camera sits at the origin and the wreck is 44 tiles away: outside a radius of 4 tiles,
    // and a puff over bare void would be a lie about where the meshed world ends.
    const SceneOptions plain = wake_options(0.0F, 0.0F, 4);
    SceneOptions with = plain;
    with.events = wreck.events;
    sim::Scenario scenario;
    scenario.seed = 7ULL;
    scenario.spawns = {{sim::UnitKind::Tank, sim::Side::Player, 40, 40}};
    const sim::Sim quiet = sim::Sim::create(scenario);
    EXPECT_EQ(build_scene(quiet, plain).vertices.size(), build_scene(quiet, with).vertices.size());
}

TEST(SceneWake, TheWakeIsDerivedNotBakedIn) {
    // The same battle measured twice draws the same wake to the byte: nothing about a puff is
    // stored, every position comes out of the sim's own state. And the placement is off the sim's
    // numbers rather than a fixed shape -- move the SEED and the puffs move with it, which is the
    // same machinery that keeps one wreck's smoke from standing in the same place as another's.
    const WakeFrame once = wake_at(3);
    const WakeFrame twice = wake_at(3);
    ASSERT_TRUE(once.wreck_valid);
    ASSERT_FALSE(once.wake.empty());
    ASSERT_EQ(once.wake.size(), twice.wake.size());
    for (std::size_t i = 0; i < once.wake.size(); ++i) {
        EXPECT_FLOAT_EQ(once.wake.at(i).position[0], twice.wake.at(i).position[0]) << i;
        EXPECT_FLOAT_EQ(once.wake.at(i).position[1], twice.wake.at(i).position[1]) << i;
        EXPECT_FLOAT_EQ(once.wake.at(i).position[2], twice.wake.at(i).position[2]) << i;
    }
    const Wreck wreck = a_wreck_just_made();
    ASSERT_TRUE(wreck.valid);
    sim::Scenario scenario;
    scenario.seed = 7ULL;
    scenario.spawns = {{sim::UnitKind::Tank, sim::Side::Player, 40, 40},
                       {sim::UnitKind::Scavenger, sim::Side::Enemy, 44, 40}};
    sim::Sim sim = sim::Sim::create(scenario);
    for (sim::Tick tick = 0; tick < wreck.stamp + 3; ++tick) {
        sim.step();
    }
    SceneOptions other = wake_options(static_cast<float>(wreck.tile_x) * kMetresPerTile,
                                      static_cast<float>(wreck.tile_y) * kMetresPerTile);
    other.seed = 8ULL;
    other.events = wreck.events;
    const Scene scene = build_scene(sim, other);
    ASSERT_GE(scene.vertices.size(), once.wake.size());
    bool moved = false;
    for (std::size_t i = 0; i < once.wake.size(); ++i) {
        const SceneVertex& v = scene.vertices.at(scene.vertices.size() - once.wake.size() + i);
        moved = moved || std::abs(v.position[2] - once.wake.at(i).position[2]) > kWakeTolerance;
    }
    EXPECT_TRUE(moved) << "the wake is a fixed shape: the seed did not move it";
}

/// The shadow patches of a frame: every vertex in the shadow's own ink and alpha, in emission
/// order. The patch constants are the ONLY way this finds them, so a second copy of the ink in the
/// renderer would make the counts below come out short.
std::vector<protekt::game::SceneVertex> shadow_vertices(const SceneOptions& options,
                                                        const Scene& scene) {
    std::vector<protekt::game::SceneVertex> found;
    for (const auto& vertex : scene.vertices) {
        if (protekt::game::haze::is_ink(options, vertex, protekt::game::kShadowInk)
            && vertex.alpha == protekt::game::kShadowAlpha) {
            found.push_back(vertex);
        }
    }
    return found;
}

/// The unit's centre in metres, as the scene reads it off the sim's fixed-point tile.
std::array<float, 2> centre_metres(const protekt::sim::Unit& unit) {
    return {static_cast<float>(unit.x.raw) / static_cast<float>(protekt::sim::Fixed::kOne)
                * kMetresPerTile,
            static_cast<float>(unit.y.raw) / static_cast<float>(protekt::sim::Fixed::kOne)
                * kMetresPerTile};
}

protekt::sim::Sim a_tank_and_a_building() {
    protekt::sim::Scenario scenario;
    scenario.seed = 20261028;
    scenario.spawns.push_back(
        protekt::sim::Spawn{protekt::sim::UnitKind::Tank, protekt::sim::Side::Player, 57, 41});
    scenario.spawns.push_back(protekt::sim::Spawn{protekt::sim::UnitKind::DeployedRecycler,
                                                  protekt::sim::Side::Player, 62, 41});
    return protekt::sim::Sim::create(scenario);
}

TEST(Scene, ALivingUnitInViewHasOneFlatShadowPatchAtItsOwnTile) {
    protekt::sim::Scenario scenario;
    scenario.seed = 20261028;
    scenario.spawns.push_back(
        protekt::sim::Spawn{protekt::sim::UnitKind::Tank, protekt::sim::Side::Player, 57, 41});
    const protekt::sim::Sim sim = protekt::sim::Sim::create(scenario);
    const protekt::sim::Unit& tank = sim.units().front();
    const auto [cx, cz] = centre_metres(tank);

    const SceneOptions options = options_at(cx, cz, sim.seed(), 8);
    const Scene scene = build_scene(sim, options);
    const auto patch = shadow_vertices(options, scene);

    ASSERT_EQ(patch.size(), protekt::game::kShadowVertexCount)
        << "one quad, two triangles, six vertices, for the one living unit";
    const float ground
        = protekt::game::ground_metres(sim.seed(), cx / kMetresPerTile, cz / kMetresPerTile)
          + protekt::game::kShadowLiftMetres;
    const float half = protekt::game::kShadowHalfMetres;
    for (const auto& vertex : patch) {
        EXPECT_NEAR(vertex.position[1], ground, 1.0e-4F)
            << "flat on the terrain under the unit: a floating patch is a decal, a buried one is "
               "invisible";
        EXPECT_LE(std::abs(vertex.position[0] - cx), half + 1.0e-4F)
            << "at the unit's own tile, not somewhere else on the field";
        EXPECT_LE(std::abs(vertex.position[2] - cz), half + 1.0e-4F);
    }
}

TEST(Scene, ADeadUnitCastsNoShadow) {
    const protekt::sim::Sim sim = a_battle_that_leaves_litter();
    std::size_t living = 0;
    bool any_dead = false;
    for (const protekt::sim::Unit& unit : sim.units()) {
        living += unit.alive ? 1U : 0U;
        any_dead = any_dead || !unit.alive;
    }
    ASSERT_TRUE(any_dead) << "the rig must kill a unit, or this proves nothing";
    ASSERT_GT(living, 0U);

    const auto [cx, cz] = centre_metres(sim.units().front());
    const SceneOptions options = options_at(cx, cz, sim.seed(), 40);
    const Scene scene = build_scene(sim, options);
    EXPECT_EQ(shadow_vertices(options, scene).size(), protekt::game::kShadowVertexCount * living)
        << "a patch per LIVING unit: the wreck's patch would be a shadow under nothing";
}

TEST(Scene, APatchIsAShadeWiderThanTheHullDrawnAboveIt) {
    const protekt::sim::Sim sim = a_tank_and_a_building();
    ASSERT_EQ(sim.units().size(), 2U);
    const auto [cx, cz] = centre_metres(sim.units().front());
    const SceneOptions options = options_at(cx, cz, sim.seed(), 12);
    const Scene scene = build_scene(sim, options);
    const auto patch = shadow_vertices(options, scene);
    ASSERT_EQ(patch.size(), 2U * protekt::game::kShadowVertexCount);

    const auto width = [&patch](std::size_t first) {
        float low = patch.at(first).position[0];
        float high = low;
        for (std::size_t i = first; i < first + protekt::game::kShadowVertexCount; ++i) {
            low = std::min(low, patch.at(i).position[0]);
            high = std::max(high, patch.at(i).position[0]);
        }
        return high - low;
    };
    // Emission order is the sim's unit order: the tank was spawned first.
    //
    // A TANK AND A BUILDING CAST THE SAME PATCH, and that is the rule, not an oversight: every hull
    // -- the factory's skyline, the turret, the recycler, the tank's fenders -- is drawn inside
    // `kHullHalfMetres`, so one number sizes every patch. The sim's `Footprint` is a 2x2-TILE AREA
    // A DEPLOYED BUILDING RESERVES (no unit may enter it), not the size of the structure drawn
    // inside it; sizing the patch from that reservation drew a black square 2.5x the building's own
    // width. Measure what is DRAWN.
    EXPECT_NEAR(width(0), width(protekt::game::kShadowVertexCount), 1.0e-4F)
        << "one hull width for every kind, so one patch width for every kind";
    EXPECT_NEAR(width(0), 2.0F * protekt::game::kShadowHalfMetres, 1.0e-4F);
    EXPECT_GT(protekt::game::kShadowHalfMetres, protekt::game::kHullHalfMetres)
        << "wider than the hull it belongs to, or the ink hides under the hull";
    EXPECT_LE(protekt::game::kShadowHalfMetres, protekt::game::kHullHalfMetres * 1.5F)
        << "and not so wide that it reads as a puddle rather than a shadow";
    EXPECT_LT(width(protekt::game::kShadowVertexCount),
              protekt::game::kMetresPerTile
                  * static_cast<float>(protekt::sim::kBuildingFootprintTiles))
        << "NOT the reserved footprint: a patch 2x2 tiles across is four times the building's "
           "width";
}

TEST(Scene, TheShadowIsTheNamedInkAndAlphaAndStaysRestrained) {
    const protekt::sim::Sim sim = a_tank_and_a_building();
    const auto [cx, cz] = centre_metres(sim.units().front());
    const SceneOptions options = options_at(cx, cz, sim.seed(), 12);
    const Scene scene = build_scene(sim, options);
    const auto patch = shadow_vertices(options, scene);
    ASSERT_EQ(patch.size(), 2U * protekt::game::kShadowVertexCount)
        << "found by the named constants alone: the renderer holds no second copy";
    EXPECT_GT(protekt::game::kShadowAlpha, 0.0F);
    EXPECT_LE(protekt::game::kShadowAlpha, 0.5F)
        << "the terrain texture must still show through the patch";
    for (const float channel : protekt::game::kShadowInk) {
        EXPECT_LT(channel, 0.15F) << "a shadow is dark";
    }
}

TEST(Scene, TheShadowFrameIsAPureFunctionOfSimAndOptions) {
    const protekt::sim::Sim sim = a_tank_and_a_building();
    const auto [cx, cz] = centre_metres(sim.units().front());
    const SceneOptions options = options_at(cx, cz, sim.seed(), 12);
    const auto first = shadow_vertices(options, build_scene(sim, options));
    const auto second = shadow_vertices(options, build_scene(sim, options));
    ASSERT_FALSE(first.empty());
    ASSERT_EQ(first.size(), second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(first.at(i).position, second.at(i).position)
            << "nothing is stored between frames, so the same inputs draw the same patch";
    }
}

TEST(Scene, AUnitOutOfViewCastsNoShadow) {
    const protekt::sim::Sim sim = a_tank_and_a_building();
    // Far outside the meshed radius of a camera at the origin: the unit emission culls it, and so
    // must its shadow, by the same rule.
    const SceneOptions away = options_at(0.0F, 0.0F, sim.seed(), 8);
    const Scene scene = build_scene(sim, away);
    EXPECT_TRUE(shadow_vertices(away, scene).empty())
        << "a patch with no ground meshed under it would float over bare void";

    // THE CONTROL, without which the line above would pass just as well if nothing ever emitted a
    // patch at all: the same rig, camera on the units.
    const auto [cx, cz] = centre_metres(sim.units().front());
    const SceneOptions on_them = options_at(cx, cz, sim.seed(), 12);
    EXPECT_FALSE(shadow_vertices(on_them, build_scene(sim, on_them)).empty())
        << "the cull is what removes the patch, not a renderer that emits nothing";
}

TEST(Scene, TheViewersOwnHullStillCastsItsShadow) {
    protekt::sim::Scenario scenario;
    scenario.seed = 20261028;
    scenario.spawns.push_back(
        protekt::sim::Spawn{protekt::sim::UnitKind::Tank, protekt::sim::Side::Player, 57, 41});
    const protekt::sim::Sim sim = protekt::sim::Sim::create(scenario);
    const auto [cx, cz] = centre_metres(sim.units().front());
    SceneOptions options = options_at(cx, cz, sim.seed(), 8);
    options.viewer_unit = sim.units().front().id;
    EXPECT_EQ(shadow_vertices(options, build_scene(sim, options)).size(),
              protekt::game::kShadowVertexCount)
        << "first person draws no hull for the player, so the patch is the cue they see ahead";
}

TEST(Scene, TheShadowIsDrawnBeforeTheHull) {
    protekt::sim::Scenario scenario;
    scenario.seed = 20261028;
    scenario.spawns.push_back(
        protekt::sim::Spawn{protekt::sim::UnitKind::Tank, protekt::sim::Side::Player, 57, 41});
    const protekt::sim::Sim sim = protekt::sim::Sim::create(scenario);
    const auto [cx, cz] = centre_metres(sim.units().front());
    const SceneOptions options = options_at(cx, cz, sim.seed(), 8);
    const Scene scene = build_scene(sim, options);
    std::size_t last = 0;
    for (std::size_t i = 0; i < scene.vertices.size(); ++i) {
        if (protekt::game::haze::is_ink(options, scene.vertices.at(i), protekt::game::kShadowInk)
            && scene.vertices.at(i).alpha == protekt::game::kShadowAlpha) {
            last = i;
        }
    }
    ASSERT_GT(last, 0U);
    EXPECT_GE(scene.vertices.size() - (last + 1), kTankVertices)
        << "the hull (and the bars after it) come after the patch, so it reads as ground";
}

// ------------------------------------------------- the world hides what the radar cannot reach

/// VIS: a hostile farther from the viewer than `viewer_radar_radius` is not drawn -- hull, shadow,
/// bar, readout. Every test builds the SAME frame twice, once with the hostile and once without
/// it, so "nothing" is a diff of whole vertex lists and not one field trusted.
namespace reach {

using protekt::game::kRadarRadiusTiles;
using protekt::game::kScoutRadarRadiusTiles;
using protekt::game::viewer_radar_radius;
using protekt::sim::Scenario;
using protekt::sim::Side;
using protekt::sim::Sim;
using protekt::sim::Spawn;
using protekt::sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr int kX
    = 20; ///< seed 42: this row is clear of craters at +3, +40, +41, +45, +79, +80, +81
constexpr int kY = 40;

/// A viewer of `kind` at (kX, kY), an optional own tank `own_east` tiles east, an optional enemy
/// tank `enemy_east` tiles east. Positions are asserted: a spawn moved off a crater tests nothing.
Sim battle(UnitKind kind, int own_east, int enemy_east) {
    std::vector<Spawn> spawns{{kind, Side::Player, kX, kY}};
    if (own_east > 0) {
        spawns.push_back({UnitKind::Tank, Side::Player, kX + own_east, kY});
    }
    if (enemy_east > 0) {
        spawns.push_back({UnitKind::Tank, Side::Enemy, kX + enemy_east, kY});
    }
    Sim sim = Sim::create(Scenario{kSeed, spawns});
    for (std::size_t i = 1; i < sim.units().size(); ++i) {
        const int want = kX + (sim.units().at(i).side == Side::Player ? own_east : enemy_east);
        EXPECT_EQ(sim.units().at(i).x, protekt::sim::Fixed::from_int(want)) << "moved off its tile";
        EXPECT_EQ(sim.units().at(i).y, protekt::sim::Fixed::from_int(kY));
    }
    return sim;
}

SceneOptions frame_options(const Sim& sim) {
    SceneOptions options = options_at(static_cast<float>(kX) * kMetresPerTile,
                                      static_cast<float>(kY) * kMetresPerTile, kSeed, 100);
    options.viewer_unit = sim.units().front().id;
    options.viewer_side = Side::Player;
    options.camera.yaw = 1.5707963F; // looking east, down the row the units stand on
    return options;
}

Scene frame(const Sim& sim, const SceneOptions& options) {
    return protekt::game::build_scene(sim, options);
}

bool same_vertices(const Scene& a, const Scene& b) {
    return a.vertices.size() == b.vertices.size()
           && std::memcmp(a.vertices.data(), b.vertices.data(),
                          a.vertices.size() * sizeof(protekt::game::SceneVertex))
                  == 0;
}

/// How many vertices the enemy adds to the frame: with it, minus without it.
std::ptrdiff_t enemy_adds(UnitKind viewer, int enemy_east, bool with_readout = false) {
    const Sim with = battle(viewer, 0, enemy_east);
    const Sim without = battle(viewer, 0, 0);
    SceneOptions a = frame_options(with);
    if (with_readout) {
        a.attack_target = with.units().at(1).id;
    }
    return static_cast<std::ptrdiff_t>(frame(with, a).vertices.size())
           - static_cast<std::ptrdiff_t>(frame(without, frame_options(without)).vertices.size());
}

TEST(WorldReach, AHostileInsideTheReachIsDrawnWithHullShadowAndBar) {
    const std::ptrdiff_t added = enemy_adds(UnitKind::Tank, 39);
    EXPECT_GE(added, static_cast<std::ptrdiff_t>(kTankVertices + protekt::game::kShadowVertexCount
                                                 + protekt::game::kUnitBarVertexCount));
}

TEST(WorldReach, TheSameHostileJustBeyondTheReachAddsNothingAtAll) {
    EXPECT_EQ(enemy_adds(UnitKind::Tank, 41), 0) << "no hull, no shadow, no bar";
    const Sim with = battle(UnitKind::Tank, 0, 41);
    const Sim without = battle(UnitKind::Tank, 0, 0);
    EXPECT_TRUE(
        same_vertices(frame(with, frame_options(with)), frame(without, frame_options(without))))
        << "the frame is the frame of a world with nobody there, vertex for vertex";
}

TEST(WorldReach, AHostileExactlyAtTheReachIsDrawn) {
    ASSERT_EQ(viewer_radar_radius(battle(UnitKind::Tank, 0, 40), protekt::sim::UnitId{1}),
              kRadarRadiusTiles);
    EXPECT_GE(enemy_adds(UnitKind::Tank, 40),
              static_cast<std::ptrdiff_t>(kTankVertices + protekt::game::kShadowVertexCount))
        << "the boundary is inside, as the radar panel's own `distance <= radius` has it";
}

TEST(WorldReach, TheAttackReadoutOverAHiddenHostileIsNotDrawnEither) {
    EXPECT_EQ(enemy_adds(UnitKind::Tank, 41, true), 0) << "its label and bar are the giveaway";
    EXPECT_GT(enemy_adds(UnitKind::Tank, 39, true), enemy_adds(UnitKind::Tank, 39, false))
        << "control: in reach, the readout IS drawn";
}

TEST(WorldReach, YourOwnUnitBeyondTheReachIsStillDrawn) {
    const Sim with = battle(UnitKind::Tank, 60, 0);
    const Sim without = battle(UnitKind::Tank, 0, 0);
    const std::ptrdiff_t added
        = static_cast<std::ptrdiff_t>(frame(with, frame_options(with)).vertices.size())
          - static_cast<std::ptrdiff_t>(frame(without, frame_options(without)).vertices.size());
    EXPECT_GE(added, static_cast<std::ptrdiff_t>(kTankVertices + protekt::game::kShadowVertexCount
                                                 + protekt::game::kUnitBarVertexCount))
        << "you always know where your own things are: this is not a distance cull";
}

TEST(WorldReach, AScoutSeesTheHostileATankCannotAndNotBeyondItsOwnEighty) {
    // The scout's reach is 80, twice the tank's. Measured from the VIEWER, so a scout's hull and
    // shadow must appear where a tank's do not -- and the whole bundle must be there: a radar BLIP
    // alone is a handful of vertices, and an assertion of "more than nothing" was satisfied by the
    // blip while the hull was hidden (found by inverting the reach to a fixed 40: the clause stayed
    // green, because the blip still used the real per-kind radius).
    constexpr std::ptrdiff_t kWholeUnit = static_cast<std::ptrdiff_t>(
        kTankVertices + protekt::game::kShadowVertexCount + protekt::game::kUnitBarVertexCount);
    EXPECT_EQ(enemy_adds(UnitKind::Tank, 45), 0);
    EXPECT_GE(enemy_adds(UnitKind::Scout, 45), kWholeUnit) << "the scout's reason to exist";
    EXPECT_GE(enemy_adds(UnitKind::Scout, 80), kWholeUnit) << "exactly at 80 is inside";
    EXPECT_EQ(enemy_adds(UnitKind::Scout, 81), 0);
}

TEST(WorldReach, WithNoViewerNothingIsHidden) {
    const Sim sim = battle(UnitKind::Tank, 0, 45);
    SceneOptions options = frame_options(sim);
    options.viewer_unit = protekt::sim::UnitId{};
    const Sim alone = battle(UnitKind::Tank, 0, 0);
    EXPECT_GE(frame(sim, options).vertices.size(),
              frame(alone, options).vertices.size() + kTankVertices)
        << "an observer frame has no radar to be limited by";
}

TEST(WorldReach, TheRadarPanelIsTheSameWithOrWithoutTheCull) {
    // The radar's blips come from `radar_marks`, which the world cull does not touch: inside the
    // reach a hostile is a blip, beyond it nothing -- the same rule from the other side.
    for (const int east : {39, 40, 41}) {
        const Sim sim = battle(UnitKind::Tank, 0, east);
        const auto marks
            = protekt::game::radar_marks(sim.units(), sim.units().front().id, 0.0F,
                                         protekt::game::radar_panel(frame_options(sim).extent),
                                         viewer_radar_radius(sim, sim.units().front().id));
        const auto hostile = std::count_if(marks.begin(), marks.end(), [](const auto& mark) {
            return mark.tone == protekt::game::RadarTone::Hostile;
        });
        EXPECT_EQ(hostile, east <= 40 ? 1 : 0) << "east " << east;
    }
}

TEST(WorldReach, TheRadarOverlayInTheFrameStillShowsTheBlipInsideAndNoneBeyond) {
    // The same frame, hostile in versus hostile out: the radar's tail grows by exactly the blip
    // inside the reach and by nothing beyond it, so the cull did not eat the panel.
    const Sim in = battle(UnitKind::Tank, 0, 39);
    const Sim out = battle(UnitKind::Tank, 0, 41);
    const Sim none = battle(UnitKind::Tank, 0, 0);
    SceneOptions radar_in = frame_options(in);
    SceneOptions radar_out = frame_options(out);
    SceneOptions radar_none = frame_options(none);
    EXPECT_GT(frame(in, radar_in).vertices.size() - frame(none, radar_none).vertices.size(),
              kTankVertices)
        << "world hull plus a blip";
    EXPECT_EQ(frame(out, radar_out).vertices.size(), frame(none, radar_none).vertices.size());
}

// THE DEPLOY BAR (RB6): a recycler counting `deploy_ticks` wears a second bar over its health bar,
// its fill `deploy_ticks / kDeployTicks`. The sim's own count is the only number; nothing is
// hashed.

/// Every bar in the frame, wherever it stands: an outline quad, a plate quad and a fill quad in the
/// bar colours, found by pattern so nothing after the bar pass (HUD panels) can hide one.
std::vector<UnitBar> bars_in(const SceneOptions& options, const Scene& scene) {
    std::vector<UnitBar> bars;
    const auto& v = scene.vertices;
    for (std::size_t first = 0; first + kUnitBarVertexCount <= v.size(); ++first) {
        if (v.at(first).colour == kBarOutlineRgb && v.at(first + 6).colour == kBarPlateRgb
            && (v.at(first + 12).colour == kOwnBarRgb || v.at(first + 12).colour == kHostileBarRgb)
            && v.at(first + 17).colour == v.at(first + 12).colour) {
            bars.push_back(
                UnitBar{rect_of(options, scene, first), rect_of(options, scene, first + 6),
                        rect_of(options, scene, first + 12), v.at(first + 12).colour, 0});
            first += kUnitBarVertexCount - 1;
        }
    }
    return bars;
}

/// A viewer tank at (kX, kY) and a recycler of `side` `east` tiles east of it, ordered to deploy on
/// the site it already stands on and stepped `ticks` ticks. The order is asserted accepted and the
/// count asserted to be the ticks, so a refused order cannot pass for a missing bar.
Sim deploying(Side side, int east, int ticks) {
    Sim sim = Sim::create(Scenario{
        kSeed,
        {{UnitKind::Tank, Side::Player, kX, kY}, {UnitKind::Recycler, side, kX + east, kY}}});
    const protekt::sim::UnitId id = sim.units().at(1).id;
    sim.enqueue(protekt::sim::Order{id, protekt::sim::orders::Deploy{kX + east - 1, kY - 1}});
    for (int i = 0; i < ticks; ++i) {
        sim.step();
    }
    EXPECT_EQ(sim.command_log().size(), 1U) << "the deploy order was refused";
    EXPECT_EQ(sim.find(id)->deploy_ticks, ticks);
    return sim;
}

std::vector<UnitBar> bars_of_deploy(Side side, int east, int ticks) {
    const Sim sim = deploying(side, east, ticks);
    const SceneOptions options = frame_options(sim);
    return bars_in(options, frame(sim, options));
}

TEST(DeployBar, AMidCountRecyclerWearsASecondBarWhoseFillIsTheSimsFraction) {
    const std::vector<UnitBar> before = bars_of_deploy(Side::Player, 10, 0);
    ASSERT_EQ(before.size(), 1U) << "the health bar alone, nothing counting";
    for (const int ticks : {1, protekt::sim::kDeployTicks / 2, protekt::sim::kDeployTicks - 1}) {
        const std::vector<UnitBar> bars = bars_of_deploy(Side::Player, 10, ticks);
        ASSERT_EQ(bars.size(), 2U) << "health bar plus deploy bar at tick " << ticks;
        const UnitBar& deploy = bars.back();
        EXPECT_NEAR(deploy.fill.width() / deploy.track.width(),
                    static_cast<float>(ticks) / static_cast<float>(protekt::sim::kDeployTicks),
                    0.01F)
            << "ticks " << ticks;
        EXPECT_NEAR(deploy.fill.left, deploy.track.left, 0.1F) << "grows from the left";
    }
}

TEST(DeployBar, ItGrowsAndIsWholeJustBeforeTheBuildingStands) {
    const UnitBar first = bars_of_deploy(Side::Player, 10, 1).back();
    const UnitBar half = bars_of_deploy(Side::Player, 10, protekt::sim::kDeployTicks / 2).back();
    const UnitBar last = bars_of_deploy(Side::Player, 10, protekt::sim::kDeployTicks - 1).back();
    EXPECT_LT(first.fill.width(), half.fill.width());
    EXPECT_LT(half.fill.width(), last.fill.width());
    EXPECT_NEAR(last.fill.width(), last.track.width(), last.track.width() / 100.0F);
}

TEST(DeployBar, ItStandsAboveTheHealthBarAndNeverOverlapsIt) {
    const std::vector<UnitBar> bars = bars_of_deploy(Side::Player, 10, 100);
    ASSERT_EQ(bars.size(), 2U);
    EXPECT_LT(bars.at(1).outline.bottom, bars.at(0).outline.top) << "clear of the health bar";
    EXPECT_FLOAT_EQ(bars.at(1).track.centre_x(), bars.at(0).track.centre_x());
}

TEST(DeployBar, ARecyclerNotCountingAddsExactlyNothing) {
    // Ordered but not yet counted (tick 0), against a recycler never ordered: the same frame.
    const Sim ordered = deploying(Side::Player, 10, 0);
    const Sim idle = Sim::create(Scenario{
        kSeed,
        {{UnitKind::Tank, Side::Player, kX, kY}, {UnitKind::Recycler, Side::Player, kX + 10, kY}}});
    EXPECT_TRUE(
        same_vertices(frame(ordered, frame_options(ordered)), frame(idle, frame_options(idle))));
    EXPECT_EQ(bars_of_deploy(Side::Player, 10, 0).size(), 1U);
}

TEST(DeployBar, TheBuildingThatStandsAfterTheCountWearsNone) {
    EXPECT_EQ(bars_of_deploy(Side::Player, 10, 0).size(), 1U);
    Sim sim = deploying(Side::Player, 10, protekt::sim::kDeployTicks - 1);
    sim.step();
    ASSERT_EQ(sim.units().at(1).kind, UnitKind::DeployedRecycler);
    const SceneOptions options = frame_options(sim);
    EXPECT_LE(bars_in(options, frame(sim, options)).size(), 1U) << "the count is zero: no bar";
}

TEST(DeployBar, ADeployingHostileInsideTheRadarShowsItAndBeyondItShowsNothing) {
    const std::vector<UnitBar> in = bars_of_deploy(Side::Enemy, 39, 100);
    ASSERT_EQ(in.size(), 2U) << "a hostile counting inside the reach";
    EXPECT_EQ(in.back().fill_colour, kHostileBarRgb);
    const Sim out = deploying(Side::Enemy, 41, 100);
    const Sim none = battle(UnitKind::Tank, 0, 0);
    EXPECT_TRUE(same_vertices(frame(out, frame_options(out)), frame(none, frame_options(none))))
        << "beyond the reach: no hull, no health bar, no deploy bar";
}

TEST(DeployBar, YourOwnDeployingRecyclerBeyondTheReachStillShowsIt) {
    EXPECT_EQ(bars_of_deploy(Side::Player, 60, 100).size(), 2U);
}

} // namespace reach

} // namespace
