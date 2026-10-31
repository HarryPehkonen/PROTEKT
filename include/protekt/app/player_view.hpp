#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "protekt/app/audio_pipeline.hpp"
#include "protekt/game/menu.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/session.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::app {

/// What the player's eyes, radar and ears are attached to. Every function here reads
/// `Session::driven()` when it is CALLED, so an X swap moves the camera, the radar and the
/// listener with the throttle. Nobody keeps a copy of the driven id: that was the bug.

/// Whether `id` names a living unit; a unit that is not in the sim is not alive.
[[nodiscard]] bool is_alive(const sim::Sim& sim, sim::UnitId id);

/// HOW FAR THE EYE SITS ABOVE THE BARREL'S LINE, in metres. HARRI'S TO TUNE, and he will judge it
/// by eye: "as long as I can see a certain amount of the end of the barrel". 0 puts the eye exactly
/// on the barrel's axis, so the view and the shot share one line and the muzzle tip sits on the
/// view axis (the crosshair); each extra metre drops the tip further down the screen (atan(lift / 2
/// m) at level pitch: 27 degrees for 1 m), and past about 1.15 m, where that angle reaches the 30
/// degree half field of view, it leaves the frame. A lift is the only knob: the height
/// itself is derived (`kEyeAboveHullMetres`), never typed, so it cannot split from the barrel.
/// MEASURED (2026-10-30): 0 is NOT usable, which the first version of this shipped. At 0 the eye
/// sits inside the turret's own box (0.8 to 1.2 m over the hull) and, since the renderer culls no
/// back faces, the whole frame is the turret's interior and its front face hides the barrel. About
/// 0.2 m clears the roof panel; 0.4 m shows the barrel as a spur rising from the bottom of the
/// frame, which is the "certain amount of the end of the barrel" Harri asked to see, so 0.4 is the
/// default. He sets it: it is the one knob.
inline constexpr float kEyeAboveBarrelMetres = 0.4F;

/// The eye's height above the hull's base: the tank barrel's own axis
/// (`game::kTankBarrelAxisMetres`) plus the lift. The one place the eye's position is composed from
/// named parts.
inline constexpr float kEyeAboveHullMetres = game::kTankBarrelAxisMetres + kEyeAboveBarrelMetres;

/// The player's eye: `kEyeAboveHullMetres` above the hull under the driven tank (on the barrel's
/// own line), looking where the turret is pointed (the view, not the hull's heading). The camera
/// rides the hull: once the tank hovers, the eye is that far above the HULL. Before the hover has a
/// height the hull is at rest.
[[nodiscard]] game::Camera camera_for(const sim::Sim& sim, const game::Session& session,
                                      std::uint64_t seed, std::optional<float> hull_z);

/// The camera turned `centirad` hundredths of a radian round, and nothing else: the pitch, the eye
/// and the sim are as they were, and the yaw is wrapped. For `--look-yaw`, which aims a capture at
/// a bearing without going through the mouse (the mouse turns the hull, and the hull is the sim's).
[[nodiscard]] game::Camera camera_yawed(game::Camera camera, std::int32_t centirad);

/// The unit the radar is drawn from: the driven tank, or no unit when the radar is off.
[[nodiscard]] sim::UnitId radar_viewer(const game::Session& session, bool no_radar);

/// The audio side of a frame: the listener sits at the camera, and the player the hit flash and
/// the listening are about is the driven tank.
[[nodiscard]] AudioFrame audio_frame_for(const sim::Sim& sim, const game::Session& session,
                                         const game::Camera& camera, bool trigger_pulled,
                                         bool player_alive_before);

/// What is under the cursor, for the menu's state row: a living unit of the player's side is
/// `Selectable`, a living enemy `Hostile`, bare ground `Ground`, and no cursor, sky or the far side
/// of the horizon `None`. It asks `game::pick`; there is no second picker.
[[nodiscard]] game::MenuTarget menu_target_of(const sim::Sim& sim,
                                              const game::SceneOptions& options);

/// The hostile asset SPACE would attack this frame, or an invalid id: `game::attack_target` for the
/// session's selection and driven craft, withheld while the deploy or the attack list is armed
/// (SPACE means something else then, and the menu is not showing the offer). The menu's `SPACE
/// Attack` row and the scene's red line, label and bar are all drawn from this one answer.
[[nodiscard]] sim::UnitId attack_offer(const sim::Sim& sim, const game::Session& session,
                                       const game::SceneOptions& options);

/// The menu rows for this frame. The one command that arms is the deploy (DEPLOY): while it is
/// armed the state row reads `SPACE DEPLOY` and, with the reticle on the ground, the distance to
/// it. Empty when `attack_armed`: the attack list and the menu share one slot under the notice, and
/// one panel at a time is the rule.
[[nodiscard]] std::vector<game::MenuRow> menu_for(const sim::Sim& sim, const game::Session& session,
                                                  const game::SceneOptions& options,
                                                  bool attack_armed);

} // namespace protekt::app
