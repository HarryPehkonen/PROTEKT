#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "protekt/sim/fixed.hpp"
#include "protekt/sim/projectile.hpp"

namespace protekt::sim {

/// A direction or an offset in the map plane, in Q16.16. Not a position: nothing here is a tile
/// coordinate, and nothing here is unit length by construction.
struct Fixed2D {
    Fixed x{};
    Fixed y{};
};

/// The most pellets one shot may carry. A bound, not a preference: the fan is a fixed-size array,
/// so a shot's pattern is a value with no allocation in it and a replay has nothing to re-derive.
inline constexpr int kMaxPellets = 12;

/// The directions one shot's pellets fly along, in firing order. The first `count` entries of
/// `dir` are meaningful; the rest are zero and are never read.
struct PelletFan {
    std::array<Fixed2D, kMaxPellets> dir{};
    int count{};
};

/// The pellet pattern of a shot along `aim`, in one sentence: `count` directions fanned evenly
/// across a cone about it, pellet i leaning sideways by `half_tan * (2i - (count - 1)) / (count -
/// 1)` tiles per tile flown, so the first and last sit exactly on the cone's edge and a lone pellet
/// flies straight.
///
/// WHY the cone is the argument and not a step: the half-angle is the spec's data (as a tangent,
/// since the sim has no trigonometry), and every lean is a fraction of it no greater than one in
/// magnitude. A pellet outside the cone is impossible by construction, whatever `count` is.
///
/// WHY a fixed fan and not a random draw: the sim has no RNG and a replay must reproduce byte for
/// byte, so the pattern is a pure function of the aim direction and the two numbers above. The
/// same shot from the same state produces the same pellets on every machine, and there is nothing
/// to seed. WHY no trigonometry: the sim forbids floating point, and the fan needs none -- the
/// spread axis is the aim turned a quarter turn, and the length is the sim's own integer square
/// root.
///
/// `aim` is a direction, not a position, and need not be unit length: every returned direction is
/// normalised to about Fixed::kOne, so a caller may pass an unnormalised aim and still get a fan
/// of the same shape. A zero `aim` has no direction to fan about and yields `count` zero
/// directions -- a defined answer, like every other operation in the sim.
///
/// `count` is clamped to [1, kMaxPellets]: a shot with no pellets is not a shot, and the array is
/// the bound. With count == 1 the offset is exactly zero, so the single pellet IS the normalised
/// aim -- the same code path, not a special case.
[[nodiscard]] inline PelletFan pellet_fan(Fixed2D aim, int count, Fixed half_tan) noexcept {
    PelletFan fan{};
    if (count < 1) {
        count = 1;
    }
    if (count > kMaxPellets) {
        count = kMaxPellets;
    }
    fan.count = count;

    // The axis the fan spreads along: the aim turned a quarter turn, (-y, x). This is the whole
    // reason the pattern needs no sine and no cosine. `fixed_sub` rather than a unary minus,
    // because Fixed has none and because negation must saturate like every other operation.
    const Fixed2D perp{fixed_sub(Fixed::zero(), aim.y), aim.x};

    for (int i = 0; i < count; ++i) {
        // The lean: half_tan times a fraction in [-1, 1]. Integer division truncates toward zero,
        // so |lean| <= |half_tan| for every i: this line is what keeps the pellet in the cone. A
        // negative half_tan is a mirrored cone, not a wider one. (count == 1 leans by nothing.)
        const std::int64_t across = 2 * i - (count - 1);
        const std::int64_t lean64
            = count > 1 ? static_cast<std::int64_t>(half_tan.raw) * across / (count - 1) : 0;
        const Fixed lateral = Fixed::from_raw(static_cast<std::int32_t>(lean64));
        const Fixed2D raw{fixed_add(aim.x, fixed_mul(perp.x, lateral)),
                          fixed_add(aim.y, fixed_mul(perp.y, lateral))};

        // Both components are int32, so the sum of their squares is below 2^63 and cannot wrap:
        // the widest pair is (2^31 - 1, 2^31 - 1), whose squares sum to 2^63 - 2^33 + 2.
        const std::int64_t dx = raw.x.raw;
        const std::int64_t dy = raw.y.raw;
        const std::int64_t len = isqrt(dx * dx + dy * dy);
        if (len == 0) {
            fan.dir.at(static_cast<std::size_t>(i)) = Fixed2D{};
            continue;
        }
        // Scale each component to a magnitude of kOne. Integer division truncates towards zero, so
        // (-a) / b == -(a / b): a fan symmetric about the aim stays EXACTLY symmetric here. The
        // quotient cannot leave int32: |dx| <= len, so |dx * kOne / len| <= kOne.
        fan.dir.at(static_cast<std::size_t>(i))
            = Fixed2D{Fixed::from_raw(static_cast<std::int32_t>(dx * Fixed::kOne / len)),
                      Fixed::from_raw(static_cast<std::int32_t>(dy * Fixed::kOne / len))};
    }
    return fan;
}

} // namespace protekt::sim
