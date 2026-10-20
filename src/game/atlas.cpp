#include "protekt/game/atlas.hpp"

#include "atlas_manifest.hpp"
#include "protekt/game/png.hpp"

#include <charconv>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <ostream>
#include <string>
#include <system_error>
#include <utility>

namespace protekt::game {

namespace {

constexpr std::string_view kBlank = " \t\r";

/// The next whitespace-separated word of `line`, which is consumed from it. Empty when none is
/// left.
std::string_view next_word(std::string_view& line) {
    const std::size_t start = line.find_first_not_of(kBlank);
    if (start == std::string_view::npos) {
        line = {};
        return {};
    }
    line.remove_prefix(start);
    const std::size_t end = line.find_first_of(kBlank);
    const std::string_view word = line.substr(0, end);
    line = end == std::string_view::npos ? std::string_view{} : line.substr(end);
    return word;
}

/// One tile's colour, RGB. Tile 0 is white (the identity); the tank's parts are near-neutral
/// multipliers because the side colour, the selection tint and the face light all stay in the
/// vertex colour and multiply on top; unassigned tiles are magenta so a stray UV is loud.
constexpr std::array<std::array<std::uint8_t, 3>, kTileCount> kTileColours{{
    {255, 255, 255}, // 0 tank_hull: unit multiplier
    {64, 64, 64},    // 1 tank_tracks: dark (the scene's kTrackTint)
    {240, 240, 240}, // 2 tank_turret
    {217, 217, 217}, // 3 tank_barrel
    {255, 255, 255}, // 4 factory_walls
    {235, 235, 235}, // 5 factory_roof
    {240, 240, 240}, // 6 turret_body
    {255, 0, 255},
    {255, 0, 255},
    {255, 0, 255},
    {255, 0, 255},
    {255, 0, 255},
    {255, 0, 255},
    {255, 0, 255},
    {255, 0, 255},
    {255, 0, 255},
}};

} // namespace

UvRect tile_rect(int index) noexcept {
    if (index < 0 || index >= kTileCount) {
        index = kFallbackTile;
    }
    const auto side = static_cast<float>(kTilesPerRow);
    const std::uint32_t col_index = static_cast<std::uint32_t>(index) % kTilesPerRow;
    const std::uint32_t row_index = (static_cast<std::uint32_t>(index) - col_index) / kTilesPerRow;
    const auto col = static_cast<float>(col_index);
    const auto row = static_cast<float>(row_index);
    return UvRect{(col / side) + kHalfTexel, (row / side) + kHalfTexel,
                  ((col + 1.0F) / side) - kHalfTexel, ((row + 1.0F) / side) - kHalfTexel};
}

AtlasManifest AtlasManifest::parse(std::string_view text) {
    AtlasManifest manifest;
    while (!text.empty()) {
        const std::size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);

        const std::string_view name = next_word(line);
        const std::string_view number = next_word(line);
        if (name.empty() || name.front() == '#' || number.empty() || !next_word(line).empty()) {
            continue;
        }
        int index = 0;
        const auto [end, error]
            = std::from_chars(number.data(), number.data() + number.size(), index);
        if (error != std::errc{} || end != number.data() + number.size() || index < 0
            || index >= kTileCount) {
            continue;
        }
        manifest.tiles_[std::string{name}] = index;
    }
    return manifest;
}

int AtlasManifest::tile(std::string_view name) const {
    const auto found = tiles_.find(name);
    return found == tiles_.end() ? kFallbackTile : found->second;
}

AtlasManifest embedded_atlas_manifest() { return AtlasManifest::parse(kAtlasManifestText); }

namespace {

AtlasManifest& active_manifest() {
    static AtlasManifest manifest = embedded_atlas_manifest();
    return manifest;
}

} // namespace

const AtlasManifest& atlas_manifest() { return active_manifest(); }

void install_atlas_manifest(AtlasManifest manifest) { active_manifest() = std::move(manifest); }

std::vector<std::uint8_t> atlas_pixels() {
    std::vector<std::uint8_t> pixels(std::size_t{kAtlasSide} * kAtlasSide * 4U);
    for (std::uint32_t y = 0; y < kAtlasSide; ++y) {
        for (std::uint32_t x = 0; x < kAtlasSide; ++x) {
            const std::uint32_t tile = ((y / kTileSide) * kTilesPerRow) + (x / kTileSide);
            const auto& rgb = kTileColours.at(tile);
            const std::size_t at = ((std::size_t{y} * kAtlasSide) + x) * 4U;
            pixels.at(at) = rgb.at(0);
            pixels.at(at + 1) = rgb.at(1);
            pixels.at(at + 2) = rgb.at(2);
            pixels.at(at + 3) = 255;
        }
    }
    return pixels;
}

AtlasAssets load_atlas_assets(std::span<const std::filesystem::path> dirs, std::ostream& log) {
    AtlasAssets assets;
    for (const auto& dir : dirs) {
        if (!assets.pixels.empty()) {
            break;
        }
        std::vector<std::uint8_t> rgba;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        const std::filesystem::path file = dir / "atlas.png";
        if (read_png_rgba(file, rgba, width, height) && width == kAtlasSide
            && height == kAtlasSide) {
            assets.pixels = std::move(rgba);
            assets.pixels_source = file.string();
        }
    }
    for (const auto& dir : dirs) {
        if (!assets.manifest_source.empty()) {
            break;
        }
        const std::filesystem::path file = dir / "atlas_manifest.txt";
        std::ifstream in(file, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>()};
        AtlasManifest parsed = AtlasManifest::parse(text);
        if (parsed.size() > 0) {
            assets.manifest = std::move(parsed);
            assets.manifest_source = file.string();
        }
    }
    if (assets.pixels.empty()) {
        assets.pixels = atlas_pixels();
        assets.pixels_source = kEmbeddedSource;
        log << "protekt: no usable assets/atlas.png (searched the working directory and the "
               "executable's); using the embedded atlas\n";
    }
    if (assets.manifest_source.empty()) {
        assets.manifest = embedded_atlas_manifest();
        assets.manifest_source = kEmbeddedSource;
        log << "protekt: no usable assets/atlas_manifest.txt; using the embedded manifest\n";
    }
    return assets;
}

std::vector<std::filesystem::path> default_asset_dirs() {
    std::vector<std::filesystem::path> dirs{std::filesystem::path{"assets"}};
    std::error_code ec;
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && exe.has_parent_path()) {
        dirs.push_back(exe.parent_path() / "assets");
    }
    return dirs;
}

AtlasAssets load_game_atlas(std::ostream& log) {
    const auto dirs = default_asset_dirs();
    AtlasAssets assets = load_atlas_assets(dirs, log);
    install_atlas_manifest(assets.manifest);
    return assets;
}

} // namespace protekt::game
