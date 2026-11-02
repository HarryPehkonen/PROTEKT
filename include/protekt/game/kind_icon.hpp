#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "protekt/sim/units.hpp"

namespace protekt::game {

/// The vehicle-type ICON under a command-row key (Harri, from a BZ2 crop: "an image of the type of
/// vehicle, which is turrets in this case"): a pale, plain, side-view silhouette of the kind, no
/// shading and no colour for whose it is -- the TYPE is shown, not the side.
///
/// A mask is `kIconCells` rows of `kIconCells` characters, '#' lit and '.' clear. It is 72 because
/// the box the layout reserves under a plate is 72 px square at 720 and one cell is one pixel there
/// (Harri: "four to five times the current lengths", the 16 px icons of the first cut, and his BZ2
/// capture shows icons 65-75 px wide under a ~65 px keycap). The art is RE-DRAWN for that size, not
/// the old 16 cells scaled up: each kind is a recipe of boxes, discs and polygons -- lit parts and
/// cut-outs, road wheels, hatches, barrels -- painted onto the grid once, on first use. Vehicles
/// face right and stand on the grid's baseline, row `kIconGround`.
inline constexpr std::size_t kIconCells = 72;

/// The row a vehicle's tracks or wheels rest on (the last lit row of a standing silhouette). One
/// row is left under it for the outline the plate draws round the ink, so an icon with its
/// outline fills the box to its floor, as the first cut's did.
inline constexpr std::size_t kIconGround = kIconCells - 2;

using IconMask = std::array<std::string_view, kIconCells>;

/// The mask drawn for `kind`: its own silhouette, the two buildings (the factory and the deployed
/// recycler) included, or `fallback_icon()` for a kind with none (any kind added later).
[[nodiscard]] const IconMask& kind_icon(sim::UnitKind kind) noexcept;

/// The armory's mask, Harri's "the one with a missile on each side of it". The armory is not a
/// `UnitKind` yet -- the game cannot build one -- so the numbered key `3` asks for it by position;
/// it is drawn the moment that key holds something.
[[nodiscard]] const IconMask& armory_icon() noexcept;

/// The deliberate "no art for this kind" mark: a hollow box with a dot in it.
[[nodiscard]] const IconMask& fallback_icon() noexcept;

} // namespace protekt::game
