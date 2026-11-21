#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "protekt/world/world.hpp"

namespace protekt::world {

/// A DESIGNED GROUND: a rectangle of tiles that REPLACES the seed's world wherever it lies, with
/// its own solid mask. It is how a map file becomes the ground a battle is played on.
///
/// The rectangle sits at the world's origin: tile (x, y) of the file is world tile (x, y). Outside
/// it the ground is a Crater at height 0 -- the file's edge is the world's edge, impassable, so no
/// unit ever drives onto ground the file did not draw. Inside it the file wins entirely; the seed
/// fills nothing there.
struct MapGround {
    int width{};
    int height{};
    std::vector<Tile> tiles{};         ///< width * height, row-major
    std::vector<std::uint8_t> solid{}; ///< width * height, 1 = impassable whatever the terrain
};

/// Installs `ground` as the world. While one is installed `sample` and `generate` answer from it
/// (`world::sample` is then a pure function of (installed ground, x, y) and the seed is ignored
/// for tiles). With none installed -- the default, and every battle that does not name a map --
/// nothing here is consulted and the world is exactly the seed's.
///
/// It is process-wide state, so it is installed ONCE, before a battle is built and before any
/// thread reads the world, and removed after; use `ScopedGround`.
void install_ground(MapGround ground);
void clear_ground() noexcept;
[[nodiscard]] bool ground_installed() noexcept;

/// Counts every install and every clear. A memo of the world keyed by seed alone (the world map's
/// terrain grid) adds this to its key, so a ground installed after the memo was filled is not
/// answered from the seed's old tiles.
[[nodiscard]] std::uint64_t ground_epoch() noexcept;

/// True when the installed ground marks the tile solid. False with no ground installed.
[[nodiscard]] bool ground_solid(int world_x, int world_y) noexcept;

/// Installs on construction, clears on destruction.
class ScopedGround {
public:
    explicit ScopedGround(MapGround ground) { install_ground(std::move(ground)); }
    ~ScopedGround() { clear_ground(); }
    ScopedGround(const ScopedGround&) = delete;
    ScopedGround& operator=(const ScopedGround&) = delete;
    ScopedGround(ScopedGround&&) = delete;
    ScopedGround& operator=(ScopedGround&&) = delete;
};

namespace detail {
/// The installed ground's answer for a tile, or false when none is installed (`tile` untouched).
[[nodiscard]] bool ground_tile(int world_x, int world_y, Tile& tile) noexcept;
} // namespace detail

} // namespace protekt::world
