#include "protekt/sim/scrap.hpp"

#include <array>

namespace protekt::sim {

namespace {

/// How many tiles the scatter square holds. The SIDE of that square is `kScrapSpan`, defined once
/// in the header beside `kScrapScatterRadius`; this is only the square's area, so the span itself
/// still lives in exactly one place.
inline constexpr int kSpanTiles = kScrapSpan * kScrapSpan;

struct Offset {
    int dx{};
    int dy{};
};

/// splitmix64's finaliser: one input bit flipped moves every output bit. NOT a random number
/// generator -- a pure function, which is the whole point: the same inputs give the same answer on
/// every machine, at every optimisation level, in every process.
[[nodiscard]] constexpr std::uint64_t mix(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

/// The piece's own number, out of the three things that decide where it lies.
[[nodiscard]] constexpr std::uint64_t piece_hash(std::uint64_t seed, std::uint32_t id,
                                                 int piece) noexcept {
    return mix(seed
               ^ mix((static_cast<std::uint64_t>(id) << 32U) ^ static_cast<std::uint64_t>(piece)));
}

/// The centre of a tile, in fixed point: tile `t` spans [t, t + 1), so its centre is t + a half.
[[nodiscard]] Fixed tile_centre(int tile) noexcept {
    return fixed_add(Fixed::from_int(tile), Fixed::from_raw(Fixed::kOne / 2));
}

/// Every tile in the scatter square, in a fixed scan order.
[[nodiscard]] std::array<Offset, kSpanTiles> scatter_square() noexcept {
    std::array<Offset, kSpanTiles> tiles{};
    int index = 0;
    for (int dy = -kScrapScatterRadius; dy <= kScrapScatterRadius; ++dy) {
        for (int dx = -kScrapScatterRadius; dx <= kScrapScatterRadius; ++dx) {
            tiles.at(static_cast<std::size_t>(index)) = Offset{dx, dy};
            ++index;
        }
    }
    return tiles;
}

} // namespace

void scatter(std::vector<Scrap>& out, std::uint64_t seed, UnitId id, Fixed x, Fixed y) noexcept {
    const int wreck_x = x.floor_int();
    const int wreck_y = y.floor_int();
    // The tiles this wreck may still use. A piece takes the tile its own hash names; a tile already
    // taken by an earlier piece of the SAME wreck is struck off the list, so one wreck never leaves
    // two pieces on one tile -- and because the list only ever holds tiles of the scatter square,
    // every piece stays within `kScrapScatterRadius` of the wreck.
    std::array<Offset, kSpanTiles> free_tiles = scatter_square();
    int free_count = kSpanTiles;
    for (int piece = 0; piece < kScrapPerWreck; ++piece) {
        if (out.size() >= kMaxScrap || free_count == 0) {
            return;
        }
        const std::uint64_t h = piece_hash(seed, id.value, piece);
        const int pick = static_cast<int>(h % static_cast<std::uint64_t>(free_count));
        const Offset offset = free_tiles.at(static_cast<std::size_t>(pick));
        // Strike the picked tile off: the last free tile moves into its slot. The order of the
        // tiles still free changes with it, but only for this wreck's own pieces, and the same
        // inputs always change it the same way.
        free_tiles.at(static_cast<std::size_t>(pick))
            = free_tiles.at(static_cast<std::size_t>(free_count - 1));
        --free_count;
        out.push_back(Scrap{tile_centre(wreck_x + offset.dx), tile_centre(wreck_y + offset.dy)});
    }
}

} // namespace protekt::sim