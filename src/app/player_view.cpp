#include "protekt/app/player_view.hpp"

#include "protekt/game/pick.hpp"

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

game::MenuTarget menu_target_of(const sim::Sim& sim, const game::SceneOptions& options) {
    if (!options.aim.has_value()) {
        return game::MenuTarget::None;
    }
    const game::Pick picked = game::pick(sim, options, options.aim->x, options.aim->y);
    if (picked.has_unit()) {
        const sim::Unit* unit = sim.find(picked.unit);
        if (unit != nullptr && unit->alive) {
            return unit->side == sim::Side::Player ? game::MenuTarget::Selectable
                                                   : game::MenuTarget::Hostile;
        }
    }
    return picked.has_tile() ? game::MenuTarget::Ground : game::MenuTarget::None;
}

std::vector<game::MenuRow> menu_for(const sim::Sim& sim, const game::Session& session,
                                    const game::SceneOptions& options, bool attack_armed) {
    // The attack list and the menu share one slot under the notice: one panel at a time.
    if (attack_armed) {
        return {};
    }
    const game::MenuInput in{game::menu_subject_of(sim, session.selection().units()),
                             sim::Side::Player, game::ArmedCommand{}, menu_target_of(sim, options),
                             static_cast<int>(sim.ore(sim::Side::Player))};
    return game::menu_rows(in);
}

} // namespace protekt::app
