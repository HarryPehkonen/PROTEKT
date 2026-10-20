#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

/// The texture atlas, as the GAME sees it: a fixed 4x4 grid of 64x64 tiles in a 256x256 image, a
/// manifest that names the tiles, and the UV rectangle each tile occupies. The renderer knows none
/// of this -- it is handed pixels and vertices.
///
/// The atlas is still made in memory (solid tiles); a real image file is a later slice.
namespace protekt::game {

inline constexpr std::uint32_t kTilesPerRow = 4;
inline constexpr std::uint32_t kTileSide = 64;
inline constexpr std::uint32_t kAtlasSide = kTilesPerRow * kTileSide;
inline constexpr int kTileCount = static_cast<int>(kTilesPerRow * kTilesPerRow);

/// The tile an unlisted part name gets. Tile 0 is solid white, so it is a multiplicative identity:
/// geometry that sets no UV (0, 0 is this tile's corner) draws in its own vertex colour.
inline constexpr int kFallbackTile = 0;

/// A tile's rectangle in UV space; (u0, v0) is its top-left corner, (u1, v1) the bottom-right.
struct UvRect {
    float u0{};
    float v0{};
    float u1{};
    float v1{};
};

/// The exact rectangle of tile `index` (row-major). An index outside [0, kTileCount) gets the
/// fallback tile's, so a bad index can never address outside the atlas.
[[nodiscard]] UvRect tile_rect(int index) noexcept;

/// Part name -> tile index. Parsing NEVER throws or fails: a line that is blank, a comment, or not
/// "name index" with an index in range is skipped, and the rest are kept. A repeated name keeps its
/// last line.
class AtlasManifest {
public:
    [[nodiscard]] static AtlasManifest parse(std::string_view text);

    /// The tile of `name`, or `kFallbackTile` when it is not listed.
    [[nodiscard]] int tile(std::string_view name) const;
    [[nodiscard]] UvRect rect(std::string_view name) const { return tile_rect(tile(name)); }
    [[nodiscard]] std::size_t size() const noexcept { return tiles_.size(); }

private:
    std::map<std::string, int, std::less<>> tiles_;
};

/// The committed manifest (assets/atlas_manifest.txt, embedded at configure time), parsed once.
[[nodiscard]] const AtlasManifest& atlas_manifest();

/// The in-memory atlas: kAtlasSide x kAtlasSide RGBA8, tightly packed, each tile one solid colour.
[[nodiscard]] std::vector<std::uint8_t> atlas_pixels();

} // namespace protekt::game
