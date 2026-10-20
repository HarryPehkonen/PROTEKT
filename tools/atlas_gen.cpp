// protekt-atlas-gen [--seed N] [--out PATH]
// Writes the texture atlas as a PNG (default assets/atlas.png). The pixels come from the SAME
// in-memory builder the game falls back to (game::atlas_pixels), so there is exactly one definition
// of a tile; this tool only gives it a file name. The output is a pure function of the pixels: two
// runs write byte-identical files. --seed is accepted for the paint pass to come; the solid tiles
// take none, so today it changes nothing.
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

#include "protekt/app/args.hpp"
#include "protekt/game/atlas.hpp"
#include "protekt/game/png.hpp"

int main(int argc, char** argv) {
    std::uint64_t seed = 1;
    std::string out = "assets/atlas.png";
    const std::span<char*> args{argv, static_cast<std::size_t>(argc)};
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--seed" && i + 1 < args.size()) {
            if (!protekt::app::parse_number(args[++i], seed)) {
                std::cerr << "protekt-atlas-gen: --seed needs a whole number\n";
                return 2;
            }
        } else if (arg == "--out" && i + 1 < args.size()) {
            out = args[++i];
        } else {
            std::cerr << "usage: protekt-atlas-gen [--seed N] [--out PATH]\n";
            return 2;
        }
    }
    const auto pixels = protekt::game::atlas_pixels();
    if (!protekt::game::write_png_rgba(out, pixels, protekt::game::kAtlasSide,
                                       protekt::game::kAtlasSide)) {
        std::cerr << "protekt-atlas-gen: cannot write " << out << "\n";
        return 3;
    }
    std::cout << "protekt-atlas-gen: wrote " << out << " (" << protekt::game::kAtlasSide << "x"
              << protekt::game::kAtlasSide << ", seed " << seed << ")\n";
    return 0;
}
