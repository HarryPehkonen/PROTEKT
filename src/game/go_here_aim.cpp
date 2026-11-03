#include "protekt/game/go_here_aim.hpp"

#include <algorithm>

#include "protekt/game/deploy_aim.hpp"

namespace protekt::game {

std::optional<GoHereOffer> go_here_offer(const sim::Sim& sim, const SceneOptions& options,
                                         std::span<const sim::UnitId> selected,
                                         sim::UnitId driven) noexcept {
    if (!options.aim.has_value()) {
        return std::nullopt;
    }
    // Who could take the order: the player's living units but the one being driven, that move.
    const bool someone_moves
        = std::any_of(selected.begin(), selected.end(), [&sim, driven](sim::UnitId id) {
              const sim::Unit* unit = sim.find(id);
              return unit != nullptr && unit->alive && unit->side == sim::Side::Player
                     && id != driven && sim::is_drivable(unit->kind);
          });
    if (!someone_moves) {
        return std::nullopt;
    }
    // The deploy owns SPACE where it is offered: a pool for a lone scavenger, bare ground for
    // turrets. Asked here, not left to the caller, so the two prompts can never both be true.
    if (deploy_offer(sim, options, selected, driven).has_value()) {
        return std::nullopt;
    }
    const Pick picked = pick(sim, options, options.aim->x, options.aim->y);
    if (picked.has_unit() || !picked.has_tile()) {
        return std::nullopt;
    }
    return GoHereOffer{picked.tile_x, picked.tile_y};
}

} // namespace protekt::game
