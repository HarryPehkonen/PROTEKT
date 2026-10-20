#include "protekt/game/png.hpp"

// stb is vendored and warning-noisy by design; it is included as a SYSTEM header (see
// CMakeLists.txt) and compiled in this one translation unit.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <span>

namespace protekt::game {

namespace {

void collect(void* sink, void* data, int size) {
    auto* bytes = static_cast<std::vector<std::uint8_t>*>(sink);
    const std::span<const std::uint8_t> chunk{static_cast<const std::uint8_t*>(data),
                                              static_cast<std::size_t>(size)};
    bytes->insert(bytes->end(), chunk.begin(), chunk.end());
}

} // namespace

bool write_png_rgba(const std::filesystem::path& path, std::span<const std::uint8_t> rgba,
                    std::uint32_t width, std::uint32_t height) {
    constexpr auto kMax = static_cast<std::uint32_t>(std::numeric_limits<int>::max() / 4);
    if (width == 0 || height == 0 || width > kMax || height > kMax
        || rgba.size() != std::size_t{width} * height * 4U) {
        return false;
    }
    std::vector<std::uint8_t> file;
    const int ok
        = stbi_write_png_to_func(collect, &file, static_cast<int>(width), static_cast<int>(height),
                                 4, rgba.data(), static_cast<int>(width * 4U));
    if (ok == 0) {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    std::copy(file.begin(), file.end(), std::ostreambuf_iterator<char>(out));
    out.flush();
    return out.good();
}

bool read_png_rgba(const std::filesystem::path& path, std::vector<std::uint8_t>& rgba,
                   std::uint32_t& width, std::uint32_t& height) {
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) {
        return false;
    }
    const std::vector<stbi_uc> raw{std::istreambuf_iterator<char>(in),
                                   std::istreambuf_iterator<char>()};
    int w = 0;
    int h = 0;
    int channels = 0;
    stbi_uc* decoded
        = stbi_load_from_memory(raw.data(), static_cast<int>(raw.size()), &w, &h, &channels, 4);
    if (decoded == nullptr) {
        return false;
    }
    const std::size_t bytes = std::size_t(w) * static_cast<std::size_t>(h) * 4U;
    const std::span<const stbi_uc> pixels{decoded, bytes};
    rgba.assign(pixels.begin(), pixels.end());
    stbi_image_free(decoded);
    width = static_cast<std::uint32_t>(w);
    height = static_cast<std::uint32_t>(h);
    return true;
}

} // namespace protekt::game
