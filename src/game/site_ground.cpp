#include "protekt/game/site_ground.hpp"

namespace protekt::game {

void SiteGroundMap::add(SiteKind kind, int anchor_x, int anchor_y) {
    const TileRange range = site_range(kind, anchor_x, anchor_y);
    for (int y = range.y0; y <= range.y1; ++y) {
        for (int x = range.x0; x <= range.x1; ++x) {
            const auto [at, inserted] = tiles_.try_emplace({x, y}, kind);
            if (!inserted && at->second < kind) {
                at->second = kind;
            }
        }
    }
}

std::optional<SiteKind> SiteGroundMap::at(int tile_x, int tile_y) const {
    const auto found = tiles_.find({tile_x, tile_y});
    if (found == tiles_.end()) {
        return std::nullopt;
    }
    return found->second;
}

} // namespace protekt::game
