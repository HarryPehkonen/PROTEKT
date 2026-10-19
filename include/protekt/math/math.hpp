/// Presentation maths: the camera convention, in ONE place.
///
/// This is the project's only floating-point code, and it is deliberate that it
/// sits apart. It feeds the rasteriser and the picker, and nothing here may
/// reach the tick loop: the simulation is Q16.16 and integer so that a replay
/// is bit-identical on every machine, and a float that reached it would make
/// the world depend on the compiler. `CODING_STANDARDS.md` states the rule;
/// this header is the only place allowed to break it, and it breaks it for the
/// camera only.
///
/// The reason the module exists is that the camera was derived in three places
/// at once -- the renderer's fallback view, the scene builder, and the picker's
/// ray -- and they agreed by comment rather than by construction. `basis_from`
/// is what removes the possibility: `look_at` builds its matrix from exactly
/// that basis, and the picker turns exactly that basis into a ray, so a click
/// cannot land somewhere other than where it looks.

#pragma once

#include <array>

namespace protekt::math {

/// Column-major, `m[column * 4 + row]`: what GLSL's `mat4` expects in a push
/// constant.
using Mat4 = std::array<float, 16>;
using Vec3 = std::array<float, 3>;

inline constexpr float kPi = 3.14159265358979F;

/// The field of view: 60 degrees as `1.0472` radians, which is 59.99997 degrees.
/// It is NOT the same float as `60.0F * kPi / 180.0F`: they differ by about 1e-7,
/// so they rasterise slightly different frames. This is the value the game draws
/// with, written once. Changing the spelling changes every recorded pixel hash in
/// the project, so it is a decision, not a tidy-up.
inline constexpr float kFovRadians = 1.0472F;

/// The projection's depth range, shared by every camera in the project.
inline constexpr float kNearPlane = 0.1F;
inline constexpr float kFarPlane = 2000.0F;

[[nodiscard]] Mat4 look_at(const Vec3& eye, const Vec3& target, const Vec3& up) noexcept;

/// Vulkan clip space: Z in [0,1] and Y pointing DOWN.
[[nodiscard]] Mat4 perspective(float fovy_radians, float aspect, float near_plane,
                               float far_plane) noexcept;

[[nodiscard]] Mat4 multiply(const Mat4& a, const Mat4& b) noexcept;
[[nodiscard]] Vec3 cross(const Vec3& a, const Vec3& b) noexcept;
[[nodiscard]] float dot(const Vec3& a, const Vec3& b) noexcept;
[[nodiscard]] Vec3 add(const Vec3& a, const Vec3& b) noexcept;

/// Unit length. `v` must not be the zero vector: the result is undefined there,
/// and the camera is the only caller, where a zero vector means the eye and its
/// target are the same point.
[[nodiscard]] Vec3 normalise(const Vec3& v) noexcept;

/// The camera's three axes, right-handed.
///
/// `forward` is where a camera with this yaw and pitch looks; `right` is
/// `worldUp x forward`; `up` is `forward x right`. Both the view matrix and the
/// picker's ray are built from this, which is the point: there is one answer to
/// "which way is the camera facing", and no second one to drift away from it.
struct Basis {
    Vec3 forward;
    Vec3 right;
    Vec3 up;
};

[[nodiscard]] Basis basis_from(float yaw, float pitch) noexcept;

/// The view matrix for a camera at `eye` with these angles, followed by the
/// projection, which is what the vertex shader multiplies. Pitching down and
/// using `distance` metres of forward is how a target point is chosen, which is
/// why this takes angles rather than a target: a camera that is looking at
/// itself has no forward vector at all.
[[nodiscard]] Mat4 view_projection(const Vec3& eye, float yaw, float pitch, float aspect,
                                   float fov_radians = kFovRadians, float near_plane = kNearPlane,
                                   float far_plane = kFarPlane) noexcept;

/// A point in the world, transformed by `m`, as Vulkan's clip space sees it:
/// `w` is returned separately because a point behind the camera has `w <= 0`
/// and is not on the screen at all.
[[nodiscard]] std::array<float, 4> transform(const Mat4& m, const Vec3& point) noexcept;

} // namespace protekt::math
