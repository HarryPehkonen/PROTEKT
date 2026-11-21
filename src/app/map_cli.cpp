// protekt-map --seed <u64> --size <N> [--chunk <cx> <cy>]      print one chunk as ASCII + its hash
// protekt-map gen --seed <u64> [--size <N>] [--out PATH]      write a seed's map as a TEXT document
// protekt-map draw MAP.map [--out PATH.png]                   render a map document to an image
// protekt-map check MAP.map                                   load it, say what it holds
// A bad command line is refused the way its siblings refuse one: a line on stderr, exit code 2.
// Prints the world as ASCII plus the region hash. Its purpose is the one thing unit tests
// cannot check: that the SAME seed gives the SAME bytes in a DIFFERENT process.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

#include "protekt/app/args.hpp"
#include "protekt/app/map_file.hpp"
#include "protekt/world/world.hpp"

namespace {

/// The document subcommands: gen, draw, check. Same refusal shape as the rest: a line on stderr, 2.
int run_document(std::string_view verb, std::span<char*> args) {
    std::uint64_t seed = 1;
    int size = protekt::world::kChunkSize;
    std::string out;
    std::string input;
    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        const bool has_value = i + 1 < args.size();
        if (arg == "--out" || (verb == "gen" && (arg == "--seed" || arg == "--size"))) {
            if (!has_value) {
                std::cerr << "protekt-map: " << arg << " needs a value\n";
                return 2;
            }
            const std::string_view value = args[++i];
            if (arg == "--out") {
                out = value;
            } else if (arg == "--seed" && !protekt::app::parse_number(value, seed)) {
                std::cerr << "protekt-map: --seed wants an unsigned integer, got '" << value
                          << "'\n";
                return 2;
            } else if (arg == "--size" && !protekt::app::parse_number(value, size)) {
                std::cerr << "protekt-map: --size wants an integer, got '" << value << "'\n";
                return 2;
            }
        } else if (verb != "gen" && input.empty() && !arg.starts_with("-")) {
            input = arg;
        } else {
            std::cerr << "protekt-map: unknown option '" << arg << "'\n";
            return 2;
        }
    }
    if (verb == "gen") {
        const std::string text = protekt::app::write_map(protekt::app::generate_map(seed, size));
        if (out.empty()) {
            std::cout << text;
            return 0;
        }
        std::ofstream file(out, std::ios::binary | std::ios::trunc);
        file << text;
        if (!file) {
            std::cerr << "protekt-map: cannot write " << out << "\n";
            return 2;
        }
        std::cout << "protekt-map: wrote " << out << " (seed " << seed << ")\n";
        return 0;
    }
    if (input.empty()) {
        std::cerr << "protekt-map: " << verb << " needs a map file\n";
        return 2;
    }
    const protekt::app::MapParse parsed = protekt::app::load_map_file(input);
    if (!parsed.ok) {
        std::cerr << "protekt-map: " << input << ": " << parsed.error << "\n";
        return 2;
    }
    const protekt::app::MapDoc& map = parsed.map;
    if (verb == "check") {
        std::cout << std::format("{}: \"{}\" seed {} {}x{}, {} spawns, {} places, {} volumes\n",
                                 input, map.name, map.seed, map.width, map.height,
                                 map.spawns.size(), map.places.size(), map.volumes.size());
        return 0;
    }
    if (out.empty()) {
        out = std::filesystem::path(input).replace_extension(".png").string();
    }
    if (!protekt::app::draw_map(map, out)) {
        std::cerr << "protekt-map: cannot write " << out << "\n";
        return 2;
    }
    std::cout << "protekt-map: drew " << out << " (" << map.width * protekt::app::kMapDrawScale
              << "x" << map.height * protekt::app::kMapDrawScale << ")\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::span<char*> all{argv, static_cast<std::size_t>(argc)};
    if (all.size() >= 2) {
        const std::string_view verb = all[1];
        if (verb == "gen" || verb == "draw" || verb == "check") {
            return run_document(verb, all);
        }
    }
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
