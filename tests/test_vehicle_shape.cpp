// ONE SHAPE PER KIND: the world mesh and the 72 px plate under the command row are both derived
// from `shape_of(kind)`, so they cannot disagree. These tests pin each shape's size in metres and
// then check that the plate and the mesh, measured SEPARATELY from what they emit, both say it.
// Changing one number in `src/game/vehicle_shape.cpp` therefore moves the plate test and the mesh
// test together. Every vehicle is authored -- the scavenger, the tank, the hover tank, the scout,
// the missile scout, the rocket tank, the assault tank, the mortar bike, the walker, the APC, the
// bomber and the defence turret (the harvester shares the scavenger's kind and plate; the deployed
// turret is the turret's kind in a new state and shares its shape); the buildings and the recycler
// have no shape and keep drawing what they always drew. Presentation only: the sim is never
// touched.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
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
// The hover tank's, measured before this slice on the hand-built body and kept: 3.106 m from the
// tail to the barrel's tip, 1.203 m across at the tail, 1.120 m to the top of the fin. Its shape
// alone, without the barrel, runs from the tail (-1.3) to the nose (1.5): 2.8 m.
constexpr float kHoverLengthMetres = 3.1F;
constexpr float kHoverBodyMetres = 2.8F;
constexpr float kHoverHeightMetres = 1.1F;
constexpr float kHoverWidthMetres = 1.2F;
constexpr float kHoverAirMetres = 0.15F; ///< the skirt's bottom over the ground: the air gap
// The scout's and the missile scout's, which wore the tank's hand-built body: measured before, a
// mesh 3.312 m long (a 2 m barrel they never fired), 1.240 m high and 1.201 m across, and a plate
// 62 cells wide by 48 (scout) or 45 (missile scout) high. Their shapes carry no gun: 2.55 m from
// the rear of the tail (-1.25) to the nose (1.3), 1.2 m across the wheels' hubs.
constexpr float kScoutLengthMetres = 2.55F;
constexpr float kScoutWidthMetres = 1.2F;
constexpr float kScoutHeightMetres = 1.3F;     ///< the radar dish's top
constexpr float kMslScoutHeightMetres = 1.24F; ///< the missile noses' tips
constexpr float kMeshWasLongMetres = 3.312F;
// The rocket tank's, measured before this slice on the hand-built body: a mesh 2.601 m long (-1.294
// to 1.307), 1.201 m across and 1.195 m to the top of the rack, on a plate 58 cells wide by 52
// high. Its shape is the tank's chassis (2.6 m of track, 1.2 m across the fenders) under a rack
// that tops out at 1.15 m, with the 3 cm team-colour panel lying on it.
constexpr float kRocketLengthMetres = 2.6F;
constexpr float kRocketWidthMetres = 1.2F;
constexpr float kRocketRackMetres = 1.15F; ///< the rack's top: the shape's highest point
constexpr float kRocketTopMetres = 1.18F;  ///< and the panel on it: the mesh's
constexpr float kRocketWasTopMetres = 1.195F;
// The assault tank's, measured before on the hand-built body: a mesh 3.414 m long (-1.294 to the
// barrel's tip at 2.120), 1.201 m across and 1.543 m to the top of the roof panel, on a plate 68
// cells wide by 57 high. Its shape is the tank's chassis under a block 1.5 m to the roof and a
// hatch to 1.58 m; the barrel (to 2.1 m) and the panel are not parts.
constexpr float kAssaultLengthMetres = 3.4F; ///< the rear of the tracks (-1.3) to the barrel's tip
constexpr float kAssaultWidthMetres = 1.2F;
constexpr float kAssaultHeightMetres = 1.58F;  ///< the shape's highest point: the hatch
constexpr float kAssaultTopMetres = 1.585F;    ///< measured on the mesh, the hatch's top
constexpr float kAssaultWasTopMetres = 1.543F; ///< and what it was

// The mortar bike's, measured before this slice on the hand-built body: a mesh 1.400 m long (-0.694
// to 0.706), 1.200 m across and 1.140 m to the top of the collar, on a plate 66 cells wide by 53
// high. Its shape is a bike: 1.96 m from the rear tyre to the front, 0.7 m across the handlebars
// (the box was as wide as a tank), and 1.10 m to the top of the collar.
constexpr float kMortarLengthMetres = 1.96F;
constexpr float kMortarWidthMetres = 0.7F;
constexpr float kMortarHeightMetres = 1.1F;
constexpr float kMortarWasLongMetres = 1.4F;
constexpr float kMortarWasWideMetres = 1.2F;
constexpr float kMortarWasHighMetres = 1.14F;
// The walker's, measured before on the hand-built body: a mesh 2.305 m long (-0.484 to the gun's
// tip at 1.821), 0.780 m across and 1.739 m to the top of the roof panel, on a plate 63 cells wide
// by 53 high. Its shape is 2.0 m from the rear foot to the front foot, 0.88 m across the feet and
// 1.71 m to the top of the roof panel; the gun arm (to 1.8 m) is not a part.
constexpr float kWalkerBodyMetres = 2.0F;
constexpr float kWalkerWidthMetres = 0.88F;
constexpr float kWalkerHeightMetres = 1.71F;
constexpr float kWalkerGunTipMetres = 1.8F;
constexpr float kWalkerWasHighMetres = 1.739F;
constexpr float kWalkerWasWideMetres = 0.78F;
// The APC's, measured before on the hand-built body: a mesh 3.005 m long (-1.496 to 1.509), 1.200 m
// across and 0.650 m to the top of the roof panel, on a plate 64 cells wide by 42 high. Its shape
// is the same 3.0 m by 1.2 m, and 1.10 m to the top of the turret: it grew 0.45 m taller, because
// the box was lower than the wheels-and-turret machine its plate has always drawn.
constexpr float kApcLengthMetres = 3.0F;
constexpr float kApcWidthMetres = 1.2F;
constexpr float kApcHeightMetres = 1.1F;
constexpr float kApcWasHighMetres = 0.65F;
// The bomber's, measured before this slice on the hand-built body: a mesh 2.004 m long (-0.898 to
// 1.106), 2.402 m across and 0.449 m high (a flat slab, a nose and a panel, 108 vertices), on a
// plate 65 cells wide by 56 high. Its shape is 2.4 m from the tail to the nosecone's tip, the same
// 2.4 m across the wing, and 1.10 m to the top of the tail fin.
constexpr float kBomberLengthMetres = 2.4F;
constexpr float kBomberWidthMetres = 2.4F;
constexpr float kBomberHeightMetres = 1.1F;
constexpr float kBomberWasLongMetres = 2.004F;
constexpr float kBomberWasWideMetres = 2.402F;
constexpr float kBomberWasHighMetres = 0.449F;
// The defence turret's, measured before this slice on BOTH hand-built bodies, which disagreed: the
// undriven one a pedestal and a housing, 2.6 m long (-0.6 to the barrel's tip at 2.0), 1.2 m
// across and 1.8 m high, on a plate 67 cells wide by 36 high; the DRIVEN one a tank's hull under a
// squat turret, 3.3 m long (-1.3 to 2.0), 1.2 m across and 1.23 m high. Its shape is 1.4 m of
// flared base (-0.7 to 0.7), 1.2 m across it and 1.8 m to the top of the aerial; the gun runs on to
// 2.0 m, so the whole is 2.7 m from the rear of the base.
constexpr float kTurretBaseMetres = 1.4F;
constexpr float kTurretWholeMetres = 2.7F;
constexpr float kTurretTipMetres = 2.0F;
constexpr float kTurretWidthMetres = 1.2F;
constexpr float kTurretHeightMetres = 1.8F;
constexpr float kTurretWasLongUndrivenMetres = 2.6F;
constexpr float kTurretWasHighDrivenMetres = 1.23F;

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
[[nodiscard]] std::vector<BodyVertex> body_of(UnitKind kind, float turret = 0.0F,
                                              float pitch = 0.0F) {
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
    options.facing = SceneOptions::Facing{unit.id, 0.0F, turret, pitch}; // exactly along +z
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

/// The kinds that have an authored shape.
constexpr std::array<UnitKind, 17> kShaped{
    UnitKind::Scavenger,      UnitKind::Tank,       UnitKind::HoverTank,   UnitKind::Scout,
    UnitKind::MslScout,       UnitKind::RocketTank, UnitKind::AssaultTank, UnitKind::MortarBike,
    UnitKind::Walker,         UnitKind::Apc,        UnitKind::Bomber,      UnitKind::Turret,
    UnitKind::DeployedTurret, UnitKind::Tug,        UnitKind::Constructor, UnitKind::ServiceTruck,
    UnitKind::ServicePod};

TEST(VehicleShape, AKindWithNoShapeHasNoneAndIsNeverAnotherKindsShape) {
    // `shape_of` says "not converted" with an EMPTY list, and nothing about the other queries can
    // turn it into a silhouette: no coverage, no extent. Callers test `.empty()` to keep their own
    // recipe, so an unconverted kind keeps drawing exactly what it drew.
    std::size_t authored = 0;
    for (const UnitKind kind : every_kind()) {
        if (std::find(kShaped.begin(), kShaped.end(), kind) != kShaped.end()) {
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
    EXPECT_EQ(authored, std::size_t{17})
        << "the scavenger, tank, hover tank, scout, missile scout, rocket tank, assault tank, "
           "mortar bike, walker, APC, bomber, turret, deployed turret, tug, constructor, service "
           "truck and service pod";
    // Sixteen machines, sixteen shapes: the deployed turret is the turret's own shape and plate.
    for (std::size_t a = 0; a < kShaped.size(); ++a) {
        for (std::size_t b = a + 1; b < kShaped.size(); ++b) {
            if (kShaped.at(a) == UnitKind::Turret && kShaped.at(b) == UnitKind::DeployedTurret) {
                continue;
            }
            EXPECT_NE(&shape_of(kShaped.at(a)), &shape_of(kShaped.at(b))) << a << " and " << b;
        }
    }
    EXPECT_EQ(&shape_of(UnitKind::Turret), &shape_of(UnitKind::DeployedTurret));
    EXPECT_EQ(&kind_icon(UnitKind::Turret), &kind_icon(UnitKind::DeployedTurret));
}

TEST(VehicleShape, ThePlateOfAnUnconvertedKindIsStillItsOwnDrawing) {
    // The recycler has no shape: its plate is a hand-drawn recipe, none of them the projection of
    // any shaped kind's parts.
    EXPECT_TRUE(shape_of(UnitKind::Recycler).empty());
    for (const UnitKind shaped : kShaped) {
        EXPECT_NE(&kind_icon(UnitKind::Recycler), &kind_icon(shaped));
        EXPECT_NE(kind_icon(UnitKind::Recycler), kind_icon(shaped)) << static_cast<int>(shaped);
    }
}

TEST(VehicleShape, TheShapedVehiclesAllHaveDifferentPlates) {
    // No two of the eleven shaped vehicles draw the same picture.
    const std::array<UnitKind, 11> vehicles{
        UnitKind::Tank,       UnitKind::HoverTank,   UnitKind::Scout,      UnitKind::MslScout,
        UnitKind::RocketTank, UnitKind::AssaultTank, UnitKind::MortarBike, UnitKind::Walker,
        UnitKind::Apc,        UnitKind::Bomber,      UnitKind::Turret};
    for (std::size_t a = 0; a < vehicles.size(); ++a) {
        for (std::size_t b = a + 1; b < vehicles.size(); ++b) {
            EXPECT_NE(kind_icon(vehicles.at(a)), kind_icon(vehicles.at(b))) << a << " and " << b;
        }
    }
}

TEST(VehicleShape, EveryShapedPlateThatStandsOnTheGroundStandsOnTheHandDrawnOnesLine) {
    // The row's icons stand on one line, `kIconGround`: the hand-drawn recycler's tracks are on it,
    // and so is every shaped plate whose shape starts at the ground (up_min 0). The hover tank's
    // does not, deliberately: its air gap is that silhouette's one hover cue, so it keeps the
    // baseline it was authored with and is not asserted here.
    EXPECT_EQ(plate_box(kind_icon(UnitKind::Recycler)).last_row, static_cast<int>(kIconGround))
        << "the hand-drawn baseline";
    std::size_t grounded = 0;
    for (const UnitKind kind : kShaped) {
        float lowest = kInfinity;
        for (const ShapePart& part : shape_of(kind)) {
            for (const auto& corner : part.profile) {
                lowest = part.lit ? std::min(lowest, corner[1]) : lowest;
            }
        }
        if (lowest > 0.01F) {
            EXPECT_EQ(kind, UnitKind::HoverTank) << "only the hover tank rides above the ground";
            continue;
        }
        ++grounded;
        EXPECT_EQ(plate_box(kind_icon(kind)).last_row, static_cast<int>(kIconGround))
            << "kind " << static_cast<int>(kind);
    }
    EXPECT_EQ(grounded, std::size_t{16}) << "every shaped kind but the hover tank";
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
    // The plate holds the hull's rear and the barrel's tip, one cell in from each edge. Fitted by
    // length with a one-cell margin each side: 70 cells for the 3.3 m from the rear of the tracks
    // to the muzzle collar's end, exactly, not within a cell or two.
    EXPECT_EQ(box.width(), 70) << "cells, rear of the tracks to the barrel's tip (3.3 m)";
    EXPECT_EQ(box.first_col, 1);
    EXPECT_EQ(box.last_col, 70);
    const float reach = kTankBarrelTip - shape_extent(UnitKind::Tank).along_min;
    const float scale = static_cast<float>(box.width()) / reach;
    EXPECT_NEAR(static_cast<float>(box.height()), kTankHeightMetres * scale, 1.5F);
    EXPECT_NEAR(static_cast<float>(box.height()), 28.0F, 1.0F) << "cells, 1.3 m at 21.2 per metre";
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

/// Turns the turret of a lone `kind` and checks the shape's two groups against the mesh: a vertex
/// inside a Hull part's box (and no Turret part's) stays put, one inside a Turret part's box (and
/// no Hull part's) moves. `min_hull` and `min_turret` are how many of each there must be, so that a
/// test of the groups cannot pass by finding none.
void expect_turret_group_is_real(UnitKind kind, std::size_t min_hull, std::size_t min_turret) {
    constexpr float kTurn = 1.0F;
    const std::vector<BodyVertex> before = body_of(kind, 0.0F);
    const std::vector<BodyVertex> after = body_of(kind, kTurn);
    ASSERT_EQ(before.size(), after.size());
    // The ground under a vehicle is within a degree of level, which leans the body by up to 2.6 cm
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
        for (const ShapePart& part : shape_of(kind)) {
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
    EXPECT_GT(hull_only, min_hull);
    EXPECT_GT(turret_only, min_turret);
}

TEST(TankFromShape, TheGroupIsRealTurnTheTurretAndTheTurretPartsMoveAndTheHullDoesNot) {
    expect_turret_group_is_real(UnitKind::Tank, 300,
                                100); // the tracks, wheels and hull; the housing, bustle, hatch
}

// ------------------------------------------------------------------------- the hover tank

/// Where a metre of `along` and of `up` lands on the hover tank's plate, which is fitted by length:
/// 70 cells for the 3.1 m from the tail to the barrel's tip, one cell in from the left edge, the
/// ground line at row `kIconGround`. Written as numbers and not as the plate's own constants, so a
/// plate that stops fitting this way fails here.
constexpr float kHoverCellsPerMetre = 70.0F / 3.1F;
[[nodiscard]] std::size_t hover_col(float along) {
    return static_cast<std::size_t>(1.0F + ((along + 1.3F) * kHoverCellsPerMetre));
}
[[nodiscard]] std::size_t hover_row(float up) {
    return static_cast<std::size_t>(static_cast<float>(kIconGround) - (up * kHoverCellsPerMetre));
}

TEST(HoverTankFromShape, TheShapeIsAsLargeInMetresAsItWasAuthored) {
    const ShapeExtent extent = shape_extent(UnitKind::HoverTank);
    EXPECT_NEAR(extent.along_max - extent.along_min, kHoverBodyMetres, 0.005F)
        << "metres, tail to nose";
    EXPECT_NEAR(extent.up_max, kHoverHeightMetres, 0.005F) << "metres, the fin's top";
    EXPECT_NEAR(2.0F * extent.across_max, kHoverWidthMetres, 0.005F) << "metres, across the tail";
}

TEST(HoverTankFromShape, EveryPartIsConvexAndHasRoomForItsBevel) {
    expect_parts_convex_with_room(UnitKind::HoverTank);
}

TEST(HoverTankFromShape, ThePlateIsTheShapeSeenSideOnPlusItsSkirtAndBarrel) {
    const PlateBox box = plate_box(kind_icon(UnitKind::HoverTank));
    // Fitted by length with a one-cell margin: the tail to the barrel's tip is the whole width.
    EXPECT_EQ(box.width(), 70) << "cells, tail to the barrel's tip (3.1 m)";
    EXPECT_EQ(box.first_col, 1);
    // Measured: the fin's top (1.1 m) to the skirt's bottom (0.15 m) is 21.5 cells, 22 rows.
    EXPECT_NEAR(static_cast<float>(box.height()), 22.0F, 1.0F) << "cells";
    // And it hovers: the skirt's bottom is 0.15 m, three cells, over the ground line.
    EXPECT_EQ(box.last_row, static_cast<int>(kIconGround) - 3) << "three cells of air under it";
    EXPECT_LT(box.last_row, static_cast<int>(kIconGround));
    EXPECT_NEAR(static_cast<float>(kIconGround - static_cast<std::size_t>(box.last_row)),
                kHoverAirMetres * kHoverCellsPerMetre, 0.6F);
}

TEST(HoverTankFromShape, TheMeshIsTheShapeInMetres) {
    const MeshBox mesh = mesh_of(UnitKind::HoverTank);
    ASSERT_GT(mesh.vertices, 0U);
    // Measured before this slice: 3.106 m long, 1.203 m across, 1.120 m to the top of the fin. The
    // wedge, the fin and the turret are the shape; the skirt and the barrel are not.
    EXPECT_NEAR(mesh.along(), kHoverLengthMetres, 0.06F) << "metres, tail to barrel tip";
    EXPECT_NEAR(mesh.high.at(2), 1.8F, 0.06F) << "metres, the barrel's tip ahead of the centre";
    EXPECT_NEAR(mesh.low.at(2), -1.3F, 0.06F) << "metres, the tail behind it";
    EXPECT_NEAR(mesh.high.at(1), kHoverHeightMetres, 0.06F) << "metres, the fin";
    EXPECT_NEAR(mesh.low.at(1), kHoverAirMetres, 0.06F) << "metres, the skirt's bottom";
    EXPECT_NEAR(mesh.across(), kHoverWidthMetres, 0.06F) << "metres";
}

TEST(HoverTankFromShape, ThePlateAndTheMeshAreTheSameMachine) {
    const PlateBox plate = plate_box(kind_icon(UnitKind::HoverTank));
    const MeshBox mesh = mesh_of(UnitKind::HoverTank);
    const float plate_ratio
        = static_cast<float>(plate.height()) / static_cast<float>(plate.width());
    EXPECT_NEAR(plate_ratio, mesh.height() / mesh.along(), 0.04F);
}

TEST(HoverTankFromShape, TheSkirtAndTheBarrelAreOnThePlateAndNotInTheShape) {
    const IconMask& mask = kind_icon(UnitKind::HoverTank);
    const auto lit
        = [&](float along, float up) { return mask.at(hover_row(up)).at(hover_col(along)) == '#'; };
    // The middle of the skirt band (0.15 to 0.4 m) under the wedge, and the middle of the barrel
    // (0.7 to 0.8 m up) over the nose: lit on the plate, covered by no part.
    EXPECT_TRUE(lit(0.0F, 0.27F)) << "the skirt";
    EXPECT_FALSE(shape_covers(UnitKind::HoverTank, 0.0F, 0.27F));
    EXPECT_TRUE(lit(1.4F, 0.75F)) << "the barrel";
    EXPECT_FALSE(shape_covers(UnitKind::HoverTank, 1.7F, 0.75F));
    EXPECT_TRUE(lit(1.7F, 0.75F)) << "the barrel, near its tip";
    // The air under the skirt is clear, and so is the sky over the nose.
    EXPECT_FALSE(lit(0.0F, 0.07F)) << "the gap of air";
    EXPECT_FALSE(lit(1.2F, 0.9F)) << "over the nose, under the barrel's line of sight";
}

TEST(HoverTankFromShape, TheMeshHasDepthAndIsNotACutOut) {
    const MeshBox mesh = mesh_of(UnitKind::HoverTank);
    EXPECT_GT(mesh.across(), 1.0F);
    EXPECT_GE(mesh.layers.size(), std::size_t{30}) << "distinct lateral positions, in centimetres";
    EXPECT_GT(mesh.vertices, std::size_t{400}) << "a cut-out is a few dozen";
    for (const ShapePart& part : shape_of(UnitKind::HoverTank)) {
        if (part.lit && part.role == ShapeRole::Hull) {
            EXPECT_GT(part.bevel, 0.0F) << "a body part with a square edge";
        }
    }
}

TEST(HoverTankFromShape, NothingOnItIsATrackOrAWheel) {
    // The look is the point: a wedge with no skirts of track. The shape has only Hull and Metal
    // parts -- no Track (the dark links) and no Glass (a cut or an inlay in the dark tile).
    std::set<ShapeRole> roles;
    for (const ShapePart& part : shape_of(UnitKind::HoverTank)) {
        roles.insert(part.role);
        EXPECT_TRUE(part.lit);
    }
    EXPECT_EQ(roles, (std::set<ShapeRole>{ShapeRole::Hull, ShapeRole::Metal}));
}

TEST(HoverTankFromShape, TheWedgeSlopesDownAndNarrowsToTheNose) {
    // Side on, the deck is lower at the nose than at the tail; in plan the steps narrow.
    const ShapeExtent extent = shape_extent(UnitKind::HoverTank);
    EXPECT_TRUE(shape_covers(UnitKind::HoverTank, -1.2F, 0.8F)) << "the tail's deck is 0.85 m";
    EXPECT_FALSE(shape_covers(UnitKind::HoverTank, 1.4F, 0.6F)) << "the nose's deck is 0.5 m";
    EXPECT_TRUE(shape_covers(UnitKind::HoverTank, 1.4F, 0.45F));
    float widest_at_nose = 0.0F;
    float widest_at_tail = 0.0F;
    for (const ShapePart& part : shape_of(UnitKind::HoverTank)) {
        float front = -kInfinity;
        float rear = kInfinity;
        for (const auto& corner : part.profile) {
            front = std::max(front, corner[0]);
            rear = std::min(rear, corner[0]);
        }
        const float half = std::max(std::abs(part.across_lo), std::abs(part.across_hi));
        if (part.role == ShapeRole::Hull && part.group == ShapeGroup::Hull) {
            if (front > extent.along_max - 0.01F) {
                widest_at_nose = half;
            }
            if (rear < extent.along_min + 0.01F) {
                widest_at_tail = half;
            }
        }
    }
    EXPECT_NEAR(widest_at_tail, 0.6F, 0.005F) << "metres, half the width at the tail";
    EXPECT_NEAR(widest_at_nose, 0.2F, 0.005F) << "metres, half the width at the nose";
}

TEST(HoverTankFromShape, TheTurretGroupIsRealTurnTheTurretAndOnlyItMoves) {
    EXPECT_EQ(std::count_if(shape_of(UnitKind::HoverTank).begin(),
                            shape_of(UnitKind::HoverTank).end(),
                            [](const ShapePart& part) { return part.group == ShapeGroup::Turret; }),
              1)
        << "the turret is the one part that turns";
    expect_turret_group_is_real(UnitKind::HoverTank, 250, 50); // measured: 348 and 73
}

// ------------------------------------------------------------------------- the scout

TEST(ScoutFromShape, TheShapeIsAsLargeInMetresAsItWasAuthored) {
    const ShapeExtent extent = shape_extent(UnitKind::Scout);
    EXPECT_NEAR(extent.along_max - extent.along_min, kScoutLengthMetres, 0.005F) << "metres";
    EXPECT_NEAR(extent.up_max, kScoutHeightMetres, 0.005F) << "metres, the dish";
    EXPECT_NEAR(2.0F * extent.across_max, kScoutWidthMetres, 0.005F) << "metres, hub to hub";
}

TEST(ScoutFromShape, EveryPartIsConvexAndHasRoomForItsBevel) {
    expect_parts_convex_with_room(UnitKind::Scout);
}

TEST(ScoutFromShape, ThePlateIsTheShapeSeenSideOn) {
    const PlateBox box = plate_box(kind_icon(UnitKind::Scout));
    // Fitted by length with a one-cell margin: the rear to the nose is the whole width, 70 cells
    // for 2.55 m, so 27.5 cells a metre and the dish's 1.3 m is 36 rows.
    EXPECT_EQ(box.width(), 70) << "cells, the shape's length";
    EXPECT_EQ(box.first_col, 1);
    EXPECT_NEAR(static_cast<float>(box.height()), 36.0F, 1.0F) << "cells (48 before)";
    EXPECT_EQ(box.last_row, static_cast<int>(kIconGround)) << "the wheels stand on the ground line";
}

TEST(ScoutFromShape, TheMeshIsTheShapeInMetres) {
    const MeshBox mesh = mesh_of(UnitKind::Scout);
    ASSERT_GT(mesh.vertices, 0U);
    EXPECT_NEAR(mesh.along(), kScoutLengthMetres, 0.06F) << "metres";
    EXPECT_NEAR(mesh.high.at(2), 1.3F, 0.06F) << "metres, the nose";
    EXPECT_NEAR(mesh.low.at(2), -1.25F, 0.06F) << "metres, the tail";
    EXPECT_NEAR(mesh.high.at(1), kScoutHeightMetres, 0.06F) << "metres, the dish";
    EXPECT_NEAR(mesh.across(), kScoutWidthMetres, 0.06F) << "metres (1.201 before)";
    // It looks and points no gun: the 2 m barrel it wore and never fired is gone, so the mesh is
    // 0.76 m shorter than the 3.312 m it measured before.
    EXPECT_LT(mesh.along(), kMeshWasLongMetres - 0.5F);
}

TEST(ScoutFromShape, ThePlateAndTheMeshAreTheSameMachine) {
    const PlateBox plate = plate_box(kind_icon(UnitKind::Scout));
    const MeshBox mesh = mesh_of(UnitKind::Scout);
    const float plate_ratio
        = static_cast<float>(plate.height()) / static_cast<float>(plate.width());
    EXPECT_NEAR(plate_ratio, mesh.height() / mesh.along(), 0.04F);
}

TEST(ScoutFromShape, TheMeshHasDepthAndIsNotACutOut) {
    const MeshBox mesh = mesh_of(UnitKind::Scout);
    EXPECT_GT(mesh.across(), 1.0F);
    EXPECT_GE(mesh.layers.size(), std::size_t{25}) << "distinct lateral positions, in centimetres";
    EXPECT_GT(mesh.vertices, std::size_t{1500}) << "a cut-out is a few dozen";
    for (const ShapePart& part : shape_of(UnitKind::Scout)) {
        if (part.lit && part.role == ShapeRole::Hull) {
            EXPECT_GT(part.bevel, 0.0F) << "a body part with a square edge";
        }
    }
}

TEST(ScoutFromShape, NothingOnItTurnsAndItCarriesNoGun) {
    for (const ShapePart& part : shape_of(UnitKind::Scout)) {
        EXPECT_EQ(part.group, ShapeGroup::Hull);
    }
    // Turning the turret moves nothing: no vertex of the body changes, and there is no barrel.
    const std::vector<BodyVertex> before = body_of(UnitKind::Scout, 0.0F);
    const std::vector<BodyVertex> after = body_of(UnitKind::Scout, 1.0F);
    ASSERT_EQ(before.size(), after.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(before.at(i).across, after.at(i).across) << i;
        EXPECT_EQ(before.at(i).along, after.at(i).along) << i;
    }
}

TEST(ScoutFromShape, ItIsTheOnlyOneWithAMast) {
    // The scout's mast stands 0.1 m wide on the centreline; the missile scout has a rack there.
    EXPECT_TRUE(shape_covers(UnitKind::Scout, -0.6F, 1.0F)) << "the mast, 1.0 m up";
    EXPECT_FALSE(shape_covers(UnitKind::MslScout, -0.6F, 1.3F)) << "nothing as tall at the rear";
    EXPECT_TRUE(shape_covers(UnitKind::Scout, -0.5F, 1.2F)) << "the dish";
}

// ------------------------------------------------------------------------- the missile scout

TEST(MslScoutFromShape, TheShapeIsAsLargeInMetresAsItWasAuthored) {
    const ShapeExtent extent = shape_extent(UnitKind::MslScout);
    EXPECT_NEAR(extent.along_max - extent.along_min, kScoutLengthMetres, 0.005F) << "metres";
    EXPECT_NEAR(extent.up_max, kMslScoutHeightMetres, 0.005F) << "metres, the missile noses";
    EXPECT_NEAR(2.0F * extent.across_max, kScoutWidthMetres, 0.005F) << "metres, hub to hub";
}

TEST(MslScoutFromShape, EveryPartIsConvexAndHasRoomForItsBevel) {
    expect_parts_convex_with_room(UnitKind::MslScout);
}

TEST(MslScoutFromShape, ThePlateIsTheShapeSeenSideOn) {
    const PlateBox box = plate_box(kind_icon(UnitKind::MslScout));
    EXPECT_EQ(box.width(), 70) << "cells, the shape's length";
    EXPECT_EQ(box.first_col, 1);
    EXPECT_NEAR(static_cast<float>(box.height()), 34.0F, 1.0F) << "cells (45 before)";
    EXPECT_EQ(box.last_row, static_cast<int>(kIconGround)) << "the wheels stand on the ground line";
}

TEST(MslScoutFromShape, TheMeshIsTheShapeInMetres) {
    const MeshBox mesh = mesh_of(UnitKind::MslScout);
    ASSERT_GT(mesh.vertices, 0U);
    EXPECT_NEAR(mesh.along(), kScoutLengthMetres, 0.06F) << "metres";
    EXPECT_NEAR(mesh.high.at(1), kMslScoutHeightMetres, 0.06F) << "metres, the noses";
    EXPECT_NEAR(mesh.across(), kScoutWidthMetres, 0.06F) << "metres (1.201 before)";
    EXPECT_LT(mesh.along(), kMeshWasLongMetres - 0.5F) << "the unfired barrel is gone";
}

TEST(MslScoutFromShape, ThePlateAndTheMeshAreTheSameMachine) {
    const PlateBox plate = plate_box(kind_icon(UnitKind::MslScout));
    const MeshBox mesh = mesh_of(UnitKind::MslScout);
    const float plate_ratio
        = static_cast<float>(plate.height()) / static_cast<float>(plate.width());
    EXPECT_NEAR(plate_ratio, mesh.height() / mesh.along(), 0.04F);
}

TEST(MslScoutFromShape, TheMeshHasDepthAndIsNotACutOut) {
    const MeshBox mesh = mesh_of(UnitKind::MslScout);
    EXPECT_GT(mesh.across(), 1.0F);
    EXPECT_GE(mesh.layers.size(), std::size_t{25}) << "distinct lateral positions, in centimetres";
    EXPECT_GT(mesh.vertices, std::size_t{1700}) << "a cut-out is a few dozen";
    for (const ShapePart& part : shape_of(UnitKind::MslScout)) {
        if (part.lit && part.role == ShapeRole::Hull) {
            EXPECT_GT(part.bevel, 0.0F) << "a body part with a square edge";
        }
    }
}

TEST(MslScoutFromShape, ItIsTheScoutsChassisWithARackInPlaceOfTheMast) {
    // The chassis is written once, so the first parts of the two shapes are the same parts.
    const std::vector<ShapePart>& scout = shape_of(UnitKind::Scout);
    const std::vector<ShapePart>& missile = shape_of(UnitKind::MslScout);
    constexpr std::size_t kChassisParts = 8; ///< the wedge, two wheels of three, the canopy
    ASSERT_GE(scout.size(), kChassisParts);
    ASSERT_GE(missile.size(), kChassisParts);
    for (std::size_t i = 0; i < kChassisParts; ++i) {
        EXPECT_EQ(scout.at(i).profile, missile.at(i).profile) << "part " << i;
        EXPECT_EQ(scout.at(i).across_lo, missile.at(i).across_lo) << "part " << i;
        EXPECT_EQ(scout.at(i).across_hi, missile.at(i).across_hi) << "part " << i;
        EXPECT_EQ(scout.at(i).group, ShapeGroup::Hull) << "part " << i;
        EXPECT_EQ(missile.at(i).group, ShapeGroup::Hull) << "part " << i;
    }
    EXPECT_EQ(scout.size(), kChassisParts + 2U) << "the mast and the dish";
    EXPECT_EQ(missile.size(), kChassisParts + 6U) << "the rack, its two slits and three noses";
    EXPECT_NE(kind_icon(UnitKind::Scout), kind_icon(UnitKind::MslScout));
}

TEST(MslScoutFromShape, TheRackIsTheTurretGroupAndTurnsWithTheTurret) {
    std::size_t turret_parts = 0;
    for (const ShapePart& part : shape_of(UnitKind::MslScout)) {
        turret_parts += part.group == ShapeGroup::Turret ? 1U : 0U;
    }
    EXPECT_EQ(turret_parts, std::size_t{6}) << "the rack, its two slits and three noses";
    expect_turret_group_is_real(UnitKind::MslScout, 1200, 150); // measured: 1677 and 258
}

// ------------------------------------------------------------------------- the rocket tank

TEST(RocketTankFromShape, TheShapeIsAsLargeInMetresAsItWasAuthored) {
    const ShapeExtent extent = shape_extent(UnitKind::RocketTank);
    EXPECT_NEAR(extent.along_max - extent.along_min, kRocketLengthMetres, 0.005F)
        << "metres, the tracks' rear to their front";
    EXPECT_NEAR(extent.up_max, kRocketRackMetres, 0.005F) << "metres, the rack's top";
    EXPECT_NEAR(2.0F * extent.across_max, kRocketWidthMetres, 0.005F)
        << "metres, across the fenders";
}

TEST(RocketTankFromShape, EveryPartIsConvexAndHasRoomForItsBevel) {
    expect_parts_convex_with_room(UnitKind::RocketTank);
}

TEST(RocketTankFromShape, ThePlateIsTheShapeSeenSideOn) {
    const PlateBox box = plate_box(kind_icon(UnitKind::RocketTank));
    // Fitted by length with a one-cell margin each side: the 2.6 m of track is 70 cells exactly.
    EXPECT_EQ(box.width(), 70) << "cells, the tracks' rear to their front (2.6 m)";
    EXPECT_EQ(box.first_col, 1);
    EXPECT_EQ(box.last_col, 70);
    // 1.15 m, the rack's top, at 26.9 cells to the metre: 31 rows, resting on the ground line.
    EXPECT_EQ(box.height(), 31) << "cells, the ground to the rack's top (1.15 m)";
    EXPECT_EQ(box.last_row, static_cast<int>(kIconGround)) << "the tracks stand on the ground line";
}

TEST(RocketTankFromShape, TheMeshIsTheShapeInMetres) {
    const MeshBox mesh = mesh_of(UnitKind::RocketTank);
    ASSERT_GT(mesh.vertices, 0U);
    EXPECT_NEAR(mesh.along(), kRocketLengthMetres, 0.04F) << "metres, the tracks' length";
    EXPECT_NEAR(mesh.high.at(2), 1.3F, 0.04F) << "metres, the tracks' front";
    EXPECT_NEAR(mesh.low.at(2), -1.3F, 0.04F) << "metres, the tracks' rear";
    EXPECT_NEAR(mesh.high.at(1), kRocketTopMetres, 0.04F) << "metres, the panel on the rack";
    EXPECT_NEAR(mesh.across(), kRocketWidthMetres, 0.04F) << "metres";
}

TEST(RocketTankFromShape, ItIsAsBigAsTheHandBuiltOneWas) {
    // Measured before it had a shape: 2.601 m long, 1.201 m across, 1.195 m high. The shape changes
    // how it is described, not how big it is; only the plate grew, because a shaped plate is fitted
    // to the box (70 cells, it was 58).
    const MeshBox mesh = mesh_of(UnitKind::RocketTank);
    EXPECT_NEAR(mesh.along(), 2.601F, 0.04F) << "metres";
    EXPECT_NEAR(mesh.across(), 1.201F, 0.04F) << "metres";
    EXPECT_NEAR(mesh.high.at(1), kRocketWasTopMetres, 0.04F) << "metres, and under the bar's 1.2";
    EXPECT_LT(mesh.high.at(1), 1.2F) << "metres, the health bar's anchor";
}

TEST(RocketTankFromShape, ThePlateAndTheMeshAreTheSameMachine) {
    const PlateBox plate = plate_box(kind_icon(UnitKind::RocketTank));
    const MeshBox mesh = mesh_of(UnitKind::RocketTank);
    const float plate_ratio
        = static_cast<float>(plate.height()) / static_cast<float>(plate.width());
    EXPECT_NEAR(plate_ratio, mesh.high.at(1) / mesh.along(), 0.02F);
}

TEST(RocketTankFromShape, TheMeshHasDepthAndIsNotACutOut) {
    const MeshBox mesh = mesh_of(UnitKind::RocketTank);
    EXPECT_GT(mesh.across(), 1.0F);
    EXPECT_GE(mesh.layers.size(), std::size_t{30}) << "distinct lateral positions, in centimetres";
    EXPECT_GT(mesh.vertices, std::size_t{1000}) << "a cut-out is a few dozen";
    for (const ShapePart& part : shape_of(UnitKind::RocketTank)) {
        if (part.lit && part.role == ShapeRole::Hull) {
            EXPECT_GT(part.bevel, 0.0F) << "a body part with a square edge";
        }
    }
}

TEST(RocketTankFromShape, TheRackIsTheTurretGroupSlantedAndCutAndThereIsNoGun) {
    // Whatever stands on the turret's base is the turret group: the mount and the rack, with the
    // lane cut in it. The rack climbs towards the front and ends well inside the tracks.
    std::size_t turret_parts = 0;
    for (const ShapePart& part : shape_of(UnitKind::RocketTank)) {
        turret_parts += part.group == ShapeGroup::Turret ? 1U : 0U;
    }
    EXPECT_EQ(turret_parts, std::size_t{3}) << "the mount, the rack, the lane";
    EXPECT_FALSE(shape_covers(UnitKind::RocketTank, -0.6F, 1.1F)) << "the rack's rear is low";
    EXPECT_TRUE(shape_covers(UnitKind::RocketTank, -0.6F, 0.97F)) << "its rear is 0.95 m up";
    EXPECT_TRUE(shape_covers(UnitKind::RocketTank, 0.4F, 1.1F)) << "and its front reaches 1.15 m";
    EXPECT_FALSE(shape_covers(UnitKind::RocketTank, 0.4F, 0.97F))
        << "slanted: open under the front";
    EXPECT_FALSE(shape_covers(UnitKind::RocketTank, 0.8F, 1.0F))
        << "ends 0.55 m ahead of the centre";
    EXPECT_FALSE(shape_covers(UnitKind::RocketTank, -0.1F, 1.05F))
        << "the lane is cut in its flank";
    EXPECT_TRUE(shape_covers(UnitKind::RocketTank, -0.1F, 1.13F)) << "with rack above it";
    expect_turret_group_is_real(UnitKind::RocketTank, 2000, 150); // measured: 2124 and 198;
}

// ------------------------------------------------------------------------- the assault tank

TEST(AssaultTankFromShape, TheShapeIsAsLargeInMetresAsItWasAuthored) {
    const ShapeExtent extent = shape_extent(UnitKind::AssaultTank);
    EXPECT_NEAR(extent.along_max - extent.along_min, kRocketLengthMetres, 0.005F)
        << "metres, the tracks' rear to their front";
    EXPECT_NEAR(extent.up_max, kAssaultHeightMetres, 0.005F) << "metres, the hatch's top";
    EXPECT_NEAR(2.0F * extent.across_max, kAssaultWidthMetres, 0.005F)
        << "metres, across the fenders";
}

TEST(AssaultTankFromShape, EveryPartIsConvexAndHasRoomForItsBevel) {
    expect_parts_convex_with_room(UnitKind::AssaultTank);
}

TEST(AssaultTankFromShape, ThePlateIsTheShapeSeenSideOnPlusItsBarrel) {
    const PlateBox box = plate_box(kind_icon(UnitKind::AssaultTank));
    // Fitted by length with a one-cell margin each side: the rear of the tracks to the collar's end
    // is 3.4 m, 70 cells exactly.
    EXPECT_EQ(box.width(), 70) << "cells, rear of the tracks to the barrel's tip (3.4 m)";
    EXPECT_EQ(box.first_col, 1);
    EXPECT_EQ(box.last_col, 70);
    // 1.58 m, the hatch's top, at 20.6 cells to the metre: 33 rows, resting on the ground line.
    EXPECT_EQ(box.height(), 33) << "cells, the ground to the hatch's top (1.58 m)";
    EXPECT_EQ(box.last_row, static_cast<int>(kIconGround)) << "the tracks stand on the ground line";
}

TEST(AssaultTankFromShape, TheMeshIsTheShapeInMetres) {
    const MeshBox mesh = mesh_of(UnitKind::AssaultTank);
    ASSERT_GT(mesh.vertices, 0U);
    // The body is the shape; the barrel, which is not a part of it, runs on to 2.1 m.
    EXPECT_NEAR(mesh.along(), kAssaultLengthMetres, 0.04F) << "metres, tracks' rear to barrel tip";
    EXPECT_NEAR(mesh.high.at(2), 2.1F, 0.04F) << "metres, the barrel's tip";
    EXPECT_NEAR(mesh.low.at(2), -1.3F, 0.04F) << "metres, the tracks' rear";
    EXPECT_NEAR(mesh.high.at(1), kAssaultTopMetres, 0.04F) << "metres, the hatch";
    EXPECT_NEAR(mesh.across(), kAssaultWidthMetres, 0.04F) << "metres";
}

TEST(AssaultTankFromShape, ItIsAsBigAsTheHandBuiltOneWas) {
    // Measured before it had a shape: 3.414 m long, 1.201 m across, 1.543 m high. The shape changes
    // how it is described, not how big it is: the one difference is the commander's hatch, 4 cm
    // over the old top. The plate changed more: it was a 68 by 57 cell slab, taller than the mesh
    // it stood for, and is now the mesh's own proportions, 70 by 33.
    const MeshBox mesh = mesh_of(UnitKind::AssaultTank);
    EXPECT_NEAR(mesh.along(), 3.414F, 0.04F) << "metres";
    EXPECT_NEAR(mesh.across(), 1.201F, 0.04F) << "metres";
    EXPECT_NEAR(mesh.high.at(1), kAssaultWasTopMetres, 0.05F) << "metres";
}

TEST(AssaultTankFromShape, ThePlateAndTheMeshAreTheSameMachine) {
    const PlateBox plate = plate_box(kind_icon(UnitKind::AssaultTank));
    const MeshBox mesh = mesh_of(UnitKind::AssaultTank);
    const float plate_ratio
        = static_cast<float>(plate.height()) / static_cast<float>(plate.width());
    EXPECT_NEAR(plate_ratio, mesh.high.at(1) / mesh.along(), 0.02F);
}

TEST(AssaultTankFromShape, TheBarrelIsOnThePlateAndNotInTheShape) {
    const ShapeExtent extent = shape_extent(UnitKind::AssaultTank);
    EXPECT_FALSE(shape_covers(UnitKind::AssaultTank, 1.9F, 1.2F)) << "the barrel is not a part";
    EXPECT_NEAR(extent.along_max, 1.3F, 0.005F) << "metres: the shape ends at the tracks' front";
    // The plate's last lit column is the muzzle collar, 0.8 m past the tracks' front.
    const IconMask& mask = kind_icon(UnitKind::AssaultTank);
    const PlateBox box = plate_box(mask);
    bool muzzle = false;
    for (std::size_t row = 0; row < kIconCells; ++row) {
        muzzle = muzzle || mask.at(row).at(static_cast<std::size_t>(box.last_col)) == '#';
    }
    EXPECT_TRUE(muzzle);
    const float cells_per_metre = 70.0F / 3.4F;
    const int tracks_front = 1 + static_cast<int>(2.6F * cells_per_metre);
    EXPECT_GT(box.last_col, tracks_front + 10) << "the barrel reaches well past the tracks";
}

TEST(AssaultTankFromShape, TheMeshHasDepthAndIsNotACutOut) {
    const MeshBox mesh = mesh_of(UnitKind::AssaultTank);
    EXPECT_GT(mesh.across(), 1.0F);
    EXPECT_GE(mesh.layers.size(), std::size_t{35}) << "distinct lateral positions, in centimetres";
    EXPECT_GT(mesh.vertices, std::size_t{1000}) << "a cut-out is a few dozen";
    for (const ShapePart& part : shape_of(UnitKind::AssaultTank)) {
        if (part.lit && part.role == ShapeRole::Hull) {
            EXPECT_GT(part.bevel, 0.0F) << "a body part with a square edge";
        }
    }
}

TEST(AssaultTankFromShape, TheBlockIsTallerAndWiderThanTheTanksTurretAndHasNoBustle) {
    // The tank's turret housing is 0.72 m across and 0.4 m tall with a bustle behind it; the
    // assault tank's block is 1.0 m across and 0.7 m tall, and nothing sticks out of its rear.
    // The block is the widest part of the turret group, whatever its role; the rear is the rearmost
    // corner of any part in it.
    float block_half = 0.0F;
    float block_height = 0.0F;
    float rearmost = kInfinity;
    for (const ShapePart& part : shape_of(UnitKind::AssaultTank)) {
        if (part.group != ShapeGroup::Turret) {
            continue;
        }
        float lo = kInfinity;
        float hi = -kInfinity;
        for (const auto& corner : part.profile) {
            lo = std::min(lo, corner[1]);
            hi = std::max(hi, corner[1]);
            rearmost = std::min(rearmost, corner[0]);
        }
        if (part.across_hi > block_half) {
            block_half = part.across_hi;
            block_height = hi - lo;
        }
    }
    EXPECT_NEAR(block_half, 0.5F, 0.005F) << "metres, half the block's width (the tank's is 0.36)";
    EXPECT_NEAR(block_height, 0.7F, 0.005F) << "metres (the tank's is 0.4)";
    EXPECT_NEAR(rearmost, -0.7F, 0.005F) << "metres: it ends there, with no bustle behind it";
    std::size_t turret_parts = 0;
    for (const ShapePart& part : shape_of(UnitKind::AssaultTank)) {
        turret_parts += part.group == ShapeGroup::Turret ? 1U : 0U;
    }
    EXPECT_EQ(turret_parts, std::size_t{2}) << "the block and the hatch";
    expect_turret_group_is_real(UnitKind::AssaultTank, 2000, 150); // measured: 2104 and 211;
}

/// The lowest and highest `up` of a part's profile.
[[nodiscard]] std::array<float, 2> up_span(const ShapePart& part) {
    float lowest = kInfinity;
    float highest = -kInfinity;
    for (const auto& corner : part.profile) {
        lowest = std::min(lowest, corner[1]);
        highest = std::max(highest, corner[1]);
    }
    return {lowest, highest};
}

/// Does `part` have a straight edge climbing at `degrees` over level, within half a degree?
[[nodiscard]] bool has_edge_at(const ShapePart& part, float degrees) {
    for (std::size_t i = 0; i < part.profile.size(); ++i) {
        const auto& a = part.profile.at(i);
        const auto& b = part.profile.at((i + 1) % part.profile.size());
        const float rise = std::atan2(b[1] - a[1], b[0] - a[0]) * 180.0F / 3.14159265F;
        if (std::abs(std::abs(rise) - degrees) < 0.5F
            || std::abs(std::abs(rise) - (180.0F - degrees)) < 0.5F) {
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------------- the mortar bike

/// Where a metre of `along` and of `up` lands on a plate fitted by length: 70 cells for `reach`
/// metres from the shape's rear, one cell in from the left edge, the ground line at `kIconGround`.
/// Written as numbers and not as the plate's own constants, so a plate that stops fitting this way
/// fails here.
[[nodiscard]] std::size_t plate_col(float along, float rear, float reach) {
    return static_cast<std::size_t>(1.0F + ((along - rear) * 68.0F / reach));
}
[[nodiscard]] std::size_t plate_row(float up, float reach) {
    return static_cast<std::size_t>(static_cast<float>(kIconGround) - (up * 68.0F / reach));
}

TEST(MortarBikeFromShape, TheShapeIsAsLargeInMetresAsItWasAuthored) {
    const ShapeExtent extent = shape_extent(UnitKind::MortarBike);
    EXPECT_NEAR(extent.along_max - extent.along_min, kMortarLengthMetres, 0.005F)
        << "metres, rear tyre to front tyre";
    EXPECT_NEAR(extent.up_max, kMortarHeightMetres, 0.005F) << "metres, the collar's top";
    EXPECT_NEAR(2.0F * extent.across_max, kMortarWidthMetres, 0.005F)
        << "metres, across the handlebars";
}

TEST(MortarBikeFromShape, EveryPartIsConvexAndHasRoomForItsBevel) {
    expect_parts_convex_with_room(UnitKind::MortarBike);
}

TEST(MortarBikeFromShape, ThePlateIsTheShapeSeenSideOn) {
    const PlateBox box = plate_box(kind_icon(UnitKind::MortarBike));
    EXPECT_EQ(box.width(), 70) << "cells, fitted by length: the 1.96 m from tyre to tyre";
    EXPECT_EQ(box.first_col, 1);
    EXPECT_EQ(box.last_col, 70);
    EXPECT_NEAR(static_cast<float>(box.height()), 39.0F, 1.0F) << "cells, 1.1 m at 34.7 per metre";
    EXPECT_EQ(box.last_row, static_cast<int>(kIconGround)) << "the wheels stand on the ground line";
}

TEST(MortarBikeFromShape, TheMeshIsTheShapeInMetres) {
    const MeshBox mesh = mesh_of(UnitKind::MortarBike);
    ASSERT_GT(mesh.vertices, 0U);
    EXPECT_NEAR(mesh.along(), kMortarLengthMetres, 0.06F) << "metres";
    EXPECT_NEAR(mesh.high.at(1), kMortarHeightMetres, 0.06F) << "metres";
    EXPECT_NEAR(mesh.across(), kMortarWidthMetres, 0.06F) << "metres";
    EXPECT_LT(mesh.high.at(1), 1.2F) << "metres, under the health bar's anchor";
}

TEST(MortarBikeFromShape, ItIsABikeAndNoLongerTheBoxItWas) {
    // Measured before it had a shape: 1.400 m long, 1.200 m across, 1.140 m high, a low wide box.
    // The height is kept; the plate's two wheels in line make it longer and a good deal narrower.
    const MeshBox mesh = mesh_of(UnitKind::MortarBike);
    EXPECT_NEAR(mesh.high.at(1), kMortarWasHighMetres, 0.06F) << "metres, as tall as it was";
    EXPECT_GT(mesh.along(), kMortarWasLongMetres + 0.4F) << "metres, longer than the box";
    EXPECT_LT(mesh.across(), kMortarWasWideMetres - 0.4F) << "metres, narrower than the box";
}

TEST(MortarBikeFromShape, ThePlateAndTheMeshAreTheSameMachine) {
    const PlateBox plate = plate_box(kind_icon(UnitKind::MortarBike));
    const MeshBox mesh = mesh_of(UnitKind::MortarBike);
    const float plate_ratio
        = static_cast<float>(plate.height()) / static_cast<float>(plate.width());
    EXPECT_NEAR(plate_ratio, mesh.high.at(1) / mesh.along(), 0.04F);
}

TEST(MortarBikeFromShape, TheMeshHasDepthAndIsNotACutOut) {
    const MeshBox mesh = mesh_of(UnitKind::MortarBike);
    EXPECT_GT(mesh.across(), 0.6F);
    EXPECT_GE(mesh.layers.size(), std::size_t{20}) << "distinct lateral positions, in cm";
    EXPECT_GT(mesh.vertices, std::size_t{1000}) << "a cut-out is a few dozen";
    for (const ShapePart& part : shape_of(UnitKind::MortarBike)) {
        if (part.lit && part.role == ShapeRole::Hull) {
            EXPECT_GT(part.bevel, 0.0F) << "a body part with a square edge";
        }
    }
}

TEST(MortarBikeFromShape, ItHasTwoWheelsInLineOnTheCentrelineAndNoTrack) {
    // A wheel is a lit dark disc of a dozen corners standing on the ground; found by its shape.
    std::vector<std::array<float, 2>> centres;
    for (const ShapePart& part : shape_of(UnitKind::MortarBike)) {
        if (part.lit && part.role == ShapeRole::Track && part.profile.size() >= 12U) {
            EXPECT_FALSE(part.mirrored) << "a bike's wheel is on the centreline, not one a side";
            EXPECT_NEAR(part.across_lo, -part.across_hi, 0.001F);
            float along = 0.0F;
            float up = 0.0F;
            for (const auto& corner : part.profile) {
                along += corner[0] / static_cast<float>(part.profile.size());
                up += corner[1] / static_cast<float>(part.profile.size());
            }
            centres.push_back({along, up});
        }
    }
    ASSERT_EQ(centres.size(), std::size_t{2});
    EXPECT_NEAR(centres.at(0)[1], centres.at(1)[1], 0.001F) << "both at one height: in line";
    EXPECT_NEAR(std::abs(centres.at(0)[0] - centres.at(1)[0]), 1.3F, 0.005F)
        << "metres, the wheelbase";
}

TEST(MortarBikeFromShape, TheMortarIsTheTurretGroupAngledUp45DegreesAndTurnsWithTheAim) {
    // The mortar lobs: the tube and the collar, the two dark turret parts that are not wheels, both
    // climb at 45 degrees. Four parts turn -- the mount, the tube, the collar and the bore.
    std::size_t turret_parts = 0;
    std::size_t slanted = 0;
    for (const ShapePart& part : shape_of(UnitKind::MortarBike)) {
        turret_parts += part.group == ShapeGroup::Turret ? 1U : 0U;
        if (part.group == ShapeGroup::Turret && part.role == ShapeRole::Track && part.lit) {
            EXPECT_TRUE(has_edge_at(part, 45.0F)) << "metres: the tube and its collar lean 45 deg";
            ++slanted;
        }
    }
    EXPECT_EQ(turret_parts, std::size_t{4}) << "the mount, the tube, the collar and the bore";
    EXPECT_EQ(slanted, std::size_t{2}) << "the tube and the collar";
    expect_turret_group_is_real(UnitKind::MortarBike, 800, 150); // measured: 1009 and 218
}

// ------------------------------------------------------------------------- the walker

constexpr float kWalkerReach
    = kWalkerGunTipMetres + 1.0F; ///< the rear foot (-1.0) to the gun's tip

TEST(WalkerFromShape, TheShapeIsAsLargeInMetresAsItWasAuthored) {
    const ShapeExtent extent = shape_extent(UnitKind::Walker);
    EXPECT_NEAR(extent.along_max - extent.along_min, kWalkerBodyMetres, 0.005F)
        << "metres, rear foot to front foot";
    EXPECT_NEAR(extent.up_max, kWalkerHeightMetres, 0.005F) << "metres, the roof panel's top";
    EXPECT_NEAR(2.0F * extent.across_max, kWalkerWidthMetres, 0.005F) << "metres, across the feet";
}

TEST(WalkerFromShape, EveryPartIsConvexAndHasRoomForItsBevel) {
    expect_parts_convex_with_room(UnitKind::Walker);
}

TEST(WalkerFromShape, ThePlateIsTheShapeSeenSideOnPlusItsGunArm) {
    const PlateBox box = plate_box(kind_icon(UnitKind::Walker));
    EXPECT_EQ(box.width(), 70) << "cells, rear foot to the gun's tip (2.8 m)";
    EXPECT_EQ(box.first_col, 1);
    EXPECT_EQ(box.last_col, 70);
    EXPECT_NEAR(static_cast<float>(box.height()), 43.0F, 1.0F) << "cells, 1.71 m at 24.3 per metre";
    EXPECT_EQ(box.last_row, static_cast<int>(kIconGround)) << "the feet stand on the ground line";
}

TEST(WalkerFromShape, TheMeshIsTheShapeInMetres) {
    const MeshBox mesh = mesh_of(UnitKind::Walker);
    ASSERT_GT(mesh.vertices, 0U);
    EXPECT_NEAR(mesh.high.at(2), kWalkerGunTipMetres, 0.06F) << "metres, the gun's tip";
    EXPECT_NEAR(mesh.low.at(2), -1.0F, 0.06F) << "metres, the rear foot";
    EXPECT_NEAR(mesh.high.at(1), kWalkerHeightMetres, 0.06F) << "metres, the roof panel";
    EXPECT_NEAR(mesh.across(), kWalkerWidthMetres, 0.06F) << "metres, across the feet";
}

TEST(WalkerFromShape, ItIsAsTallAsItWasAndTheGunIsAsLongAndItIsWiderAcrossTheFeet) {
    // Measured before it had a shape: a gun tip at 1.821 m, 1.739 m high, 0.780 m across.
    const MeshBox mesh = mesh_of(UnitKind::Walker);
    EXPECT_NEAR(mesh.high.at(2), 1.821F, 0.06F) << "metres";
    EXPECT_NEAR(mesh.high.at(1), kWalkerWasHighMetres, 0.06F) << "metres";
    EXPECT_GT(mesh.across(), kWalkerWasWideMetres) << "metres: broad feet";
    EXPECT_GT(mesh.high.at(1), mesh.across() * 1.5F) << "taller than it is wide";
}

TEST(WalkerFromShape, ThePlateAndTheMeshAreTheSameMachine) {
    const PlateBox plate = plate_box(kind_icon(UnitKind::Walker));
    const MeshBox mesh = mesh_of(UnitKind::Walker);
    const float plate_ratio
        = static_cast<float>(plate.height()) / static_cast<float>(plate.width());
    EXPECT_NEAR(plate_ratio, mesh.high.at(1) / mesh.along(), 0.04F);
}

TEST(WalkerFromShape, TheMeshHasDepthAndIsNotACutOut) {
    const MeshBox mesh = mesh_of(UnitKind::Walker);
    EXPECT_GT(mesh.across(), 0.8F);
    EXPECT_GE(mesh.layers.size(), std::size_t{20}) << "distinct lateral positions, in cm";
    EXPECT_GT(mesh.vertices, std::size_t{500}) << "a cut-out is a few dozen";
    for (const ShapePart& part : shape_of(UnitKind::Walker)) {
        if (part.lit && part.role == ShapeRole::Hull) {
            EXPECT_GT(part.bevel, 0.0F) << "a body part with a square edge";
        }
    }
}

TEST(WalkerFromShape, LegsAreWhatSaysWalkerTwoFeetOnOppositeSidesWithAGapBetween) {
    // A foot is a lit part whose lowest point is the ground. There are two, one a side of the
    // centreline (it is mid-stride), neither mirrored, and nothing is under the body between them.
    std::vector<const ShapePart*> feet;
    for (const ShapePart& part : shape_of(UnitKind::Walker)) {
        if (part.lit && up_span(part).at(0) < 0.001F) {
            feet.push_back(&part);
        }
    }
    ASSERT_EQ(feet.size(), std::size_t{2});
    EXPECT_FALSE(feet.at(0)->mirrored);
    EXPECT_FALSE(feet.at(1)->mirrored);
    EXPECT_LT(feet.at(0)->across_hi * feet.at(1)->across_hi, 0.0F) << "one a side";
    EXPECT_TRUE(shape_covers(UnitKind::Walker, -0.65F, 0.1F)) << "the rear foot";
    EXPECT_TRUE(shape_covers(UnitKind::Walker, 0.65F, 0.1F)) << "the front foot";
    EXPECT_FALSE(shape_covers(UnitKind::Walker, 0.0F, 0.5F)) << "daylight between the legs";
    EXPECT_TRUE(shape_covers(UnitKind::Walker, 0.0F, 1.3F) == false) << "the cockpit glass is cut";
}

TEST(WalkerFromShape, TheGunArmIsOnThePlateAndNotInTheShape) {
    const IconMask& mask = kind_icon(UnitKind::Walker);
    EXPECT_FALSE(shape_covers(UnitKind::Walker, 1.4F, 1.28F));
    EXPECT_EQ(mask.at(plate_row(1.28F, kWalkerReach)).at(plate_col(1.4F, -1.0F, kWalkerReach)), '#')
        << "the arm, level, 0.9 m out from the body's front face";
    EXPECT_EQ(mask.at(plate_row(1.28F, kWalkerReach)).at(plate_col(1.7F, -1.0F, kWalkerReach)), '#')
        << "and near its tip";
    EXPECT_EQ(mask.at(plate_row(1.6F, kWalkerReach)).at(plate_col(1.4F, -1.0F, kWalkerReach)), '.')
        << "the sky over the arm";
}

TEST(WalkerFromShape, TheGunArmTurnsAndElevatesAndTheShapeDoesNeither) {
    // The walker has no Turret part; its gun arm is the one thing that turns, and it elevates.
    const std::vector<BodyVertex> level = body_of(UnitKind::Walker, 0.0F, 0.0F);
    const std::vector<BodyVertex> turned = body_of(UnitKind::Walker, 1.0F, 0.0F);
    const std::vector<BodyVertex> raised = body_of(UnitKind::Walker, 0.0F, 0.3F);
    ASSERT_EQ(level.size(), turned.size());
    ASSERT_EQ(level.size(), raised.size());
    constexpr float kSlack = 0.04F;
    std::size_t shape_vertices_seen = 0;
    std::size_t gun_turned = 0;
    std::size_t gun_raised = 0;
    for (std::size_t i = 0; i < level.size(); ++i) {
        const BodyVertex& v = level.at(i);
        // The arm's root sits on the body's front face (0.5 m): a vertex there is either's.
        if (std::abs(v.along - 0.5F) < 0.03F && std::abs(v.across) < 0.1F && v.up > 1.15F
            && v.up < 1.4F) {
            continue;
        }
        bool in_shape = false;
        for (const ShapePart& part : shape_of(UnitKind::Walker)) {
            in_shape = in_shape || in_box_of(part, level.at(i), kSlack);
        }
        const auto moved = [&](const BodyVertex& w) {
            return std::abs(level.at(i).across - w.across) > 1.0e-4F
                   || std::abs(level.at(i).along - w.along) > 1.0e-4F
                   || std::abs(level.at(i).up - w.up) > 1.0e-4F;
        };
        if (in_shape) {
            ++shape_vertices_seen;
            EXPECT_FALSE(moved(turned.at(i))) << "a body vertex turned with the gun: " << i;
            EXPECT_FALSE(moved(raised.at(i))) << "a body vertex rose with the gun: " << i;
        } else {
            gun_turned += moved(turned.at(i)) ? 1U : 0U;
            gun_raised += moved(raised.at(i)) ? 1U : 0U;
        }
    }
    EXPECT_GT(shape_vertices_seen, std::size_t{400});
    EXPECT_GT(gun_turned, std::size_t{10}) << "the arm swings with the turret";
    EXPECT_GT(gun_raised, std::size_t{10}) << "and lifts with the pitch";
}

// ------------------------------------------------------------------------- the APC

TEST(ApcFromShape, TheShapeIsAsLargeInMetresAsItWasAuthored) {
    const ShapeExtent extent = shape_extent(UnitKind::Apc);
    EXPECT_NEAR(extent.along_max - extent.along_min, kApcLengthMetres, 0.005F)
        << "metres, tail to nose";
    EXPECT_NEAR(extent.up_max, kApcHeightMetres, 0.005F) << "metres, the turret's roof";
    EXPECT_NEAR(2.0F * extent.across_max, kApcWidthMetres, 0.005F) << "metres, hub to hub";
}

TEST(ApcFromShape, EveryPartIsConvexAndHasRoomForItsBevel) {
    expect_parts_convex_with_room(UnitKind::Apc);
}

TEST(ApcFromShape, ThePlateIsTheShapeSeenSideOn) {
    const PlateBox box = plate_box(kind_icon(UnitKind::Apc));
    EXPECT_EQ(box.width(), 70) << "cells, the 3.0 m from tail to nose";
    EXPECT_EQ(box.first_col, 1);
    EXPECT_EQ(box.last_col, 70);
    EXPECT_NEAR(static_cast<float>(box.height()), 26.0F, 1.0F) << "cells, 1.1 m at 22.7 per metre";
    EXPECT_EQ(box.last_row, static_cast<int>(kIconGround)) << "the wheels stand on the ground line";
}

TEST(ApcFromShape, TheMeshIsTheShapeInMetres) {
    const MeshBox mesh = mesh_of(UnitKind::Apc);
    ASSERT_GT(mesh.vertices, 0U);
    EXPECT_NEAR(mesh.along(), kApcLengthMetres, 0.06F) << "metres";
    EXPECT_NEAR(mesh.high.at(1), kApcHeightMetres, 0.06F) << "metres";
    EXPECT_NEAR(mesh.across(), kApcWidthMetres, 0.06F) << "metres";
}

TEST(ApcFromShape, ItIsAsLongAndAsWideAsItWasAndTallerThanTheBoxWas) {
    // Measured before it had a shape: 3.005 m long, 1.200 m across, 0.650 m high.
    const MeshBox mesh = mesh_of(UnitKind::Apc);
    EXPECT_NEAR(mesh.along(), 3.005F, 0.06F) << "metres";
    EXPECT_NEAR(mesh.across(), 1.2F, 0.06F) << "metres";
    EXPECT_GT(mesh.high.at(1), kApcWasHighMetres + 0.4F) << "metres: wheels and a turret";
    EXPECT_LE(mesh.high.at(1), 1.2F) << "metres, the health bar's anchor";
}

TEST(ApcFromShape, ThePlateAndTheMeshAreTheSameMachine) {
    const PlateBox plate = plate_box(kind_icon(UnitKind::Apc));
    const MeshBox mesh = mesh_of(UnitKind::Apc);
    const float plate_ratio
        = static_cast<float>(plate.height()) / static_cast<float>(plate.width());
    EXPECT_NEAR(plate_ratio, mesh.high.at(1) / mesh.along(), 0.04F);
}

TEST(ApcFromShape, TheMeshHasDepthAndIsNotACutOut) {
    const MeshBox mesh = mesh_of(UnitKind::Apc);
    EXPECT_GT(mesh.across(), 1.0F);
    EXPECT_GE(mesh.layers.size(), std::size_t{25}) << "distinct lateral positions, in cm";
    EXPECT_GT(mesh.vertices, std::size_t{2000}) << "a cut-out is a few dozen";
    for (const ShapePart& part : shape_of(UnitKind::Apc)) {
        if (part.lit && part.role == ShapeRole::Hull) {
            EXPECT_GT(part.bevel, 0.0F) << "a body part with a square edge";
        }
    }
}

TEST(ApcFromShape, ItRidesOnFourWheelsASideAndHasNoTrack) {
    // A wheel is a lit dark disc of a dozen corners, one a side mirrored; found by its shape.
    std::vector<float> wheels;
    for (const ShapePart& part : shape_of(UnitKind::Apc)) {
        if (part.lit && part.role == ShapeRole::Track && part.profile.size() >= 12U) {
            EXPECT_TRUE(part.mirrored);
            float along = 0.0F;
            for (const auto& corner : part.profile) {
                along += corner[0] / static_cast<float>(part.profile.size());
            }
            wheels.push_back(along);
            EXPECT_LT(up_span(part).at(1), 0.6F) << "a wheel, not a track run along the hull";
        }
    }
    ASSERT_EQ(wheels.size(), std::size_t{4});
    std::sort(wheels.begin(), wheels.end());
    for (std::size_t i = 1; i < wheels.size(); ++i) {
        EXPECT_NEAR(wheels.at(i) - wheels.at(i - 1), 0.7F, 0.005F) << "metres, the pitch";
    }
}

TEST(ApcFromShape, ThreeFiringPortsAreCutThroughTheFlank) {
    std::size_t ports = 0;
    for (const ShapePart& part : shape_of(UnitKind::Apc)) {
        if (!part.lit && part.role == ShapeRole::Glass && part.profile.size() == 4U
            && !part.mirrored) {
            ++ports;
            EXPECT_GT(part.across_hi, 0.45F) << "through the hull's whole width";
        }
    }
    EXPECT_EQ(ports, std::size_t{3});
    EXPECT_FALSE(shape_covers(UnitKind::Apc, -0.65F, 0.57F)) << "a port";
    EXPECT_TRUE(shape_covers(UnitKind::Apc, -0.65F, 0.75F)) << "the hull over it";
}

TEST(ApcFromShape, TheTurretAndItsGunAreTheTurretGroupAndTurn) {
    std::size_t turret_parts = 0;
    for (const ShapePart& part : shape_of(UnitKind::Apc)) {
        turret_parts += part.group == ShapeGroup::Turret ? 1U : 0U;
    }
    EXPECT_EQ(turret_parts, std::size_t{2}) << "the housing and the gun";
    const IconMask& mask = kind_icon(UnitKind::Apc);
    EXPECT_TRUE(shape_covers(UnitKind::Apc, 1.1F, 0.96F))
        << "the gun is a part: it is in the shape";
    EXPECT_EQ(mask.at(plate_row(0.96F, 3.0F)).at(plate_col(1.1F, -1.5F, 3.0F)), '#');
    expect_turret_group_is_real(UnitKind::Apc, 2500, 80); // measured: 3220 and 116
}

// ------------------------------------------------------------------------- the bomber

TEST(BomberFromShape, TheShapeIsAsLargeInMetresAsItWasAuthored) {
    const ShapeExtent extent = shape_extent(UnitKind::Bomber);
    EXPECT_NEAR(extent.along_max - extent.along_min, kBomberLengthMetres, 0.005F)
        << "metres, tail to the nosecone's tip";
    EXPECT_NEAR(extent.up_max, kBomberHeightMetres, 0.005F) << "metres, the tail fin";
    EXPECT_NEAR(2.0F * extent.across_max, kBomberWidthMetres, 0.005F) << "metres, the wing's span";
}

TEST(BomberFromShape, EveryPartIsConvexAndHasRoomForItsBevel) {
    expect_parts_convex_with_room(UnitKind::Bomber);
}

TEST(BomberFromShape, ThePlateIsTheShapeSeenSideOn) {
    const PlateBox box = plate_box(kind_icon(UnitKind::Bomber));
    EXPECT_EQ(box.width(), 70) << "cells, the 2.4 m from the tail to the nosecone's tip";
    EXPECT_EQ(box.first_col, 1);
    EXPECT_EQ(box.last_col, 70);
    EXPECT_NEAR(static_cast<float>(box.height()), 32.0F, 1.0F) << "cells, 1.1 m at 29.2 per metre";
    EXPECT_EQ(box.last_row, static_cast<int>(kIconGround)) << "the bomb stands on the ground line";
}

TEST(BomberFromShape, TheMeshIsTheShapeInMetres) {
    const MeshBox mesh = mesh_of(UnitKind::Bomber);
    ASSERT_GT(mesh.vertices, 0U);
    EXPECT_NEAR(mesh.along(), kBomberLengthMetres, 0.06F) << "metres";
    EXPECT_NEAR(mesh.high.at(1), kBomberHeightMetres, 0.06F) << "metres, the tail fin";
    EXPECT_NEAR(mesh.across(), kBomberWidthMetres, 0.06F) << "metres, the wing's span";
    EXPECT_LT(mesh.high.at(1), 1.2F) << "metres, under the health bar's anchor";
}

TEST(BomberFromShape, ItKeepsItsSpanAndIsLongerAndTallerThanTheSlabWas) {
    // Measured before it had a shape: 2.004 m long, 2.402 m across, 0.449 m high. The span is the
    // widest on the ground and stays; the slab had no fin and no bomb.
    const MeshBox mesh = mesh_of(UnitKind::Bomber);
    EXPECT_NEAR(mesh.across(), kBomberWasWideMetres, 0.06F) << "metres: the span is kept";
    EXPECT_GT(mesh.along(), kBomberWasLongMetres + 0.3F) << "metres: a fuselage and a nosecone";
    EXPECT_GT(mesh.high.at(1), kBomberWasHighMetres + 0.5F) << "metres: a tail fin and a bomb";
}

TEST(BomberFromShape, ThePlateAndTheMeshAreTheSameMachine) {
    const PlateBox plate = plate_box(kind_icon(UnitKind::Bomber));
    const MeshBox mesh = mesh_of(UnitKind::Bomber);
    const float plate_ratio
        = static_cast<float>(plate.height()) / static_cast<float>(plate.width());
    EXPECT_NEAR(plate_ratio, mesh.high.at(1) / mesh.along(), 0.04F);
}

TEST(BomberFromShape, TheMeshHasDepthAndIsNotACutOut) {
    const MeshBox mesh = mesh_of(UnitKind::Bomber);
    EXPECT_GT(mesh.across(), 2.0F);
    EXPECT_GE(mesh.layers.size(), std::size_t{25}) << "distinct lateral positions, in cm";
    EXPECT_GT(mesh.vertices, std::size_t{500}) << "a cut-out is a few dozen";
    for (const ShapePart& part : shape_of(UnitKind::Bomber)) {
        if (part.lit && part.role == ShapeRole::Hull) {
            EXPECT_GT(part.bevel, 0.0F) << "a body part with a square edge";
        }
    }
}

TEST(BomberFromShape, ItHasAFuselageAFinAWingAndABombAndNoWheelOrTrack) {
    // Found by geometry. The wing is the widest part and shallower than it is long; the fin is the
    // tallest, a thin blade; the bomb is the lit part that reaches the ground line, a dark body.
    const ShapePart* widest = nullptr;
    const ShapePart* tallest = nullptr;
    std::size_t on_the_ground = 0;
    for (const ShapePart& part : shape_of(UnitKind::Bomber)) {
        EXPECT_EQ(part.group, ShapeGroup::Hull) << "nothing on a bomber aims";
        EXPECT_LT(part.profile.size(), 12U) << "a wheel or a track end would be a ring of corners";
        if (!part.lit) {
            continue;
        }
        widest = widest == nullptr || part.across_hi > widest->across_hi ? &part : widest;
        tallest
            = tallest == nullptr || up_span(part).at(1) > up_span(*tallest).at(1) ? &part : tallest;
        if (up_span(part).at(0) < 0.001F) {
            ++on_the_ground;
            EXPECT_EQ(part.role, ShapeRole::Track) << "the bomb is dark";
        }
    }
    ASSERT_NE(widest, nullptr);
    EXPECT_NEAR(widest->across_hi - widest->across_lo, 2.4F, 0.005F) << "metres: the wing";
    EXPECT_LT(up_span(*widest).at(1) - up_span(*widest).at(0), 0.2F) << "metres: a thin wing";
    EXPECT_LT(tallest->across_hi - tallest->across_lo, 0.1F) << "metres: the fin is a blade";
    EXPECT_NEAR(up_span(*tallest).at(1), 1.1F, 0.005F) << "metres: the model's top";
    EXPECT_GE(on_the_ground, std::size_t{1}) << "the bomb hangs to the ground line";
    // The canopy is a cut through the fuselage: clear over solid, the roof lit above it.
    EXPECT_FALSE(shape_covers(UnitKind::Bomber, 0.3F, 0.66F)) << "the canopy";
    EXPECT_TRUE(shape_covers(UnitKind::Bomber, 0.3F, 0.76F)) << "the roof over it";
}

TEST(BomberFromShape, NothingOnItTurnsAndItCarriesNoGun) {
    const std::vector<BodyVertex> before = body_of(UnitKind::Bomber, 0.0F);
    const std::vector<BodyVertex> after = body_of(UnitKind::Bomber, 1.0F, 0.4F);
    ASSERT_EQ(before.size(), after.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(before.at(i).across, after.at(i).across) << "vertex " << i;
        EXPECT_EQ(before.at(i).along, after.at(i).along) << "vertex " << i;
        EXPECT_EQ(before.at(i).up, after.at(i).up) << "vertex " << i;
    }
}

// ------------------------------------------------------------------------- the defence turret

TEST(TurretFromShape, TheShapeIsAsLargeInMetresAsItWasAuthored) {
    const ShapeExtent extent = shape_extent(UnitKind::Turret);
    EXPECT_NEAR(extent.along_max - extent.along_min, kTurretBaseMetres, 0.005F)
        << "metres, the flared base's length";
    EXPECT_NEAR(extent.up_max, kTurretHeightMetres, 0.005F) << "metres, the top of the aerial";
    EXPECT_NEAR(2.0F * extent.across_max, kTurretWidthMetres, 0.005F) << "metres, the base";
    // The deployed turret is this machine in another state.
    const ShapeExtent deployed = shape_extent(UnitKind::DeployedTurret);
    EXPECT_EQ(deployed.along_min, extent.along_min);
    EXPECT_EQ(deployed.along_max, extent.along_max);
    EXPECT_EQ(deployed.up_max, extent.up_max);
}

TEST(TurretFromShape, EveryPartIsConvexAndHasRoomForItsBevel) {
    expect_parts_convex_with_room(UnitKind::Turret);
}

TEST(TurretFromShape, ThePlateIsTheShapeSeenSideOnPlusItsGun) {
    for (const UnitKind kind : {UnitKind::Turret, UnitKind::DeployedTurret}) {
        const PlateBox box = plate_box(kind_icon(kind));
        EXPECT_EQ(box.width(), 70) << "cells, the base's rear to the gun's tip (2.7 m)";
        EXPECT_EQ(box.first_col, 1);
        EXPECT_EQ(box.last_col, 70);
        EXPECT_NEAR(static_cast<float>(box.height()), 47.0F, 1.0F)
            << "cells, 1.8 m at 25.9 per metre";
        EXPECT_EQ(box.last_row, static_cast<int>(kIconGround)) << "the base stands on the ground";
    }
}

TEST(TurretFromShape, TheMeshIsTheShapeInMetres) {
    const MeshBox mesh = mesh_of(UnitKind::Turret);
    ASSERT_GT(mesh.vertices, 0U);
    // The gun, which is not a part, runs on to 2.0 m; the base's rear is 0.7 m behind the centre
    // (the lowest 5 cm of the mesh is the shadow's and is not measured).
    EXPECT_NEAR(mesh.high.at(2), kTurretTipMetres, 0.06F) << "metres, the muzzle collar's end";
    EXPECT_NEAR(mesh.low.at(2), -0.7F, 0.1F) << "metres, the base's rear";
    EXPECT_NEAR(mesh.high.at(1), kTurretHeightMetres, 0.06F) << "metres, the aerial";
    EXPECT_NEAR(mesh.across(), kTurretWidthMetres, 0.06F) << "metres, the base";
}

TEST(TurretFromShape, ItIsAsHighAsTheUndrivenOneWasAndNoLongerATanksHullWhenDriven) {
    // Measured before it had a shape: the undriven turret 2.6 m long, 1.2 m across, 1.8 m high;
    // the DRIVEN one 3.3 m long and 1.23 m high. The mesh is one machine now: the tip is where the
    // barrel always was, the height the undriven one's (and the picker's), the base a decimetre
    // deeper, and the hull of a tank is gone.
    const MeshBox mesh = mesh_of(UnitKind::Turret);
    EXPECT_NEAR(mesh.high.at(2), kTurretTipMetres, 0.06F) << "metres: the tip is unmoved";
    EXPECT_NEAR(mesh.high.at(1), kTurretHeightMetres, 0.06F) << "metres: the undriven one's top";
    EXPECT_GT(mesh.high.at(1), kTurretWasHighDrivenMetres + 0.5F) << "metres";
    EXPECT_LT(mesh.along(), 3.3F - 0.5F) << "metres: not the tank's hull's 3.3";
    EXPECT_GT(mesh.along(), kTurretWasLongUndrivenMetres - 0.1F) << "metres";
    EXPECT_NEAR(mesh.across(), kTurretWidthMetres, 0.06F) << "metres: the tank's width, kept";
}

TEST(TurretFromShape, ThePlateAndTheMeshAreTheSameMachine) {
    const PlateBox plate = plate_box(kind_icon(UnitKind::Turret));
    const MeshBox mesh = mesh_of(UnitKind::Turret);
    const float plate_ratio
        = static_cast<float>(plate.height()) / static_cast<float>(plate.width());
    EXPECT_NEAR(plate_ratio, mesh.high.at(1) / kTurretWholeMetres, 0.04F);
}

TEST(TurretFromShape, TheGunIsOnThePlateAndNotInTheShape) {
    // The gun is level from the dome's front face (0.5 m) to the tip (2.0 m), 1.2 to 1.36 m up. The
    // shape stops at the base's front and the dome's face; the plate carries the rest.
    EXPECT_FALSE(shape_covers(UnitKind::Turret, 1.2F, 1.28F));
    const IconMask& mask = kind_icon(UnitKind::Turret);
    const float cells_per_metre = 70.0F / kTurretWholeMetres;
    const auto col = [&](float along) {
        return static_cast<std::size_t>(1.0F + ((along + 0.7F) * cells_per_metre));
    };
    const auto row = [&](float up) {
        return static_cast<std::size_t>(static_cast<float>(kIconGround) - (up * cells_per_metre));
    };
    EXPECT_EQ(mask.at(row(1.28F)).at(col(1.2F)), '#') << "the barrel, 0.7 m past the dome";
    EXPECT_EQ(mask.at(row(1.28F)).at(col(1.9F)), '#') << "the muzzle collar";
    EXPECT_EQ(plate_box(mask).last_col, 70) << "and the muzzle collar's end is the plate's edge";
    EXPECT_EQ(mask.at(row(1.0F)).at(col(1.2F)), '.') << "the barrel is not a wall under it";
}

TEST(TurretFromShape, TheMeshHasDepthAndIsNotACutOut) {
    const MeshBox mesh = mesh_of(UnitKind::Turret);
    EXPECT_GT(mesh.across(), 1.0F);
    EXPECT_GE(mesh.layers.size(), std::size_t{15}) << "distinct lateral positions, in cm";
    EXPECT_GT(mesh.vertices, std::size_t{300}) << "a cut-out is a few dozen";
    for (const ShapePart& part : shape_of(UnitKind::Turret)) {
        if (part.lit && part.role != ShapeRole::Track) {
            EXPECT_GT(part.bevel, 0.0F) << "a body part with a square edge";
        }
    }
}

TEST(TurretFromShape, NothingOnItIsATrackAWheelOrAHullOfATank) {
    // The driven turret wore a tank's hull: tracks, wheels, fenders. None of that is here: no part
    // is mirrored (no pair of runs), none is a ring of corners (a wheel), and nothing is wider than
    // the base.
    for (const ShapePart& part : shape_of(UnitKind::Turret)) {
        EXPECT_FALSE(part.mirrored) << "a turret is not a machine with a left and a right track";
        // A tyre is a ring of twelve; a cut hull block is at most eight (a box with every corner
        // cut).
        EXPECT_LT(part.profile.size(), 12U) << "a wheel is a ring of corners";
        EXPECT_LE(part.across_hi, 0.6F);
    }
}

TEST(TurretFromShape, TheGroupsAreTheMachinesOwn) {
    // An oracle that does not read the tag: whatever stands wholly on or over the pedestal's top
    // (1.1 m) is the dome group, and whatever reaches below it is the base.
    std::size_t turret_parts = 0;
    std::size_t hull_parts = 0;
    for (const ShapePart& part : shape_of(UnitKind::Turret)) {
        const bool on_the_pedestal = up_span(part).at(0) >= 1.1F - 0.001F;
        EXPECT_EQ(part.group == ShapeGroup::Turret, on_the_pedestal);
        (on_the_pedestal ? turret_parts : hull_parts) += 1U;
    }
    EXPECT_EQ(turret_parts, std::size_t{3}) << "the dome, its slit and the aerial";
    EXPECT_EQ(hull_parts, std::size_t{2}) << "the base and the pedestal";
}

TEST(TurretFromShape, TheGroupIsRealTurnTheAimAndTheDomeMovesAndTheBaseDoesNot) {
    // The player can DRIVE a turret and aim it: the same group helper as the tank's.
    expect_turret_group_is_real(UnitKind::Turret, 100, 200); // measured: 120 and 246
}

TEST(TurretFromShape, TheDeployedTurretIsTheSameMachineAsTheTurret) {
    const MeshBox turret = mesh_of(UnitKind::Turret);
    const MeshBox deployed = mesh_of(UnitKind::DeployedTurret);
    EXPECT_EQ(deployed.vertices, turret.vertices);
    EXPECT_EQ(deployed.low, turret.low);
    EXPECT_EQ(deployed.high, turret.high);
}

// ------------------------------------------------------------------------- the chamfered hulls
// (AT)

/// The sharpest turn, in degrees, any corner of `profile` makes: 90 for a box's corner, 45 for a
/// box's corner that has been cut once.
[[nodiscard]] float sharpest_turn_degrees(const std::vector<std::array<float, 2>>& profile) {
    float sharpest = 0.0F;
    for (std::size_t i = 0; i < profile.size(); ++i) {
        const auto& a = profile.at(i);
        const auto& b = profile.at((i + 1) % profile.size());
        const auto& c = profile.at((i + 2) % profile.size());
        const float ux = b[0] - a[0];
        const float uy = b[1] - a[1];
        const float vx = c[0] - b[0];
        const float vy = c[1] - b[1];
        const float turn = std::atan2((ux * vy) - (uy * vx), (ux * vx) + (uy * vy));
        sharpest = std::max(sharpest, std::abs(turn) * 180.0F / std::numbers::pi_v<float>);
    }
    return sharpest;
}

/// The corners smooth shading cannot blend start at `kCreaseAngleDegrees` (75); a chamfered hull's
/// every turn stays at least 5 under that. A cut 90 is two 45s -- the 40 to 50 degree crowd AP
/// measured as "meant to be round" -- and the tank's glacis foot, a 127 degree turn, is two 63.7s.
constexpr float kChamferedTurnDegrees = kCreaseAngleDegrees - 5.0F;

[[nodiscard]] bool is_hull_body(const ShapePart& part) {
    return part.lit && (part.role == ShapeRole::Hull || part.role == ShapeRole::Load);
}

TEST(ChamferedHull, NoScavengerBodyPartHasACornerSmoothShadingCouldNotBlend) {
    std::size_t checked = 0;
    for (const ShapePart& part : shape_of(UnitKind::Scavenger)) {
        if (is_hull_body(part)) {
            ++checked;
            EXPECT_LT(sharpest_turn_degrees(part.profile), kChamferedTurnDegrees)
                << "a hull or load part with a box corner";
        }
    }
    EXPECT_GE(checked, std::size_t{5}) << "the floor, the wall, the gate, the scrap and the cab";
}

TEST(ChamferedHull, NoTankBodyPartHasACornerSmoothShadingCouldNotBlend) {
    std::size_t checked = 0;
    for (const ShapePart& part : shape_of(UnitKind::Tank)) {
        if (is_hull_body(part)) {
            ++checked;
            EXPECT_LT(sharpest_turn_degrees(part.profile), kChamferedTurnDegrees)
                << "a hull part with a box corner";
        }
    }
    EXPECT_GE(checked, std::size_t{5}) << "two hull blocks, a fender, the turret and its bustle";
}

TEST(ChamferedHull, TheCutsAreTheFortyToFiftyDegreeBandAndNotASoapBar) {
    // READABLE: a cut is a band, not a dissolve. The sharpest corner of a hull part is still a real
    // turn of 30 degrees or more -- a cut 90 is two 45s -- and a part has few corners, so it is a
    // chamfered block and not a smear; the extent tests above pin the overall size. Every shaped
    // kind (AY), not only the scavenger and the tank.
    for (const UnitKind kind : kShaped) {
        for (const ShapePart& part : shape_of(kind)) {
            if (is_hull_body(part)) {
                EXPECT_GE(sharpest_turn_degrees(part.profile), 30.0F)
                    << "dissolved into a ball: kind " << static_cast<int>(kind);
                EXPECT_LE(part.profile.size(), std::size_t{14})
                    << "a cut is two corners, not a curve";
            }
        }
    }
}

TEST(ChamferedHull, EveryShapedKindHasItsHullAndLoadCornersCut) {
    // AY: the cut is every authored kind's, not only the scavenger's and the tank's. No hull or
    // load part of any of the sixteen (the turret and its deployed form share one) keeps a corner
    // that smooth shading would leave hard.
    std::size_t checked = 0;
    for (const UnitKind kind : kShaped) {
        for (const ShapePart& part : shape_of(kind)) {
            if (is_hull_body(part)) {
                ++checked;
                EXPECT_LT(sharpest_turn_degrees(part.profile), kChamferedTurnDegrees)
                    << "a hull or load part with a box corner: kind " << static_cast<int>(kind);
            }
        }
    }
    EXPECT_GE(checked, std::size_t{40});
}

/// The lengths of the edges of `profile` that run on neither axis: the chords a cut leaves.
[[nodiscard]] std::vector<float> chord_lengths(const std::vector<std::array<float, 2>>& profile) {
    std::vector<float> chords;
    chords.reserve(profile.size());
    for (std::size_t i = 0; i < profile.size(); ++i) {
        const auto& a = profile.at(i);
        const auto& b = profile.at((i + 1) % profile.size());
        const float dx = std::abs(b[0] - a[0]);
        const float dy = std::abs(b[1] - a[1]);
        if (dx > 1.0e-4F && dy > 1.0e-4F && std::abs(dx - dy) < 1.0e-3F) {
            chords.push_back(std::hypot(dx, dy)); // a 45 degree cut: the box corner's own
        }
    }
    return chords;
}

/// The hull part of `kind` whose profile starts at `along_lo` and spans `up_lo` to `up_hi`. (The
/// far end is not asked: a cut corner there is no longer at the box's edge.)
[[nodiscard]] const ShapePart& part_spanning(UnitKind kind, float along_lo, float up_lo,
                                             float up_hi) {
    for (const ShapePart& part : shape_of(kind)) {
        float a_lo = kInfinity;
        float u_lo = kInfinity;
        float u_hi = -kInfinity;
        for (const auto& corner : part.profile) {
            a_lo = std::min(a_lo, corner[0]);
            u_lo = std::min(u_lo, corner[1]);
            u_hi = std::max(u_hi, corner[1]);
        }
        if (part.lit && part.role == ShapeRole::Hull && std::abs(a_lo - along_lo) < 1.0e-3F
            && std::abs(u_lo - up_lo) < 1.0e-3F && std::abs(u_hi - up_hi) < 1.0e-3F) {
            return part;
        }
    }
    ADD_FAILURE() << "no such hull part";
    return shape_of(kind).front();
}

TEST(ChamferedHull, TheCutIsPronouncedOnTheTankAndTheScavenger) {
    // AY, MEASURED: AT cut 0.10 m and the owner asked for more. The tank's engine deck (0.8 m by
    // 0.62 m) is the largest cut the two machines' big blocks can ALL take in full -- its cap,
    // 0.4 of its 0.62 m edge, is 0.248 -- so the cut is 0.25 m on the blocks that can take it:
    // a chord of 0.25 * sqrt(2) = 0.354 m, 2.5 times AT's 0.141.
    constexpr float kChordMetres = 0.25F * 1.41421356F;
    const ShapePart& deck = part_spanning(UnitKind::Tank, -1.2F, 0.0F, 0.62F);
    const std::vector<float> deck_chords = chord_lengths(deck.profile);
    ASSERT_EQ(deck_chords.size(), std::size_t{4}) << "four box corners, four cuts";
    for (const float chord : deck_chords) {
        EXPECT_GE(chord, kChordMetres * 0.99F);
        EXPECT_LE(chord, kChordMetres * 1.01F) << "past the cap two cuts would meet";
    }
    // The compartment (0.85 m tall) takes the same cut in full on its rear corners.
    const ShapePart& compartment = part_spanning(UnitKind::Tank, -0.45F, 0.0F, 0.85F);
    const std::vector<float> compartment_chords = chord_lengths(compartment.profile);
    ASSERT_FALSE(compartment_chords.empty());
    EXPECT_GE(*std::max_element(compartment_chords.begin(), compartment_chords.end()),
              kChordMetres * 0.99F);
    // The scavenger's cab (0.9 m by 0.8 m) too.
    const ShapePart& cab = part_spanning(UnitKind::Scavenger, 0.3F, 0.45F, 1.25F);
    const std::vector<float> cab_chords = chord_lengths(cab.profile);
    ASSERT_FALSE(cab_chords.empty());
    EXPECT_GE(*std::max_element(cab_chords.begin(), cab_chords.end()), kChordMetres * 0.99F);
}

TEST(ChamferedHull, NoCutOverlapsItsNeighboursAndNoPartIsMostlyGone) {
    // The ceiling: a cut is at most 0.45 of the shorter edge it lands on, so two cuts sharing an
    // edge leave a tenth of it flat. An edge that has collapsed to nothing (or past it, which the
    // convexity test above also catches) is two cuts that met. And a part is not dissolved: it
    // keeps more than 55% of its bounding box: a square with all four
    // corners cut at the 0.45 cap keeps 1 - 4 * (0.45^2 / 2) = 59.5%, so the floor is under the
    // most the cap can take.
    for (const UnitKind kind : kShaped) {
        for (const ShapePart& part : shape_of(kind)) {
            if (!is_hull_body(part)) {
                continue;
            }
            float a_lo = kInfinity;
            float a_hi = -kInfinity;
            float u_lo = kInfinity;
            float u_hi = -kInfinity;
            float area = 0.0F;
            for (std::size_t i = 0; i < part.profile.size(); ++i) {
                const auto& a = part.profile.at(i);
                const auto& b = part.profile.at((i + 1) % part.profile.size());
                EXPECT_GT(std::hypot(b[0] - a[0], b[1] - a[1]), 0.004F)
                    << "two cuts met: kind " << static_cast<int>(kind);
                a_lo = std::min(a_lo, a[0]);
                a_hi = std::max(a_hi, a[0]);
                u_lo = std::min(u_lo, a[1]);
                u_hi = std::max(u_hi, a[1]);
                area += (a[0] * b[1]) - (b[0] * a[1]);
            }
            area *= 0.5F;
            EXPECT_GT(area, 0.55F * (a_hi - a_lo) * (u_hi - u_lo))
                << "dissolved: kind " << static_cast<int>(kind);
        }
    }
}

TEST(ChamferedHull, TheMeshGainsFacetsButStaysTheSameSizeOfMachine) {
    // Measured before the chamfer, in vertices as the frame draws them (shadow excluded, three to a
    // triangle): the scavenger 3,436 (1,145 triangles) and the tank 2,544 (848). A cut adds facets
    // -- more than before, under double -- and the extent tests above pin the size.
    constexpr std::size_t kScavengerBefore = 3436;
    constexpr std::size_t kTankBefore = 2544;
    const std::size_t scavenger = mesh_of(UnitKind::Scavenger).vertices;
    const std::size_t tank = mesh_of(UnitKind::Tank).vertices;
    EXPECT_GT(scavenger, kScavengerBefore);
    EXPECT_LT(scavenger, 2 * kScavengerBefore);
    EXPECT_GT(tank, kTankBefore);
    EXPECT_LT(tank, 2 * kTankBefore);
}

} // namespace
} // namespace protekt::game
