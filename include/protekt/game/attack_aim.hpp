#pragma once

#include <array>
#include <optional>
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

/// The living, available hostile whose middle lies nearest the crosshair's ray within `degrees` of
/// it (the angle in 3D, to the middle), ties to the lower id; invalid when there is none or no
/// crosshair. The scan `attack_target` falls back on and `assist_pick` is, so the two offers
/// measure "near the crosshair" the same way.
[[nodiscard]] sim::UnitId hostile_nearest_ray(const sim::Sim& sim, const SceneOptions& options,
                                              float degrees) noexcept;

/// The same scan over ANY living, available asset but the driven hull, of either side: the unit
/// the crosshair is "pointing approximately at" for the health bars. It is `hostile_nearest_ray`
/// with the side filter off and nothing else, so the two can never measure the cone differently.
[[nodiscard]] sim::UnitId asset_nearest_ray(const sim::Sim& sim, const SceneOptions& options,
                                            float degrees) noexcept;

/// The angle, in degrees, between the crosshair's ray and the direction from the eye to a world
/// point (x, up, z in metres): the one measure of "near the crosshair" that the attack offer, the
/// assist, the health bars and the deploy offer all take, so none of them can disagree about it.
/// Nothing when there is no crosshair, or the point is at the eye. A point behind the eye is a
/// large angle, not an error.
[[nodiscard]] std::optional<float> degrees_off_ray(const SceneOptions& options,
                                                   const std::array<float, 3>& point) noexcept;

/// The metres from the eye to an asset's middle (the same point `degrees_off_ray` and the red line
/// use): the number the pointed-at readout prints under the unit.
[[nodiscard]] float distance_metres(const SceneOptions& options, const sim::Unit& unit) noexcept;

/// "POINTING AT IT", the one answer the health bars, the name and distance readout and `T`'s lock
/// all take: the asset the crosshair is exactly on (`aimed`, the frame's `pick`), else the nearest
/// any-side asset within `kAssistConeDegrees` of the ray (`asset_nearest_ray`). The driven hull is
/// never it. Invalid when there is none or no crosshair.
[[nodiscard]] sim::UnitId pointed_asset(const sim::Sim& sim, const SceneOptions& options,
                                        const Pick& aimed) noexcept;

/// The hostile asset SPACE would attack, or an invalid id when it would attack nothing.
///
///  * `selected` must hold something ARMED that can take the order -- a living unit of the player's
///    that is not `driven` and whose kind has a weapon by the sim's own table
///    (`spec_of(kind).range()` above zero: a tank, a turret, a scout; not a scavenger or a
///    recycler).
///  * A unit the crosshair is exactly on is the answer if it is hostile; if it is the player's own,
///    nothing is offered (a friendly in the way is not a target, and not a hostile behind it).
///  * Otherwise the living hostile whose middle lies nearest the ray, within
///    `kAttackAimToleranceDegrees`. Ties go to the lower id.
/// Needs `options.aim`: with no crosshair there is nothing to point with.
[[nodiscard]] sim::UnitId attack_target(const sim::Sim& sim, const SceneOptions& options,
                                        std::span<const sim::UnitId> selected,
                                        sim::UnitId driven) noexcept;

} // namespace protekt::game
