#pragma once

#include <array>
#include <optional>

#include "protekt/game/scene.hpp"
#include "protekt/game/weapon.hpp"
#include "protekt/sim/sim.hpp"

/// ASSIST-TARGETTING (`T`): a hint, never a hand on the barrel.
///
/// `T` latches the hostile nearest the crosshair, and the HUD then draws a thin red line from that
/// asset to a small red dot: the dot is where the BARREL has to point for the weapon that is up to
/// hit the asset if it keeps doing what it is doing now. The player aims at the dot and pulls the
/// trigger. The shot is still `basis_from(yaw, pitch).forward`, exactly as it was -- nothing here
/// reaches the sim, an order or a hash, which is what `ManualFire.DoesNotLeadOrAimAtAMovingEnemy`,
/// `ManualFire.ATargetUnderTheMarkDoesNotBendTheShell` and
/// `AiAim.ThePlayersManualShotNeitherLeadsNorLearns` keep true.
///
/// The lead is NOT the AI's `Unit::aim_x`: that is a per-gun belief learned from the gun's own
/// misses, and the player's gun never learns (it is zero, and pinned zero). It is the target's
/// velocity over the shot's flight time, and the flight time depends on the lead, so the solve
/// iterates it. A target that changes course or speed after the dot is drawn is missed, as it is
/// for the AI.
namespace protekt::game {

/// "A few degrees": the angle between the crosshair's ray and the direction to an asset's middle
/// within which `T` will take it. The reticle's ticks reach 1.8 degrees from the centre (its
/// half-size is 0.055 of the half-screen at the 30 degree half-FOV), so 4 degrees is a little over
/// twice the cross's own reach: the player does not have to land the cross on a small tank, yet
/// still means "that one" and not "something over there". The angle is taken in 3D, to the asset's
/// middle, so an asset on a ridge or in a crater is as assistable as one on level ground.
inline constexpr float kAssistConeDegrees = 4.0F;

/// The iteration is bounded: each pass moves the aim point by the target's velocity times the new
/// flight time, which shrinks the change by about the ratio of the target's closing speed to the
/// shot's speed (a full-speed tank against the slowest weapon here is about 0.4), so the error
/// from a 30 tick start is under a tenth of a tick in about eight passes. Sixteen is twice that.
/// A solve that has not settled by then is not drawn: a target as fast as the shot has no lead.
inline constexpr int kAssistMaxPasses = 16;
/// Settled: the flight time moved by less than this many ticks between two passes (the target
/// moves under 0.02 tile in that time, a twenty-fifth of the 0.5 tile hit radius).
inline constexpr double kAssistSettleTicks = 0.1;

/// The line's and the dot's ink: red, and its own value so the line is its own thing to find in a
/// frame and to test (it is neither the reticle's hostile red nor the attack offer's line).
inline constexpr std::array<float, 3> kAssistInk{1.0F, 0.15F, 0.15F};
/// The dot is a fan of this many triangles round its centre.
inline constexpr int kAssistDotSlices = 12;

/// The hostile asset `T` would take: the living, available hostile whose middle lies nearest the
/// crosshair's ray within `kAssistConeDegrees`, ties to the lower id. Invalid with no crosshair or
/// nothing in the cone. Pure in `(sim, options)`.
[[nodiscard]] sim::UnitId assist_pick(const sim::Sim& sim, const SceneOptions& options) noexcept;

/// Where to aim for `weapon` fired from `shooter` to hit `target` if it holds its course.
struct AssistShot {
    /// Where the target's middle will be when the shot arrives, world metres (x, up, z).
    std::array<float, 3> aim_metres{};
    /// The barrel direction that gets there, a unit vector in the same axes as
    /// `basis_from(yaw, pitch).forward`: put the view's forward on this and the shot goes there.
    std::array<float, 3> barrel{};
    /// The red dot: along `barrel` from the eye, as far as the aim point is. The crosshair on the
    /// dot IS the view's forward along `barrel`.
    std::array<float, 3> dot_metres{};
    float flight_ticks{}; ///< how long the shot flies
    int passes{};         ///< how many passes the iteration took to settle
};

/// The solve. A flat weapon (gun, rocket, shotgun) flies its spec's bolt speed along a straight
/// line, plus the shooter's own velocity as the sim adds it; the mortar and the bouncer (whose
/// first landing is the mortar's) fly the mortar's fixed muzzle speed on its parabola and the LOW
/// arc is the one shown, because the high one is a pitch of 60 degrees or more with the dot far
/// above the target. The eye is `options.camera`. Empty when no solution is drawn: the aim point
/// is beyond the weapon's travel, the mortar cannot reach it at any legal elevation, or the
/// iteration did not settle in `kAssistMaxPasses`.
[[nodiscard]] std::optional<AssistShot> assist_shot(const SceneOptions& options, Weapon weapon,
                                                    const sim::Unit& shooter,
                                                    const sim::Unit& target) noexcept;

} // namespace protekt::game
