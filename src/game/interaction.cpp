#include "protekt/game/interaction.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

#include "protekt/ai/utility.hpp"
#include "protekt/cmd/command.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/input/bindings.hpp"
#include "protekt/math/math.hpp"

namespace protekt::game {
namespace {

/// A float on the unit circle as the sim's Q16.16.
[[nodiscard]] sim::Fixed to_fixed(float value) noexcept {
    return sim::Fixed::from_raw(
        static_cast<std::int32_t>(std::lround(static_cast<double>(value) * sim::Fixed::kOne)));
}

} // namespace

audio::Voice shot_voice() noexcept { return audio::Voice{audio::SoundKind::YourShot, 1.0F, 0.0F}; }

Interaction::Interaction(sim::UnitId driven) noexcept : driven_(driven) {}

void Interaction::handle(const input::InputFrame& frame, const sim::Sim& sim,
                         const SceneOptions& options) {
    message_ = std::string_view{};
    pending_.clear();
    shot_voice_ = false;

    selection_.retain_if([&sim](sim::UnitId id) {
        const sim::Unit* unit = sim.find(id);
        return unit != nullptr && unit->alive && unit->side == sim::Side::Player;
    });

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
            // THE DIRECTION. The shot is aimed from the MUZZLE at the point the mark's ray
            // reaches, so it crosses the reticle where the reticle is drawn instead of running
            // parallel to it, a muzzle-drop below (game::shot_direction). Out of the camera's
            // basis (math::basis_from; yaw 0 looks down +Z) it is (x, up, z) in world metres, and
            // the map plane is x = world x, y = world z -- a tile is two metres either way, so
            // only the direction matters. With no driven tank to fire from, the camera's forward.
            // tests/test_interaction.cpp checks it against `math::basis_from` itself.
            const sim::Unit* gun = sim.find(driven_);
            const std::array<float, 3> dir
                = gun != nullptr
                      ? shot_direction(*gun, options, picked, *options.aim)
                      : math::basis_from(options.camera.yaw, options.camera.pitch).forward;
            pending_.push_back(sim::Order{
                driven_,
                sim::orders::FireTowards{to_fixed(dir.at(0)), to_fixed(dir.at(2)),
                                         named ? hit->id : sim::UnitId{}, to_fixed(dir.at(1))}});
            // Every manual shot is one order, one bolt, and the app's own sound: the sim reports
            // no Fired event for it, so this is the only thing the player hears.
            shot_voice_ = true;
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
            remember(orders);
            pending_.insert(pending_.end(), orders.begin(), orders.end());
        }
    }

    for (const input::KeyEvent& key : frame.keys) {
        if (!key.pressed) {
            continue;
        }
        if (key.scancode == input::kKeyEscape || key.scancode == input::kKeyTab) {
            // Tab clears, as Escape does.
            selection_.clear();
        } else if (key.scancode == input::kKeyX) {
            swap_to_crosshair(sim, options);
        } else if (key.scancode == input::kKeyM) {
            move_to_crosshair_ground(sim, options);
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
                // Space TOGGLES the own unit under the mark in or out of the selection, so a
                // group is built one press at a time with no modifier (Shift-Space still does
                // the same). Ground, sky, hostiles and the driven tank (nothing to order) are
                // not refused, they are ignored: a stray press never clears the selection.
                if (hit_is_ours && hit->id != driven_) {
                    toggle(hit->id);
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
            remember(orders);
            pending_.insert(pending_.end(), orders.begin(), orders.end());
            message_ = "attacking the target under the crosshair.";
        }
    }
}

void Interaction::tend(const sim::Observation& own_side) {
    // The same numbers the enemy's commander retreats and returns on.
    const ai::UtilityTuning tuning{};
    const sim::UnitView* home = ai::home_factory(own_side);
    for (const sim::UnitView& unit : own_side.own) {
        if (unit.kind != sim::UnitKind::Tank || unit.id == driven_) {
            continue;
        }
        if (unit.hp <= tuning.retreat_hp) {
            if (home == nullptr) {
                continue; // no base to run to: keep fighting
            }
            retreating_.insert(unit.id);
            // Every frame, on purpose: it overwrites itself and outranks the old stance.
            const std::vector<sim::UnitId> roster{unit.id};
            const std::vector<sim::Order> go = cmd::move_to(roster, sim::Fixed::from_int(home->x),
                                                            sim::Fixed::from_int(home->y));
            pending_.insert(pending_.end(), go.begin(), go.end());
        } else if (unit.hp >= tuning.return_hp && retreating_.erase(unit.id) > 0) {
            const auto last = remembered_.find(unit.id);
            if (last != remembered_.end()) {
                pending_.push_back(sim::Order{unit.id, last->second});
            }
        }
    }
}

void Interaction::remember(const std::vector<sim::Order>& orders) {
    for (const sim::Order& order : orders) {
        if (order.unit == driven_) {
            continue;
        }
        if (order.kind() == sim::OrderKind::Stop) {
            remembered_.erase(order.unit);
        } else {
            remembered_[order.unit] = order.payload;
        }
    }
}

std::vector<sim::Order> Interaction::take_orders() noexcept {
    return std::exchange(pending_, std::vector<sim::Order>{});
}

bool Interaction::take_shot_voice() noexcept { return std::exchange(shot_voice_, false); }

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

void Interaction::swap_to_crosshair(const sim::Sim& sim, const SceneOptions& options) {
    if (!options.aim.has_value()) {
        refuse("There is nothing under the crosshair to swap into.");
        return;
    }
    const Pick picked = pick(sim, options, options.aim->x, options.aim->y);
    const sim::Unit* hit = picked.has_unit() ? sim.find(picked.unit) : nullptr;
    if (hit == nullptr || !hit->alive) {
        refuse("There is nothing under the crosshair to swap into.");
    } else if (hit->side != sim::Side::Player) {
        refuse("That tank is not yours -- you can only swap into your own.");
    } else if (hit->kind != sim::UnitKind::Tank) {
        refuse("That is not a tank -- you can only swap into a tank.");
    } else if (hit->id == driven_) {
        refuse("You are already driving that tank.");
    } else {
        // The old tank would otherwise trundle off on its last MoveTo with nobody at the helm: Stop
        // idles it and the sim's steering zeroes its velocity. It is an ordinary unit from now on.
        const sim::UnitId vacated = driven_;
        const std::vector<sim::UnitId> roster{vacated};
        const std::vector<sim::Order> stop = cmd::stop(roster);
        remembered_.erase(vacated); // a Stop forgets; remember() skips the driven tank, as it was
        pending_.insert(pending_.end(), stop.begin(), stop.end());
        driven_ = hit->id;
        message_ = "swapped tanks -- you are driving the one you were looking at.";
    }
}

void Interaction::move_to_crosshair_ground(const sim::Sim& sim, const SceneOptions& options) {
    if (!options.aim.has_value()) {
        return; // no mark, nothing to point at; see the click loop
    }
    if (selection_.empty()) {
        refuse("Nothing is selected to give an order to.");
        return;
    }
    const std::vector<sim::UnitId> roster = orderable();
    if (roster.empty()) {
        refuse("You are driving that one; select another unit to give orders.");
        return;
    }
    // The ground the mark's ray meets first. A unit under the mark does not matter: the pick
    // reports the tile behind it too, and M is always a move, never an attack.
    const Pick picked = pick(sim, options, options.aim->x, options.aim->y);
    if (!picked.has_tile()) {
        refuse("There is no ground there to send anyone to.");
        return;
    }
    const std::vector<sim::Order> orders = cmd::move_to(roster, sim::Fixed::from_int(picked.tile_x),
                                                        sim::Fixed::from_int(picked.tile_y));
    if (orders.empty()) {
        refuse("That order cannot be carried out.");
        return;
    }
    remember(orders);
    pending_.insert(pending_.end(), orders.begin(), orders.end());
}

void Interaction::select_only(sim::UnitId id) noexcept {
    selection_.clear();
    selection_.add(id);
}

void Interaction::toggle(sim::UnitId id) noexcept { selection_.toggle(id); }

void Interaction::refuse(std::string_view why) noexcept { message_ = why; }

} // namespace protekt::game
