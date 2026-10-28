#pragma once

#include <cstddef>
#include <vector>

#include "protekt/sim/sim.hpp"

namespace protekt::game {

struct SceneOptions;

/// How many targets the list holds: keys 1-9 for the nearest nine, 0 for the tenth.
inline constexpr std::size_t kAttackListRows = 10;

/// One row of the attack list: the key that names it, what it is, how far, and which unit.
struct TargetRow {
    int key{}; ///< 1-9, or 0 for the tenth
    sim::UnitKind kind{};
    sim::UnitId id{};
    int metres{}; ///< whole metres, tiles * kMetresPerTile, rounded half up
};

/// The nearest living hostiles of `from`'s side, within `radius_tiles` of `centre`, nearest first,
/// ties to the lower id, at most `kAttackListRows`. EMPTY when `centre` is not a living unit of
/// `from` -- there is nothing to measure from, so there is no list. Pure: same sim, same list.
///
/// With `view` given, a hostile is listed only if `available_to_viewer` under it (VIS): the list
/// never names what the world view does not draw. Null leaves the radius as the only bound.
[[nodiscard]] std::vector<TargetRow> nearest_enemies(const sim::Sim& sim, sim::Side from,
                                                     sim::UnitId centre, int radius_tiles,
                                                     const SceneOptions* view = nullptr);

} // namespace protekt::game
