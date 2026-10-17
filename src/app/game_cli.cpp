#include <cstddef>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

#include "protekt/app/game.hpp"

int main(int argc, char** argv) {
    const std::span<char*> raw{argv, static_cast<std::size_t>(argc)};
    std::vector<std::string_view> args;
    for (std::size_t i = 1; i < raw.size(); ++i) {
        args.emplace_back(raw[i]);
    }
    return protekt::app::run_game_cli(args, std::cout, std::cerr);
}
