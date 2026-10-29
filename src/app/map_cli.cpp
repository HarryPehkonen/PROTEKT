// protekt-map --seed <u64> --size <N> [--chunk <cx> <cy>]
// A bad command line is refused the way its siblings refuse one: a line on stderr, exit code 2.
// Prints the world as ASCII plus the region hash. Its purpose is the one thing unit tests
// cannot check: that the SAME seed gives the SAME bytes in a DIFFERENT process.
#include <cstddef>
#include <cstdint>
#include <format>
#include <iostream>
#include <span>
#include <string_view>

#include "protekt/app/args.hpp"
#include "protekt/world/world.hpp"

int main(int argc, char** argv) {
    std::uint64_t seed = 1;
    int size = 32;
    int chunk_x = 0;
    int chunk_y = 0;
    const std::span<char*> args{argv, static_cast<std::size_t>(argc)};
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        const auto next = [&](std::string_view& value) {
            if (i + 1 >= args.size()) {
                std::cerr << "protekt-map: " << arg << " needs a value\n";
                return false;
            }
            value = args[++i];
            return true;
        };
        std::string_view text;
        std::string_view second;
        if (arg == "--seed") {
            if (!next(text)) {
                return 2;
            }
            if (!protekt::app::parse_number(text, seed)) {
                std::cerr << "protekt-map: --seed wants an unsigned integer, got '" << text
                          << "'\n";
                return 2;
            }
        } else if (arg == "--size") {
            if (!next(text)) {
                return 2;
            }
            if (!protekt::app::parse_number(text, size)) {
                std::cerr << "protekt-map: --size wants an integer, got '" << text << "'\n";
                return 2;
            }
        } else if (arg == "--chunk") {
            if (!next(text) || !next(second)) {
                return 2;
            }
            if (!protekt::app::parse_number(text, chunk_x)
                || !protekt::app::parse_number(second, chunk_y)) {
                std::cerr << "protekt-map: --chunk wants two integers, got '" << text << "' '"
                          << second << "'\n";
                return 2;
            }
        } else {
            std::cerr << "protekt-map: unknown option '" << arg << "'\n";
            return 2;
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
    std::cout << std::format(
        "hash 0x{:016X}\n",
        protekt::world::region_hash(seed, std::int64_t{chunk_x} + 1, std::int64_t{chunk_y} + 1));
    return 0;
}
