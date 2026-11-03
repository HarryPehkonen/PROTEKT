// The view rides the vehicle: the camera's frame is the hull's, so a hull that leans gives a view
// that leans with it, the eye stays over the same part of the vehicle, and on level ground nothing
// is any different from the upright camera it replaces. All presentation -- the sim is not touched.
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "protekt/app/player_view.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/session.hpp"
#include "protekt/math/math.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::app {
namespace {

using game::Camera;
using math::Vec3;
using sim::Side;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr game::Extent kExtent{1280, 720};
constexpr Vec3 kWorldUp{0.0F, 1.0F, 0.0F};

constexpr std::array<game::CameraPosition, game::kCameraPositionCount> kPositions{
    game::CameraPosition::Barrel, game::CameraPosition::Chase, game::CameraPosition::HighChase};

// ------------------------------------------------------------------------------ finding a slope

[[nodiscard]] float slope_of(const Vec3& normal) {
    return std::hypot(normal[0], normal[2]) / normal[1];
}
[[nodiscard]] Vec3 normal_at(int tx, int ty) {
    return game::ground_normal(kSeed, sim::Fixed::from_int(tx), sim::Fixed::from_int(ty));
}

/// A tile where `want` holds of the ground normal, found by looking; false when there is none.
template <typename Want> [[nodiscard]] bool find_tile(Want want, std::array<int, 2>& found) {
    for (int ty = 20; ty < 220; ++ty) {
        for (int tx = 20; tx < 220; ++tx) {
            if (want(normal_at(tx, ty))) {
                found = {tx, ty};
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] bool is_level(const Vec3& n) { return n == kWorldUp; }
/// Leaning, and under the clamp, so the lean is the ground's own.
[[nodiscard]] bool is_gentle(const Vec3& n) {
    return slope_of(n) > 0.1F && slope_of(n) < game::kMaxTiltSlope - 0.02F;
}

/// One player tank standing on `tile`, and the session that drives it.
struct Hull {
    sim::Sim sim;
    sim::UnitId id{};
    Vec3 pivot{};  ///< the point under the hull's centre, at the height the hull rides at rest
    Vec3 normal{}; ///< `game::ground_normal` there
};

[[nodiscard]] Hull hull_on(const std::array<int, 2>& tile) {
    Hull hull;
    hull.sim = sim::Sim::create(sim::Scenario{
        kSeed, std::vector<sim::Spawn>{{UnitKind::Tank, Side::Player, tile.at(0), tile.at(1)}}});
    const sim::Unit& unit = hull.sim.units().front();
    hull.id = unit.id;
    hull.pivot = {static_cast<float>(unit.x.raw) / 65536.0F * game::kMetresPerTile,
                  game::ground_metres(kSeed, unit.x, unit.y) + game::kRideHeightMetres,
                  static_cast<float>(unit.y.raw) / 65536.0F * game::kMetresPerTile};
    hull.normal = game::ground_normal(kSeed, unit.x, unit.y);
    return hull;
}

[[nodiscard]] Vec3 sub(const Vec3& a, const Vec3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
[[nodiscard]] Vec3 eye_of(const Camera& c) { return {c.x, c.y, c.z}; }

/// Presses V `presses` times, to reach a camera position the way the player does.
[[nodiscard]] game::Session session_at(const Hull& hull, std::size_t presses) {
    game::Session session{hull.id};
    game::SceneOptions options;
    options.seed = kSeed;
    options.extent = kExtent;
    for (std::size_t i = 0; i < presses; ++i) {
        input::InputFrame frame;
        frame.keys.push_back(input::KeyEvent{input::kKeyV, true, false});
        std::vector<sim::Order> out;
        session.frame(frame, hull.sim, options, out);
    }
    return session;
}

using Mat4Probe = math::Mat4;
[[nodiscard]] Mat4Probe view_with_up(const Vec3& up) {
    return math::view_projection({0.0F, 0.0F, 0.0F}, 0.3F, 0.5F, 1.0F, math::kFovRadians,
                                 math::kNearPlane, math::kFarPlane, up);
}

// ---------------------------------------------------------------- the camera's up is the hull's

TEST(ViewTilt, TheCamerasUpOnASlopedTileIsTheHullsUpAndNotTheWorlds) {
    std::array<int, 2> tile{};
    ASSERT_TRUE(find_tile(is_gentle, tile)) << "no gentle slope on the map, so nothing is proved";
    const Hull hull = hull_on(tile);
    ASSERT_FALSE(is_level(hull.normal)) << "the vacuous guard: the hull must be leaning";
    for (std::size_t i = 0; i < kPositions.size(); ++i) {
        const Camera camera = camera_for(hull.sim, session_at(hull, i), kSeed, std::nullopt);
        EXPECT_EQ(camera.up, hull.normal) << "position " << i << ": not the hull's up";
        EXPECT_NE(camera.up, kWorldUp) << "position " << i << ": still the world's up";
    }
}

TEST(ViewTilt, TheCamerasBasisHasTheHullsUpAndTheViewsOwnForward) {
    std::array<int, 2> tile{};
    ASSERT_TRUE(find_tile(is_gentle, tile));
    const Hull hull = hull_on(tile);
    const Camera camera = camera_for(hull.sim, session_at(hull, 0), kSeed, std::nullopt);
    const math::Basis tilted = math::basis_from(camera.yaw, camera.pitch, camera.up);
    const math::Basis upright = math::basis_from(camera.yaw, camera.pitch);
    // Where the view points is the player's yaw and pitch, whatever frame they are expressed in.
    EXPECT_EQ(tilted.forward, upright.forward);
    // The frame is the hull's: right is square to the hull's up, and the camera's up leans with it.
    EXPECT_NEAR(math::dot(tilted.right, hull.normal), 0.0F, 1.0e-5F);
    EXPECT_NEAR(math::dot(tilted.right, tilted.forward), 0.0F, 1.0e-5F);
    EXPECT_NEAR(math::dot(tilted.up, tilted.forward), 0.0F, 1.0e-5F);
    EXPECT_NEAR(math::dot(tilted.up, tilted.right), 0.0F, 1.0e-5F);
    EXPECT_NEAR(math::dot(tilted.up, tilted.up), 1.0F, 1.0e-5F);
    EXPECT_GT(math::dot(tilted.up, hull.normal), 0.9F)
        << "the camera's up is the hull's side of up";
    // And it is NOT the upright frame: the vacuous guard.
    EXPECT_GT(std::abs(math::dot(tilted.up, upright.up) - 1.0F), 1.0e-4F);
    EXPECT_NE(tilted.right, upright.right);
}

TEST(ViewTilt, TheViewMatrixAndThePickRayAreBuiltFromTheSameTiltedFrame) {
    std::array<int, 2> tile{};
    ASSERT_TRUE(find_tile(is_gentle, tile));
    const Hull hull = hull_on(tile);
    const Camera camera = camera_for(hull.sim, session_at(hull, 0), kSeed, std::nullopt);
    const Camera upright = [&] {
        Camera c = camera;
        c.up = kWorldUp;
        return c;
    }();
    // The ray through a pixel, followed 20 m, comes back to the same pixel through the view
    // matrix. With the ray built from the world frame and the matrix from the hull's, it would not.
    for (const std::array<float, 2> pixel :
         {std::array<float, 2>{640.0F, 360.0F}, std::array<float, 2>{200.0F, 100.0F},
          std::array<float, 2>{1100.0F, 650.0F}}) {
        const bool centre = pixel.at(0) == 640.0F;
        const game::RayPoint p = game::ray_point(camera, kExtent, pixel.at(0), pixel.at(1), 20.0F);
        const game::Projected back = game::project(camera, kExtent, p.x, p.y, p.z);
        EXPECT_NEAR(back.x, pixel.at(0), 0.05F);
        EXPECT_NEAR(back.y, pixel.at(1), 0.05F);
        // Can fail: the same point through the UPRIGHT matrix is somewhere else.
        const game::Projected off = game::project(upright, kExtent, p.x, p.y, p.z);
        if (!centre) { // the screen's centre is on the view axis, which a roll leaves where it is
            EXPECT_GT(std::hypot(off.x - pixel.at(0), off.y - pixel.at(1)), 5.0F)
                << "the lean must move the picture, or this proves nothing";
        }
    }
}

// ------------------------------------------------------------------------------- level ground

/// The upright basis exactly as it was written before `hull_up` existed.
[[nodiscard]] math::Basis basis_as_it_was(float yaw, float pitch) {
    const float cp = std::cos(pitch);
    const Vec3 forward{std::sin(yaw) * cp, std::sin(pitch), std::cos(yaw) * cp};
    const float right_length = std::sqrt((forward[2] * forward[2]) + (forward[0] * forward[0]));
    const Vec3 right{forward[2] / right_length, 0.0F, -forward[0] / right_length};
    return math::Basis{forward, right, math::cross(forward, right)};
}

TEST(ViewTilt, OnLevelGroundTheBasisIsTheUprightOneExactly) {
    for (float yaw = -3.1F; yaw < 3.2F; yaw += 0.37F) {
        for (float pitch = -1.3F; pitch <= 1.31F; pitch += 0.26F) {
            const math::Basis was = basis_as_it_was(yaw, pitch);
            for (const math::Basis& now :
                 {math::basis_from(yaw, pitch), math::basis_from(yaw, pitch, kWorldUp)}) {
                EXPECT_EQ(now.forward, was.forward) << yaw << ' ' << pitch;
                EXPECT_EQ(now.right, was.right) << yaw << ' ' << pitch;
                EXPECT_EQ(now.up, was.up) << yaw << ' ' << pitch;
            }
        }
    }
}

TEST(ViewTilt, OnLevelGroundTheViewMatrixIsTheUprightOneExactly) {
    const Vec3 eye{12.0F, 7.5F, -3.0F};
    for (float yaw : {0.0F, 0.9F, -2.2F}) {
        for (float pitch : {-0.4F, 0.0F, 0.7F}) {
            EXPECT_EQ(math::view_projection(eye, yaw, pitch, 16.0F / 9.0F),
                      math::view_projection(eye, yaw, pitch, 16.0F / 9.0F, math::kFovRadians,
                                            math::kNearPlane, math::kFarPlane, kWorldUp));
        }
    }
    // And the game's wrapper, given a default camera, is the matrix the upright call gives.
    Camera camera;
    camera.x = 12.0F;
    camera.y = 7.5F;
    camera.z = -3.0F;
    camera.yaw = 0.9F;
    camera.pitch = -0.4F;
    EXPECT_EQ(game::view_projection(camera, 16.0F / 9.0F),
              math::view_projection(eye, 0.9F, -0.4F, 16.0F / 9.0F));
}

TEST(ViewTilt, OnLevelGroundTheCameraIsWhatItWasBeforeTheViewRodeTheHull) {
    std::array<int, 2> tile{};
    ASSERT_TRUE(find_tile(is_level, tile)) << "no level ground on the map, so nothing is proved";
    const Hull hull = hull_on(tile);
    ASSERT_EQ(hull.normal, kWorldUp);
    for (std::size_t i = 0; i < kPositions.size(); ++i) {
        const game::Session session = session_at(hull, i);
        const Camera camera = camera_for(hull.sim, session, kSeed, std::nullopt);
        EXPECT_EQ(camera.up, kWorldUp);
        // The eye as the upright camera computed it: behind the hull along the yaw, over it.
        const EyePlacement placement = eye_placement(kPositions.at(i));
        const std::array<float, 3> forward = math::basis_from(session.view_yaw(), 0.0F).forward;
        const float x = hull.pivot[0] - (forward.at(0) * placement.behind_metres);
        const float z = hull.pivot[2] - (forward.at(2) * placement.behind_metres);
        const float y = std::max(
            hull.pivot[1] + placement.above_hull_metres,
            game::ground_metres(kSeed, x / game::kMetresPerTile, z / game::kMetresPerTile)
                + kEyeGroundClearanceMetres);
        EXPECT_EQ(camera.x, x) << "position " << i;
        EXPECT_EQ(camera.y, y) << "position " << i;
        EXPECT_EQ(camera.z, z) << "position " << i;
    }
}

TEST(ViewTilt, ALookStraightAlongTheHullsUpFallsBackToUprightInsteadOfNaN) {
    // Cannot happen in play (pitch stops at 1.3 rad, the lean at 22.6 degrees); the guard is cheap.
    const Vec3 forward = math::basis_from(0.3F, 0.5F).forward;
    const math::Basis guarded = math::basis_from(0.3F, 0.5F, forward);
    for (std::size_t c = 0; c < 3; ++c) {
        EXPECT_TRUE(std::isfinite(guarded.right.at(c)));
        EXPECT_TRUE(std::isfinite(guarded.up.at(c)));
    }
    EXPECT_EQ(guarded.right, math::basis_from(0.3F, 0.5F).right);
    const Mat4Probe probe = view_with_up(forward);
    for (const float value : probe) {
        EXPECT_TRUE(std::isfinite(value));
    }
}

// -------------------------------------------------------------------- the eye in the hull's frame

/// `offset` carried through the hull's lean: the vector the hull-level offset becomes. Same
/// rotation the body is drawn with, so a disagreement between eye and body would show here.
[[nodiscard]] Vec3 leaned(const game::GroundTilt& tilt, const Vec3& pivot, const Vec3& offset) {
    return sub(tilt.apply(Vec3{pivot[0] + offset[0], pivot[1] + offset[1], pivot[2] + offset[2]}),
               pivot);
}

TEST(ViewTilt, TheEyeStaysAtTheSameOffsetInTheHullsFrameAsTheHullLeans) {
    std::array<int, 2> tile{};
    ASSERT_TRUE(find_tile(is_gentle, tile));
    const Hull hull = hull_on(tile);
    // Lifted well clear of the ground, so the clearance clamp (a world-frame rule) is not engaged.
    const Vec3 high_pivot{hull.pivot[0], hull.pivot[1] + 200.0F, hull.pivot[2]};
    const game::GroundTilt tilt = game::tilt_about(high_pivot, hull.normal);
    ASSERT_FALSE(tilt.level());
    for (std::size_t i = 0; i < kPositions.size(); ++i) {
        const EyePlacement placement = eye_placement(kPositions.at(i));
        for (float yaw : {0.0F, 1.1F, -2.0F}) {
            const Camera on_flat
                = camera_on_hull(high_pivot, kWorldUp, placement, yaw, -0.1F, kSeed);
            const Camera on_slope
                = camera_on_hull(high_pivot, hull.normal, placement, yaw, -0.1F, kSeed);
            const Vec3 flat_offset = sub(eye_of(on_flat), high_pivot);
            const Vec3 slope_offset = sub(eye_of(on_slope), high_pivot);
            // The hull-relative offset: the flat one, carried through the lean -- the same.
            const Vec3 want = leaned(tilt, high_pivot, flat_offset);
            for (std::size_t c = 0; c < 3; ++c) {
                EXPECT_NEAR(slope_offset.at(c), want.at(c), 2.0e-3F)
                    << "position " << i << " yaw " << yaw << " axis " << c;
            }
            // Read in the hull's own axes: as high over the hull as the placement says, and as far
            // behind it along the turret's heading, whatever the lean.
            EXPECT_NEAR(math::dot(slope_offset, hull.normal), placement.above_hull_metres, 2.0e-3F);
            const std::array<float, 3> heading = math::basis_from(yaw, 0.0F).forward;
            const Vec3 hull_forward
                = leaned(tilt, high_pivot, Vec3{heading.at(0), 0.0F, heading.at(2)});
            EXPECT_NEAR(math::dot(slope_offset, hull_forward), -placement.behind_metres, 2.0e-3F);
            // The world-relative offset is NOT the flat one: the vacuous guard.
            EXPECT_GT(std::hypot(slope_offset[0] - flat_offset[0], slope_offset[1] - flat_offset[1],
                                 slope_offset[2] - flat_offset[2]),
                      0.05F)
                << "position " << i << ": the eye did not move with the lean";
        }
    }
}

TEST(ViewTilt, TheCameraForADrivenTankRidesTheHullAtEveryPosition) {
    std::array<int, 2> tile{};
    ASSERT_TRUE(find_tile(is_gentle, tile));
    const Hull hull = hull_on(tile);
    const game::GroundTilt tilt = game::tilt_about(hull.pivot, hull.normal);
    for (std::size_t i = 0; i < kPositions.size(); ++i) {
        const game::Session session = session_at(hull, i);
        const Camera camera = camera_for(hull.sim, session, kSeed, std::nullopt);
        const EyePlacement placement = eye_placement(kPositions.at(i));
        const std::array<float, 3> heading = math::basis_from(session.view_yaw(), 0.0F).forward;
        const Vec3 level_eye{hull.pivot[0] - (heading.at(0) * placement.behind_metres),
                             hull.pivot[1] + placement.above_hull_metres,
                             hull.pivot[2] - (heading.at(2) * placement.behind_metres)};
        const Vec3 want = tilt.apply(level_eye);
        EXPECT_NEAR(camera.x, want[0], 1.0e-4F) << "position " << i;
        EXPECT_NEAR(camera.z, want[2], 1.0e-4F) << "position " << i;
        // The ground clearance may lift a chase eye behind a rise, never lower it.
        EXPECT_GE(camera.y, want[1] - 1.0e-4F) << "position " << i;
        EXPECT_EQ(camera.up, hull.normal) << "position " << i << ": a chase view leans too";
        // A chase view is a camera mounted on the vehicle: its height over the hull, measured along
        // the hull's up, is the placement's whatever the lean (when the clamp is not engaged).
        if (camera.y <= want[1] + 1.0e-4F) {
            EXPECT_NEAR(math::dot(sub(eye_of(camera), hull.pivot), hull.normal),
                        placement.above_hull_metres, 1.0e-3F)
                << "position " << i;
        }
    }
}

// ------------------------------------------------------------------------ the muzzle stays in
// frame

/// The muzzle tip on a leaning hull: the tip's level position, carried through the lean.
[[nodiscard]] Vec3 tip_on(const Hull& hull, float yaw) {
    const std::array<float, 3> heading = math::basis_from(yaw, 0.0F).forward;
    const game::GroundTilt tilt = game::tilt_about(hull.pivot, hull.normal);
    return tilt.apply(Vec3{hull.pivot[0] + (heading.at(0) * game::kTankBarrelTip),
                           hull.pivot[1] + game::kTankBarrelAxisMetres,
                           hull.pivot[2] + (heading.at(2) * game::kTankBarrelTip)});
}

TEST(ViewTilt, TheMuzzleTipStaysInFrameAtEveryPositionOnASlopeLookingUpAndAcrossIt) {
    std::array<int, 2> tile{};
    ASSERT_TRUE(find_tile(is_gentle, tile));
    const Hull hull = hull_on(tile);
    // Looking up the slope, and across it (a roll): the two ways a lean meets the view. Down the
    // slope the nose drops toward the frame's bottom edge -- the view's pitch is the player's, and
    // is not turned to chase it.
    const float uphill = std::atan2(-hull.normal[0], -hull.normal[2]);
    for (float yaw : {uphill, uphill + 1.5707963F, uphill - 1.5707963F}) {
        for (std::size_t i = 0; i < kPositions.size(); ++i) {
            const EyePlacement placement = eye_placement(kPositions.at(i));
            Camera camera = camera_on_hull(hull.pivot, hull.normal, placement, yaw, 0.0F, kSeed);
            const Vec3 tip = tip_on(hull, yaw);
            const game::Projected seen = game::project(camera, kExtent, tip[0], tip[1], tip[2]);
            EXPECT_GT(seen.depth, math::kNearPlane) << "position " << i << " yaw " << yaw;
            EXPECT_GE(seen.x, 0.0F) << "position " << i << " yaw " << yaw;
            EXPECT_LE(seen.x, static_cast<float>(kExtent.width)) << "position " << i;
            EXPECT_GE(seen.y, 0.0F) << "position " << i << " yaw " << yaw;
            EXPECT_LE(seen.y, static_cast<float>(kExtent.height)) << "position " << i;
            // Can fail: lift the eye by a metre and a half and the tip leaves the frame.
            if (i == 0) {
                camera.y += 1.5F;
                const game::Projected lost = game::project(camera, kExtent, tip[0], tip[1], tip[2]);
                EXPECT_GT(lost.y, static_cast<float>(kExtent.height)) << "yaw " << yaw;
            }
        }
    }
}

// ------------------------------- the reticle is drawn in the VIEW's frame, not the world's

/// The reticle's 30 vertices as the PLAYER sees them: SCREEN pixels, through the renderer's own
/// projection. Deliberately NOT through `basis_from` -- the reticle and that function sharing one
/// expression is exactly what let the mark stay plumb with the world while every other mark on the
/// layer leaned with it, so a test that measured the mark the same way could never have seen it.
[[nodiscard]] std::vector<game::Projected> reticle_pixels(const Camera& camera,
                                                          const game::Scene& scene) {
    std::vector<game::Projected> px;
    if (scene.vertices.size() < game::kReticleVertexCount) {
        return px;
    }
    for (std::size_t i = scene.vertices.size() - game::kReticleVertexCount;
         i < scene.vertices.size(); ++i) {
        const std::array<float, 3>& at = scene.vertices.at(i).position;
        px.push_back(game::project(camera, kExtent, at.at(0), at.at(1), at.at(2)));
    }
    return px;
}

/// How many distinct values in `values`, counting anything within `eps` of one already counted as
/// the same one. Two means a quad's sides are parallel to that screen axis; four means it is
/// turned.
[[nodiscard]] std::size_t distinct_in(const std::vector<float>& values, float eps) {
    std::vector<float> seen;
    for (const float value : values) {
        bool duplicate = false;
        for (const float already : seen) {
            if (std::abs(value - already) <= eps) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            seen.push_back(value);
        }
    }
    return seen.size();
}

TEST(ViewTilt, TheReticleIsDrawnInTheViewsFrameSoItsTicksAndDotStaySquareToTheScreen) {
    std::array<int, 2> tile{};
    ASSERT_TRUE(find_tile(is_gentle, tile)) << "no gentle slope on the map, so nothing is proved";
    const Hull hull = hull_on(tile);
    const Camera camera = camera_for(hull.sim, session_at(hull, 0), kSeed, std::nullopt);
    ASSERT_NE(camera.up, kWorldUp) << "the vacuous guard: the view must be leaning";

    game::SceneOptions options;
    options.seed = kSeed;
    options.radius_tiles = 8;
    options.extent = kExtent;
    options.camera = camera;
    // Or the world map's vertices are the tail of the list and this reads the wrong mark.
    options.show_world_map = false;
    options.aim = game::Aim{static_cast<float>(kExtent.width) * 0.5F,
                            static_cast<float>(kExtent.height) * 0.5F};
    const std::vector<game::Projected> px
        = reticle_pixels(camera, game::build_scene(hull.sim, options));
    ASSERT_EQ(px.size(), game::kReticleVertexCount) << "no reticle was drawn at all";

    // Each of the four ticks and then the dot must be a rectangle whose sides are parallel to the
    // screen's own axes: two distinct x and two distinct y across its six corners. Built on the
    // world's up under a leaning camera a corner is TURNED, and then there are four of each.
    constexpr float kHalfPixel = 0.5F;
    for (std::size_t quad = 0; quad + 6 <= px.size(); quad += 6) {
        std::vector<float> xs;
        std::vector<float> ys;
        for (std::size_t i = quad; i < quad + 6; ++i) {
            xs.push_back(px.at(i).x);
            ys.push_back(px.at(i).y);
        }
        EXPECT_EQ(distinct_in(xs, kHalfPixel), 2U)
            << "quad " << (quad / 6) << " is turned about the screen's x axis";
        EXPECT_EQ(distinct_in(ys, kHalfPixel), 2U)
            << "quad " << (quad / 6) << " is turned about the screen's y axis";
    }

    // Not vacuous: the mark has real extent, and the four ticks sit at four different places, so
    // "two distinct values" cannot be a collapsed figure satisfying the assertion by accident.
    const auto width_of = [&px](std::size_t quad) {
        float lo = px.at(quad).x;
        float hi = lo;
        for (std::size_t i = quad; (i < quad + 6) && (i < px.size()); ++i) {
            lo = std::fmin(lo, px.at(i).x);
            hi = std::fmax(hi, px.at(i).x);
        }
        return hi - lo;
    };
    EXPECT_GT(width_of(24), 2.0F) << "the dot must be a couple of pixels wide";
    // The ticks are FINE lines now (Harri, 2026-11-03: "the lines in the crosshairs should be
    // finer"), 1.4 px at this 720 frame, so the floor is a pixel, not the couple it was.
    EXPECT_GT(width_of(0), 1.0F) << "a tick must still be a line a pixel or more wide";
    for (std::size_t a = 0; a < 4; ++a) {
        for (std::size_t b = a + 1; b < 4; ++b) {
            const float dx = px.at(a * 6).x - px.at(b * 6).x;
            const float dy = px.at(a * 6).y - px.at(b * 6).y;
            EXPECT_GT(std::hypot(dx, dy), 4.0F) << "ticks " << a << " and " << b << " are apart";
        }
    }
}

} // namespace
} // namespace protekt::app
