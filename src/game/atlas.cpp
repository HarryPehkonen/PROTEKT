#include "protekt/game/atlas.hpp"

#include "atlas_manifest.hpp"
#include "protekt/game/png.hpp"

#include <algorithm>
#include <array>
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

using Rgb = std::array<std::uint8_t, 3>;

/// The tiles, by index. Tile 0 is the identity and an unassigned tile would be magenta, so a stray
/// UV is loud (all sixteen are assigned now). The tank's tones are near-neutral MULTIPLIERS: the
/// side colour, the selection tint and the face light all stay in the vertex colour and multiply on
/// top.
constexpr int kTracksTile = 1;
constexpr int kTurretTile = 2;
constexpr int kBarrelTile = 3;
constexpr int kFactoryWallsTile = 4;
constexpr int kFactoryRoofTile = 5;
constexpr int kTurretBodyTile = 6;
constexpr int kFirstHullTile = 7; ///< tank_hull_a; b and c follow
/// The team camo sets: the player's a, b, c, then the enemy's, on the last six tiles.
constexpr int kFirstTeamHullTile = 10;

constexpr Rgb kWhite{255, 255, 255};
constexpr Rgb kMagenta{255, 0, 255};
/// A camo palette: a mid tone (a minor accent), the dark tone the tile is filled with, and the
/// light (sand) tone painted over both in big patches.
struct Palette {
    Rgb mid;
    Rgb dark;
    Rgb light;
};

/// The neutral camo of tank_hull_a/b/c. The mid sits well under 255 so the sand can be BRIGHTER
/// than it: camo reads by lightness, and a sand that only shifted the hue would vanish on a blue
/// tank.
constexpr Palette kNeutralCamo{{190, 190, 186}, {86, 92, 80}, {255, 246, 214}};
/// The TEAM camo. A neutral camo multiplied by a saturated side colour collapses into one hue in
/// several brightnesses, which reads as shading, not camo; so each team's tiles carry the team's
/// own hue family -- a dark tone near 0.32 luma, a mid accent near 0.60 -- and a light tone near
/// 0.85 from ANOTHER hue family, so the pattern reads by hue as well as by lightness. The hull's
/// vertex colour is only a pale tint of the side colour (see the scene), so these tones survive it.
/// Player: navy and sand, steel-blue accent. Enemy: oxblood and pale sage, brick accent.
constexpr Palette kPlayerCamo{{110, 160, 205}, {55, 82, 160}, {240, 216, 170}};
constexpr Palette kEnemyCamo{{215, 140, 105}, {160, 60, 50}, {196, 226, 190}};
constexpr Rgb kTrackBase{64, 64, 66};    ///< the scene's old kTrackTint
constexpr Rgb kTrackLink{132, 130, 124}; ///< the lighter face of a link
constexpr Rgb kTrackGap{38, 38, 40};     ///< the shadow under it

using Tile = std::array<Rgb, std::size_t{kTileSide} * kTileSide>;

Rgb& texel(Tile& tile, std::uint32_t x, std::uint32_t y) {
    return tile.at((std::size_t{y} * kTileSide) + x);
}

/// splitmix64's finaliser: a fixed integer hash, so a texel is the same on every machine.
std::uint64_t mix(std::uint64_t z) noexcept {
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

/// The hash walk the patterns are drawn with: each step is the next splitmix64 output. Integers
/// only, so no float rounding can make two machines paint differently.
class HashWalk {
public:
    HashWalk(std::uint64_t seed, int tile) noexcept
        : state_(mix(seed ^ (static_cast<std::uint64_t>(tile) * 0xD1B54A32D192ED03ULL))) {}

    /// A whole number in [lo, hi].
    int between(int lo, int hi) noexcept {
        state_ += 0x9E3779B97F4A7C15ULL;
        const auto span = static_cast<std::uint64_t>(hi) - static_cast<std::uint64_t>(lo) + 1U;
        return lo + static_cast<int>(mix(state_) % span);
    }

private:
    std::uint64_t state_;
};

/// The pattern's carriers are the dark and the light (sand) tone, the pair a hue apart: the tile is
/// filled with the dark one and the sand goes on first, in a few BIG patches; the mid tone is only
/// a minor accent after it. Every patch paints only the dark ground, so a later patch never breaks
/// up an earlier one. A hull face shows the whole 64-texel tile in about 40 screen pixels, so a
/// dozen small patches render as speckle and the face reads as one colour: each pattern puts 2-4
/// bold sand patches, every one 16+ texels across, on the tile.
constexpr int kSandPatches = 3;    ///< sand patches a tile is painted with
constexpr int kAccentPatches = 3;  ///< mid-tone patches, painted after the sand, around it
constexpr int kMinPatchRadius = 8; ///< so a patch spans 16+ texels

/// Texel (x, y) becomes `tone` if it is still `ground`.
void paint_over(Tile& tile, int x, int y, const Rgb& ground, const Rgb& tone) {
    Rgb& at = texel(tile, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
    at = at == ground ? tone : at;
}

/// Camo A, SPLODGES: blobs, each a cluster of overlapping discs stamped at hashed offsets from a
/// hashed centre. Blob `i` of `count` is centred in its own strip of the tile's width, alternately
/// in the top and the bottom half (`phase` flips which), so the blobs spread over the whole tile
/// and stay apart rather than running into one.
void paint_blobs(Tile& tile, HashWalk& walk, int count, int phase, int lo, int hi,
                 const Rgb& ground, const Rgb& tone) {
    constexpr int kDiscs = 4;
    const int strip = 64 / count;
    for (int blob = 0; blob < count; ++blob) {
        const int cx = (blob * strip) + walk.between(strip / 4, strip - (strip / 4));
        const int cy = ((blob + phase) % 2) == 0 ? walk.between(10, 24) : walk.between(39, 53);
        const int size = walk.between(lo, hi);
        for (int disc = 0; disc < kDiscs; ++disc) {
            const int dx = disc == 0 ? 0 : walk.between(-size / 2, size / 2);
            const int dy = disc == 0 ? 0 : walk.between(-size / 2, size / 2);
            const int r = disc == 0 ? size : walk.between(kMinPatchRadius, size);
            const int ox = cx + dx;
            const int oy = cy + dy;
            for (int y = std::max(oy - r, 0); y <= std::min(oy + r, 63); ++y) {
                for (int x = std::max(ox - r, 0); x <= std::min(ox + r, 63); ++x) {
                    if (((x - ox) * (x - ox)) + ((y - oy) * (y - oy)) <= r * r) {
                        paint_over(tile, x, y, ground, tone);
                    }
                }
            }
        }
    }
}

Tile paint_splodges(std::uint64_t seed, int tile_index, const Palette& camo) {
    Tile tile{};
    tile.fill(camo.dark);
    HashWalk walk(seed, tile_index);
    paint_blobs(tile, walk, kSandPatches, 0, kMinPatchRadius + 3, kMinPatchRadius + 5, camo.dark,
                camo.light);
    paint_blobs(tile, walk, kAccentPatches, 1, kMinPatchRadius, kMinPatchRadius + 1, camo.dark,
                camo.mid);
    return tile;
}

/// Camo B, STRIPES: broad diagonal bands -- dark, sand, then a narrow mid accent -- with hashed
/// widths, whose edges wander a few texels along their length, so they read as brushed stripes
/// rather than a ruler's.
Tile paint_stripes(std::uint64_t seed, int tile_index, const Palette& camo) {
    Tile tile{};
    HashWalk walk(seed, tile_index);
    // The band each diagonal position falls in, for positions -24 .. 151 (offset by 24).
    constexpr int kOffset = 24;
    std::array<Rgb, 176> bands{};
    int at = 0;
    for (int band = 0; at < static_cast<int>(bands.size()); ++band) {
        Rgb tone = camo.dark;
        int width = walk.between(2 * kMinPatchRadius, (2 * kMinPatchRadius) + 4);
        if (band % 3 == 1) {
            tone = camo.light;
            width = walk.between(2 * kMinPatchRadius, (2 * kMinPatchRadius) + 4);
        } else if (band % 3 == 2) {
            tone = camo.mid;
            width = walk.between(7, 9);
        }
        for (int i = 0; i < width && at < static_cast<int>(bands.size()); ++i) {
            bands.at(static_cast<std::size_t>(at++)) = tone;
        }
    }
    // The wander: a hashed offset every 12 texels along the stripe, joined by straight lines.
    constexpr int kKnot = 12;
    std::array<int, 12> knots{};
    for (int& knot : knots) {
        knot = walk.between(-6, 6);
    }
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const int along = x - y + 64; // 1 .. 127
            const int k = along / kKnot;
            const int f = along % kKnot;
            const int wander = ((knots.at(static_cast<std::size_t>(k)) * (kKnot - f))
                                + (knots.at(static_cast<std::size_t>(k) + 1U) * f))
                               / kKnot;
            const int across = x + y + wander + kOffset; // 14 .. 156
            texel(tile, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y))
                = bands.at(static_cast<std::size_t>(across));
        }
    }
    return tile;
}

/// Camo C, BLOCKS: a digital pattern. Walkers step one 4-texel cell at a time in hashed directions
/// and stamp a 5x5-cell square wherever they stand, so the patches are big blocky clusters whose
/// every edge lies on the 4-texel grid. Walker `i` of `count` starts in its own strip of the tile's
/// width, alternately at the top and the bottom (`phase` flips which), so the clusters spread over
/// all of it and stay apart.
void walk_blocks(Tile& tile, HashWalk& walk, int count, int phase, int steps_lo, int steps_hi,
                 const Rgb& ground, const Rgb& tone) {
    constexpr int kCell = 4;
    constexpr int kCells = 64 / kCell;
    constexpr int kStamp = ((2 * kMinPatchRadius) / kCell) + 1; // cells a side: 20 texels
    const int strip = (kCells - kStamp) / count;
    for (int walker = 0; walker < count; ++walker) {
        int cx = (walker * strip) + walk.between(0, strip);
        int cy = ((walker + phase) % 2) == 0 ? walk.between(0, 2)
                                             : walk.between(kCells - kStamp - 2, kCells - kStamp);
        const int steps = walk.between(steps_lo, steps_hi);
        for (int step = 0; step < steps; ++step) {
            for (int y = cy * kCell; y < (cy + kStamp) * kCell; ++y) {
                for (int x = cx * kCell; x < (cx + kStamp) * kCell; ++x) {
                    paint_over(tile, x, y, ground, tone);
                }
            }
            switch (walk.between(0, 3)) {
            case 0:
                cx = std::min(cx + 1, kCells - kStamp);
                break;
            case 1:
                cx = std::max(cx - 1, 0);
                break;
            case 2:
                cy = std::min(cy + 1, kCells - kStamp);
                break;
            default:
                cy = std::max(cy - 1, 0);
                break;
            }
        }
    }
}

Tile paint_blocks(std::uint64_t seed, int tile_index, const Palette& camo) {
    Tile tile{};
    tile.fill(camo.dark);
    HashWalk walk(seed, tile_index);
    walk_blocks(tile, walk, kSandPatches, 0, 2, 4, camo.dark, camo.light);
    walk_blocks(tile, walk, kAccentPatches, 1, 2, 3, camo.dark, camo.mid);
    return tile;
}

/// The track: a dark band with a lighter link stripe every 8 rows, a shadow row under each, so the
/// tile reads as eight links across its height.
Tile paint_track() {
    Tile tile{};
    for (std::uint32_t y = 0; y < kTileSide; ++y) {
        const std::uint32_t row = y % 8U;
        const Rgb tone = row == 5U || row == 6U ? kTrackLink : (row == 7U ? kTrackGap : kTrackBase);
        for (std::uint32_t x = 0; x < kTileSide; ++x) {
            texel(tile, x, y) = tone;
        }
    }
    return tile;
}

/// Metal and walls: `base` with a slow wash of up to +/- `amount` (a few percent), the bilinear
/// blend of hashed values on a 16-texel lattice that wraps, so it has no seam and no texel noise.
Tile paint_metal(std::uint64_t seed, int tile_index, Rgb base, int amount) {
    Tile tile{};
    constexpr int kLattice = 16;
    constexpr int kKnots = 64 / kLattice;
    HashWalk walk(seed, tile_index);
    std::array<int, std::size_t{kKnots} * kKnots> knots{};
    for (int& knot : knots) {
        knot = walk.between(-amount, amount);
    }
    const auto knot = [&](int i, int j) {
        const auto row = static_cast<std::size_t>(j % kKnots);
        const auto column = static_cast<std::size_t>(i % kKnots);
        return knots.at((row * kKnots) + column);
    };
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const int i = x / kLattice;
            const int j = y / kLattice;
            const int fx = x % kLattice;
            const int fy = y % kLattice;
            const int top = (knot(i, j) * (kLattice - fx)) + (knot(i + 1, j) * fx);
            const int bottom = (knot(i, j + 1) * (kLattice - fx)) + (knot(i + 1, j + 1) * fx);
            const int shift = ((top * (kLattice - fy)) + (bottom * fy)) / (kLattice * kLattice);
            Rgb& out = texel(tile, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            for (std::size_t c = 0; c < 3; ++c) {
                out.at(c) = static_cast<std::uint8_t>(std::clamp(base.at(c) + shift, 0, 255));
            }
        }
    }
    return tile;
}

Tile solid(Rgb rgb) {
    Tile tile{};
    tile.fill(rgb);
    return tile;
}

/// Camo variant `variant` (0 splodges, 1 stripes, 2 blocks) in `camo`, painted as tile `index`.
Tile paint_camo(std::uint64_t seed, int index, int variant, const Palette& camo) {
    switch (variant) {
    case 0:
        return paint_splodges(seed, index, camo);
    case 1:
        return paint_stripes(seed, index, camo);
    default:
        return paint_blocks(seed, index, camo);
    }
}

Tile paint_tile(std::uint64_t seed, int index) {
    if (index >= kFirstHullTile && index < kFirstHullTile + kHullVariantCount) {
        return paint_camo(seed, index, index - kFirstHullTile, kNeutralCamo);
    }
    if (index >= kFirstTeamHullTile && index < kFirstTeamHullTile + kHullVariantCount) {
        return paint_camo(seed, index, index - kFirstTeamHullTile, kPlayerCamo);
    }
    if (index >= kFirstTeamHullTile + kHullVariantCount
        && index < kFirstTeamHullTile + (2 * kHullVariantCount)) {
        return paint_camo(seed, index, index - kFirstTeamHullTile - kHullVariantCount, kEnemyCamo);
    }
    switch (index) {
    case 0:
        return solid(kWhite);
    case kTracksTile:
        return paint_track();
    case kTurretTile:
        return paint_metal(seed, index, {205, 205, 203}, 6);
    case kBarrelTile:
        return paint_metal(seed, index, {182, 182, 180}, 5);
    case kFactoryWallsTile:
        return paint_metal(seed, index, {244, 244, 244}, 9);
    case kFactoryRoofTile:
        return paint_metal(seed, index, {228, 228, 228}, 8);
    case kTurretBodyTile:
        return paint_metal(seed, index, {236, 236, 236}, 8);
    default:
        return solid(kMagenta);
    }
}

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

std::vector<std::uint8_t> atlas_pixels(std::uint64_t seed) {
    std::vector<std::uint8_t> pixels(std::size_t{kAtlasSide} * kAtlasSide * 4U);
    for (int index = 0; index < kTileCount; ++index) {
        const Tile tile = paint_tile(seed, index);
        const std::uint32_t x0 = (static_cast<std::uint32_t>(index) % kTilesPerRow) * kTileSide;
        const std::uint32_t y0 = (static_cast<std::uint32_t>(index) / kTilesPerRow) * kTileSide;
        for (std::uint32_t y = 0; y < kTileSide; ++y) {
            for (std::uint32_t x = 0; x < kTileSide; ++x) {
                const Rgb& rgb = tile.at((std::size_t{y} * kTileSide) + x);
                const std::size_t at = ((std::size_t{y0 + y} * kAtlasSide) + x0 + x) * 4U;
                pixels.at(at) = rgb.at(0);
                pixels.at(at + 1) = rgb.at(1);
                pixels.at(at + 2) = rgb.at(2);
                pixels.at(at + 3) = 255;
            }
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
