#include "protekt/app/player_view.hpp"

#include <algorithm>
#include <array>

#include "protekt/game/attack_aim.hpp"
#include "protekt/game/deploy_aim.hpp"
#include "protekt/game/go_here_aim.hpp"
#include "protekt/game/nav_marks.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/repair_aim.hpp"
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

game::Camera camera_on_hull(const math::Vec3& pivot, const math::Vec3& hull_up,
                            const EyePlacement& placement, float yaw, float pitch,
                            std::uint64_t seed) {
    game::Camera camera;
    const std::array<float, 3> forward = math::basis_from(yaw, 0.0F).forward;
    // The offset is laid out as if the hull stood level, then the whole of it is turned about the
    // hull's ground pivot by the same rotation that turns the body: the eye stays over the same
    // part of the vehicle as it leans. A level hull rotates nothing.
    const game::GroundTilt tilt = game::tilt_about(pivot, hull_up);
    const math::Vec3 eye
        = tilt.apply(math::Vec3{pivot[0] - (forward.at(0) * placement.behind_metres),
                                pivot[1] + placement.above_hull_metres,
                                pivot[2] - (forward.at(2) * placement.behind_metres)});
    camera.x = eye[0];
    camera.z = eye[2];
    camera.y = std::max(eye[1], game::ground_metres(seed, camera.x / game::kMetresPerTile,
                                                    camera.z / game::kMetresPerTile)
                                    + kEyeGroundClearanceMetres);
    camera.yaw = yaw;
    camera.pitch = pitch;
    camera.up = hull_up;
    return camera;
}

game::Camera camera_for(const sim::Sim& sim, const game::Session& session, std::uint64_t seed,
                        std::optional<float> hull_z) {
    for (const sim::Unit& unit : sim.units()) {
        if (unit.id != session.driven()) {
            continue;
        }
        const math::Vec3 pivot{
            static_cast<float>(unit.x.raw) / 65536.0F * game::kMetresPerTile,
            hull_z.value_or(game::ground_metres(seed, unit.x, unit.y) + game::kRideHeightMetres),
            static_cast<float>(unit.y.raw) / 65536.0F * game::kMetresPerTile};
        return camera_on_hull(pivot, game::ground_normal(seed, unit.x, unit.y),
                              eye_placement(session.camera_position()), session.view_yaw(),
                              session.view_pitch(), seed);
    }
    game::Camera camera;
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
    if (session.deploy_armed() || session.attack_armed() || session.repair_armed()
        || session.nav_list_armed()) {
        return sim::UnitId{};
    }
    return game::attack_target(sim, options, session.selection().units(), session.driven());
}

sim::UnitId repair_offer(const sim::Sim& sim, const game::Session& session,
                         const game::SceneOptions& options) {
    // The attack owns SPACE first (`Interaction` takes them in that order), as does an armed
    // command.
    if (session.deploy_armed() || session.attack_armed() || session.repair_armed()
        || session.nav_list_armed() || attack_offer(sim, session, options).valid()) {
        return sim::UnitId{};
    }
    return game::repair_target(sim, options, session.selection().units(), session.driven());
}

namespace {

std::optional<game::DeployOffer> deploy_offer_now(const sim::Sim& sim, const game::Session& session,
                                                  const game::SceneOptions& options) {
    // Never over the attack or repair offers or an armed command, which own SPACE (the same order
    // `Interaction` takes them in).
    if (session.deploy_armed() || session.attack_armed() || session.repair_armed()
        || session.nav_list_armed() || attack_offer(sim, session, options).valid()
        || repair_offer(sim, session, options).valid()) {
        return std::nullopt;
    }
    return game::deploy_offer(sim, options, session.selection().units(), session.driven());
}

/// The Go Here offer, withheld wherever an offer before it owns SPACE: an armed command, the
/// attack or the repair here, and the deploy inside `go_here_offer` itself -- the order
/// `Interaction` takes them in. The menu's `SPACE Go Here` row and the white line to its spot are
/// both drawn from this one answer.
std::optional<game::GoHereOffer> go_here_offer_now(const sim::Sim& sim,
                                                   const game::Session& session,
                                                   const game::SceneOptions& options) {
    if (session.deploy_armed() || session.attack_armed() || session.repair_armed()
        || session.nav_list_armed() || attack_offer(sim, session, options).valid()
        || repair_offer(sim, session, options).valid()) {
        return std::nullopt;
    }
    return game::go_here_offer(sim, options, session.selection().units(), session.driven());
}

} // namespace

std::optional<game::DeployLineTarget> deploy_line_target(const sim::Sim& sim,
                                                         const game::Session& session,
                                                         const game::SceneOptions& options) {
    const std::optional<game::DeployOffer> offer = deploy_offer_now(sim, session, options);
    if (!offer.has_value() || offer->site != game::DeploySite::Pool) {
        return std::nullopt;
    }
    return game::DeployLineTarget{offer->tile_x, offer->tile_y};
}

std::optional<game::DeployLineTarget> go_here_line_target(const sim::Sim& sim,
                                                          const game::Session& session,
                                                          const game::SceneOptions& options) {
    const std::optional<game::GoHereOffer> offer = go_here_offer_now(sim, session, options);
    if (!offer.has_value()) {
        return std::nullopt;
    }
    return game::DeployLineTarget{offer->tile_x, offer->tile_y};
}

std::optional<game::DeployLineTarget> nav_line_target(game::NavState state,
                                                      const game::NavAim& aim) {
    if (state != game::NavState::Placing || !aim.on_ground()) {
        return std::nullopt;
    }
    return game::DeployLineTarget{aim.tile_x, aim.tile_y};
}

std::vector<game::MenuRow> menu_for(const sim::Sim& sim, const game::Session& session,
                                    const game::SceneOptions& options, bool attack_armed) {
    game::MenuInput in{game::menu_subject_of(sim, session.selection().units()),
                       game::faction_of(options.factions, sim::Side::Player), game::ArmedCommand{},
                       menu_target_of(sim, options), static_cast<int>(sim.ore(sim::Side::Player))};
    if (attack_armed) {
        // `vehicle menu -> 3 Attack`: the list is the menu (AW), its rows named by the hostiles'
        // own faction.
        in.faction = game::faction_of(options.factions, sim::Side::Enemy);
        in.attack_list = &session.attack_targets();
        return game::menu_rows(in);
    }
    if (session.construct_armed()) {
        // `constructor menu -> 3 Construct`: the panel is the menu.
        in.construct_panel = true;
        return game::menu_rows(in);
    }
    if (session.repair_armed()) {
        // `service truck menu -> 3 Repair`: the list is the menu.
        in.repair_list = &session.repair_targets();
        return game::menu_rows(in);
    }
    if (session.nav_list_armed()) {
        // `vehicle menu -> 2 Go to Nav`: the list is the menu.
        in.nav_list = &session.nav_list();
        return game::menu_rows(in);
    }
    if (session.unit_list_armed()) {
        // `function key -> ~ Select Unit`: the list is the menu.
        in.unit_list = &session.unit_list();
        return game::menu_rows(in);
    }
    in.group = session.unit_key_holds_several(sim, options);
    in.attack = attack_offer(sim, session, options).valid();
    in.repair = repair_offer(sim, session, options).valid();
    in.deployed = game::any_deployed_turret(sim, session.selection().units());
    // `SPACE Deploy` only where SPACE would deploy, then `SPACE Go Here` only where it would
    // send the selection to the ground: each withheld over the offers before it and over an armed
    // command, which own SPACE (the same order `Interaction` takes them in).
    in.deploy = deploy_offer_now(sim, session, options).has_value();
    in.go_here = go_here_offer_now(sim, session, options).has_value();
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
