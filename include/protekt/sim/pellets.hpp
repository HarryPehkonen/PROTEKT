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

/// The pellet pattern of a shot along `aim`: `count` directions fanned symmetrically about it,
/// pellet i pushed sideways by `lateral_step * (i - (count - 1) / 2)`.
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
[[nodiscard]] inline PelletFan pellet_fan(Fixed2D aim, int count, Fixed lateral_step) noexcept {
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
        // (i - (count - 1) / 2) in Q16.16, exactly: the numerator is doubled so that the half step
        // of an even count is representable rather than truncated away.
        const int half_steps = 2 * i - (count - 1);
        const Fixed steps
            = Fixed::from_raw(static_cast<std::int32_t>(half_steps * (Fixed::kOne / 2)));
        const Fixed lateral = fixed_mul(lateral_step, steps);
        const Fixed2D raw{fixed_add(aim.x, fixed_mul(perp.x, lateral)),
                          fixed_add(aim.y, fixed_mul(perp.y, lateral))};

        // Both components are int32, so the sum of their squares is below 2^63 and cannot wrap:
        // the widest pair is (2^31 - 1, 2^31 - 1), whose squares sum to 2^63 - 2^33 + 2.
        const std::int64_t dx = raw.x.raw;
        const std::int64_t dy = raw.y.raw;
        const std::int64_t len = isqrt(dx * dx + dy * dy);
        if (len == 0) {
            fan.dir[static_cast<std::size_t>(i)] = Fixed2D{};
            continue;
        }
        // Scale each component to a magnitude of kOne. Integer division truncates towards zero, so
        // (-a) / b == -(a / b): a fan symmetric about the aim stays EXACTLY symmetric here. The
        // quotient cannot leave int32: |dx| <= len, so |dx * kOne / len| <= kOne.
        fan.dir[static_cast<std::size_t>(i)]
            = Fixed2D{Fixed::from_raw(static_cast<std::int32_t>(dx * Fixed::kOne / len)),
                      Fixed::from_raw(static_cast<std::int32_t>(dy * Fixed::kOne / len))};
    }
    return fan;
}

} // namespace protekt::sim
