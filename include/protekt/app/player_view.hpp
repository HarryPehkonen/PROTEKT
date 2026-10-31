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

/// HOW FAR THE EYE SITS ABOVE THE BARREL'S LINE, in metres, at the first camera position (on the
/// barrel). HARRI'S TO TUNE, and he will judge it by eye. 1.0 is HIS ask (2026-10-30: "maybe 1m
/// above", "as long as I can see a certain amount of the end of the barrel"). 0 puts the eye
/// exactly on the barrel's axis, so the view and the shot share one line; each extra metre drops
/// the muzzle tip further down the screen (atan(lift / 2 m) at level pitch: 26.6 degrees for 1 m),
/// and past about 1.15 m, where that angle reaches the 30 degree half field of view, it leaves the
/// frame. So 1.0 is just inside that edge: the tip is in frame at level pitch (looking down brings
/// it up, looking up pushes it out), which is the "certain amount of the end of the barrel" he
/// wants, and it is as high as the barrel can be seen from. MEASURED (2026-10-30): 0 is unusable --
/// the eye sits inside the turret's own box (0.8 to 1.2 m over the hull) and, since the renderer
/// culls no back faces, the whole frame is the turret's interior; about 0.2 m clears the roof
/// panel, and 0.4 m, the old default, showed the barrel as a spur at the bottom of the frame. A
/// lift is the only knob: the height itself is derived (`kEyeAboveHullMetres`), never typed, so it
/// cannot split from the barrel.
inline constexpr float kEyeAboveBarrelMetres = 1.0F;

/// The eye's height above the hull's base at the first camera position: the tank barrel's own axis
/// (`game::kTankBarrelAxisMetres`) plus the lift. The one place the eye's height is composed from
/// named parts; the chase positions are built from the same two.
inline constexpr float kEyeAboveHullMetres = game::kTankBarrelAxisMetres + kEyeAboveBarrelMetres;

/// THE SECOND POSITION, a chase view. PROVISIONAL, and Harri's to set. Behind the hull's centre by
/// twice the muzzle's reach (`game::kTankBarrelTip`, 2 m): 4 m is clear of the hull's rear with
/// room to read it, and the barrel's tip then stands 6 m ahead of the eye, so the whole barrel is
/// in front of the camera. At the first position's height, so only the setback changes.
inline constexpr float kChaseBehindMetres = 2.0F * game::kTankBarrelTip;

/// THE THIRD POSITION, a higher chase view. PROVISIONAL, and Harri's to set. Twice the chase's
/// setback (8 m) and `kHighChaseLiftMetres` above the barrel's axis. The pitch is still the
/// turret's, so the lift cannot be much more than the setback allows: the hull must land inside the
/// frame at the session's starting pitch of about -7 degrees (atan(3.4 m / 8 m) = 23 degrees below
/// the horizontal puts it 16 below the view axis, inside the 30 degree half field of view).
inline constexpr float kHighChaseBehindMetres = 2.0F * kChaseBehindMetres;
inline constexpr float kHighChaseLiftMetres = 3.0F;

/// Where the eye is for one camera position: how far BEHIND the hull's centre along the view's
/// yaw (the eye looks where the turret points, never back at the hull), and how high above the
/// hull's base.
struct EyePlacement {
    float behind_metres{};
    float above_hull_metres{};
};

/// The placement for each position, from the constants above and nothing typed here.
[[nodiscard]] EyePlacement eye_placement(game::CameraPosition position) noexcept;

/// The clearance the eye keeps over the ground beneath IT, so a chase eye behind a rise looks over
/// it instead of through it. 0.5 m: well clear of the terrain mesh's bilinear surface at the
/// camera's near plane. The barrel position never reaches it (its eye is 2 m over the hull).
inline constexpr float kEyeGroundClearanceMetres = 0.5F;

/// The player's eye, at the session's camera position (`Session::camera_position`): the
/// placement's height above the hull under the driven tank, and its setback behind the tank along
/// the view, looking where the turret is pointed (the view, not the hull's heading) from all three.
/// The first position is on the barrel's own line. The camera rides the hull: once the tank hovers,
/// the eye is that far above the HULL. Before the hover has a height the hull is at rest.
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
