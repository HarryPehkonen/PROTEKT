// protekt-map --seed <u64> --size <N> [--chunk <cx> <cy>]
// Prints the world as ASCII plus the region hash. Its purpose is the one thing unit tests
// cannot check: that the SAME seed gives the SAME bytes in a DIFFERENT process.
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iostream>
#include <span>
#include <string_view>

#include "protekt/world/world.hpp"

namespace {

std::uint64_t parse_u64(std::string_view text, std::uint64_t fallback) {
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} ? value : fallback;
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t seed = 1;
    int size = 32;
    int chunk_x = 0;
    int chunk_y = 0;
    const std::span<char*> args{argv, static_cast<std::size_t>(argc)};
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--seed" && i + 1 < args.size()) {
            seed = parse_u64(args[++i], seed);
        } else if (arg == "--size" && i + 1 < args.size()) {
            size = std::atoi(args[++i]);
        } else if (arg == "--chunk" && i + 2 < args.size()) {
            chunk_x = std::atoi(args[++i]);
            chunk_y = std::atoi(args[++i]);
        }
    }
    if (size < 2) {
        size = 2;
    }
    if (size > protekt::world::kChunkSize) {
        size = protekt::world::kChunkSize;
    }

    const protekt::world::Chunk chunk = protekt::world::generate(seed, chunk_x, chunk_y);
    std::cout << std::format("seed 0x{:016X}  chunk ({},{})  {}x{}\n", seed, chunk_x, chunk_y, size,
                             size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const protekt::world::Tile& tile
                = chunk.tiles.at(static_cast<std::size_t>(y) * protekt::world::kChunkSize + x);
            const int level = tile.height;
            const char glyph = level < -256  ? '~'
                               : level < -64 ? '-'
                               : level < 64  ? '.'
                               : level < 256 ? '+'
                                             : '^';
            std::cout << glyph;
        }
        std::cout << '\n';
    }
    std::cout << std::format("hash 0x{:016X}\n",
                             protekt::world::region_hash(seed, chunk_x + 1, chunk_y + 1));
    return 0;
}
