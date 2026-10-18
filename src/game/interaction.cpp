#include "protekt/game/interaction.hpp"

#include <algorithm>
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

} // namespace

Interaction::Interaction(sim::UnitId driven) noexcept : driven_(driven) {}

void Interaction::handle(const input::InputFrame& frame, const sim::Sim& sim,
                         const SceneOptions& options) {
    message_ = std::string_view{};
    pending_.clear();

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
        const bool hit_is_ours = hit != nullptr && hit->alive && hit->side == sim::Side::Player;
        const bool hit_is_hostile = hit != nullptr && hit->alive && hit->side != sim::Side::Player;

        // The mouse half of the control set: this is the whole of what a left and a right click
        // mean. The keys are in game/session.cpp (throttle) and input/bindings.hpp (movement), and
        // the table a player reads is README.md, "How to play".
        if (click.button == input::MouseButton::Left) {
            if (hit_is_ours) {
                if (click.shift) {
                    toggle(hit->id);
                } else {
                    select_only(hit->id);
                }
            } else if (hit_is_hostile) {
                // THE TRIGGER. A left click on a hostile is not a selection and not a movement
                // order: it names a target for the unit the player is sitting in, through the same
                // FocusFire order the AI would use. It is the one order the driven unit accepts,
                // and it is safe precisely because it sets a target and nothing else -- no stance,
                // no destination -- so it cannot fight the keyboard.
                const std::vector<sim::Order> shot = cmd::focus_fire(driven_, hit->id);
                pending_.insert(pending_.end(), shot.begin(), shot.end());
                message_ = "engaging the target under the crosshair.";
            } else {
                selection_.clear();
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
        if (key.scancode == input::kKeyEscape) {
            selection_.clear();
        } else if (key.scancode == input::kKeyTab) {
            cycle(key.shift, sim);
        }
    }
}

std::vector<sim::Order> Interaction::take_orders() noexcept {
    return std::exchange(pending_, std::vector<sim::Order>{});
}

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
