#pragma once

#include <cstdint>

#include "protekt/world/world.hpp"

/// Which way to point a tank that has just appeared.
///
/// A tank facing a crater wall presses W and does not move, and the simulation is right to refuse
/// the wall -- so choosing where to look is the GAME's job, not the simulation's. This is a pure
/// function of the terrain, so it is deterministic, testable without a display, and moves no golden
/// values: heading is presentation state, not simulation state.
namespace protekt::game {

/// Directions tried, evenly spaced. 64 is fine enough that the best of them is within three degrees
/// of optimal and coarse enough to stay a few microseconds.
inline constexpr int kHeadingsToTry = 64;
/// How far ahead a heading is judged. Long enough to leave the spawn crater field, short enough
/// that the check stays local.
inline constexpr int kClearRunTiles = 16;

/// How many tiles of passable ground lie ahead of `(tile_x, tile_y)` along `heading`, counting from
/// the unit's own tile and stopping at the first crater, the map edge, or `max_tiles`.
[[nodiscard]] int clear_run(std::uint64_t seed, int tile_x, int tile_y, float heading,
                            int max_tiles = kClearRunTiles) noexcept;

/// The heading whose clear run is longest, out of `kHeadingsToTry` evenly spaced directions. Ties
/// go to the lowest heading, so the choice does not depend on the order anything is evaluated in.
[[nodiscard]] float clearest_heading(std::uint64_t seed, int tile_x, int tile_y) noexcept;

} // namespace protekt::game
