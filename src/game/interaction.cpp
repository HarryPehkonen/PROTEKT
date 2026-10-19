#include "protekt/game/interaction.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <utility>

#include "protekt/cmd/command.hpp"
#include "protekt/game/pick.hpp"

namespace protekt::game {
namespace {

[[nodiscard]] std::vector<sim::UnitId> living_own(const sim::Sim& sim) {
    std::vector<sim::UnitId> ids;
    for (const sim::Unit& unit : sim.units()) {
        if (unit.alive && unit.side == sim::Side::Player) {
            ids.push_back(unit.id);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

/// A float on the unit circle as the sim's Q16.16.
[[nodiscard]] sim::Fixed to_fixed(float value) noexcept {
    return sim::Fixed::from_raw(
        static_cast<std::int32_t>(std::lround(static_cast<double>(value) * sim::Fixed::kOne)));
}

} // namespace

audio::Voice shot_at_nothing_voice() noexcept {
    return audio::Voice{audio::SoundKind::YourShot, 1.0F, 0.0F};
}

Interaction::Interaction(sim::UnitId driven) noexcept : driven_(driven) {}

void Interaction::handle(const input::InputFrame& frame, const sim::Sim& sim,
                         const SceneOptions& options) {
    message_ = std::string_view{};
    pending_.clear();
    shot_at_nothing_ = false;

    selection_.retain(living_own(sim));

    for (const input::Click& click : frame.clicks) {
        if (!click.pressed) {
            continue;
        }
        if (!options.aim.has_value()) {
            // No mark, no click: a click with nothing to point at has nothing it could mean. A
            // headless or scripted frame has no aim, and no clicks either.
            continue;
        }
        // The AIM POINT, not the click's own pixel: the shot leaves the middle of the screen, so
        // what the mark covers is what the click acts on. See game/aim.hpp.
        const Pick picked = pick(sim, options, options.aim->x, options.aim->y);
        const sim::Unit* hit = picked.has_unit() ? sim.find(picked.unit) : nullptr;
        const bool hit_is_hostile = hit != nullptr && hit->alive && hit->side != sim::Side::Player;

        // The mouse half of the control set: this is the whole of what a left and a right click
        // mean. The keys are in game/session.cpp (throttle) and input/bindings.hpp (movement), and
        // the table a player reads is README.md, "How to play".
        if (click.button == input::MouseButton::Left) {
            // THE TRIGGER. It is not a selection (Space is) and not a movement order, and it
            // never aims for the player: the bolt flies along the turret's aim and hits whatever
            // that path crosses. A unit under the mark is only INTENT -- what the shot is for --
            // and the sim permits it on one of our own too (friendly fire on purpose). The
            // selection survives a shot; only Tab and Escape clear it.
            if (has_fired_ && sim.tick() - last_shot_tick_ < kManualShotIntervalTicks) {
                continue; // the button, not the gun, is what this limits
            }
            has_fired_ = true;
            last_shot_tick_ = sim.tick();
            // The driven tank itself is nothing to name: it cannot shoot itself.
            const bool named = hit != nullptr && hit->alive && hit->id != driven_;
            // THE DIRECTION. The camera's forward is (sin yaw, 0, cos yaw) in world metres
            // (math::basis_from; yaw 0 looks down +Z), and the map plane is x = world x,
            // y = world z -- a tile is two metres either way, so only the direction matters.
            // So the aim on the map is (sin yaw, cos yaw), with no sign flipped: the
            // mirrored-screen note in session.cpp is about which way the YAW turns, not about
            // where a given yaw points. tests/test_interaction.cpp checks it against
            // `math::basis_from` itself.
            const float yaw = options.camera.yaw;
            pending_.push_back(sim::Order{
                driven_, sim::orders::FireTowards{to_fixed(std::sin(yaw)), to_fixed(std::cos(yaw)),
                                                  named ? hit->id : sim::UnitId{}}});
            // Every manual shot is one order, one bolt, and the app's own sound: the sim reports
            // no Fired event for it, so this is the only thing the player hears.
            shot_at_nothing_ = true;
            if (named) {
                message_ = hit_is_hostile ? "engaging the target under the crosshair."
                                          : "firing on your own unit.";
            }
        } else if (click.button == input::MouseButton::Right) {
            if (selection_.empty()) {
                refuse("Nothing is selected to give an order to.");
                continue;
            }
            const std::vector<sim::UnitId> roster = orderable();
            if (roster.empty()) {
                refuse("You are driving that one; select another unit to give orders.");
                continue;
            }
            std::vector<sim::Order> orders;
            if (hit_is_hostile) {
                orders = cmd::attack(roster, hit->id);
            } else if (picked.has_tile()) {
                orders = cmd::move_to(roster, sim::Fixed::from_int(picked.tile_x),
                                      sim::Fixed::from_int(picked.tile_y));
            } else {
                refuse("There is no ground there to send anyone to.");
                continue;
            }
            if (orders.empty()) {
                refuse("That order cannot be carried out.");
                continue;
            }
            pending_.insert(pending_.end(), orders.begin(), orders.end());
        }
    }

    for (const input::KeyEvent& key : frame.keys) {
        if (!key.pressed) {
            continue;
        }
        if (key.scancode == input::kKeyEscape || key.scancode == input::kKeyTab) {
            // Tab clears, as Escape does. Cycling (`cycle`) is unbound: it lost its key to this and
            // needs a new one.
            selection_.clear();
        } else if (key.scancode == input::kKeySpace || key.scancode == input::kKeyT) {
            if (!options.aim.has_value()) {
                continue; // no mark, nothing to point at; see the click loop
            }
            // The crosshair is read fresh on every press, so T after a kill retargets for free.
            const Pick picked = pick(sim, options, options.aim->x, options.aim->y);
            const sim::Unit* hit = picked.has_unit() ? sim.find(picked.unit) : nullptr;
            const bool hit_is_ours = hit != nullptr && hit->alive && hit->side == sim::Side::Player;
            const bool hit_is_hostile
                = hit != nullptr && hit->alive && hit->side != sim::Side::Player;

            if (key.scancode == input::kKeySpace) {
                // Space is a left click on an own unit and nothing else: ground, sky and hostiles
                // are not refused, they are ignored.
                if (hit_is_ours) {
                    if (key.shift) {
                        toggle(hit->id);
                    } else {
                        select_only(hit->id);
                    }
                }
                continue;
            }
            if (selection_.empty()) {
                refuse("Nothing is selected to give an order to.");
                continue;
            }
            const std::vector<sim::UnitId> roster = orderable();
            if (roster.empty()) {
                refuse("You are driving that one; select another unit to give orders.");
                continue;
            }
            if (!hit_is_hostile) {
                refuse("Aim at an enemy to attack it.");
                continue;
            }
            const std::vector<sim::Order> orders = cmd::attack(roster, hit->id);
            if (orders.empty()) {
                refuse("That order cannot be carried out.");
                continue;
            }
            pending_.insert(pending_.end(), orders.begin(), orders.end());
            message_ = "attacking the target under the crosshair.";
        }
    }
}

std::vector<sim::Order> Interaction::take_orders() noexcept {
    return std::exchange(pending_, std::vector<sim::Order>{});
}

bool Interaction::take_shot_at_nothing() noexcept { return std::exchange(shot_at_nothing_, false); }

const cmd::Selection& Interaction::selection() const noexcept { return selection_; }

sim::UnitId Interaction::driven() const noexcept { return driven_; }

std::string_view Interaction::message() const noexcept { return message_; }

std::vector<sim::UnitId> Interaction::orderable() const {
    std::vector<sim::UnitId> roster;
    for (const sim::UnitId id : selection_.units()) {
        if (id != driven_) {
            roster.push_back(id);
        }
    }
    return roster;
}

void Interaction::select_only(sim::UnitId id) noexcept {
    selection_.clear();
    selection_.add(id);
}

void Interaction::toggle(sim::UnitId id) noexcept { selection_.toggle(id); }

void Interaction::cycle(bool backwards, const sim::Sim& sim) {
    const std::vector<sim::UnitId> own = living_own(sim);
    if (own.empty()) {
        refuse("There are no units to select.");
        return;
    }
    sim::UnitId next = backwards ? own.back() : own.front();
    if (!selection_.empty()) {
        const std::span<const sim::UnitId> chosen = selection_.units();
        if (backwards) {
            const auto it = std::lower_bound(own.begin(), own.end(), chosen.front());
            if (it != own.begin()) {
                next = *(it - 1);
            }
        } else {
            const auto it = std::upper_bound(own.begin(), own.end(), chosen.back());
            if (it != own.end()) {
                next = *it;
            }
        }
    }
    select_only(next);
}

void Interaction::refuse(std::string_view why) noexcept { message_ = why; }

} // namespace protekt::game
