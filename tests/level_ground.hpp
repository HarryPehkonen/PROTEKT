#pragma once

#include "protekt/world/ground.hpp"
#include "protekt/world/world.hpp"

#include <cstddef>

namespace protekt::testing {

/// LEVEL GROUND (G7): the whole 256 x 256 map drawn Flat at height 0, for a test whose subject is
/// not the slope. Since G7 a unit nobody drives ROLLS on ground steeper than friction holds (25.6
/// a tile) -- 11 to 15 percent of seeds 1, 7 and 42 -- so a fixture that stood an idle unit on a
/// hillside and asserted where it stood, or how a fight on it went, was asserting the hill.
/// Installed with `world::ScopedGround`, so it is cleared when the test ends.
[[nodiscard]] inline world::MapGround level_ground() {
    constexpr int kSide = 256;
    world::MapGround ground;
    ground.width = kSide;
    ground.height = kSide;
    ground.tiles.assign(static_cast<std::size_t>(kSide) * kSide,
                        world::Tile{0, world::Terrain::Flat});
    ground.solid.assign(static_cast<std::size_t>(kSide) * kSide, 0);
    return ground;
}

} // namespace protekt::testing
