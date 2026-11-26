#include "protekt/game/kick.hpp"

#include <algorithm>
#include <cstddef>

namespace protekt::game {

namespace {

/// The highest the heave may take the view: short of straight up, where the camera's frame turns
/// over. The player's own pitch is limited well below this (`kLookPitchLimit`).
constexpr float kHeaveCeiling = 1.5F;

[[nodiscard]] const KickLook& look_of(sim::Kick kick) noexcept {
    return kKickLooks.at(static_cast<std::size_t>(kick));
}

} // namespace

FireCue cue_of(const sim::Sim::Shot& shot) noexcept {
    return FireCue{shot.shooter, shot.weapon, sim::weapon_spec(shot.weapon).kick,
                   fire_sound(shot.weapon)};
}

void ViewKick::kick(sim::Kick kick, sim::Tick now) noexcept {
    kick_ = kick;
    at_ = now;
    armed_ = true;
}

float ViewKick::level(sim::Tick now) const noexcept {
    if (!armed_ || now < at_) {
        return 0.0F;
    }
    const sim::Tick ticks = look_of(kick_).ticks;
    const sim::Tick elapsed = now - at_;
    if (elapsed >= ticks) {
        return 0.0F;
    }
    return 1.0F - (static_cast<float>(elapsed) / static_cast<float>(ticks));
}

float ViewKick::heave(sim::Tick now) const noexcept {
    return level(now) * look_of(kick_).heave_radians;
}

float ViewKick::flash(sim::Tick now) const noexcept { return level(now) * look_of(kick_).flash; }

Camera kicked_camera(Camera camera, float heave) noexcept {
    if (heave > 0.0F) {
        camera.pitch = std::min(camera.pitch + heave, std::max(camera.pitch, kHeaveCeiling));
    }
    return camera;
}

} // namespace protekt::game
