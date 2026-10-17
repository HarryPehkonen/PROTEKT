#pragma once

#include <cstdint>

namespace protekt::rng {

/// The only randomness in PROTEKT: SplitMix64 (Steele/Lea/Flood), fully specified below.
/// No std:: distribution is used anywhere, because std::uniform_int_distribution's mapping
/// is implementation-defined -- a world built with it is not portable, and "same seed, same
/// world" is this project's primary requirement.
class SplitMix64 {
public:
    explicit constexpr SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

    /// state_ += 0x9E3779B97F4A7C15
    /// z = state_
    /// z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9
    /// z = (z ^ (z >> 27)) * 0x94D049BB133111EB
    /// return z ^ (z >> 31)
    [[nodiscard]] std::uint64_t next() noexcept;

    /// A value in [0, bound). bound must be > 0.
    /// Defined as: static_cast<std::uint32_t>(next() >> 32) % bound
    [[nodiscard]] std::uint32_t bounded(std::uint32_t bound) noexcept;

private:
    std::uint64_t state_;
};

/// One seeded stream per subsystem (world, sim, ai), so that consuming values in one cannot
/// shift the values another sees.
/// Defined as: SplitMix64(seed ^ (0x9E3779B97F4A7C15 * stream_id)).next()
[[nodiscard]] std::uint64_t stream_seed(std::uint64_t seed, std::uint64_t stream_id) noexcept;

} // namespace protekt::rng
