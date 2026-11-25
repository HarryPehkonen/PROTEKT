#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "protekt/sim/volume.hpp"

namespace protekt::game {

/// NO INVISIBLE WALLS (G9; Harri, driving the Proving Ground: "There is still some invisible wall
/// in the middle of the proving-grounds map", and his rule: an impassable tile with nothing drawn
/// on it is an invisible wall). A tile the sim will not let a unit into -- SOLID GROUND (the map's
/// solid mask, `world::ground_solid`) or the footprint of a TERRAIN VOLUME (`Scenario::volumes`)
/// -- is drawn in the map tool's red hatching, in the game as in `protekt-map draw`. ONE look, two
/// consumers: `app::draw_map` paints it at `kMapDrawScale` pixels a tile, the atlas paints it into
/// the `kBlockedGroundTile` tile the scene lays on those world tiles. PRESENTATION ONLY: the sim
/// decides what stops a unit; this only asks the same data.

/// The two tones of the hatching: a bright brick red and a near-black red, the map tool's colours.
/// Chosen for a driver with weak eyesight: no ground the game paints is red, and the pair is the
/// highest-contrast stripe the map tool already taught him, so it reads at a glance.
inline constexpr std::array<std::uint8_t, 3> kBlockedHatchBright{170, 34, 34};
inline constexpr std::array<std::uint8_t, 3> kBlockedHatchDark{64, 14, 14};

/// Diagonal stripes: pixel (`x`, `y`) of a tile drawn `side` pixels across is bright when
/// (x + y) mod (side / 2) falls in the first half -- two bright and two dark bands to a tile, the
/// map tool's 8-pixel tile (`(x + y) % 4 < 2`) exactly, and the same proportion on a 64-texel atlas
/// tile, where the bands continue from one tile into the next.
[[nodiscard]] constexpr bool blocked_hatch_bright(int x, int y, int side) noexcept {
    const int period = side / 2;
    return (x + y) % period < period / 2;
}

/// The atlas tile the scene paints a blocked world tile with.
inline constexpr std::string_view kBlockedGroundTile = "ground_blocked";

/// Whether world tile (`tile_x`, `tile_y`) is drawn blocked: solid ground, or inside the footprint
/// of any of `volumes`. Every volume is drawn, as the map tool draws every one: a volume is set
/// down to stop something, and a volume a hover could clear still stops the tanks beside it.
[[nodiscard]] bool ground_blocked(std::span<const sim::PlacedVolume> volumes, int tile_x,
                                  int tile_y) noexcept;

} // namespace protekt::game
