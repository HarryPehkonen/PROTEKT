// ONE SHAPE PER KIND: the world mesh and the 72 px plate under the command row are both derived
// from `shape_of(kind)`, so they cannot disagree. These tests pin each shape's size in metres and
// then check that the plate and the mesh, measured SEPARATELY from what they emit, both say it.
// Changing one number in `src/game/vehicle_shape.cpp` therefore moves the plate test and the mesh
// test together. Two kinds are authored so far, the scavenger and the tank; every other kind has no
// shape and keeps drawing what it always drew. Presentation only: the sim is never touched.
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
#include "protekt/game/scene.hpp"
#include "protekt/game/vehicle_shape.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::game {
namespace {

using sim::Side;
using sim::Spawn;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr float kInfinity = std::numeric_limits<float>::max();

// The shape's size, in metres, as the brief states it. A number of the source that moves breaks
// these, the plate's and the mesh's together.
constexpr float kLengthMetres = 3.13F; ///< rear of the tracks (-1.25) to the pincer's tip (1.88)
constexpr float kHeightMetres = 1.79F; ///< the ground to the top of the boom's elbow pin
constexpr float kWidthMetres = 1.24F;  ///< hub to hub across the tracks
// The tank's, measured before this slice on the hand-built body and kept: 2.6 m of track, 1.2 m
// across the fenders (the tank's own half-width, 0.6 m, either side), and 1.3 m to the top of the
// commander's hatch (the turret's roof, 1.2 m, was the old top).
constexpr float kTankLengthMetres = 2.6F;
constexpr float kTankHeightMetres = 1.3F;
constexpr float kTankWidthMetres = 1.2F;

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

/// A vehicle is leaned to the ground it stands on, which would shear its bounds, so the machine is
/// measured on ground within a degree of level. The sim nudges a spawn to the nearest free tile,
/// so the ground that counts is the unit's own: the first tile whose `kind` ends up on level
/// ground.
[[nodiscard]] sim::Sim level_world(UnitKind kind) {
    constexpr float kLevelSine = 0.02F;
    for (int ty = 40; ty < 90; ++ty) {
        for (int tx = 40; tx < 90; ++tx) {
            sim::Sim sim = sim::Sim::create(
                sim::Scenario{kSeed, std::vector<Spawn>{{kind, Side::Player, tx, ty}}});
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

/// One vertex of a machine as the frame draws it, in the machine's own frame: `across` and `along`
/// from its centre, `up` over the ground it stands on.
struct BodyVertex {
    float across;
    float up;
    float along;
};

/// A lone human `kind` on level ground, faced exactly 0 (so along is z and across is x) with its
/// turret at `turret` radians, measured from the vertices alone: the vertices it adds to the frame,
/// its shadow left out by taking only what stands over the ground.
[[nodiscard]] std::vector<BodyVertex> body_of(UnitKind kind, float turret = 0.0F) {
    const sim::Sim with = level_world(kind);
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
    options.facing = SceneOptions::Facing{unit.id, 0.0F, turret, 0.0F}; // exactly along +z
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
    std::vector<BodyVertex> body;
    body.reserve(scene.vertices.size() - head - tail);
    for (std::size_t i = head; i + tail < scene.vertices.size(); ++i) {
        const auto& p = scene.vertices.at(i).position;
        if (p.at(1) < ground + 0.05F) {
            continue; // the shadow lies 3 cm over the ground (the machine's bottom is lost with it)
        }
        body.push_back(BodyVertex{p.at(0) - x, p.at(1) - ground, p.at(2) - z});
    }
    return body;
}

struct MeshBox {
    std::array<float, 3> low{kInfinity, kInfinity, kInfinity};
    std::array<float, 3> high{-kInfinity, -kInfinity, -kInfinity};
    std::size_t vertices = 0;
    std::set<int> layers; ///< the distinct lateral positions, in centimetres
    [[nodiscard]] float across() const { return high.at(0) - low.at(0); }
    [[nodiscard]] float height() const { return high.at(1) - low.at(1); }
    [[nodiscard]] float along() const { return high.at(2) - low.at(2); }
};

[[nodiscard]] MeshBox mesh_of(UnitKind kind) {
    MeshBox box;
    for (const BodyVertex& v : body_of(kind)) {
        ++box.vertices;
        const std::array<float, 3> at{v.across, v.up, v.along};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            box.low.at(axis) = std::min(box.low.at(axis), at.at(axis));
            box.high.at(axis) = std::max(box.high.at(axis), at.at(axis));
        }
        box.layers.insert(static_cast<int>(std::lround(v.across * 100.0F)));
    }
    return box;
}

/// Every kind the sim has, by index.
[[nodiscard]] std::vector<UnitKind> every_kind() {
    std::vector<UnitKind> kinds;
    kinds.reserve(sim::kUnitKindCount);
    for (int i = 0; i < sim::kUnitKindCount; ++i) {
        kinds.push_back(static_cast<UnitKind>(i));
    }
    return kinds;
}

/// Every part of the shape is a convex profile with room for its bevel: the mesh insets each
/// profile by its bevel with a miter at every corner, which is right only for a convex profile; and
/// a bevel must leave a lateral middle, or the faces cross.
void expect_parts_convex_with_room(UnitKind kind) {
    ASSERT_FALSE(shape_of(kind).empty());
    for (const ShapePart& part : shape_of(kind)) {
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

// ------------------------------------------------------------------------- the mechanism

TEST(VehicleShape, AKindWithNoShapeHasNoneAndIsNeverAnotherKindsShape) {
    // `shape_of` says "not converted" with an EMPTY list, and nothing about the other queries can
    // turn it into a silhouette: no coverage, no extent. Callers test `.empty()` to keep their own
    // recipe, so an unconverted kind keeps drawing exactly what it drew.
    std::size_t authored = 0;
    for (const UnitKind kind : every_kind()) {
        if (kind == UnitKind::Scavenger || kind == UnitKind::Tank) {
            EXPECT_FALSE(shape_of(kind).empty());
            ++authored;
            continue;
        }
        EXPECT_TRUE(shape_of(kind).empty()) << "kind " << static_cast<int>(kind);
        EXPECT_FALSE(shape_covers(kind, 0.0F, 0.3F)) << "kind " << static_cast<int>(kind);
        const ShapeExtent extent = shape_extent(kind);
        EXPECT_EQ(extent.along_max - extent.along_min, 0.0F);
        EXPECT_EQ(extent.up_max, 0.0F);
        EXPECT_EQ(extent.across_max, 0.0F);
    }
    EXPECT_EQ(authored, std::size_t{2}) << "the scavenger and the tank, so far";
    EXPECT_NE(&shape_of(UnitKind::Scavenger), &shape_of(UnitKind::Tank));
}

TEST(VehicleShape, ThePlateOfAnUnconvertedKindIsStillItsOwnDrawing) {
    // The scout is not a tank: it has no shape, so its plate is not the tank's projection.
    EXPECT_NE(&kind_icon(UnitKind::Scout), &kind_icon(UnitKind::Tank));
    EXPECT_NE(kind_icon(UnitKind::Scout), kind_icon(UnitKind::Tank));
    EXPECT_NE(kind_icon(UnitKind::AssaultTank), kind_icon(UnitKind::Tank));
}

// ------------------------------------------------------------------------- the scavenger

TEST(ScavengerShape, TheShapeIsAsLargeInMetresAsItWasAuthored) {
    const ShapeExtent extent = shape_extent(UnitKind::Scavenger);
    EXPECT_NEAR(extent.along_max - extent.along_min, kLengthMetres, 0.005F);
    EXPECT_NEAR(extent.up_max, kHeightMetres, 0.005F);
    EXPECT_NEAR(2.0F * extent.across_max, kWidthMetres, 0.005F);
}

TEST(ScavengerShape, EveryPartIsConvexAndHasRoomForItsBevel) {
    expect_parts_convex_with_room(UnitKind::Scavenger);
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
    const MeshBox mesh = mesh_of(UnitKind::Scavenger);
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
    const MeshBox mesh = mesh_of(UnitKind::Scavenger);
    const float plate_ratio
        = static_cast<float>(plate.height()) / static_cast<float>(plate.width());
    const float mesh_ratio = mesh.height() / mesh.along();
    EXPECT_NEAR(plate_ratio, mesh_ratio, 0.04F);
}

TEST(ScavengerShape, TheMeshHasDepthAndIsNotACutOut) {
    const MeshBox mesh = mesh_of(UnitKind::Scavenger);
    // A flat side-on cut-out is a few centimetres thick. This is a vehicle as wide as a tank, built
    // of parts at many lateral positions -- the pods, the floor, the hopper's walls, the cab, the
    // boom's two arms, the pincer between them -- each chamfered.
    EXPECT_GT(mesh.across(), 1.0F);
    EXPECT_GE(mesh.layers.size(), std::size_t{40}) << "distinct lateral positions, in centimetres";
    EXPECT_GT(mesh.vertices, std::size_t{1500}) << "a cut-out is a few dozen";
    // Every body part -- the pods, the floor, the hopper, its scrap, the cab -- is chamfered; only
    // the wheels' hubs and rings (small round things) and the glass inlay are plain extrusions.
    std::size_t body = 0;
    for (const ShapePart& part : shape_of(UnitKind::Scavenger)) {
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

// ------------------------------------------------------------------------- the tank

TEST(TankFromShape, TheShapeIsAsLargeInMetresAsItWasAuthored) {
    const ShapeExtent extent = shape_extent(UnitKind::Tank);
    EXPECT_NEAR(extent.along_max - extent.along_min, kTankLengthMetres, 0.005F);
    EXPECT_NEAR(extent.up_max, kTankHeightMetres, 0.005F);
    EXPECT_NEAR(2.0F * extent.across_max, kTankWidthMetres, 0.005F);
}

TEST(TankFromShape, EveryPartIsConvexAndHasRoomForItsBevel) {
    expect_parts_convex_with_room(UnitKind::Tank);
}

TEST(TankFromShape, ThePlateIsTheShapeSeenSideOnPlusItsBarrel) {
    const PlateBox box = plate_box(kind_icon(UnitKind::Tank));
    // The plate holds the hull's rear and the barrel's tip, one cell in from each edge.
    const float reach = kTankBarrelTip - shape_extent(UnitKind::Tank).along_min;
    EXPECT_GE(box.width(), static_cast<int>(kIconCells) - 4);
    EXPECT_LE(box.width(), static_cast<int>(kIconCells) - 2);
    const float scale = static_cast<float>(box.width()) / reach;
    EXPECT_NEAR(static_cast<float>(box.height()), kTankHeightMetres * scale, 1.5F);
    EXPECT_EQ(box.last_row, static_cast<int>(kIconGround)) << "the tracks stand on the ground line";
}

TEST(TankFromShape, TheMeshIsTheShapeInMetres) {
    const MeshBox mesh = mesh_of(UnitKind::Tank);
    ASSERT_GT(mesh.vertices, 0U);
    // The body is the shape; the barrel, which is not a part of it, runs on to `kTankBarrelTip`.
    const ShapeExtent extent = shape_extent(UnitKind::Tank);
    EXPECT_NEAR(mesh.along(), kTankBarrelTip - extent.along_min, 0.06F);
    EXPECT_NEAR(mesh.height(), kTankHeightMetres, 0.06F);
    EXPECT_NEAR(mesh.across(), kTankWidthMetres, 0.06F);
}

TEST(TankFromShape, ThePlateAndTheMeshAreTheSameMachine) {
    const PlateBox plate = plate_box(kind_icon(UnitKind::Tank));
    const MeshBox mesh = mesh_of(UnitKind::Tank);
    const float plate_ratio
        = static_cast<float>(plate.height()) / static_cast<float>(plate.width());
    EXPECT_NEAR(plate_ratio, mesh.height() / mesh.along(), 0.04F);
}

TEST(TankFromShape, TheBarrelIsOnThePlateAndNotInTheShape) {
    // The plate's last lit column is the muzzle, past anything the shape covers.
    const ShapeExtent extent = shape_extent(UnitKind::Tank);
    EXPECT_FALSE(shape_covers(UnitKind::Tank, extent.along_max + 0.3F, 1.05F));
    const IconMask& mask = kind_icon(UnitKind::Tank);
    const PlateBox box = plate_box(mask);
    bool muzzle = false;
    for (std::size_t row = 0; row < kIconCells; ++row) {
        muzzle = muzzle || mask.at(row).at(static_cast<std::size_t>(box.last_col)) == '#';
    }
    EXPECT_TRUE(muzzle);
}

TEST(TankFromShape, TheMeshHasDepthAndIsNotACutOut) {
    const MeshBox mesh = mesh_of(UnitKind::Tank);
    EXPECT_GT(mesh.across(), 1.0F);
    EXPECT_GE(mesh.layers.size(), std::size_t{25}) << "distinct lateral positions, in centimetres";
    EXPECT_GT(mesh.vertices, std::size_t{1000});
    // The body parts, the tracks, hull, fenders, housing and bustle, are chamfered.
    std::size_t body = 0;
    for (const ShapePart& part : shape_of(UnitKind::Tank)) {
        if (part.lit && (part.role == ShapeRole::Hull || part.role == ShapeRole::Track)
            && part.profile.size() > 4U) {
            ++body;
        }
        if (part.lit && part.role == ShapeRole::Hull) {
            EXPECT_GT(part.bevel, 0.0F) << "a body part with a square edge";
        }
    }
    EXPECT_GE(body, std::size_t{1});
}

TEST(TankFromShape, TheGroupsAreTheMachinesOwn) {
    // An oracle that does not read the tag: whatever stands wholly on or over the turret's base is
    // the turret, and whatever reaches below it is the hull.
    std::size_t turret_parts = 0;
    for (const ShapePart& part : shape_of(UnitKind::Tank)) {
        float lowest = kInfinity;
        for (const auto& corner : part.profile) {
            lowest = std::min(lowest, corner[1]);
        }
        const bool on_the_turret = lowest >= kTankTurretBase - 0.001F;
        EXPECT_EQ(part.group == ShapeGroup::Turret, on_the_turret);
        turret_parts += on_the_turret ? 1U : 0U;
    }
    EXPECT_GE(turret_parts, std::size_t{4}) << "housing, bustle, hatch and its slit";
    for (const ShapePart& part : shape_of(UnitKind::Scavenger)) {
        EXPECT_EQ(part.group, ShapeGroup::Hull) << "nothing on a scavenger turns";
    }
}

/// Does the point (in the machine's own frame, at turret 0) lie inside the box of a part, give or
/// take `slack` metres?
[[nodiscard]] bool in_box_of(const ShapePart& part, const BodyVertex& v, float slack) {
    float along_lo = kInfinity;
    float along_hi = -kInfinity;
    float up_lo = kInfinity;
    float up_hi = -kInfinity;
    for (const auto& corner : part.profile) {
        along_lo = std::min(along_lo, corner[0]);
        along_hi = std::max(along_hi, corner[0]);
        up_lo = std::min(up_lo, corner[1]);
        up_hi = std::max(up_hi, corner[1]);
    }
    const bool across = (v.across >= part.across_lo - slack && v.across <= part.across_hi + slack)
                        || (part.mirrored && -v.across >= part.across_lo - slack
                            && -v.across <= part.across_hi + slack);
    return across && v.along >= along_lo - slack && v.along <= along_hi + slack
           && v.up >= up_lo - slack && v.up <= up_hi + slack;
}

TEST(TankFromShape, TheGroupIsRealTurnTheTurretAndTheTurretPartsMoveAndTheHullDoesNot) {
    constexpr float kTurn = 1.0F;
    const std::vector<BodyVertex> before = body_of(UnitKind::Tank, 0.0F);
    const std::vector<BodyVertex> after = body_of(UnitKind::Tank, kTurn);
    ASSERT_EQ(before.size(), after.size());
    // The ground under a tank is within a degree of level, which leans the body by up to 2.6 cm
    // over its length: a vertex within 4 cm of a part's box is that part's. A vertex in the boxes
    // of both groups is ambiguous and judged by neither.
    constexpr float kSlack = 0.04F;
    std::size_t hull_only = 0;
    std::size_t turret_only = 0;
    for (std::size_t i = 0; i < before.size(); ++i) {
        const BodyVertex& v = before.at(i);
        const BodyVertex& w = after.at(i);
        bool in_hull = false;
        bool in_turret = false;
        for (const ShapePart& part : shape_of(UnitKind::Tank)) {
            if (in_box_of(part, v, kSlack)) {
                (part.group == ShapeGroup::Turret ? in_turret : in_hull) = true;
            }
        }
        const bool moved = std::abs(v.across - w.across) > 1.0e-4F
                           || std::abs(v.along - w.along) > 1.0e-4F
                           || std::abs(v.up - w.up) > 1.0e-4F;
        if (in_hull && !in_turret) {
            ++hull_only;
            EXPECT_FALSE(moved) << "a Hull vertex moved with the turret: vertex " << i;
        } else if (in_turret && !in_hull && std::hypot(v.across, v.along) > 0.05F) {
            ++turret_only;
            EXPECT_TRUE(moved) << "a Turret vertex stayed put when the turret turned: vertex " << i;
        }
    }
    EXPECT_GT(hull_only, std::size_t{300}) << "the tracks, wheels and hull";
    EXPECT_GT(turret_only, std::size_t{100}) << "the housing, bustle and hatch";
}

} // namespace
} // namespace protekt::game
