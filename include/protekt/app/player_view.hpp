#pragma once

#include <cstdint>
#include <optional>

#include "protekt/app/audio_pipeline.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/session.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::app {

/// What the player's eyes, radar and ears are attached to. Every function here reads
/// `Session::driven()` when it is CALLED, so an X swap moves the camera, the radar and the
/// listener with the throttle. Nobody keeps a copy of the driven id: that was the bug.

/// Whether `id` names a living unit; a unit that is not in the sim is not alive.
[[nodiscard]] bool is_alive(const sim::Sim& sim, sim::UnitId id);

/// The player's eye: kEyeHeight metres above the ground under the driven tank, looking where the
/// turret is pointed (the view, not the hull's heading). The camera rides the hull: once the tank
/// hovers, the eye is kEyeHeight above the HULL. Before the hover has a height the hull is at rest.
[[nodiscard]] game::Camera camera_for(const sim::Sim& sim, const game::Session& session,
                                      std::uint64_t seed, std::optional<float> hull_z);

/// The unit the radar is drawn from: the driven tank, or no unit when the radar is off.
[[nodiscard]] sim::UnitId radar_viewer(const game::Session& session, bool no_radar);

/// The audio side of a frame: the listener sits at the camera, and the player the hit flash and
/// the listening are about is the driven tank.
[[nodiscard]] AudioFrame audio_frame_for(const sim::Sim& sim, const game::Session& session,
                                         const game::Camera& camera, bool trigger_pulled,
                                         bool player_alive_before);

} // namespace protekt::app
