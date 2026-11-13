#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "protekt/game/interaction.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/sim.hpp"

/// WHAT SPACE DEPLOYS: the place the crosshair is on, for the selection that can deploy there, or
/// nothing at all.
///
/// One function answers for the order (`Interaction`) and for the menu's `SPACE Deploy` row, so the
/// prompt the player is offered and the thing SPACE does can never be two different places -- the
/// same arrangement as `attack_target`, and for the same reason. The prompt exists ONLY in the
/// context that makes it true (Harri, 2026-11-01): it is a row the menu computes from this answer,
/// never one of the permanent rows.
namespace protekt::game {

/// What SPACE would deploy onto.
enum class DeploySite : std::uint8_t {
    Pool, ///< a biometal pool under the pointing cone: ONE selected scavenger drives there and
          ///< deploys
    Spot, ///< bare ground under the crosshair: the selected turrets drive there and deploy
};

struct DeployOffer {
    DeploySite site{DeploySite::Spot};
    int tile_x{};
    int tile_y{};
};

/// The offer for `selected`, or nothing.
///
///  * A SCAVENGER: exactly ONE unit is selected, it is a living scavenger of the player's that is
///    not `driven`, and a pool the sim would still accept (`sim::pool_taken` is false) lies within
///    `sim::kPointingConeDegrees` of the crosshair's ray -- the cone the AI's deploy points with
///    and the player's assist takes, pinned equal to it (`kAssistConeDegrees`); there is no second
///    arc -- and within the radar's reach of the viewer, measured by the radar's own number
///    (`viewer_radar_radius`). A GROUP is never offered it, and neither is a scavenger with no pool
///    under the cone. When two pools are in the cone the one nearest the ray wins, ties to the
///    first listed.
///  * TURRETS: every selected unit that can be ordered (the player's, living, not `driven`) is a
///    turret, plain or deployed, there is at least one, and the crosshair is on bare ground the
///    sim would let a turret stand on. A unit under the crosshair is not bare ground: it is a
///    follow or an attack.
/// Needs `options.aim`: with no crosshair there is nothing to point with.
[[nodiscard]] std::optional<DeployOffer> deploy_offer(const sim::Sim& sim,
                                                      const SceneOptions& options,
                                                      std::span<const sim::UnitId> selected,
                                                      sim::UnitId driven) noexcept;

/// POWER MUST REACH IT (K4). A building that draws power (`draws_power`) is connected on a spot
/// only when that spot touches a POWERED building of the player's (`site_connected`); every other
/// kind is always connected. The preview's colour and SPACE's refusal both ask this.
[[nodiscard]] bool placement_connected(const sim::Sim& sim, const Placing& placing, int tile_x,
                                       int tile_y);
inline constexpr std::string_view kRefusalNotConnected
    = "Needs power: nothing powered touches that spot.";

/// The preview an armed building (`Interaction::placing`) draws at the tile the reticle meets: its
/// footprint as turned, and `valid` -- `sim::place_ok`'s own answer, so the colour is a claim about
/// what SPACE will do. The same function the app draws from and the tests read.
[[nodiscard]] DeployPreview placement_preview(const sim::Sim& sim, const Placing& placing,
                                              int tile_x, int tile_y) noexcept;

} // namespace protekt::game
