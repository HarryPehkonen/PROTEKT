#include "protekt/app/player_view.hpp"

namespace protekt::app {

bool is_alive(const sim::Sim& sim, sim::UnitId id) {
    for (const sim::Unit& unit : sim.units()) {
        if (unit.id == id) {
            return unit.alive;
        }
    }
    return false;
}

game::Camera camera_for(const sim::Sim& sim, const game::Session& session, std::uint64_t seed,
                        std::optional<float> hull_z) {
    constexpr float kEyeHeight = 3.0F;
    game::Camera camera;
    for (const sim::Unit& unit : sim.units()) {
        if (unit.id != session.driven()) {
            continue;
        }
        camera.x = static_cast<float>(unit.x.raw) / 65536.0F * game::kMetresPerTile;
        camera.z = static_cast<float>(unit.y.raw) / 65536.0F * game::kMetresPerTile;
        camera.y
            = hull_z.value_or(game::ground_metres(seed, unit.x, unit.y) + game::kRideHeightMetres)
              + kEyeHeight;
        break;
    }
    camera.yaw = session.view_yaw();
    camera.pitch = session.view_pitch();
    return camera;
}

sim::UnitId radar_viewer(const game::Session& session, bool no_radar) {
    return no_radar ? sim::UnitId{} : session.driven();
}

AudioFrame audio_frame_for(const sim::Sim& sim, const game::Session& session,
                           const game::Camera& camera, bool trigger_pulled,
                           bool player_alive_before) {
    return AudioFrame{game::Listener{camera.x, camera.z, camera.yaw, sim::Side::Player},
                      trigger_pulled, player_alive_before, is_alive(sim, session.driven()),
                      session.driven()};
}

} // namespace protekt::app
