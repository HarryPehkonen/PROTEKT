#include "protekt/sim/fixed.hpp"

namespace protekt::sim {

namespace {

// Clamp a 64-bit intermediate into the 32-bit raw range.
Fixed saturate(std::int64_t value) noexcept {
    if (value > Fixed::kMaxRaw) {
        return Fixed::from_raw(Fixed::kMaxRaw);
    }
    if (value < Fixed::kMinRaw) {
        return Fixed::from_raw(Fixed::kMinRaw);
    }
    return Fixed::from_raw(static_cast<std::int32_t>(value));
}

} // namespace

// All arithmetic goes through int64, which cannot overflow for two int32 operands (mul and
// div included), then saturates. Multiplication floors (arithmetic shift); division truncates
// towards zero.
Fixed fixed_add(Fixed a, Fixed b) noexcept {
    return saturate(static_cast<std::int64_t>(a.raw) + static_cast<std::int64_t>(b.raw));
}

Fixed fixed_sub(Fixed a, Fixed b) noexcept {
    return saturate(static_cast<std::int64_t>(a.raw) - static_cast<std::int64_t>(b.raw));
}

Fixed fixed_mul(Fixed a, Fixed b) noexcept {
    const std::int64_t product
        = static_cast<std::int64_t>(a.raw) * static_cast<std::int64_t>(b.raw);
    return saturate(product >> Fixed::kShift);
}

Fixed fixed_div(Fixed a, Fixed b) noexcept {
    if (b.raw == 0) {
        return Fixed::max();
    }
    const std::int64_t scaled = static_cast<std::int64_t>(a.raw) * Fixed::kOne;
    return saturate(scaled / static_cast<std::int64_t>(b.raw));
}

Fixed fixed_abs(Fixed a) noexcept {
    return a.raw < 0 ? saturate(-static_cast<std::int64_t>(a.raw)) : a;
}

} // namespace protekt::sim
