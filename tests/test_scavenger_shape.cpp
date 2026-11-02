// The scavenger's ONE shape: the world mesh and the 72 px plate under the command row are both
// derived from `scavenger_shape()`, so they cannot disagree. These tests pin the shape's size in
// metres and then check that the plate and the mesh, measured SEPARATELY from what they emit, both
// say it. Changing one number in `src/game/scavenger_shape.cpp` therefore moves the plate test and
// the mesh test together. Presentation only: the sim is never touched.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <string_view>
#include <vector>

#include "protekt/game/kind_icon.hpp"
#include "protekt/game/scavenger_shape.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::game {
namespace {

using sim::Side;
using sim::Spawn;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;

// The shape's size, in metres, as the brief states it. A number of the source that moves breaks
// these, the plate's and the mesh's together.
constexpr float kLengthMetres = 3.13F; ///< rear of the tracks (-1.25) to the pincer's tip (1.88)
constexpr float kHeightMetres = 1.79F; ///< the ground to the top of the boom's elbow pin
constexpr float kWidthMetres = 1.24F;  ///< hub to hub across the tracks

struct PlateBox {
    int first_col = std::numeric_limits<int>::max();
    int last_col = -1;
    int first_row = std::numeric_limits<int>::max();
    int last_row = -1;
    [[nodiscard]] int width() const { return last_col - first_col + 1; }
    [[nodiscard]] int height() const { return last_row - first_row + 1; }
};

[[nodiscard]] PlateBox plate_box(const IconMask& mask) {
    PlateBox box;
    for (std::size_t row = 0; row < kIconCells; ++row) {
        for (std::size_t col = 0; col < kIconCells; ++col) {
            if (mask.at(row).at(col) == '#') {
                box.first_col = std::min(box.first_col, static_cast<int>(col));
                box.last_col = std::max(box.last_col, static_cast<int>(col));
                box.first_row = std::min(box.first_row, static_cast<int>(row));
                box.last_row = std::max(box.last_row, static_cast<int>(row));
            }
        }
    }
    return box;
}

struct MeshBox {
    std::array<float, 3> low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max()};
    std::array<float, 3> high{std::numeric_limits<float>::lowest(),
                              std::numeric_limits<float>::lowest(),
                              std::numeric_limits<float>::lowest()};
    std::size_t vertices = 0;
    std::set<int> layers; ///< the distinct lateral positions, in centimetres
    [[nodiscard]] float across() const { return high.at(0) - low.at(0); }
    [[nodiscard]] float height() const { return high.at(1) - low.at(1); }
    [[nodiscard]] float along() const { return high.at(2) - low.at(2); }
};

/// A vehicle is leaned to the ground it stands on, which would shear its bounds, so the machine is
/// measured on ground within a degree of level. The sim nudges a spawn to the nearest free tile,
/// so the ground that counts is the unit's own: the first tile whose scavenger ends up on level
/// ground.
[[nodiscard]] sim::Sim level_world() {
    constexpr float kLevelSine = 0.02F;
    for (int ty = 40; ty < 90; ++ty) {
        for (int tx = 40; tx < 90; ++tx) {
            sim::Sim sim = sim::Sim::create(sim::Scenario{
                kSeed, std::vector<Spawn>{{UnitKind::Scavenger, Side::Player, tx, ty}}});
            const sim::Unit& unit = sim.units().front();
            const math::Vec3 normal = ground_normal(kSeed, unit.x, unit.y);
            if (std::hypot(normal.at(0), normal.at(2)) < kLevelSine) {
                return sim;
            }
        }
    }
    ADD_FAILURE() << "no level ground within reach";
    return sim::Sim::create(sim::Scenario{kSeed, {}});
}

/// The scavenger's body as the frame draws it, measured from the vertices alone: a lone human
/// scavenger on level ground, faced exactly 0 (so along is z and across is x), its shadow left out
/// by taking only what stands over the ground.
[[nodiscard]] MeshBox mesh_of_a_scavenger() {
    const sim::Sim with = level_world();
    const sim::Sim empty = sim::Sim::create(sim::Scenario{kSeed, {}});
    const sim::Unit& unit = with.units().front();
    const float x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = ground_metres(kSeed, unit.x, unit.y);
    SceneOptions options;
    options.seed = kSeed;
    options.radius_tiles = 8;
    options.show_ship_gauge = false;
    options.show_ledger = false;
    options.factions = Factions{Faction::Human, Faction::Human};
    options.facing = SceneOptions::Facing{unit.id, 0.0F, 0.0F, 0.0F}; // exactly along +z
    options.camera.x = x;
    options.camera.z = z - 8.0F;
    options.camera.y = ground + 5.0F;
    options.camera.pitch = -0.25F;
    const Scene base = build_scene(empty, options);
    const Scene scene = build_scene(with, options);
    std::size_t head = 0;
    const auto same = [](const SceneVertex& a, const SceneVertex& b) {
        return a.position == b.position && a.colour == b.colour && a.alpha == b.alpha && a.u == b.u
               && a.v == b.v && a.fog == b.fog;
    };
    while (head < base.vertices.size() && head < scene.vertices.size()
           && same(base.vertices.at(head), scene.vertices.at(head))) {
        ++head;
    }
    std::size_t tail = 0;
    while (tail < base.vertices.size() - head && tail < scene.vertices.size() - head
           && same(base.vertices.at(base.vertices.size() - 1 - tail),
                   scene.vertices.at(scene.vertices.size() - 1 - tail))) {
        ++tail;
    }
    MeshBox box;
    for (std::size_t i = head; i + tail < scene.vertices.size(); ++i) {
        const auto& p = scene.vertices.at(i).position;
        if (p.at(1) < ground + 0.05F) {
            continue; // the shadow lies 3 cm over the ground (the machine's bottom is lost with it)
        }
        ++box.vertices;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            box.low.at(axis) = std::min(box.low.at(axis), p.at(axis) - (axis == 1 ? ground : 0.0F));
            box.high.at(axis)
                = std::max(box.high.at(axis), p.at(axis) - (axis == 1 ? ground : 0.0F));
        }
        box.layers.insert(static_cast<int>(std::lround((p.at(0) - x) * 100.0F)));
    }
    // Along and across are measured from the machine's own centre, not the world's.
    box.low.at(0) -= x;
    box.high.at(0) -= x;
    box.low.at(2) -= z;
    box.high.at(2) -= z;
    return box;
}

TEST(ScavengerShape, TheShapeIsAsLargeInMetresAsItWasAuthored) {
    const ShapeExtent extent = scavenger_extent();
    EXPECT_NEAR(extent.along_max - extent.along_min, kLengthMetres, 0.005F);
    EXPECT_NEAR(extent.up_max, kHeightMetres, 0.005F);
    EXPECT_NEAR(2.0F * extent.across_max, kWidthMetres, 0.005F);
}

TEST(ScavengerShape, EveryPartIsConvexAndHasRoomForItsBevel) {
    // The mesh insets each profile by its bevel with a miter at every corner, which is right only
    // for a convex profile; and a bevel must leave a lateral middle, or the faces cross.
    for (const ShapePart& part : scavenger_shape()) {
        ASSERT_GE(part.profile.size(), 3U);
        for (std::size_t i = 0; i < part.profile.size(); ++i) {
            const auto& a = part.profile.at(i);
            const auto& b = part.profile.at((i + 1) % part.profile.size());
            const auto& c = part.profile.at((i + 2) % part.profile.size());
            const float turn = ((b[0] - a[0]) * (c[1] - b[1])) - ((b[1] - a[1]) * (c[0] - b[0]));
            EXPECT_GE(turn, 0.0F) << "a reflex corner: the profiles are counter-clockwise convex";
        }
        EXPECT_GT(part.across_hi - part.across_lo, 2.0F * part.bevel);
    }
}

TEST(ScavengerShape, ThePlateIsTheShapeSeenSideOn) {
    const PlateBox box = plate_box(kind_icon(UnitKind::Scavenger));
    // The plate is fitted to the box by length with a one-cell margin each side, so the shape's
    // length IS the plate's width, and its height follows from the shape's proportions.
    EXPECT_EQ(box.width(), static_cast<int>(kIconCells) - 2);
    const float scale = static_cast<float>(box.width()) / kLengthMetres;
    EXPECT_NEAR(static_cast<float>(box.height()), kHeightMetres * scale, 1.5F);
    EXPECT_EQ(box.last_row, static_cast<int>(kIconGround)) << "the tracks stand on the ground line";
}

TEST(ScavengerShape, TheMeshIsTheShapeInMetres) {
    const MeshBox mesh = mesh_of_a_scavenger();
    ASSERT_GT(mesh.vertices, 0U);
    // The machine leans to the ground it stands on, so the bounds are the shape's within a few
    // percent, never a different size.
    EXPECT_NEAR(mesh.along(), kLengthMetres, kLengthMetres * 0.06F);
    EXPECT_NEAR(mesh.height(), kHeightMetres, kHeightMetres * 0.06F);
    EXPECT_NEAR(mesh.across(), kWidthMetres, kWidthMetres * 0.06F);
}

TEST(ScavengerShape, ThePlateAndTheMeshAreTheSameMachine) {
    // Measured separately and compared: the plate's proportions (cells) against the mesh's
    // (metres). They agree because both came from the one source, and either drifting from it
    // breaks this.
    const PlateBox plate = plate_box(kind_icon(UnitKind::Scavenger));
    const MeshBox mesh = mesh_of_a_scavenger();
    const float plate_ratio
        = static_cast<float>(plate.height()) / static_cast<float>(plate.width());
    const float mesh_ratio = mesh.height() / mesh.along();
    EXPECT_NEAR(plate_ratio, mesh_ratio, 0.04F);
}

TEST(ScavengerShape, TheMeshHasDepthAndIsNotACutOut) {
    const MeshBox mesh = mesh_of_a_scavenger();
    // A flat side-on cut-out is a few centimetres thick. This is a vehicle as wide as a tank, built
    // of parts at many lateral positions -- the pods, the floor, the hopper's walls, the cab, the
    // boom's two arms, the pincer between them -- each chamfered.
    EXPECT_GT(mesh.across(), 1.0F);
    EXPECT_GE(mesh.layers.size(), std::size_t{40}) << "distinct lateral positions, in centimetres";
    EXPECT_GT(mesh.vertices, std::size_t{1500}) << "a cut-out is a few dozen";
    // Every body part -- the pods, the floor, the hopper, its scrap, the cab -- is chamfered; only
    // the wheels' hubs and rings (small round things) and the glass inlay are plain extrusions.
    std::size_t body = 0;
    for (const ShapePart& part : scavenger_shape()) {
        if (part.lit
            && (part.role == ShapeRole::Hull || part.role == ShapeRole::Track
                || part.role == ShapeRole::Load)) {
            ++body;
            EXPECT_GT(part.bevel, 0.0F) << "a body part with a square edge";
        }
    }
    EXPECT_GE(body, std::size_t{6});
}

TEST(ScavengerShape, TheDeployedScavengerWearsThePlateToo) {
    EXPECT_EQ(&kind_icon(UnitKind::DeployedScavenger), &kind_icon(UnitKind::Scavenger));
}

} // namespace
} // namespace protekt::game
