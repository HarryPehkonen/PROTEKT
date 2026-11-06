#pragma once

#include <span>

#include "protekt/game/pick.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/sim.hpp"

/// WHAT SPACE REPAIRS: the damaged friendly the crosshair is on, or near enough to count, for a
/// selection that holds a service truck (Harri, 2026-11-05: "the same logic that's used to attack
/// an enemy can be used to repair a friendly").
///
/// The attack's mirror, and deliberately NOT a second aim: it takes the pick for what the crosshair
/// is exactly on and `friendly_nearest_ray` -- the attack offer's own scan with the side turned
/// round -- for what it is near, inside the same `kAttackAimToleranceDegrees`. One function answers
/// for the order (`Interaction`), the menu's `SPACE Repair` row and the green line and dot the
/// scene draws, so what the HUD offers and what SPACE does can never be two different units.
namespace protekt::game {

/// The friendly unit SPACE would send a truck to mend, or an invalid id when it would send none.
///
///  * `selected` must hold a service truck that can take the order: a living one of the player's
///    that is not `driven`.
///  * A unit the crosshair is exactly on is the answer if it is the player's own and HURT (below
///    its kind's full hit points); if it is whole, or hostile, nothing is offered -- not whatever
///    stands behind it. Otherwise the player's own unit whose middle lies nearest the ray within
///    `kAttackAimToleranceDegrees`, offered under the same rule. Ties go to the lower id.
///  * The patient is never the driven hull, and never the only truck there is to send: a truck
///    cannot mend itself.
/// Needs `options.aim`: with no crosshair there is nothing to point with.
[[nodiscard]] sim::UnitId repair_target(const sim::Sim& sim, const SceneOptions& options,
                                        std::span<const sim::UnitId> selected,
                                        sim::UnitId driven) noexcept;

} // namespace protekt::game
