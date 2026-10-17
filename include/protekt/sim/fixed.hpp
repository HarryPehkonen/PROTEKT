#pragma once

#include <cstdint>
#include <limits>

namespace protekt::sim {

/// Q16.16 fixed point. The simulation may not use floating point at all: a tick's outcome must
/// not depend on the compiler, the optimisation level or -ffast-math. Every operation here is
/// total -- defined for every input, including overflow and division by zero -- because a
/// defined answer is what makes a replay fuzzable.
struct Fixed {
    static constexpr int kShift = 16;
    static constexpr std::int32_t kOne = 1 << kShift;
    static constexpr std::int32_t kMaxRaw = std::numeric_limits<std::int32_t>::max();
    static constexpr std::int32_t kMinRaw = std::numeric_limits<std::int32_t>::min();

    std::int32_t raw{};

    [[nodiscard]] static constexpr Fixed from_int(int value) noexcept {
        return Fixed{static_cast<std::int32_t>(value) << kShift};
    }
    [[nodiscard]] static constexpr Fixed from_raw(std::int32_t value) noexcept {
        return Fixed{value};
    }
    [[nodiscard]] static constexpr Fixed max() noexcept { return Fixed{kMaxRaw}; }
    [[nodiscard]] static constexpr Fixed zero() noexcept { return Fixed{0}; }

    /// The largest integer <= this value. Floors towards negative infinity: floor_int(-1 raw)
    /// is -1, not 0. Truncation here would put the world's tile grid half a tile off for
    /// negative coordinates.
    [[nodiscard]] constexpr std::int32_t floor_int() const noexcept { return raw >> kShift; }
};

[[nodiscard]] constexpr bool operator==(const Fixed& a, const Fixed& b) noexcept {
    return a.raw == b.raw;
}
[[nodiscard]] constexpr bool operator!=(const Fixed& a, const Fixed& b) noexcept {
    return a.raw != b.raw;
}
[[nodiscard]] constexpr bool operator<(const Fixed& a, const Fixed& b) noexcept {
    return a.raw < b.raw;
}
[[nodiscard]] constexpr bool operator<=(const Fixed& a, const Fixed& b) noexcept {
    return a.raw <= b.raw;
}
[[nodiscard]] constexpr bool operator>(const Fixed& a, const Fixed& b) noexcept {
    return a.raw > b.raw;
}
[[nodiscard]] constexpr bool operator>=(const Fixed& a, const Fixed& b) noexcept {
    return a.raw >= b.raw;
}

/// Saturating addition (no UB on overflow).
[[nodiscard]] Fixed fixed_add(Fixed a, Fixed b) noexcept;
/// Saturating subtraction.
[[nodiscard]] Fixed fixed_sub(Fixed a, Fixed b) noexcept;
/// (a * b) >> 16 through a 64-bit intermediate, saturating on overflow.
[[nodiscard]] Fixed fixed_mul(Fixed a, Fixed b) noexcept;
/// a / b, saturating on overflow. b.raw == 0 returns Fixed::max() -- a defined answer, not a trap.
[[nodiscard]] Fixed fixed_div(Fixed a, Fixed b) noexcept;
/// |a|, saturating at max() rather than overflowing on kMinRaw.
[[nodiscard]] Fixed fixed_abs(Fixed a) noexcept;

} // namespace protekt::sim
