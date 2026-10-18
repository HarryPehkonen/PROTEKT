#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

/// Pixel byte-order helpers for presentation. Pure: no Vulkan types, so they are tested without a
/// device. The renderer reads back R,G,B,A; a B8G8R8A8 swapchain wants B,G,R,A.
namespace protekt::render {

/// Swap the red and blue byte of every whole pixel, in place. Alpha is untouched. Only complete
/// four-byte pixels are touched: a trailing remainder is left exactly as it was.
inline void swap_red_blue(std::span<std::uint8_t> pixels) {
    const std::size_t whole = pixels.size() - pixels.size() % 4U;
    for (std::size_t i = 0; i < whole; i += 4U) {
        std::swap(pixels[i], pixels[i + 2U]);
    }
}

/// Copy `src` into `dst` (at most dst.size() bytes, at most src.size()), swapping red and blue
/// when the destination is BGRA. Returns the number of bytes written.
inline std::size_t copy_pixels(std::span<const std::uint8_t> src, std::span<std::uint8_t> dst,
                               bool bgra) {
    const std::size_t count = std::min(src.size(), dst.size());
    std::copy_n(src.begin(), count, dst.begin());
    if (bgra) {
        swap_red_blue(dst.first(count));
    }
    return count;
}

} // namespace protekt::render
