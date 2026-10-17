#include "protekt/rng.hpp"

namespace protekt::rng {

std::uint64_t SplitMix64::next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

std::uint32_t SplitMix64::bounded(std::uint32_t bound) noexcept {
    if (bound == 0) {
        return 0;
    }
    return static_cast<std::uint32_t>(next() >> 32) % bound;
}

std::uint64_t stream_seed(std::uint64_t seed, std::uint64_t stream_id) noexcept {
    return SplitMix64(seed ^ (0x9E3779B97F4A7C15ULL * stream_id)).next();
}

} // namespace protekt::rng
