#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// The texture atlas, as the GAME sees it: a fixed 8x8 grid of 64x64 tiles in a 512x512 image, a
/// manifest that names the tiles, and the UV rectangle each tile occupies. The renderer knows none
/// of this -- it is handed pixels and vertices.
///
/// The atlas lives in two places: built in memory (`atlas_pixels()`, `embedded_atlas_manifest()`),
/// which is the single definition of the tiles and the fallback, and as files (assets/atlas.png,
/// assets/atlas_manifest.txt) that `load_atlas_assets` prefers when it finds them, so a picture can
/// be swapped without a recompile. `protekt-atlas-gen` writes the PNG FROM the in-memory builder.
namespace protekt::game {

inline constexpr std::uint32_t kTilesPerRow = 8;
inline constexpr std::uint32_t kTileSide = 64;
inline constexpr std::uint32_t kAtlasSide = kTilesPerRow * kTileSide;
inline constexpr int kTileCount = static_cast<int>(kTilesPerRow * kTilesPerRow);

/// THE LAYOUT. The atlas is four quadrants of 4x4 tiles -- top-left, top-right, bottom-left,
/// bottom-right -- and the index counts quadrant by quadrant, row-major inside each. So tiles 0-15
/// are the old 4x4 atlas, every one at the texels it always had, and the growth moved nothing.
inline constexpr std::uint32_t kQuadrantSide = 4;
inline constexpr int kQuadrantTiles = static_cast<int>(kQuadrantSide * kQuadrantSide);

/// Where tile `index` (in [0, kTileCount)) sits, in tiles from the atlas's top-left corner.
struct TileCell {
    std::uint32_t col{};
    std::uint32_t row{};
};
[[nodiscard]] constexpr TileCell tile_cell(int index) noexcept {
    const auto at = static_cast<std::uint32_t>(index);
    const std::uint32_t quadrant = at / static_cast<std::uint32_t>(kQuadrantTiles);
    const std::uint32_t inside = at % static_cast<std::uint32_t>(kQuadrantTiles);
    return {((quadrant % 2U) * kQuadrantSide) + (inside % kQuadrantSide),
            ((quadrant / 2U) * kQuadrantSide) + (inside / kQuadrantSide)};
}

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

/// Half a texel in UV space: how far every tile's rectangle is pulled in from each edge, so a
/// NEAREST fetch exactly on a face edge can never land in the neighbouring tile.
inline constexpr float kHalfTexel = 0.5F / static_cast<float>(kAtlasSide);

/// The rectangle of tile `index` (at its `tile_cell`): the tile's own cell shrunk by half a texel
/// on each side. An index outside [0, kTileCount) gets the fallback tile's, so a bad index can
/// never address outside the atlas.
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

/// The committed manifest (assets/atlas_manifest.txt, embedded at configure time), parsed afresh.
[[nodiscard]] AtlasManifest embedded_atlas_manifest();

/// The manifest the scene uses: the embedded one until `install_atlas_manifest` replaces it. Set it
/// once at startup, before any scene is built.
[[nodiscard]] const AtlasManifest& atlas_manifest();
void install_atlas_manifest(AtlasManifest manifest);

/// The seed the committed atlas is painted from. Fixed: the tiles are data, so the same atlas comes
/// out of every build and every run.
inline constexpr std::uint64_t kAtlasSeed = 1;

/// The three hull camo variants, by manifest name, in the neutral (green and sand) palette. Each is
/// its own tile; `tank_hull` stays in the manifest as an alias of the first. No tank wears them now
/// -- a tank wears its TEAM's set below -- but the names and tiles stay, so a manifest or a tool
/// that names them still works.
inline constexpr int kHullVariantCount = 3;
inline constexpr std::array<std::string_view, kHullVariantCount> kHullVariantNames{
    "tank_hull_a", "tank_hull_b", "tank_hull_c"};

/// The same three variants painted in each team's own camo palette, indexed by the side's value
/// (0 player, 1 enemy) and then the variant. A tank wears
/// `kTeamHullNames[side][hull_variant(seed, id)]`.
inline constexpr int kTeamCount = 2;
inline constexpr std::array<std::array<std::string_view, kHullVariantCount>, kTeamCount>
    kTeamHullNames{{{"tank_hull_player_a", "tank_hull_player_b", "tank_hull_player_c"},
                    {"tank_hull_enemy_a", "tank_hull_enemy_b", "tank_hull_enemy_c"}}};

/// The in-memory atlas: kAtlasSide x kAtlasSide RGBA8, tightly packed. The one definition of the
/// tile contents: the generator writes this, the fallback uploads this. PAINTED, not solid: the
/// nine hull tiles carry camo (A splodges, B stripes, C blocks) in a base, a dark and a light tone
/// -- three in the neutral palette, three in the player's, three in the enemy's;
/// the track tile is dark with lighter link stripes across its height; the turret, barrel and
/// building tiles are their base tone with a few percent of slow variation. Tile 0 stays solid
/// white, the multiplicative identity for everything that sets no UV. Every texel is a pure
/// function of (`seed`, its position): a hash walk, no RNG library, no runtime randomness.
[[nodiscard]] std::vector<std::uint8_t> atlas_pixels(std::uint64_t seed = kAtlasSeed);

/// What the game will use: the atlas pixels and manifest, and where each came from (a file path,
/// or "embedded").
struct AtlasAssets {
    std::vector<std::uint8_t> pixels;
    AtlasManifest manifest;
    std::string pixels_source;
    std::string manifest_source;
};

inline constexpr std::string_view kEmbeddedSource = "embedded";

/// Looks for atlas.png and atlas_manifest.txt in each of `dirs` in order. A file that is missing,
/// is not a kAtlasSide x kAtlasSide image, or holds no valid manifest line is skipped; if none
/// qualifies the embedded version is used and ONE line saying so goes to `log`. Never fails:
/// the game does not refuse to start over an asset.
[[nodiscard]] AtlasAssets load_atlas_assets(std::span<const std::filesystem::path> dirs,
                                            std::ostream& log);

/// The search order: ./assets (the working directory), then assets/ next to the executable.
[[nodiscard]] std::vector<std::filesystem::path> default_asset_dirs();

/// `load_atlas_assets(default_asset_dirs())`, and the loaded manifest becomes the scene's.
[[nodiscard]] AtlasAssets load_game_atlas(std::ostream& log);

} // namespace protekt::game
