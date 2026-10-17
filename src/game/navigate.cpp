#include "protekt/game/navigate.hpp"

#include <cmath>
#include <numbers>

namespace protekt::game {
namespace {

[[nodiscard]] bool passable(std::uint64_t seed, int tile_x, int tile_y) noexcept {
    if (tile_x < 0 || tile_y < 0 || tile_x >= 256 || tile_y >= 256) {
        return false;
    }
    return world::sample(seed, tile_x, tile_y).terrain != world::Terrain::Crater;
}

} // namespace

int clear_run(std::uint64_t seed, int tile_x, int tile_y, float heading, int max_tiles) noexcept {
    const double sin_h = std::sin(static_cast<double>(heading));
    const double cos_h = std::cos(static_cast<double>(heading));
    int run = 0;
    for (int step = 1; step <= max_tiles; ++step) {
        const int x = tile_x + static_cast<int>(std::llround(sin_h * step));
        const int y = tile_y + static_cast<int>(std::llround(cos_h * step));
        if (!passable(seed, x, y)) {
            break;
        }
        ++run;
    }
    return run;
}

float clearest_heading(std::uint64_t seed, int tile_x, int tile_y) noexcept {
    constexpr double kStep = 2.0 * std::numbers::pi / static_cast<double>(kHeadingsToTry);
    float best_heading = 0.0F;
    int best_run = -1;
    for (int i = 0; i < kHeadingsToTry; ++i) {
        const float heading = static_cast<float>(static_cast<double>(i) * kStep);
        const int run = clear_run(seed, tile_x, tile_y, heading);
        if (run > best_run) { // strictly greater: ties keep the lowest heading
            best_run = run;
            best_heading = heading;
        }
    }
    return best_heading;
}

} // namespace protekt::game
