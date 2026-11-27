#include "protekt/game/blocked_ground.hpp"

#include "protekt/world/ground.hpp"

namespace protekt::game {

bool volume_stops_grounded(std::span<const sim::PlacedVolume> volumes, int tile_x,
                           int tile_y) noexcept {
    for (const sim::PlacedVolume& placed : volumes) {
        if (sim::placed_solid_at(placed, tile_x, tile_y)
            && sim::volume_stops(placed.volume, sim::Fixed::zero(), sim::tallest_body())) {
            return true;
        }
    }
    return false;
}

bool ground_blocked(std::span<const sim::PlacedVolume> volumes, int tile_x, int tile_y) noexcept {
    return world::ground_solid(tile_x, tile_y) || volume_stops_grounded(volumes, tile_x, tile_y);
}

bool ground_overhead(std::span<const sim::PlacedVolume> volumes, int tile_x, int tile_y) noexcept {
    if (ground_blocked(volumes, tile_x, tile_y)) {
        return false;
    }
    for (const sim::PlacedVolume& placed : volumes) {
        if (sim::placed_solid_at(placed, tile_x, tile_y)) {
            return true;
        }
    }
    return false;
}

} // namespace protekt::game
