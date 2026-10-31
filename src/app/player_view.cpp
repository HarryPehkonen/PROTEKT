#include "protekt/app/player_view.hpp"

#include <algorithm>
#include <array>

#include "protekt/game/attack_aim.hpp"
#include "protekt/game/nav_marks.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/math/math.hpp"

namespace protekt::app {

bool is_alive(const sim::Sim& sim, sim::UnitId id) {
    for (const sim::Unit& unit : sim.units()) {
        if (unit.id == id) {
            return unit.alive;
        }
    }
    return false;
}

EyePlacement eye_placement(game::CameraPosition position) noexcept {
    switch (position) {
    case game::CameraPosition::Barrel:
        return EyePlacement{0.0F, kEyeAboveHullMetres};
    case game::CameraPosition::Chase:
        return EyePlacement{kChaseBehindMetres, kEyeAboveHullMetres};
    case game::CameraPosition::HighChase:
        return EyePlacement{kHighChaseBehindMetres,
                            game::kTankBarrelAxisMetres + kHighChaseLiftMetres};
    }
    return EyePlacement{0.0F, kEyeAboveHullMetres}; // unreachable: every position is above
}

game::Camera camera_for(const sim::Sim& sim, const game::Session& session, std::uint64_t seed,
                        std::optional<float> hull_z) {
    game::Camera camera;
    for (const sim::Unit& unit : sim.units()) {
        if (unit.id != session.driven()) {
            continue;
        }
        const EyePlacement placement = eye_placement(session.camera_position());
        const std::array<float, 3> forward = math::basis_from(session.view_yaw(), 0.0F).forward;
        camera.x = (static_cast<float>(unit.x.raw) / 65536.0F * game::kMetresPerTile)
                   - (forward.at(0) * placement.behind_metres);
        camera.z = (static_cast<float>(unit.y.raw) / 65536.0F * game::kMetresPerTile)
                   - (forward.at(2) * placement.behind_metres);
        camera.y
            = hull_z.value_or(game::ground_metres(seed, unit.x, unit.y) + game::kRideHeightMetres)
              + placement.above_hull_metres;
        camera.y = std::max(camera.y, game::ground_metres(seed, camera.x / game::kMetresPerTile,
                                                          camera.z / game::kMetresPerTile)
                                          + kEyeGroundClearanceMetres);
        break;
    }
    camera.yaw = session.view_yaw();
    camera.pitch = session.view_pitch();
    return camera;
}

game::Camera camera_yawed(game::Camera camera, std::int32_t centirad) {
    camera.yaw = game::Camera::wrap_angle(camera.yaw + static_cast<float>(centirad) / 100.0F);
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

sim::UnitId attack_offer(const sim::Sim& sim, const game::Session& session,
                         const game::SceneOptions& options) {
    if (session.deploy_armed() || session.attack_armed()) {
        return sim::UnitId{};
    }
    return game::attack_target(sim, options, session.selection().units(), session.driven());
}

std::vector<game::MenuRow> menu_for(const sim::Sim& sim, const game::Session& session,
                                    const game::SceneOptions& options, bool attack_armed) {
    // The attack list and the menu share one slot under the notice: one panel at a time.
    if (attack_armed) {
        return {};
    }
    game::MenuInput in{game::menu_subject_of(sim, session.selection().units()),
                       game::faction_of(options.factions, sim::Side::Player), game::ArmedCommand{},
                       menu_target_of(sim, options), static_cast<int>(sim.ore(sim::Side::Player))};
    in.attack = attack_offer(sim, session, options).valid();
    if (session.deploy_armed()) {
        // `un-deployed recycler menu -> 3`: the state row names the deploy and reads the distance
        // to the ground under the reticle, by the nav chain's own rule (`nav_metres`).
        in.armed = game::ArmedCommand{game::MenuCommand::Deploy};
        if (const game::NavAim aim = game::nav_aim(sim, options); aim.on_ground()) {
            in.metres = aim.metres;
        }
    }
    return game::menu_rows(in);
}

} // namespace protekt::app
