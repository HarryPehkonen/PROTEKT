#pragma once

#include <optional>
#include <span>

#include "protekt/game/pick.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/sim.hpp"

/// WHAT SPACE SENDS YOU TO: the ground the crosshair is on, for a selection that can go there, or
/// nothing at all (Harri, 2026-11-03: "I should also be able to select any vehicle or group of
/// vehicles under a function key, point at the ground, and say SPACE Go Here").
///
/// One function answers for the order (`Interaction`) and for the menu's `SPACE Go Here` row, the
/// same arrangement as `deploy_offer` and `attack_target`, so the prompt and the key can never be
/// two different places.
namespace protekt::game {

/// The ground tile SPACE would send the selection to.
struct GoHereOffer {
    int tile_x{};
    int tile_y{};
};

/// The offer for `selected`, or nothing.
///
/// At least one selected unit is the player's, living, not `driven`, and able to move (a building,
/// or a deployed scavenger, which takes no order, cannot), and the crosshair is on GROUND -- the
/// pick found a tile and no unit. A unit under the crosshair is not ground: it is an attack or a
/// follow, which SPACE already does. Withheld wherever `deploy_offer` has an answer -- a lone
/// scavenger at a pool, turrets on open ground -- because the deploy owns SPACE there; the attack
/// is asked before both by the caller. Needs `options.aim`.
[[nodiscard]] std::optional<GoHereOffer> go_here_offer(const sim::Sim& sim,
                                                       const SceneOptions& options,
                                                       std::span<const sim::UnitId> selected,
                                                       sim::UnitId driven) noexcept;

} // namespace protekt::game
