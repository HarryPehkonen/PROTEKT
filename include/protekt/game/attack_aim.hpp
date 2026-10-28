#pragma once

#include <span>

#include "protekt/game/pick.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/sim.hpp"

/// WHAT SPACE ATTACKS: the hostile asset the crosshair is on, or near enough to count.
///
/// One function answers for the order (`Interaction`), the menu's `SPACE Attack` row and the red
/// line, label and bar the scene draws, so the thing the HUD offers and the thing SPACE does can
/// never be two different units.
namespace protekt::game {

/// "Pointing APPROXIMATELY at it": the angle, in degrees, between the crosshair's ray and the
/// direction to the asset's middle within which it is still offered. A few degrees -- wide enough
/// that a player does not have to land the dot on a small tank, narrow enough that the offer still
/// means "that one" and not "something over there".
inline constexpr float kAttackAimToleranceDegrees = 4.0F;

/// Where on screen an asset's middle is (its pick box's half height above its base): the point the
/// red line ends on. `depth` <= 0 is behind the eye, and then there is nothing to draw.
[[nodiscard]] Projected attack_point(const SceneOptions& options, const sim::Unit& unit) noexcept;

/// The hostile asset SPACE would attack, or an invalid id when it would attack nothing.
///
///  * `selected` must hold something ARMED that can take the order -- a living unit of the player's
///    that is not `driven` and whose kind has a weapon by the sim's own table
///    (`spec_of(kind).range` above zero: a tank, a turret, a scout; not a scavenger or a recycler).
///  * A unit the crosshair is exactly on is the answer if it is hostile; if it is the player's own,
///    nothing is offered (a friendly in the way is not a target, and not a hostile behind it).
///  * Otherwise the living hostile whose middle lies nearest the ray, within
///    `kAttackAimToleranceDegrees`. Ties go to the lower id.
/// Needs `options.aim`: with no crosshair there is nothing to point with.
[[nodiscard]] sim::UnitId attack_target(const sim::Sim& sim, const SceneOptions& options,
                                        std::span<const sim::UnitId> selected,
                                        sim::UnitId driven) noexcept;

} // namespace protekt::game
