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

/// Whether a volume of `volumes` is solid on world tile (`tile_x`, `tile_y`) AND refuses a unit on
/// the ground there (`sim::volume_stops` at lift zero, the mover's own question). G16: asked for
/// the TALLEST vehicle (`sim::tallest_body`), so the hatch means "some vehicle cannot drive here":
/// a slab a bike passes under and a tank does not is hatched, because a hatch left off it would be
/// an invisible wall to the tank. Pure in its arguments: the map tool asks it without a world
/// installed.
[[nodiscard]] bool volume_stops_grounded(std::span<const sim::PlacedVolume> volumes, int tile_x,
                                         int tile_y) noexcept;

/// Whether world tile (`tile_x`, `tile_y`)'s GROUND is drawn blocked: solid ground, or a volume
/// that refuses a unit on the ground there (`volume_stops_grounded`). The hatch on the ground means
/// "you cannot drive here". A volume that does NOT refuse a grounded unit -- a slab whose base is
/// above a tank's head, a lip lower than its hull -- is not hatched on the ground: it is drawn as
/// its own shape (`kVolumeColour`, every volume is), and a slab overhead shades the ground under it
/// (`ground_overhead`) instead. (Before G15 every footprint was hatched, so a slab a tank drove
/// under was painted as a wall under it.)
[[nodiscard]] bool ground_blocked(std::span<const sim::PlacedVolume> volumes, int tile_x,
                                  int tile_y) noexcept;

/// THE SHAPE ITSELF (G15): every tile of every terrain volume is drawn as a lit column, from the
/// volume's base to its top there (`append_volumes` in scene.cpp), so a digit mask's V is seen as a
/// V and a floating volume floats, with air under it. Its faces wear the SAME red hatch as blocked
/// ground (`kBlockedGroundTile`) -- one look for "this stops something" -- and the stripes and the
/// light on each face show its edges, which a flat colour did not (a 6-tile wall read as one red
/// card from the driver's seat). This is the vertex colour the hatch is multiplied by: a hair off
/// white, so the hatch keeps its tones, and an exact value nothing else in a frame carries, which
/// is how a test tells a volume's faces from the hatched GROUND quads.
inline constexpr std::array<float, 3> kVolumeColour{1.0F, 0.97F, 0.97F};

/// How much of its colour the ground keeps under a volume it can be driven under: the SHADE that
/// tells a driver "something is over this tile, and it is not a wall".
inline constexpr float kOverheadShade = 0.55F;

/// Whether world tile (`tile_x`, `tile_y`) is under a volume that a unit on the ground passes:
/// some volume is solid there, and the ground is not blocked (`ground_blocked`).
[[nodiscard]] bool ground_overhead(std::span<const sim::PlacedVolume> volumes, int tile_x,
                                   int tile_y) noexcept;

} // namespace protekt::game
