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
/// A mask is `kIconCells` rows of `kIconCells` characters, '#' lit and '.' clear, the same
/// bit-per-cell idea as the HUD's 5x5 letter masks only bigger and written as the picture it is, so
/// the art can be read and edited in the source. It is 16 because the box the layout reserves
/// under a plate is 16 px square at 720 and one cell is one pixel there; vehicles face right and
/// stand on the bottom row.
inline constexpr std::size_t kIconCells = 16;

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
