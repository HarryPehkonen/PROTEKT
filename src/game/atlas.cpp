#include "protekt/game/atlas.hpp"

#include "atlas_manifest.hpp"
#include "protekt/game/png.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdlib>
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

/// The tiles, by index. Tile 0 is the identity and an unassigned tile is magenta, so a stray UV is
/// loud (the first quadrant's sixteen, the hover skirt and the four ground tiles are assigned; the
/// rest are the budget).
/// The tank's tones are near-neutral MULTIPLIERS: the side colour, the selection tint and the face
/// light all stay in the vertex colour and multiply on top.
constexpr int kTracksTile = 1;
constexpr int kTurretTile = 2;
constexpr int kBarrelTile = 3;
constexpr int kFactoryWallsTile = 4;
constexpr int kFactoryRoofTile = 5;
constexpr int kTurretBodyTile = 6;
constexpr int kFirstHullTile = 7; ///< tank_hull_a; b and c follow
/// The team camo sets: the player's a, b, c, then the enemy's, on the last six tiles.
constexpr int kFirstTeamHullTile = 10;
constexpr int kHoverSkirtTile = 16; ///< the first tile of the second quadrant
/// The ground, one tile per world::Terrain: grass (Flat), dirt (Rough), rock (Ridge), crater.
constexpr int kGrassTile = 17;
constexpr int kDirtTile = 18;
constexpr int kRockTile = 19;
constexpr int kCraterTile = 20;

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

/// Structure paint. These tiles are neutral grey MULTIPLIERS like the tank's, but they carry real
/// detail in luminance steps -- dark seams, light plates -- never hue, so the side colour and the
/// face light multiply over it and the detail still reads. A face shows the whole tile.
void put(Tile& tile, int x, int y, int grey) {
    if (x < 0 || x >= 64 || y < 0 || y >= 64) {
        return;
    }
    const auto v = static_cast<std::uint8_t>(std::clamp(grey, 0, 255));
    texel(tile, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)) = Rgb{v, v, v};
}

int grey_at(Tile& tile, int x, int y) {
    return texel(tile, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)).at(0);
}

void fill_rect(Tile& tile, int x0, int y0, int x1, int y1, int grey) {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            put(tile, x, y, grey);
        }
    }
}

void frame_rect(Tile& tile, int x0, int y0, int x1, int y1, int grey) {
    fill_rect(tile, x0, y0, x1, y0, grey);
    fill_rect(tile, x0, y1, x1, y1, grey);
    fill_rect(tile, x0, y0, x0, y1, grey);
    fill_rect(tile, x1, y0, x1, y1, grey);
}

/// A hashed value in [0, 99] for texel (x, y): the same on every machine.
int grain(std::uint64_t seed, int tile_index, int x, int y) noexcept {
    const std::uint64_t key = (static_cast<std::uint64_t>(tile_index) << 16U)
                              | (static_cast<std::uint64_t>(y) << 8U)
                              | static_cast<std::uint64_t>(x);
    return static_cast<int>(mix(seed ^ (key * 0x9E3779B97F4A7C15ULL)) % 100U);
}

/// Soot: darkens the tile inside a ragged disc, most at the centre (up to `depth` of 255).
void scorch(Tile& tile, std::uint64_t seed, int tile_index, int cx, int cy, int radius, int depth) {
    for (int y = cy - radius; y <= cy + radius; ++y) {
        for (int x = cx - radius; x <= cx + radius; ++x) {
            if (x < 0 || x >= 64 || y < 0 || y >= 64) {
                continue;
            }
            const int d2 = ((x - cx) * (x - cx)) + ((y - cy) * (y - cy))
                           + (grain(seed, tile_index, x, y) * radius / 20);
            if (d2 < radius * radius) {
                const int dark = depth * (radius * radius - d2) / (radius * radius);
                put(tile, x, y, grey_at(tile, x, y) * (255 - dark) / 255);
            }
        }
    }
}

/// Hazard stripes: diagonal bands, `period` texels, half dark and half light, inside the rect.
void hazard_band(Tile& tile, int x0, int y0, int x1, int y1, int period, int dark, int light) {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            put(tile, x, y, ((x + y) % period) < period / 2 ? dark : light);
        }
    }
}

/// A louvred vent: a light frame round dark slots.
void vent(Tile& tile, int x0, int y0, int x1, int y1) {
    fill_rect(tile, x0, y0, x1, y1, 55);
    for (int y = y0 + 2; y <= y1 - 2; y += 3) {
        fill_rect(tile, x0 + 2, y, x1 - 2, y, 150);
    }
    frame_rect(tile, x0 - 1, y0 - 1, x1 + 1, y1 + 1, 250);
    frame_rect(tile, x0 - 2, y0 - 2, x1 + 2, y1 + 2, 95);
}

/// The defence turret's housing: gun-metal plates split by dark seams with a light bevel beside
/// each, weathered edges, a hatch ring with eight bolts, and a warning stripe along the foot.
Tile paint_turret_housing(std::uint64_t seed, int tile_index) {
    Tile tile = paint_metal(seed, tile_index, {222, 222, 222}, 8);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const int edge = std::min({x, y, 63 - x, 63 - y});
            if (edge < 4 && grain(seed, tile_index, x, y) < 70 - (edge * 15)) {
                put(tile, x, y, grey_at(tile, x, y) - 95 + (edge * 12)); // weathered edges
            }
        }
    }
    for (const int row : {16, 41}) {
        fill_rect(tile, 0, row, 63, row + 1, 70);
        fill_rect(tile, 0, row + 2, 63, row + 2, 252);
    }
    fill_rect(tile, 10, 0, 11, 15, 70);
    fill_rect(tile, 50, 42, 51, 63, 70);
    // The hatch: a dark ring round a light lid, a bolt on each of eight points of the ring.
    constexpr int kCx = 32;
    constexpr int kCy = 28;
    for (int y = kCy - 15; y <= kCy + 15; ++y) {
        for (int x = kCx - 15; x <= kCx + 15; ++x) {
            const int d2 = ((x - kCx) * (x - kCx)) + ((y - kCy) * (y - kCy));
            if (d2 <= 15 * 15 && d2 >= 11 * 11) {
                put(tile, x, y, 60);
            } else if (d2 < 11 * 11) {
                put(tile, x, y, 238 - (d2 / 12));
            }
        }
    }
    constexpr std::array<std::array<int, 2>, 8> kBolts{
        {{13, 0}, {9, 9}, {0, 13}, {-9, 9}, {-13, 0}, {-9, -9}, {0, -13}, {9, -13 + 4}}};
    for (const auto& bolt : kBolts) {
        fill_rect(tile, kCx + bolt.at(0) - 1, kCy + bolt.at(1) - 1, kCx + bolt.at(0) + 1,
                  kCy + bolt.at(1) + 1, 255);
        put(tile, kCx + bolt.at(0) + 1, kCy + bolt.at(1) + 1, 40);
    }
    fill_rect(tile, kCx - 5, kCy - 1, kCx + 5, kCy, 90); // the hatch handle
    hazard_band(tile, 0, 52, 63, 61, 8, 55, 245);
    fill_rect(tile, 0, 51, 63, 51, 55);
    fill_rect(tile, 0, 62, 63, 62, 55);
    return tile;
}

/// The factory's roof: corrugated ribs, two seams across them, two louvred vents, a hazard-
/// bordered service hatch, and scorch on the corner by the door.
Tile paint_factory_roof(std::uint64_t seed, int tile_index) {
    Tile tile = paint_metal(seed, tile_index, {232, 232, 232}, 6);
    constexpr std::array<int, 8> kRib{105, 150, 205, 245, 255, 245, 205, 150};
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            put(tile, x, y, grey_at(tile, x, y) * kRib.at(static_cast<std::size_t>(x % 8)) / 255);
        }
    }
    for (const int row : {21, 42}) {
        fill_rect(tile, 0, row, 63, row + 1, 60);
        fill_rect(tile, 0, row + 2, 63, row + 2, 255);
    }
    vent(tile, 8, 6, 21, 15);
    vent(tile, 40, 6, 53, 15);
    fill_rect(tile, 20, 28, 43, 38, 250);
    hazard_band(tile, 20, 28, 43, 29, 6, 50, 250);
    hazard_band(tile, 20, 37, 43, 38, 6, 50, 250);
    frame_rect(tile, 19, 27, 44, 39, 50);
    fill_rect(tile, 28, 31, 35, 34, 120); // its handle
    scorch(tile, seed, tile_index, 52, 56, 13, 190);
    scorch(tile, seed, tile_index, 12, 50, 8, 150);
    return tile;
}

/// The factory's wall: a corrugated-panel course, a big vehicle door with a hazard-chevron band on
/// its head, slats, a side vent, and soot rising from the sill.
Tile paint_factory_wall(std::uint64_t seed, int tile_index) {
    Tile tile = paint_metal(seed, tile_index, {240, 240, 240}, 7);
    for (const int row : {9, 26}) {
        fill_rect(tile, 0, row, 63, row, 85);
        fill_rect(tile, 0, row + 1, 63, row + 1, 255);
    }
    fill_rect(tile, 0, 0, 63, 2, 200); // eaves
    // The door: x 10..45, y 19..63, a dark frame, light slats split by dark gaps.
    fill_rect(tile, 10, 19, 45, 63, 212);
    for (int y = 29; y <= 63; y += 5) {
        fill_rect(tile, 12, y, 43, y, 60);
        fill_rect(tile, 12, y + 1, 43, y + 1, 250);
    }
    fill_rect(tile, 27, 19, 28, 63, 110); // the meeting of its two leaves
    // A heavy near-black frame, six texels thick: a face is ~50 px across at gameplay distance, so
    // anything thinner is under four pixels.
    for (int i = 0; i < 6; ++i) {
        frame_rect(tile, 4 + i, 13 + i, 51 - i, 63, 28);
    }
    for (int y = 20; y <= 25; ++y) { // the chevron band over the door
        for (int x = 11; x <= 44; ++x) {
            put(tile, x, y, (((std::abs(x - 28) + y) / 3) % 2) == 0 ? 28 : 250);
        }
    }
    vent(tile, 52, 31, 58, 46);
    scorch(tile, seed, tile_index, 28, 63, 24, 200);
    scorch(tile, seed, tile_index, 56, 63, 10, 150);
    return tile;
}

/// The hover skirt: the glow of the lift under a hover tank. Near-white, so the raw skirt colour
/// the scene puts on it is what shows: a dark lip along the top where it meets the hull, then a
/// ramp that brightens toward the ground, crossed by faint vertical vanes every 8 texels.
Tile paint_hover_skirt() {
    Tile tile{};
    constexpr int kLip = 4; ///< texels of the dark lip
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const int ramp = 190 + ((65 * (y - kLip)) / (63 - kLip));
            const int grey = y < kLip ? 110 : (x % 8 == 0 ? ramp - 30 : ramp);
            put(tile, x, y, grey);
        }
    }
    return tile;
}

/// THE GROUND. Unlike the tank's multipliers these tiles carry their own colour -- the scene draws
/// the terrain in white light -- in three tones each whose share-weighted mean sits near the flat
/// colour that terrain type used to be, so grass, dirt, rock and crater still read apart at a
/// glance. Every tile WRAPS (its left edge continues its right, its top its bottom), because the
/// same tile lies on every quad of its type side by side. Detail is never finer than 8 texels: a
/// ground quad is 2 m, and at gameplay distance anything smaller is sub-pixel speckle.
struct GroundTones {
    Rgb dark;
    Rgb base;
    Rgb light;
};
constexpr GroundTones kGrass{{44, 104, 36}, {66, 142, 52}, {104, 172, 72}};
constexpr GroundTones kDirt{{82, 74, 34}, {118, 109, 48}, {158, 146, 90}};
constexpr GroundTones kRock{{96, 88, 84}, {142, 130, 124}, {190, 180, 172}};
constexpr GroundTones kCrater{{20, 16, 14}, {40, 33, 28}, {92, 84, 76}};

using Field = std::array<int, std::size_t{kTileSide} * kTileSide>;

/// Adds `weight` times one octave of value noise to `field`: hashed knots in [-1024, 1024] on a
/// lattice `cell` texels apart that wraps at the tile's edge, blended bilinearly -- seamless, and
/// with no feature finer than `cell`.
void add_wrapped_noise(Field& field, HashWalk& walk, int cell, int weight) {
    const int knots = 64 / cell;
    std::vector<int> lattice(static_cast<std::size_t>(knots) * static_cast<std::size_t>(knots));
    for (int& knot : lattice) {
        knot = walk.between(-1024, 1024);
    }
    const auto knot = [&](int i, int j) {
        return lattice.at((static_cast<std::size_t>(j % knots) * static_cast<std::size_t>(knots))
                          + static_cast<std::size_t>(i % knots));
    };
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const int i = x / cell;
            const int j = y / cell;
            const int fx = x % cell;
            const int fy = y % cell;
            const int top = (knot(i, j) * (cell - fx)) + (knot(i + 1, j) * fx);
            const int bottom = (knot(i, j + 1) * (cell - fx)) + (knot(i + 1, j + 1) * fx);
            field.at((static_cast<std::size_t>(y) * 64U) + static_cast<std::size_t>(x))
                += weight * (((top * (cell - fy)) + (bottom * fy)) / (cell * cell));
        }
    }
}

/// The value below which `percent` of the field lies: thresholds by share, not by value, so every
/// tone covers a known part of the tile whatever the knots drew.
int percentile(Field field, int percent) {
    const auto at = (field.size() * static_cast<std::size_t>(percent)) / 100U;
    std::nth_element(field.begin(), field.begin() + static_cast<std::ptrdiff_t>(at), field.end());
    return field.at(at);
}

/// Grass clumps and dirt mottling: two octaves of wrapped noise (16- and 8-texel cells, weighted
/// `coarse`:`fine`) cut into three tones at the 30th and 70th percentiles -- a dark and a light
/// patch on a base that keeps the middle 40%.
Tile paint_patches(std::uint64_t seed, int tile_index, const GroundTones& tones, int coarse,
                   int fine) {
    HashWalk walk(seed, tile_index);
    Field field{};
    add_wrapped_noise(field, walk, 16, coarse);
    add_wrapped_noise(field, walk, 8, fine);
    const int low = percentile(field, 30);
    const int high = percentile(field, 70);
    Tile tile{};
    for (std::size_t i = 0; i < tile.size(); ++i) {
        const int f = field.at(i);
        tile.at(i) = f < low ? tones.dark : (f >= high ? tones.light : tones.base);
    }
    return tile;
}

/// Bands across the tile: `position(x, y)` is where a texel lies across the bands, wrapped to
/// [0, 64), and the band it falls in gives its tone. The band widths sum to 64, so the pattern
/// wraps; the narrowest is 8 texels. The wander, a wrapped 16-texel noise of up to +/- `wander`
/// texels, bends the band edges so they read as layers rather than a ruler's lines.
template <typename Position>
Tile paint_bands(std::uint64_t seed, int tile_index, std::span<const int> widths,
                 std::span<const Rgb> tones, int wander, Position position) {
    HashWalk walk(seed, tile_index);
    Field bend{};
    add_wrapped_noise(bend, walk, 16, 1);
    std::array<std::size_t, 64> band_at{};
    std::size_t at = 0;
    for (std::size_t band = 0; band < widths.size(); ++band) {
        for (int i = 0; i < widths[band] && at < band_at.size(); ++i) {
            band_at.at(at++) = band;
        }
    }
    Tile tile{};
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * 64U) + static_cast<std::size_t>(x);
            const int across = position(x, y) + ((bend.at(i) * wander) / 1024);
            tile.at(i) = tones[band_at.at(static_cast<std::size_t>(((across % 64) + 64) % 64))];
        }
    }
    return tile;
}

/// Rock: strata, horizontal bands of the three greys.
Tile paint_rock(std::uint64_t seed, int tile_index) {
    constexpr std::array<int, 6> kWidths{10, 8, 12, 8, 14, 12};
    const std::array<Rgb, 6> tones{kRock.base, kRock.light, kRock.base,
                                   kRock.dark, kRock.base,  kRock.light};
    return paint_bands(seed, tile_index, kWidths, tones, 6, [](int /*x*/, int y) { return y; });
}

/// Crater: striations, diagonal streaks of scorched floor with pale ash between them.
Tile paint_crater(std::uint64_t seed, int tile_index) {
    constexpr std::array<int, 6> kWidths{12, 8, 10, 8, 12, 14};
    const std::array<Rgb, 6> tones{kCrater.base, kCrater.light, kCrater.dark,
                                   kCrater.base, kCrater.light, kCrater.dark};
    return paint_bands(seed, tile_index, kWidths, tones, 6, [](int x, int y) { return x + y; });
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
        return paint_factory_wall(seed, index);
    case kFactoryRoofTile:
        return paint_factory_roof(seed, index);
    case kTurretBodyTile:
        return paint_turret_housing(seed, index);
    case kHoverSkirtTile:
        return paint_hover_skirt();
    case kGrassTile:
        return paint_patches(seed, index, kGrass, 3, 1);
    case kDirtTile:
        return paint_patches(seed, index, kDirt, 1, 1);
    case kRockTile:
        return paint_rock(seed, index);
    case kCraterTile:
        return paint_crater(seed, index);
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
    const TileCell cell = tile_cell(index);
    const auto col = static_cast<float>(cell.col);
    const auto row = static_cast<float>(cell.row);
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
        const TileCell cell = tile_cell(index);
        const std::uint32_t x0 = cell.col * kTileSide;
        const std::uint32_t y0 = cell.row * kTileSide;
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
