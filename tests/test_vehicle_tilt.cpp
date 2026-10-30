// A vehicle's body follows the ground it stands on: pitch and roll from the slope under it, clamped
// at `kMaxTiltSlope`, a structure left upright, and a vehicle on level ground drawn exactly as it
// was before the tilt existed. All of it is drawing -- the sim is never touched.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "protekt/game/scene.hpp"
#include "protekt/math/math.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::game {
namespace {

using math::Vec3;
using sim::Side;
using sim::Spawn;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr float kFlatTolerance = 1.0e-3F;
/// Wider than the hull and its barrel (2 m from the centre) and well inside the HUD, which sits
/// metres from the camera: the run of vertices that belong to the unit is the ones this close.
constexpr float kBodyReachMetres = 4.0F;

/// A unit placed on the ground, and where the scene drew its body.
struct Placed {
    bool drawn{false}; ///< false when the unit could not be placed, and then nothing else is set
    float x{};         ///< world metres
    float z{};         ///< world metres
    float ground{};
    Vec3 normal{};
    std::vector<Vec3> body; ///< the unit's own vertices, relative to (x, ground, z)
};

/// Where the sim stands a unit spawned on `tile`: on the tile's corner (`fresh_unit`).
[[nodiscard]] sim::Fixed stands_at(int tile) { return sim::Fixed::from_int(tile); }

/// Draws one `kind` standing at `tile_x`, `tile_y` and returns its body. The body is cut from the
/// frame as the run of vertices after the shadow that stays inside `kBodyReachMetres` of the unit.
[[nodiscard]] Placed draw_unit(UnitKind kind, int tile_x, int tile_y) {
    const sim::Sim empty = sim::Sim::create(sim::Scenario{kSeed, {}});
    const sim::Sim with = sim::Sim::create(
        sim::Scenario{kSeed, std::vector<Spawn>{{kind, Side::Player, tile_x, tile_y}}});
    Placed placed;
    if (with.units().size() != 1U) {
        return placed;
    }
    placed.drawn = true;
    const sim::Unit& unit = with.units().front();
    placed.x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    placed.z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    placed.ground = ground_metres(kSeed, unit.x, unit.y);
    placed.normal = ground_normal(kSeed, unit.x, unit.y);

    SceneOptions options;
    options.seed = kSeed;
    options.radius_tiles = 8;
    options.show_ship_gauge = false;
    options.show_ledger = false;
    options.camera.x = placed.x;
    options.camera.z = placed.z - 8.0F;
    options.camera.y = placed.ground + 5.0F;
    options.camera.pitch = -0.25F;
    const Scene base = build_scene(empty, options);
    const Scene scene = build_scene(with, options);

    // The terrain and sky come first and are the same with or without the unit; then its shadow
    // (one quad, six vertices); then the body.
    std::size_t first = 0;
    while (first < base.vertices.size() && first < scene.vertices.size()
           && base.vertices.at(first).position == scene.vertices.at(first).position) {
        ++first;
    }
    constexpr std::size_t kShadowVertices = 6;
    for (std::size_t i = first + kShadowVertices; i < scene.vertices.size(); ++i) {
        const Vec3& p = scene.vertices.at(i).position;
        const Vec3 rel{p[0] - placed.x, p[1] - placed.ground, p[2] - placed.z};
        if (std::hypot(rel[0], rel[1], rel[2]) > kBodyReachMetres) {
            break;
        }
        placed.body.push_back(rel);
    }
    return placed;
}

/// The size of the tilt at a spot, as a slope: horizontal run of the normal over its height.
[[nodiscard]] float tilt_of(const Vec3& normal) {
    return std::hypot(normal[0], normal[2]) / normal[1];
}

/// Finds a tile on the map where `want` holds of the ground normal, by looking rather than
/// assuming; false when there is none.
template <typename Want> [[nodiscard]] bool find_tile(Want want, std::array<int, 2>& found) {
    for (int ty = 20; ty < 220; ++ty) {
        for (int tx = 20; tx < 220; ++tx) {
            if (want(ground_normal(kSeed, stands_at(tx), stands_at(ty)))) {
                found = {tx, ty};
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] bool is_level(const Vec3& n) { return n == Vec3{0.0F, 1.0F, 0.0F}; }
/// Leaning, and under the clamp, so the tilt is the ground's own and not the limit's.
[[nodiscard]] bool is_gentle_slope(const Vec3& n) {
    return tilt_of(n) > 0.1F && tilt_of(n) < kMaxTiltSlope - 0.02F;
}
[[nodiscard]] bool is_clamped(const Vec3& n) { return tilt_of(n) > kMaxTiltSlope - 1.0e-4F; }

TEST(VehicleTilt, TheGroundNormalIsExactlyUpOnLevelGroundAndUnitLengthOtherwise) {
    std::array<int, 2> level{};
    ASSERT_TRUE(find_tile(is_level, level)) << "no level ground on the map, so nothing is proved";
    EXPECT_TRUE(is_level(ground_normal(kSeed, stands_at(level.at(0)), stands_at(level.at(1)))));
    std::array<int, 2> slope{};
    ASSERT_TRUE(find_tile(is_gentle_slope, slope));
    const Vec3 n = ground_normal(kSeed, stands_at(slope.at(0)), stands_at(slope.at(1)));
    EXPECT_NEAR(math::dot(n, n), 1.0F, 1.0e-5F);
    EXPECT_LT(n[1], 1.0F);
}

TEST(VehicleTilt, NoGroundNormalLeansPastTheClampAndSomeReachIt) {
    // Every tile in the scan stays inside the limit; and the limit is REACHED somewhere, or the
    // clamp is never exercised and "stays inside" is a pass for the wrong reason.
    std::size_t scanned = 0;
    std::size_t at_limit = 0;
    for (int ty = 20; ty < 220; ++ty) {
        for (int tx = 20; tx < 220; ++tx) {
            const Vec3 n = ground_normal(kSeed, stands_at(tx), stands_at(ty));
            EXPECT_LE(tilt_of(n), kMaxTiltSlope + 1.0e-4F) << "tile " << tx << "," << ty;
            ++scanned;
            if (is_clamped(n)) {
                ++at_limit;
            }
        }
    }
    EXPECT_EQ(scanned, 200U * 200U);
    EXPECT_GT(at_limit, 0U) << "no tile is steep enough to hit the clamp";
}

TEST(VehicleTilt, AVehicleOnLevelGroundIsDrawnTheSameWhereverTheGroundIsLevel) {
    // The regression pin: level ground rotates nothing, so the body is the same shape at every
    // level spot (the existing scene tests pin that shape itself).
    std::array<int, 2> a{};
    ASSERT_TRUE(find_tile(is_level, a));
    std::array<int, 2> b{};
    bool found_b = false;
    for (int ty = a.at(1); ty < 220 && !found_b; ++ty) {
        for (int tx = 20; tx < 220 && !found_b; ++tx) {
            if (is_level(ground_normal(kSeed, stands_at(tx), stands_at(ty)))
                && std::abs(tx - a.at(0)) > 12) {
                b = {tx, ty};
                found_b = true;
            }
        }
    }
    ASSERT_TRUE(found_b) << "one level spot is no comparison";
    for (const UnitKind kind : {UnitKind::Tank, UnitKind::Bomber, UnitKind::Walker}) {
        const Placed one = draw_unit(kind, a.at(0), a.at(1));
        const Placed two = draw_unit(kind, b.at(0), b.at(1));
        ASSERT_TRUE(one.drawn && two.drawn);
        ASSERT_TRUE(is_level(one.normal) && is_level(two.normal)) << "the unit was nudged";
        ASSERT_FALSE(one.body.empty()) << "no body was found";
        ASSERT_EQ(one.body.size(), two.body.size());
        for (std::size_t i = 0; i < one.body.size(); ++i) {
            for (std::size_t c = 0; c < 3; ++c) {
                EXPECT_NEAR(one.body.at(i).at(c), two.body.at(i).at(c), kFlatTolerance)
                    << "vertex " << i;
            }
        }
    }
}

TEST(VehicleTilt, AVehicleOnASlopeIsDrawnDifferentlyAndItsUpFollowsTheGround) {
    std::array<int, 2> flat{};
    std::array<int, 2> hill{};
    ASSERT_TRUE(find_tile(is_level, flat));
    ASSERT_TRUE(find_tile(is_gentle_slope, hill));
    for (const UnitKind kind : {UnitKind::Tank, UnitKind::Bomber, UnitKind::Walker}) {
        const Placed level = draw_unit(kind, flat.at(0), flat.at(1));
        const Placed sloped = draw_unit(kind, hill.at(0), hill.at(1));
        ASSERT_TRUE(level.drawn && sloped.drawn);
        ASSERT_TRUE(is_gentle_slope(sloped.normal)) << "the unit was nudged off the slope";
        ASSERT_FALSE(level.body.empty());
        ASSERT_EQ(level.body.size(), sloped.body.size());
        float most_moved = 0.0F;
        for (std::size_t i = 0; i < level.body.size(); ++i) {
            // The same point of the body, measured UP the ground's normal on the slope, is as high
            // as it was measured up on the level: the body was turned, not bent or lifted.
            EXPECT_NEAR(math::dot(sloped.body.at(i), sloped.normal), level.body.at(i)[1],
                        kFlatTolerance)
                << "vertex " << i;
            for (std::size_t c = 0; c < 3; ++c) {
                most_moved = std::max(most_moved,
                                      std::abs(sloped.body.at(i).at(c) - level.body.at(i).at(c)));
            }
        }
        EXPECT_GT(most_moved, 0.05F) << "the slope changed nothing";
    }
}

TEST(VehicleTilt, ASteepFaceTiltsNoFurtherThanTheClamp) {
    std::array<int, 2> cliff{};
    std::array<int, 2> flat{};
    ASSERT_TRUE(find_tile(is_clamped, cliff));
    ASSERT_TRUE(find_tile(is_level, flat));
    const Placed level = draw_unit(UnitKind::Tank, flat.at(0), flat.at(1));
    const Placed steep = draw_unit(UnitKind::Tank, cliff.at(0), cliff.at(1));
    ASSERT_TRUE(level.drawn && steep.drawn);
    ASSERT_TRUE(is_clamped(steep.normal)) << "the unit was nudged off the cliff";
    ASSERT_EQ(level.body.size(), steep.body.size());
    ASSERT_FALSE(steep.body.empty());
    // Turned by the clamp and no more: the body's up is the clamped normal, and heights measured
    // up it are the level body's heights.
    for (std::size_t i = 0; i < steep.body.size(); ++i) {
        EXPECT_NEAR(math::dot(steep.body.at(i), steep.normal), level.body.at(i)[1], kFlatTolerance)
            << "vertex " << i;
    }
    EXPECT_LE(tilt_of(steep.normal), kMaxTiltSlope + 1.0e-4F);
}

TEST(VehicleTilt, AStructureStandsUprightOnTheSlopeThatTiltsAVehicle) {
    std::array<int, 2> flat{};
    std::array<int, 2> hill{};
    ASSERT_TRUE(find_tile(is_level, flat));
    ASSERT_TRUE(find_tile(is_gentle_slope, hill));
    for (const UnitKind kind : {UnitKind::Factory, UnitKind::Turret}) {
        const Placed level = draw_unit(kind, flat.at(0), flat.at(1));
        const Placed sloped = draw_unit(kind, hill.at(0), hill.at(1));
        ASSERT_TRUE(level.drawn && sloped.drawn);
        ASSERT_TRUE(is_gentle_slope(sloped.normal));
        ASSERT_FALSE(level.body.empty()) << "no body was found";
        ASSERT_EQ(level.body.size(), sloped.body.size());
        for (std::size_t i = 0; i < level.body.size(); ++i) {
            for (std::size_t c = 0; c < 3; ++c) {
                EXPECT_NEAR(sloped.body.at(i).at(c), level.body.at(i).at(c), kFlatTolerance)
                    << "vertex " << i << " moved";
            }
        }
    }
}

} // namespace
} // namespace protekt::game
