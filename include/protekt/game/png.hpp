#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

/// PNG files, through the vendored stb single headers (third_party/stb). Nothing else in the
/// project includes stb: this is the whole boundary.
namespace protekt::game {

/// Writes `width` x `height` tightly packed RGBA8 to `path`. The bytes are a pure function of the
/// pixels (no timestamp, no metadata), so the same pixels always give the same file. False when
/// the size does not match the span or the file cannot be written.
[[nodiscard]] bool write_png_rgba(const std::filesystem::path& path,
                                  std::span<const std::uint8_t> rgba, std::uint32_t width,
                                  std::uint32_t height);

/// Reads any PNG stb understands as RGBA8. False, with the outputs untouched, when the file is
/// missing or is not an image.
[[nodiscard]] bool read_png_rgba(const std::filesystem::path& path, std::vector<std::uint8_t>& rgba,
                                 std::uint32_t& width, std::uint32_t& height);

} // namespace protekt::game
