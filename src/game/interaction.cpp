#include "protekt/game/interaction.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>

#include "protekt/ai/utility.hpp"
#include "protekt/cmd/command.hpp"
#include "protekt/game/menu.hpp"
#include "protekt/game/nav_marks.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/radar.hpp"
#include "protekt/game/roster.hpp"
#include "protekt/input/bindings.hpp"
#include "protekt/math/math.hpp"
#include "protekt/sim/projectile.hpp"
#include "protekt/sim/spec.hpp"

namespace protekt::game {
namespace {

/// A float on the unit circle as the sim's Q16.16.
[[nodiscard]] sim::Fixed to_fixed(float value) noexcept {
    return sim::Fixed::from_raw(
        static_cast<std::int32_t>(std::lround(static_cast<double>(value) * sim::Fixed::kOne)));
}

/// The nearest hostile this tank could shoot from where it stands: within its weapon range by the
/// sim's own squared-distance rule (Q16.16), with a line to it where the ground is known. Ties go
/// to the lower id, whatever order the observation lists them in. Null when there is none.
[[nodiscard]] const sim::UnitView* nearest_in_range(const sim::Observation& observation,
                                                    const sim::UnitView& tank) noexcept {
    const std::int64_t range = sim::spec_of(sim::UnitKind::Tank).range.raw;
    const sim::Fixed from_x = sim::Fixed::from_raw(tank.raw_x);
    const sim::Fixed from_y = sim::Fixed::from_raw(tank.raw_y);
    const sim::UnitView* best = nullptr;
    std::int64_t best_d2 = 0;
    for (const sim::UnitView& enemy : observation.enemy) {
        const std::int64_t dx = static_cast<std::int64_t>(enemy.raw_x) - tank.raw_x;
        const std::int64_t dy = static_cast<std::int64_t>(enemy.raw_y) - tank.raw_y;
        const std::int64_t d2 = dx * dx + dy * dy;
        if (d2 > range * range) {
            continue;
        }
        const bool nearer
            = best == nullptr || d2 < best_d2 || (d2 == best_d2 && enemy.id < best->id);
        if (!nearer) {
            continue;
        }
        if (observation.terrain.has_value()
            && !sim::can_see(*observation.terrain, from_x, from_y,
                             sim::Fixed::from_raw(enemy.raw_x), sim::Fixed::from_raw(enemy.raw_y),
                             enemy.kind)) {
            continue;
        }
        best = &enemy;
        best_d2 = d2;
    }
    return best;
}

/// The menu the selection owns: its kind's subject when every selected unit is of one kind, and
/// `None` for an empty or mixed selection. The same rule as the app's `menu_subject_of`, which the
/// frame draws from -- the game layer cannot call up into the app, so the rule is written here too
/// and tests/test_player_view.cpp pins the two together.
[[nodiscard]] MenuSubject subject_of_selection(const sim::Sim& sim,
                                               const cmd::Selection& selection) noexcept {
    bool any = false;
    sim::UnitKind kind{};
    for (const sim::UnitId id : selection.units()) {
        const sim::Unit* unit = sim.find(id);
        if (unit == nullptr || !unit->alive) {
            continue;
        }
        if (any && unit->kind != kind) {
            return MenuSubject::None;
        }
        kind = unit->kind;
        any = true;
    }
    return any ? menu_subject_of_kind(kind) : MenuSubject::None;
}

/// How far Space will reach to select a unit: 80% of the radar's 40-tile radius. Beyond that the
/// unit is on the radar but too far to pick out by eye, so Space leaves the selection alone.
constexpr float kSpaceSelectRangeTiles = 0.8F * static_cast<float>(kRadarRadiusTiles);

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
        if (click.button == input::MouseButton::Right) {
            // THE WEAPON SWITCH, and it is the ONLY thing that changes weapons now: `Q` and the
            // mouse wheel used to do it as well and were removed on the owner's call (2026-10-23).
            // It needs no mark, so it is settled before the aim is consulted -- a weapon change is
            // the same click whether the crosshair is on a tank or on the sky. The orders this
            // button used to give live on the keys: `G` (send to a mark), `M` (move to the ground
            // under the mark), `T` (attack what the mark is on), `F` (follow). A weapon change
            // cancels nothing, so a pending `G` or `T` stays armed across it.
            weapon_ = next_weapon(weapon_);
            continue;
        }
        if (!options.aim.has_value()) {
            // No mark, no shot: a left click with nothing to point at has nothing it could mean. A
            // headless or scripted frame has no aim, and no left clicks either.
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
            if (weapon_ == Weapon::Mortar || weapon_ == Weapon::Bouncer) {
                // THE MORTAR IS AIMED BY HAND (M2): the shell flies the barrel -- the view's own
                // yaw and pitch, which is what the range ladder reads -- and not the path to
                // whatever is under the mark. Nothing is solved, nothing led.
                const std::array<float, 3> barrel
                    = math::basis_from(options.camera.yaw, options.camera.pitch).forward;
                pending_.push_back(sim::Order{
                    driven_,
                    sim::orders::FireLob{to_fixed(barrel.at(0)), to_fixed(barrel.at(2)),
                                         named ? hit->id : sim::UnitId{}, to_fixed(barrel.at(1)),
                                         weapon_ == Weapon::Bouncer ? sim::Round::Bounce
                                                                    : sim::Round::Contact}});
            } else {
                pending_.push_back(sim::Order{
                    driven_, sim::orders::FireTowards{to_fixed(dir.at(0)), to_fixed(dir.at(2)),
                                                      named ? hit->id : sim::UnitId{},
                                                      to_fixed(dir.at(1))}});
            }
            // Every manual shot is one order and one bolt. The sound is NOT played here: pulling
            // the trigger only raises the flag, and audio_pipeline judges it against the sim's own
            // Shot record, so a trigger the sim declines (still reloading, turret not mounted) is
            // silent instead of a lie.
            shot_voice_ = true;
            if (named) {
                message_ = hit_is_hostile ? "engaging the target under the crosshair."
                                          : "firing on your own unit.";
            }
        }
    }

    for (const input::KeyEvent& key : frame.keys) {
        if (!key.pressed) {
            continue;
        }
        if (go_armed_) {
            // G is waiting. Whatever this key is, the arming ends with it: a player is never left
            // in a mode the screen has stopped saying. A digit is the answer (a MARK number, not
            // N2's roster key); Escape only cancels; any other key cancels and then acts.
            go_armed_ = false;
            if (const int mark = input::mark_digit(key); mark >= 0) {
                send_to_mark(mark, options);
                continue;
            }
            if (key.scancode == input::kKeyEscape) {
                continue;
            }
        }
        if (attack_armed_) {
            // T is waiting on the attack list, and it settles exactly as G does. The digit is a ROW
            // key (the list's numbering is mark_digit's: 1-9, then 0), never N2's roster key.
            const std::vector<TargetRow> rows = std::move(targets_);
            disarm_attack();
            if (const int row = input::mark_digit(key); row >= 0) {
                attack_row(rows, row);
                continue;
            }
            if (key.scancode == input::kKeyEscape) {
                continue;
            }
        }
        if (key.scancode == input::kKeyG && !key.ctrl && !key.alt) {
            go_armed_ = true;
        } else if (const int group = input::function_key(key); group != 0) {
            select_roster_key(sim, group);
        } else if (key.scancode == input::kKeyEscape || key.scancode == input::kKeyTab) {
            // Tab clears, as Escape does.
            selection_.clear();
        } else if (const int digit = input::mark_digit(key); digit >= 0) {
            // Reached only with no prompt open: G and T settle a digit above and `continue`.
            menu_digit(sim, digit);
        } else if (key.scancode == input::kKeyX) {
            swap_to_crosshair(sim, options);
        } else if (key.scancode == input::kKeyM) {
            move_to_crosshair_ground(sim, options);
        } else if (key.scancode == input::kKeyF) {
            follow_crosshair_unit(sim, options);
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
                // Space is the SELECTOR, and its meaning depends on what is already selected and
                // what the mark is on. The menu on the left tells the player which one is live.
                // It never ADDS to a selection: a selection comes from a function key (a whole
                // group) or from Space over one vehicle starting from empty.
                if (selection_.empty()) {
                    // Nothing selected: select the single own unit under the mark, within 80% of
                    // the radar's range (32 of its 40 tiles). Ground, sky, hostiles and the
                    // driven tank (nothing to order) are ignored: a stray press does nothing.
                    if (hit_is_ours && hit->id != driven_
                        && picked.distance_tiles <= kSpaceSelectRangeTiles) {
                        selection_.clear();
                        selection_.add(hit->id);
                        message_ = "SPACE Select";
                    }
                    continue;
                }
                // Something is selected: follow a vehicle under the mark, or attack a hostile one.
                if (hit != nullptr && hit->alive) {
                    if (sim::is_tank(hit->kind)) {
                        follow_crosshair_unit(sim, options);
                        message_ = "SPACE Follow";
                        continue;
                    }
                    if (hit_is_hostile) {
                        const std::vector<sim::UnitId> roster = orderable();
                        if (roster.empty()) {
                            refuse("You are driving that one; select another unit to give orders.");
                            continue;
                        }
                        const std::vector<sim::Order> orders = cmd::attack(roster, hit->id);
                        if (orders.empty()) {
                            refuse("That order cannot be carried out.");
                            continue;
                        }
                        remember(orders);
                        pending_.insert(pending_.end(), orders.begin(), orders.end());
                        message_ = "SPACE Attack";
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
                // Nothing hostile under the crosshair: T offers the nearest ones by number instead.
                arm_attack_list(sim);
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
        if (!sim::is_tank(unit.kind) || unit.id == driven_) {
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
        // Defend in place: no standing order, not on the way home, not wounded -- and only on the
        // beat, so the choice is a pure function of the tick and the board.
        if (own_side.tick % kDefendCadence != 0 || unit.hp <= tuning.retreat_hp
            || retreating_.count(unit.id) > 0 || remembered_.count(unit.id) > 0) {
            continue;
        }
        if (const sim::UnitView* target = nearest_in_range(own_side, unit)) {
            const std::vector<sim::Order> fire = cmd::focus_fire(unit.id, target->id);
            pending_.insert(pending_.end(), fire.begin(), fire.end());
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

bool Interaction::go_armed() const noexcept { return go_armed_; }

bool Interaction::attack_armed() const noexcept { return attack_armed_; }

const std::vector<TargetRow>& Interaction::attack_targets() const noexcept { return targets_; }

const cmd::Selection& Interaction::selection() const noexcept { return selection_; }

sim::UnitId Interaction::driven() const noexcept { return driven_; }

Weapon Interaction::weapon() const noexcept { return weapon_; }

void Interaction::trigger_refused(const sim::Sim& sim) noexcept {
    const sim::Unit* gun = sim.find(driven_);
    if (gun != nullptr && gun->alive) {
        message_ = trigger_refusal(weapon_, *gun);
    }
}

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
        refuse("That unit is not yours -- you can only swap into your own.");
    } else if (!sim::is_drivable(hit->kind)) {
        refuse("That cannot be driven -- you can only swap into your own armed units.");
    } else if (hit->id == driven_) {
        refuse("You are already driving that unit.");
    } else {
        // The old tank would otherwise trundle off on its last MoveTo with nobody at the helm: Stop
        // idles it and the sim's steering zeroes its velocity. It is an ordinary unit from now on.
        const sim::UnitId vacated = driven_;
        const std::vector<sim::UnitId> roster{vacated};
        const std::vector<sim::Order> stop = cmd::stop(roster);
        remembered_.erase(vacated); // a Stop forgets; remember() skips the driven tank, as it was
        pending_.insert(pending_.end(), stop.begin(), stop.end());
        driven_ = hit->id;
        message_ = "swapped -- you are driving the unit you were looking at.";
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

void Interaction::follow_crosshair_unit(const sim::Sim& sim, const SceneOptions& options) {
    if (selection_.empty()) {
        refuse("Nothing is selected to give an order to.");
        return;
    }
    const std::vector<sim::UnitId> roster = orderable();
    if (roster.empty()) {
        refuse("You are driving that one; select another unit to give orders.");
        return;
    }
    // F is a unit order, not a ground order: the followee is whatever LIVING unit stands
    // under the mark, on either side (tailing an enemy is allowed by design). Following the
    // driven tank is the point: after an X-swap the wingman still shadows the old hull.
    if (!options.aim.has_value()) {
        refuse("Aim at a unit to follow it.");
        return;
    }
    const Pick picked = pick(sim, options, options.aim->x, options.aim->y);
    const sim::Unit* target = picked.has_unit() ? sim.find(picked.unit) : nullptr;
    if (target == nullptr || !target->alive) {
        refuse("Aim at a living unit to follow it.");
        return;
    }
    const std::vector<sim::Order> orders = cmd::follow(roster, target->id);
    if (orders.empty()) {
        refuse("That order cannot be carried out.");
        return;
    }
    remember(orders);
    pending_.insert(pending_.end(), orders.begin(), orders.end());
    message_ = "shadowing that unit.";
}

void Interaction::menu_digit(const sim::Sim& sim, int digit) {
    const MenuInput in{subject_of_selection(sim, selection_), sim::Side::Player, ArmedCommand{},
                       MenuTarget::None, static_cast<int>(sim.ore(sim::Side::Player))};
    const std::vector<MenuRow> rows = menu_rows(in);
    const char label = static_cast<char>('0' + digit);
    const auto row = std::find_if(rows.begin(), rows.end(), [&label](const MenuRow& candidate) {
        return candidate.key == std::string_view{&label, 1};
    });
    // A row the model dims, or draws no digit for, is not a command: the digit does nothing, as it
    // did before the menu was routed. Only the rows below can act yet; the rest stay inert.
    if (row == rows.end() || !row->enabled) {
        return;
    }
    if (row->verb == kVerbFollowMe) {
        follow_driven(sim);
    } else if (row->verb == kVerbHold) {
        hold_selection(sim);
    }
}

void Interaction::follow_driven(const sim::Sim& sim) {
    const sim::Unit* craft = sim.find(driven_);
    if (craft == nullptr || !craft->alive) {
        refuse("You have no craft to follow.");
        return;
    }
    const std::vector<sim::UnitId> roster = orderable();
    if (roster.empty()) {
        refuse("You are driving that one; select another unit to give orders.");
        return;
    }
    const std::vector<sim::Order> orders = cmd::follow(roster, driven_);
    if (orders.empty()) {
        refuse("That order cannot be carried out.");
        return;
    }
    remember(orders);
    pending_.insert(pending_.end(), orders.begin(), orders.end());
    message_ = "following you.";
}

void Interaction::hold_selection(const sim::Sim& sim) {
    std::vector<sim::UnitId> roster = orderable();
    if (roster.empty()) {
        refuse("You are driving that one; select another unit to give orders.");
        return;
    }
    // GuardAt takes ONE point for its whole roster and each unit stands somewhere else, so it is
    // built a unit at a time -- in ascending id order, as every cmd:: verb emits.
    std::sort(roster.begin(), roster.end());
    std::vector<sim::Order> orders;
    for (const sim::UnitId id : roster) {
        const sim::Unit* unit = sim.find(id);
        if (unit == nullptr) {
            continue;
        }
        const std::vector<sim::UnitId> one{id};
        const std::vector<sim::Order> post = cmd::guard_at(one, unit->x, unit->y);
        orders.insert(orders.end(), post.begin(), post.end());
    }
    if (orders.empty()) {
        refuse("That order cannot be carried out.");
        return;
    }
    remember(orders);
    pending_.insert(pending_.end(), orders.begin(), orders.end());
    message_ = "holding here.";
}

void Interaction::send_to_mark(int number, const SceneOptions& options) {
    // Words for each number, static because the message is a view.
    static constexpr std::array<std::string_view, kNavSlots> kNoMark{
        "There is no nav mark 0.", "There is no nav mark 1.", "There is no nav mark 2.",
        "There is no nav mark 3.", "There is no nav mark 4.", "There is no nav mark 5.",
        "There is no nav mark 6.", "There is no nav mark 7.", "There is no nav mark 8.",
        "There is no nav mark 9."};
    const NavMarks::Mark* target = nullptr;
    const std::vector<NavMarks::Mark> marks
        = options.nav_marks != nullptr ? options.nav_marks->marks() : std::vector<NavMarks::Mark>{};
    for (const NavMarks::Mark& mark : marks) {
        if (mark.number == number) {
            target = &mark;
        }
    }
    if (target == nullptr) {
        refuse(kNoMark.at(static_cast<std::size_t>(number)));
        return;
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
    const std::vector<sim::Order> orders = cmd::move_to(
        roster, sim::Fixed::from_int(target->tile_x), sim::Fixed::from_int(target->tile_y));
    if (orders.empty()) {
        refuse("That order cannot be carried out.");
        return;
    }
    remember(orders);
    pending_.insert(pending_.end(), orders.begin(), orders.end());
    if (roster.size() < selection_.units().size()) {
        message_ = "Sent the rest to the mark; you are driving that one, so it stays put.";
    }
}

void Interaction::arm_attack_list(const sim::Sim& sim) {
    // Captured once, here: the numbers on the screen are the numbers the next key means, however
    // the board moves in between. The driven tank is the centre, as it is the radar's.
    std::vector<TargetRow> rows
        = nearest_enemies(sim, sim::Side::Player, driven_, kRadarRadiusTiles);
    if (rows.empty()) {
        refuse("no enemies within radar range.");
        return;
    }
    // No need to clear the G arming here: the only way in is T, and a G arming eats the T that
    // would have got here (interaction.cpp's go_armed_ block runs first).
    targets_ = std::move(rows);
    attack_armed_ = true;
}

void Interaction::disarm_attack() noexcept {
    attack_armed_ = false;
    targets_.clear();
}

void Interaction::attack_row(const std::vector<TargetRow>& rows, int key) {
    // Words for each key, static because the message is a view.
    static constexpr std::array<std::string_view, kAttackListRows> kNoTarget{
        "no target 0 in the list.", "no target 1 in the list.", "no target 2 in the list.",
        "no target 3 in the list.", "no target 4 in the list.", "no target 5 in the list.",
        "no target 6 in the list.", "no target 7 in the list.", "no target 8 in the list.",
        "no target 9 in the list."};
    static constexpr std::array<std::string_view, kAttackListRows> kAttacking{
        "attacking the target in row 0.", "attacking the target in row 1.",
        "attacking the target in row 2.", "attacking the target in row 3.",
        "attacking the target in row 4.", "attacking the target in row 5.",
        "attacking the target in row 6.", "attacking the target in row 7.",
        "attacking the target in row 8.", "attacking the target in row 9."};
    const auto row = std::find_if(rows.begin(), rows.end(), [key](const TargetRow& candidate) {
        return candidate.key == key;
    });
    if (row == rows.end()) {
        refuse(kNoTarget.at(static_cast<std::size_t>(key)));
        return;
    }
    // The selection is read afresh: a unit may have died since T armed the list.
    if (selection_.empty()) {
        refuse("Nothing is selected to give an order to.");
        return;
    }
    const std::vector<sim::UnitId> roster = orderable();
    if (roster.empty()) {
        refuse("You are driving that one; select another unit to give orders.");
        return;
    }
    const std::vector<sim::Order> orders = cmd::attack(roster, row->id);
    if (orders.empty()) {
        refuse("That order cannot be carried out.");
        return;
    }
    remember(orders);
    pending_.insert(pending_.end(), orders.begin(), orders.end());
    message_ = kAttacking.at(static_cast<std::size_t>(key));
}

void Interaction::select_roster_key(const sim::Sim& sim, int group_key) {
    // The roster's key belongs to a KIND now (game/roster.hpp): pressing its F-key selects the
    // whole group, wherever its units are, and replaces whatever the mouse had built. A key no
    // group holds is FREE -- the pool keeps no memory of the group that returned it -- so one
    // message covers "that group is gone" and "you never had that group" alike. Digits no longer
    // select: they are the mark and attack-list row keys, and the command row K2 will build on.
    const Roster roster = roster_of(sim, sim::Side::Player);
    const RosterGroup* group = group_for_key(roster, group_key);
    if (group == nullptr) {
        refuse(empty_key_message(group_key));
        return;
    }
    selection_.clear();
    for (const sim::UnitId id : group->units) {
        selection_.add(id);
    }
}

void Interaction::select_only(sim::UnitId id) noexcept {
    selection_.clear();
    selection_.add(id);
}

void Interaction::toggle(sim::UnitId id) noexcept { selection_.toggle(id); }

void Interaction::refuse(std::string_view why) noexcept { message_ = why; }

} // namespace protekt::game
