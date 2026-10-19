#include "protekt/math/math.hpp"

#include <cmath>
#include <cstddef>

namespace protekt::math {

Mat4 multiply(const Mat4& a, const Mat4& b) noexcept {
    Mat4 r{};
    for (std::size_t col = 0; col < 4; ++col) {
        for (std::size_t row = 0; row < 4; ++row) {
            float sum = 0.0F;
            for (std::size_t k = 0; k < 4; ++k) {
                sum += a.at(k * 4 + row) * b.at(col * 4 + k);
            }
            r.at(col * 4 + row) = sum;
        }
    }
    return r;
}

Vec3 cross(const Vec3& a, const Vec3& b) noexcept {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

float dot(const Vec3& a, const Vec3& b) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

Vec3 add(const Vec3& a, const Vec3& b) noexcept { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }

Vec3 normalise(const Vec3& v) noexcept {
    const float len = std::sqrt(dot(v, v));
    return {v[0] / len, v[1] / len, v[2] / len};
}

Basis basis_from(float yaw, float pitch) noexcept {
    const float cp = std::cos(pitch);
    const Vec3 forward{std::sin(yaw) * cp, std::sin(pitch), std::cos(yaw) * cp};
    // (0,1,0) x forward = (forward.z, 0, -forward.x), and it is already horizontal.
    const float right_length = std::sqrt((forward[2] * forward[2]) + (forward[0] * forward[0]));
    const Vec3 right{forward[2] / right_length, 0.0F, -forward[0] / right_length};
    const Vec3 up{cross(forward, right)};
    return Basis{forward, right, up};
}

Mat4 look_at(const Vec3& eye, const Vec3& target, const Vec3& up) noexcept {
    // The axis order is the whole of it: right is `up x forward`, and the camera's up is then
    // `forward x right` -- the pair together keep +X on the right for a camera looking down +Z
    // and +Y up. Flipping only one of them gives an upside-down or a left-right reversed frame
    // (the projection already flips Y for Vulkan's clip space).
    const Vec3 f = normalise({target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]});
    const Vec3 s = normalise(cross(up, f));
    const Vec3 u = cross(f, s);
    Mat4 m{};
    m[0] = s[0];
    m[4] = s[1];
    m[8] = s[2];
    m[1] = u[0];
    m[5] = u[1];
    m[9] = u[2];
    m[2] = -f[0];
    m[6] = -f[1];
    m[10] = -f[2];
    m[12] = -dot(s, eye);
    m[13] = -dot(u, eye);
    m[14] = dot(f, eye);
    m[15] = 1.0F;
    return m;
}

Mat4 perspective(float fovy_radians, float aspect, float near_plane, float far_plane) noexcept {
    const float f = 1.0F / std::tan(fovy_radians * 0.5F);
    Mat4 m{};
    m[0] = f / aspect;
    m[5] = -f;
    m[10] = far_plane / (near_plane - far_plane);
    m[11] = -1.0F;
    m[14] = far_plane * near_plane / (near_plane - far_plane);
    return m;
}

Mat4 view_projection(const Vec3& eye, float yaw, float pitch, float aspect, float fov_radians,
                     float near_plane, float far_plane) noexcept {
    const Basis b = basis_from(yaw, pitch);
    return multiply(perspective(fov_radians, aspect, near_plane, far_plane),
                    look_at(eye, add(eye, b.forward), {0.0F, 1.0F, 0.0F}));
}

std::array<float, 4> transform(const Mat4& m, const Vec3& point) noexcept {
    std::array<float, 4> out{};
    for (std::size_t row = 0; row < 4; ++row) {
        out.at(row) = (m.at(row) * point[0]) + (m.at(4 + row) * point[1])
                      + (m.at(8 + row) * point[2]) + m.at(12 + row);
    }
    return out;
}

} // namespace protekt::math
