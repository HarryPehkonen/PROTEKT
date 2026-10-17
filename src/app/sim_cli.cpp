#include <iostream>
#include <string_view>
#include <vector>

#include "protekt/app/cli.hpp"

int main(int argc, char** argv) {
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return protekt::app::run_cli(args, std::cout, std::cerr);
}
