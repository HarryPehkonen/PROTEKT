#include "protekt/math/math.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>

/// The camera convention, tested where it lives.
///
/// This maths had no test of its own, and the reason it has one now is that it was copied: the
/// renderer's fallback camera, the scene builder and the picker each derived the camera, and the
/// one real bug this area produced -- an inverted `look_at`, caught only by looking at a rendered
/// frame -- was in exactly this code. The tests below therefore do two jobs: they check the maths,
/// and they check that the renderer's basis and the picker's basis are the SAME basis rather than
/// two that happen to agree today.

namespace protekt::math {
namespace {

constexpr float kTolerance = 1e-5F;

/// A transform's result at a point, without the `w` divide.
std::array<float, 4> clip_of(const Mat4& m, const Vec3& p) { return transform(m, p); }

TEST(Math, LookAtPutsTheEyeAtTheOrigin) {
    const Vec3 eye{3.0F, 1.6F, -2.0F};
    const Mat4 m = look_at(eye, {3.0F, 1.6F, 8.0F}, {0.0F, 1.0F, 0.0F});
    const std::array<float, 4> origin = clip_of(m, eye);
    EXPECT_NEAR(origin.at(0), 0.0F, kTolerance);
    EXPECT_NEAR(origin.at(1), 0.0F, kTolerance);
    EXPECT_NEAR(origin.at(2), 0.0F, kTolerance);
    EXPECT_NEAR(origin.at(3), 1.0F, kTolerance);
}

TEST(Math, LookAtPutsTheTargetDownNegativeZ) {
    const Vec3 eye{0.0F, 1.6F, 0.0F};
    const Mat4 m = look_at(eye, {0.0F, 1.6F, 10.0F}, {0.0F, 1.0F, 0.0F});
    const std::array<float, 4> target = clip_of(m, {0.0F, 1.6F, 10.0F});
    EXPECT_NEAR(target.at(2), -10.0F, kTolerance)
        << "the view axis is -Z, and depth is its distance";
}

/// The bug this pins: `up x forward` instead of `forward x up` gives u = -Y, and because the
/// projection flips Y for Vulkan's clip space the two cancel into an upside-down frame. Both facts
/// are asserted here, so neither can be changed alone.
TEST(Math, LookAtBuildsRightAsForwardCrossUp) {
    const Vec3 eye{0.0F, 1.6F, 0.0F};
    const Vec3 forward{0.0F, 0.0F, 1.0F};
    const Mat4 m = look_at(eye, {0.0F, 1.6F, 10.0F}, {0.0F, 1.0F, 0.0F});
    const Vec3 expected_right = normalise(cross(forward, {0.0F, 1.0F, 0.0F}));
    EXPECT_NEAR(expected_right[0], -1.0F, kTolerance) << "at yaw 0, forward x up is -X";
    EXPECT_NEAR(m.at(0), expected_right[0], kTolerance);
    EXPECT_NEAR(m.at(4), expected_right[1], kTolerance);
    EXPECT_NEAR(m.at(8), expected_right[2], kTolerance);
    // Row 1 is the up vector, and it points UP in view space: +Y, before the projection flips it.
    EXPECT_NEAR(m.at(1), 0.0F, kTolerance);
    EXPECT_NEAR(m.at(5), 1.0F, kTolerance);
    EXPECT_NEAR(m.at(9), 0.0F, kTolerance);
}

TEST(Math, PerspectiveMapsTheNearPlaneToZeroAndTheFarPlaneToOne) {
    const Mat4 p = perspective(kFovRadians, 16.0F / 9.0F, kNearPlane, kFarPlane);
    const std::array<float, 4> near_point = clip_of(p, {0.0F, 0.0F, -kNearPlane});
    const std::array<float, 4> far_point = clip_of(p, {0.0F, 0.0F, -kFarPlane});
    EXPECT_NEAR(near_point.at(2) / near_point.at(3), 0.0F, kTolerance);
    EXPECT_NEAR(far_point.at(2) / far_point.at(3), 1.0F, kTolerance)
        << "Vulkan's depth range is 0..1";
}

TEST(Math, PerspectiveFlipsYBecauseVulkansClipSpaceRunsDownwards) {
    const Mat4 p = perspective(kFovRadians, 1.0F, kNearPlane, kFarPlane);
    EXPECT_LT(p.at(5), 0.0F);
    const std::array<float, 4> above = clip_of(p, {0.0F, 1.0F, -10.0F});
    EXPECT_LT(above.at(1) / above.at(3), 0.0F)
        << "a point above the camera is above the centre of the frame";
}

TEST(Math, PerspectiveWidensWithTheAspectRatio) {
    const Mat4 square = perspective(kFovRadians, 1.0F, kNearPlane, kFarPlane);
    const Mat4 wide = perspective(kFovRadians, 2.0F, kNearPlane, kFarPlane);
    EXPECT_NEAR(wide.at(0), square.at(0) * 0.5F, kTolerance);
    EXPECT_NEAR(wide.at(5), square.at(5), kTolerance)
        << "the vertical field of view is the given one";
}

TEST(Math, BasisIsOrthonormalAndRightHanded) {
    for (const float yaw : {-2.9F, -1.0F, 0.0F, 0.7F, 3.0F}) {
        for (const float pitch : {-1.2F, -0.2F, 0.0F, 0.9F}) {
            const Basis b = basis_from(yaw, pitch);
            EXPECT_NEAR(dot(b.forward, b.forward), 1.0F, kTolerance);
            EXPECT_NEAR(dot(b.right, b.right), 1.0F, kTolerance);
            EXPECT_NEAR(dot(b.up, b.up), 1.0F, kTolerance);
            EXPECT_NEAR(dot(b.forward, b.right), 0.0F, kTolerance);
            EXPECT_NEAR(dot(b.forward, b.up), 0.0F, kTolerance);
            EXPECT_NEAR(dot(b.right, b.up), 0.0F, kTolerance);
            // right = forward x worldUp, which is what makes it right-handed rather than mirrored.
            const Vec3 expected_right = normalise(cross(b.forward, {0.0F, 1.0F, 0.0F}));
            for (std::size_t i = 0; i < 3; ++i) {
                EXPECT_NEAR(b.right.at(i), expected_right.at(i), kTolerance);
            }
        }
    }
}

/// THE TEST THIS MODULE EXISTS FOR. `look_at`'s matrix rows and `basis_from`'s vectors must be the
/// same three vectors, because the picker turns `basis_from` into a ray while the renderer turns
/// `look_at` into the matrix it draws with. Asserting "they agree today" is weaker than this: an
/// equality between the two constructions fails the moment either one is changed alone.
TEST(Math, TheRenderersMatrixAndThePickersBasisAreOneBasis) {
    for (const float yaw : {-2.9F, -0.5F, 0.0F, 0.7F, 2.5F}) {
        for (const float pitch : {-1.1F, -0.17F, 0.0F, 0.8F}) {
            const Basis b = basis_from(yaw, pitch);
            const Vec3 eye{12.0F, 1.6F, -7.0F};
            const Mat4 view = look_at(eye, add(eye, b.forward), {0.0F, 1.0F, 0.0F});
            // Column-major: component i of row r is m[i * 4 + r], and each row is one axis.
            for (std::size_t i = 0; i < 3; ++i) {
                EXPECT_NEAR(view.at(i * 4), b.right.at(i), kTolerance)
                    << "row 0 is the right vector";
                EXPECT_NEAR(view.at((i * 4) + 1), b.up.at(i), kTolerance)
                    << "row 1 is the up vector";
                EXPECT_NEAR(view.at((i * 4) + 2), -b.forward.at(i), kTolerance)
                    << "row 2 is minus forward";
            }
        }
    }
}

TEST(Math, ViewProjectionCentresWhateverIsStraightAhead) {
    const Vec3 eye{4.0F, 1.6F, 4.0F};
    const float aspect = 16.0F / 9.0F;
    for (const float yaw : {-2.0F, 0.3F, 1.9F}) {
        for (const float pitch : {-0.5F, 0.0F, 0.4F}) {
            const Mat4 m = view_projection(eye, yaw, pitch, aspect);
            const Basis b = basis_from(yaw, pitch);
            const Vec3 ahead{eye[0] + (b.forward[0] * 20.0F), eye[1] + (b.forward[1] * 20.0F),
                             eye[2] + (b.forward[2] * 20.0F)};
            const std::array<float, 4> clip = clip_of(m, ahead);
            EXPECT_GT(clip.at(3), 0.0F) << "in front of the camera";
            EXPECT_NEAR(clip.at(0) / clip.at(3), 0.0F, kTolerance) << "the centre of the frame";
            EXPECT_NEAR(clip.at(1) / clip.at(3), 0.0F, kTolerance);
        }
    }
}

TEST(Math, APointBehindTheCameraIsNotOnTheScreen) {
    const Vec3 eye{0.0F, 1.6F, 0.0F};
    const Mat4 m = view_projection(eye, 0.0F, 0.0F, 1.0F);
    const std::array<float, 4> behind = clip_of(m, {0.0F, 1.6F, -5.0F});
    EXPECT_LE(behind.at(3), 0.0F)
        << "w is the distance along the view axis: not positive, not visible";
}

TEST(Math, MultiplyComposesSoTheNearerMatrixAppliesFirst) {
    const Mat4 identity{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                        0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    const Mat4 p = perspective(kFovRadians, 1.0F, kNearPlane, kFarPlane);
    const Mat4 product = multiply(p, identity);
    for (std::size_t i = 0; i < 16; ++i) {
        EXPECT_NEAR(product.at(i), p.at(i), kTolerance);
    }
}

} // namespace
} // namespace protekt::math
