// ROUND 19, L3: THE GHOST of the finished building at the placement site.
//
// The owner, 2026-11-13: "the green and red highlighting when trying to decide where to build, it
// should also display a 'ghost' of the finished building so you can see which way it's facing.
// Just a very transparent green or red glowing wireframe, depending on whether it can be built
// there or not."
//
// The ghost's vertices are the ones the scene emits at `kGhostCoreAlpha` (the bright ribbon along
// each edge): the grid is opaque and the world is opaque, so that alpha names the ghost alone.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "protekt/game/deploy_aim.hpp"
#include "protekt/game/interaction.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/units.hpp"

namespace protekt {
namespace {

using sim::Side;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;

struct Tile {
    int x{};
    int y{};
};

[[nodiscard]] Tile open_rect(int w, int h, int margin) {
    for (int y = 40; y < sim::kMapTiles - 60; ++y) {
        for (int x = 40; x < sim::kMapTiles - 60; ++x) {
            bool open = true;
            for (int ty = y - margin; ty < y + h + margin && open; ++ty) {
                for (int tx = x - margin; tx < x + w + margin && open; ++tx) {
                    open = sim::tile_passable(kSeed, tx, ty);
                }
            }
            if (open) {
                return Tile{x, y};
            }
        }
    }
    return Tile{-1, -1};
}

[[nodiscard]] sim::Sim world(std::vector<sim::Spawn> spawns) {
    return sim::Sim::create(sim::Scenario{kSeed, std::move(spawns)});
}

[[nodiscard]] game::SceneOptions view_of(float tile_x, float tile_y) {
    game::SceneOptions o;
    o.seed = kSeed;
    o.radius_tiles = 30;
    o.camera.x = tile_x * game::kMetresPerTile;
    o.camera.z = tile_y * game::kMetresPerTile;
    o.camera.y = 40.0F;
    o.show_ship_gauge = false;
    o.show_ledger = false;
    return o;
}

[[nodiscard]] game::Placing armed(UnitKind kind, bool turned) {
    sim::FootprintSize size = sim::footprint_of(kind);
    if (turned) {
        std::swap(size.width, size.height);
    }
    return game::Placing{kind, turned, 70, size};
}

/// The ghost's core vertices of a scene.
[[nodiscard]] std::vector<game::SceneVertex> ghost_of(const game::Scene& scene) {
    std::vector<game::SceneVertex> out;
    for (const game::SceneVertex& v : scene.vertices) {
        if (v.alpha == game::kGhostCoreAlpha) {
            out.push_back(v);
        }
    }
    return out;
}

struct Extent {
    float across{}; ///< along x
    float along{};  ///< along z
};
[[nodiscard]] Extent extent_of(const std::vector<game::SceneVertex>& vertices) {
    float west = 1.0e9F;
    float east = -1.0e9F;
    float north = 1.0e9F;
    float south = -1.0e9F;
    for (const game::SceneVertex& v : vertices) {
        west = std::min(west, v.position.at(0));
        east = std::max(east, v.position.at(0));
        north = std::min(north, v.position.at(2));
        south = std::max(south, v.position.at(2));
    }
    return Extent{east - west, south - north};
}

[[nodiscard]] game::Scene scene_armed(const sim::Sim& sim, const game::Placing& placing, int x,
                                      int y) {
    game::SceneOptions options = view_of(static_cast<float>(x), static_cast<float>(y));
    options.deploy_preview = game::placement_preview(sim, placing, x, y);
    return game::build_scene(sim, options);
}

TEST(PlacementGhost, IsDrawnWhileABuildingIsArmedAndNotWhenNothingIs) {
    const Tile t = open_rect(24, 24, 2);
    ASSERT_GE(t.x, 0);
    const sim::Sim sim = world({{UnitKind::Tank, Side::Player, t.x, t.y}});
    const game::Scene nothing
        = game::build_scene(sim, view_of(static_cast<float>(t.x + 8), static_cast<float>(t.y + 8)));
    EXPECT_TRUE(ghost_of(nothing).empty()) << "nothing armed: no ghost";
    const game::Scene with
        = scene_armed(sim, armed(UnitKind::TrainingCentre, false), t.x + 8, t.y + 8);
    EXPECT_FALSE(ghost_of(with).empty()) << "armed: a ghost";
    // The armed DEPLOY (the recycler's grid, no building named) draws no ghost.
    game::SceneOptions deploy = view_of(static_cast<float>(t.x + 8), static_cast<float>(t.y + 8));
    deploy.deploy_preview = game::DeployPreview{sim::footprint_at(t.x + 8, t.y + 8), true};
    EXPECT_TRUE(ghost_of(game::build_scene(sim, deploy)).empty());
}

TEST(PlacementGhost, IsTintedByTheSimsOwnVerdictAsTheSiteMovesBetweenLegalAndIllegal) {
    const Tile t = open_rect(40, 24, 2);
    ASSERT_GE(t.x, 0);
    // A standing Factory (8 x 8) centred at (x + 30, y + 12): a Training Centre site clear of it is
    // legal; one overlapping it is refused. The SAME armed building, only the site moves.
    const sim::Sim sim = world({{UnitKind::Factory, Side::Player, t.x + 30, t.y + 12},
                                {UnitKind::Tank, Side::Player, t.x, t.y}});
    const game::Placing centre = armed(UnitKind::TrainingCentre, false);
    const auto tint_at = [&](int x, int y, bool& verdict) {
        game::SceneOptions options = view_of(static_cast<float>(x), static_cast<float>(y));
        options.deploy_preview = game::placement_preview(sim, centre, x, y);
        verdict = options.deploy_preview->valid;
        const std::vector<game::SceneVertex> ghost = ghost_of(game::build_scene(sim, options));
        EXPECT_FALSE(ghost.empty());
        return ghost.empty() ? std::array<float, 3>{} : ghost.front().colour;
    };
    bool legal = false;
    bool illegal = true;
    const std::array<float, 3> green = tint_at(t.x + 6, t.y + 6, legal);
    const std::array<float, 3> red = tint_at(t.x + 28, t.y + 10, illegal);
    ASSERT_TRUE(legal) << "the clear site must be one the sim allows";
    ASSERT_FALSE(illegal) << "the site on the factory must be one the sim refuses";
    EXPECT_EQ(green, (std::array<float, 3>{game::kDeployPreviewColour.at(0),
                                           game::kDeployPreviewColour.at(1),
                                           game::kDeployPreviewColour.at(2)}));
    EXPECT_EQ(red, (std::array<float, 3>{game::kDeployPreviewBlockedColour.at(0),
                                         game::kDeployPreviewBlockedColour.at(1),
                                         game::kDeployPreviewBlockedColour.at(2)}));
    EXPECT_GT(green.at(1), green.at(0));
    EXPECT_GT(red.at(0), red.at(1));
    // And back: the tint follows the site, not the first frame.
    bool again = false;
    EXPECT_EQ(tint_at(t.x + 6, t.y + 6, again), green);
    EXPECT_TRUE(again);
}

TEST(PlacementGhost, TurnsWithTheSiteSoItsExtentAcrossAndAlongSwap) {
    const Tile t = open_rect(24, 24, 2);
    ASSERT_GE(t.x, 0);
    const sim::Sim sim = world({{UnitKind::Tank, Side::Player, t.x, t.y}});
    const Extent flat = extent_of(
        ghost_of(scene_armed(sim, armed(UnitKind::TrainingCentre, false), t.x + 8, t.y + 8)));
    const Extent turned = extent_of(
        ghost_of(scene_armed(sim, armed(UnitKind::TrainingCentre, true), t.x + 8, t.y + 8)));
    EXPECT_GT(flat.across, flat.along) << "an 8 x 4 lies along x";
    EXPECT_GT(turned.along, turned.across) << "turned a quarter it lies along z";
    EXPECT_NEAR(turned.along, flat.across, 1.0F);
    EXPECT_NEAR(turned.across, flat.along, 1.0F);
}

TEST(PlacementGhost, IsAnOutlineOfTheSameModelNotTheSolidOne) {
    const Tile t = open_rect(24, 24, 2);
    ASSERT_GE(t.x, 0);
    const sim::Sim empty = world({});
    // The same building, standing, at the site the ghost marks (a spawn's tile is its footprint's
    // north-west corner).
    const sim::Sim built = world({{UnitKind::TrainingCentre, Side::Player, t.x + 8, t.y + 8}});
    const game::SceneOptions options
        = view_of(static_cast<float>(t.x + 8), static_cast<float>(t.y + 8));
    const game::Scene solid_scene = game::build_scene(built, options);
    const game::Scene bare = game::build_scene(empty, options);
    const std::vector<game::SceneVertex> ghost
        = ghost_of(scene_armed(empty, armed(UnitKind::TrainingCentre, false), t.x + 8, t.y + 8));
    ASSERT_FALSE(ghost.empty());
    std::vector<game::SceneVertex> solid;
    for (std::size_t i = bare.world_vertex_count + game::kShadowVertexCount;
         i < solid_scene.world_vertex_count; ++i) {
        solid.push_back(solid_scene.vertices.at(i));
    }
    ASSERT_FALSE(solid.empty());
    // Outline, not surface: every ghost vertex is a translucent ribbon corner with no normal.
    for (const game::SceneVertex& v : ghost) {
        EXPECT_LT(v.alpha, 1.0F);
        EXPECT_EQ(v.normal, (std::array<float, 3>{}));
    }
    // None of the ghost's triangles is one of the solid model's: no vertex position is shared.
    for (const game::SceneVertex& g : ghost) {
        for (const game::SceneVertex& s : solid) {
            ASSERT_FALSE(std::abs(g.position.at(0) - s.position.at(0)) < 1e-4F
                         && std::abs(g.position.at(1) - s.position.at(1)) < 1e-4F
                         && std::abs(g.position.at(2) - s.position.at(2)) < 1e-4F);
        }
    }
    // ...yet it is the SAME SHAPE: the same extent across, along and up, to within a ribbon's
    // width.
    const Extent drawn = extent_of(ghost);
    const Extent real = extent_of(solid);
    EXPECT_NEAR(drawn.across, real.across, 0.5F);
    EXPECT_NEAR(drawn.along, real.along, 0.5F);
    float ghost_top = -1.0e9F;
    float solid_top = -1.0e9F;
    for (const game::SceneVertex& v : ghost) {
        ghost_top = std::max(ghost_top, v.position.at(1));
    }
    for (const game::SceneVertex& v : solid) {
        solid_top = std::max(solid_top, v.position.at(1));
    }
    float ghost_low = 1.0e9F;
    float solid_low = 1.0e9F;
    for (const game::SceneVertex& v : ghost) {
        ghost_low = std::min(ghost_low, v.position.at(1));
    }
    for (const game::SceneVertex& v : solid) {
        solid_low = std::min(solid_low, v.position.at(1));
    }
    EXPECT_NEAR(ghost_low, solid_low, 0.5F);
    EXPECT_NEAR(ghost_top, solid_top, 0.5F);
}

TEST(PlacementGhost, DrawingItMovesNoStateAndTheSceneBuiltTwiceHashesAlike) {
    const Tile t = open_rect(24, 24, 2);
    ASSERT_GE(t.x, 0);
    const sim::Sim sim = world({{UnitKind::Tank, Side::Player, t.x, t.y}});
    const std::uint64_t before = sim.state_hash();
    const game::Placing placing = armed(UnitKind::TrainingCentre, true);
    const game::Scene first = scene_armed(sim, placing, t.x + 8, t.y + 8);
    const std::uint64_t middle = sim.state_hash();
    const game::Scene second = scene_armed(sim, placing, t.x + 8, t.y + 8);
    EXPECT_EQ(before, middle);
    EXPECT_EQ(middle, sim.state_hash());
    EXPECT_EQ(first.vertices.size(), second.vertices.size()) << "and the drawing is the same too";
    EXPECT_FALSE(ghost_of(first).empty());
}

} // namespace
} // namespace protekt
