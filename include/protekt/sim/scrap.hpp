#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "protekt/sim/fixed.hpp"
#include "protekt/sim/units.hpp"

namespace protekt::sim {

/// A piece of wreckage on the ground: where a destroyed unit's hull came apart. A LIGHTWEIGHT
/// ENTITY, not a `Unit` -- it has no id, no side, no hp and takes no orders, so the roster, the
/// commander and the AI cannot see it and no unit's decision path changes when the ground is
/// littered. A scavenger walks to it and carries it home (C2); the litter itself is C1.
struct Scrap {
    Fixed x{};
    Fixed y{};
};

/// How many pieces a wreck leaves, and how far out they land (whole tiles, each piece at the
/// CENTRE of the tile it lands on). Four pieces inside two tiles reads as "a tank died here"
/// rather than as a carpet of debris.
inline constexpr int kScrapPerWreck = 4;
inline constexpr int kScrapScatterRadius = 2;
/// The side of the scatter square, in tiles: the wreck's own tile plus `kScrapScatterRadius` each
/// way. The span is what the settle step walks while a piece of one wreck already holds a tile.
inline constexpr int kScrapSpan = (2 * kScrapScatterRadius) + 1;

/// The ground stops collecting litter here. Four pieces a wreck is 128 wrecks' worth, so a war that
/// never ends leaves a bounded amount of state behind it; the cap is a dial, not a promise.
inline constexpr std::size_t kMaxScrap = 512;

/// Scatters `kScrapPerWreck` pieces around (x, y): the wreck's own tile plus a whole-tile offset
/// each way, appended to `out` in a fixed order, stopping at `kMaxScrap`.
///
/// WHERE each piece lands is a pure function of (seed, the dead unit's id, the piece's index) --
/// the same arithmetic-hash trick the camo paint uses -- so a replay scatters the same field
/// without the sim holding an RNG, and the same battle always produces the same litter in the same
/// places.
void scatter(std::vector<Scrap>& out, std::uint64_t seed, UnitId id, Fixed x, Fixed y) noexcept;

} // namespace protekt::sim