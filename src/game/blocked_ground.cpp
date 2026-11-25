#include "protekt/game/blocked_ground.hpp"

#include "protekt/world/ground.hpp"

namespace protekt::game {

bool ground_blocked(std::span<const sim::PlacedVolume> volumes, int tile_x, int tile_y) noexcept {
    if (world::ground_solid(tile_x, tile_y)) {
        return true;
    }
    for (const sim::PlacedVolume& placed : volumes) {
        if (sim::placed_solid_at(placed, tile_x, tile_y)) {
            return true;
        }
    }
    return false;
}

} // namespace protekt::game
