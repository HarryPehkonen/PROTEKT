#include "protekt/game/interaction.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "protekt/ai/utility.hpp"
#include "protekt/cmd/command.hpp"
#include "protekt/game/assist.hpp"
#include "protekt/game/attack_aim.hpp"
#include "protekt/game/deploy_aim.hpp"
#include "protekt/game/go_here_aim.hpp"
#include "protekt/game/menu.hpp"
#include "protekt/game/nav_marks.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/radar.hpp"
#include "protekt/game/repair_aim.hpp"
#include "protekt/game/row.hpp"
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
    const std::int64_t range = sim::spec_of(sim::UnitKind::Tank).range().raw;
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

/// Whether an order is a build for this producer: the test the queue's cap counts by.
auto is_build_for(sim::UnitId producer) {
    return [producer](const sim::Order& order) {
        return order.unit == producer && order.kind() == sim::OrderKind::Build;
    };
}

} // namespace

bool any_deployed_turret(const sim::Sim& sim, std::span<const sim::UnitId> selected) noexcept {
    return std::any_of(selected.begin(), selected.end(), [&sim](sim::UnitId id) {
        const sim::Unit* unit = sim.find(id);
        return unit != nullptr && unit->alive && unit->kind == sim::UnitKind::DeployedTurret;
    });
}

audio::Voice shot_voice() noexcept { return audio::Voice{audio::SoundKind::YourShot, 1.0F, 0.0F}; }

Interaction::Interaction(sim::UnitId driven) noexcept : driven_(driven) {}

void Interaction::handle(const input::InputFrame& frame, const sim::Sim& sim,
                         const SceneOptions& options) {
    message_ = std::string_view{};
    owned_ = false;
    pending_.clear();
    shot_voice_ = false;

    selection_.retain_if([&sim](sim::UnitId id) {
        const sim::Unit* unit = sim.find(id);
        return unit != nullptr && unit->alive && unit->side == sim::Side::Player;
    });
    // The assist follows its target only while there is one to draw: dead, or no longer drawn by
    // the world view (out of the radar's reach, off the meshed ground), it is let go.
    if (assist_.valid()) {
        const sim::Unit* held = sim.find(assist_);
        if (held == nullptr || !held->alive || !available_to_viewer(sim, options, *held)) {
            assist_ = sim::UnitId{};
        }
    }
    if (readout_lock_.valid()) {
        const sim::Unit* held = sim.find(readout_lock_);
        if (held == nullptr || !held->alive || !available_to_viewer(sim, options, *held)) {
            readout_lock_ = sim::UnitId{};
        }
    }
    // The deploy is armed on a recycler: once the selection is not one any more (it died, it
    // deployed, the selection went), there is nothing left to deploy and the arming goes with it.
    if (deploy_armed_ && menu_subject_of(sim, selection_.units()) != MenuSubject::Recycler) {
        deploy_armed_ = false;
    }
    // A building armed for placement is armed on a Constructor, as the deploy is on a recycler: a
    // selection that is no longer one (it died, the selection went) leaves nothing to place with.
    if (placing_.has_value()
        && menu_subject_of(sim, selection_.units()) != MenuSubject::Constructor) {
        placing_.reset();
    }

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
            // under the mark), `SPACE` (attack what the mark is on), `F` (follow). A weapon change
            // cancels nothing, so a pending `G` or attack list stays armed across it.
            // A carried weapon belongs to the HULL, so the click walks the DRIVEN kind's lineup
            // (B): a hull that is blocked from a weapon never offers it. With no hull in hand --
            // a headless frame or nothing driven -- it walks the whole set, which every kind
            // carries today anyway.
            const sim::Unit* driven = sim.find(driven_);
            weapon_ = driven != nullptr ? next_weapon(driven->kind, weapon_) : next_weapon(weapon_);
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
            // THE DIRECTION IS THE BARREL, FOR EVERY WEAPON (the owner's ruling, 2026-10-30): the
            // view's own yaw and pitch, out of the camera's basis (math::basis_from; yaw 0 looks
            // down +Z), so it is (x, up, z) in world metres, and the map plane is x = world x,
            // y = world z -- a tile is two metres either way, so only the direction matters. A bolt
            // takes time to arrive and the target may have left, so NOTHING is aimed: no solving,
            // no leading, no correction to whatever is under the mark -- that unit is named in the
            // order for attribution only. tests/test_interaction.cpp checks it against
            // `math::basis_from` itself.
            const std::array<float, 3> barrel
                = math::basis_from(options.camera.yaw, options.camera.pitch).forward;
            if (weapon_ == Weapon::Mortar || weapon_ == Weapon::Bouncer) {
                // THE MORTAR'S shell flies the barrel too.
                pending_.push_back(sim::Order{
                    driven_,
                    sim::orders::FireLob{to_fixed(barrel.at(0)), to_fixed(barrel.at(2)),
                                         named ? hit->id : sim::UnitId{}, to_fixed(barrel.at(1)),
                                         weapon_ == Weapon::Bouncer ? sim::Round::Bounce
                                                                    : sim::Round::Contact}});
            } else if (weapon_ == Weapon::Shotgun) {
                // THE SCATTER GUN (A): the same aim as the gun's trigger, a different shot -- the
                // sim reads `kShotgunSpec` for the pellet count, cone and per-pellet damage. It
                // flies the barrel as the gun does; only the fan about that line differs.
                pending_.push_back(sim::Order{
                    driven_,
                    sim::orders::FireFan{to_fixed(barrel.at(0)), to_fixed(barrel.at(2)),
                                         named ? hit->id : sim::UnitId{}, to_fixed(barrel.at(1))}});
            } else if (weapon_ == Weapon::Rocket) {
                // THE ROCKET (B): its own order because its own numbers -- `kRocketSpec` reaches 18
                // tiles against the hull gun's 12, for triple damage at a fifth of the rate. Fired
                // along the barrel, as the gun's is, so the two crosshairs agree; only the
                // round, its reach and its reload differ. Until this order existed the rocket in
                // the ring was a tank bolt wearing a rocket crosshair.
                pending_.push_back(sim::Order{
                    driven_, sim::orders::FireRocket{to_fixed(barrel.at(0)), to_fixed(barrel.at(2)),
                                                     named ? hit->id : sim::UnitId{},
                                                     to_fixed(barrel.at(1))}});
            } else {
                pending_.push_back(sim::Order{
                    driven_, sim::orders::FireTowards{
                                 to_fixed(barrel.at(0)), to_fixed(barrel.at(2)),
                                 named ? hit->id : sim::UnitId{}, to_fixed(barrel.at(1))}});
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
        // Shift is a key of its own: the batch begins when the LAST shift key goes up.
        if (key.scancode == input::kKeyLShift || key.scancode == input::kKeyRShift) {
            shift_keys_.at(key.scancode == input::kKeyRShift ? 1 : 0) = key.pressed;
            if (!key.pressed && !shift_keys_.at(0) && !shift_keys_.at(1)) {
                release_shift();
            }
            continue;
        }
        if (!key.pressed) {
            continue;
        }
        // A key pressed with no shift modifier proves shift is up, whether or not its release
        // was ever seen (focus lost while it was down): the held batch begins.
        if (!key.shift) {
            shift_keys_ = {};
            release_shift();
        }
        // TAB (Harri): before shift is released it calls the whole batch off, and nothing is
        // built; shifted with nothing held, it cancels the rest of a batch under way. The Tab
        // then goes on to clear the selection, as it always did.
        if (key.scancode == input::kKeyTab && !key.ctrl && !key.alt && key.shift) {
            if (!held_.empty()) {
                held_.clear();
                shift_batch_ = 0;
            } else {
                cancel_selected_builds(sim);
            }
        }
        // A shift gesture is a run of digit presses: any other key ends it (a digit that is not a
        // build row ends it in `menu_digit`).
        if (input::mark_digit(key) < 0) {
            shift_batch_ = 0;
        }
        // Stepping lapses the moment the player takes up a selection of something else.
        if (stepping_ && !selection_.empty() && !selection_in_group(sim, options)) {
            stepping_ = false;
        }
        if (deploy_armed_ && deploy_key(key, sim, options)) {
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
            // The attack list is waiting (`3 ATTACK` armed it), and it settles exactly as G does.
            // The digit is a ROW key (the list's numbering is mark_digit's: 1-9, then 0), never
            // N2's roster key.
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
        if (repair_armed_) {
            // The repair list is waiting (`3 Repair` opened it), and it settles as the attack list
            // does: a digit is a ROW key, Escape only cancels, any other key cancels and acts.
            const std::vector<TargetRow> rows = std::move(repairs_);
            disarm_repair();
            if (const int row = input::mark_digit(key); row >= 0) {
                repair_row(sim, rows, row);
                continue;
            }
            if (key.scancode == input::kKeyEscape) {
                continue;
            }
        }
        if (nav_list_armed_) {
            // The nav list is waiting (`2 Go to Nav` opened it): a digit is a nav SLOT digit and
            // sends the selection to that mark (`send_to_mark`, the path G takes), Escape only
            // cancels, any other key cancels and acts.
            const std::vector<NavRow> rows = std::move(nav_rows_);
            disarm_nav_list();
            if (const int mark = input::mark_digit(key); mark >= 0) {
                const bool listed
                    = std::any_of(rows.begin(), rows.end(), [mark](const NavRow& candidate) {
                          return candidate.key == mark;
                      });
                if (listed) {
                    send_to_mark(mark, options);
                } else {
                    refuse("no nav point on that key.");
                }
                continue;
            }
            if (key.scancode == input::kKeyEscape) {
                continue;
            }
        }
        if (unit_list_armed_) {
            // The unit list is waiting (`~` opened it): a digit is a ROW key and selects that
            // unit, `~` and Escape only close it, any other key closes it and acts.
            const std::vector<TargetRow> rows = std::move(unit_rows_);
            disarm_unit_list();
            if (const int row = input::mark_digit(key); row >= 0) {
                unit_list_row(sim, options, rows, row);
                continue;
            }
            if (input::nav_menu_pressed(key) || key.scancode == input::kKeyEscape) {
                continue;
            }
        }
        if (construct_armed_) {
            // The CONSTRUCT panel is up (`3 Construct` opened it). Unlike the lists it STAYS up
            // through a press: a digit is a ROW, refused in words, and the player may try another.
            // TAB and Escape close it (the panel's own `TAB Close Menu` row) and are spent doing
            // so; the selection stays, so the Constructor's menu is back. A selection that is no
            // longer a Constructor ends it, and then the key acts as it would have.
            const bool still_constructor
                = menu_subject_of(sim, selection_.units()) == MenuSubject::Constructor;
            // A building is ARMED (a row was pressed and afforded): SPACE places it where the
            // reticle meets the ground, `~` turns it a quarter, a digit picks another row. Only
            // while armed: with nothing armed `~` is what it always was, a key that closes the
            // panel.
            if (still_constructor && placing_.has_value() && !key.ctrl && !key.alt) {
                if (key.scancode == input::kKeySpace) {
                    place_at_crosshair(sim, options);
                    continue;
                }
                if (key.scancode == input::kKeyGrave) {
                    rotate_placing();
                    continue;
                }
            }
            const int digit = input::mark_digit(key);
            if (still_constructor && digit >= 0) {
                construct_row(sim, options, digit);
                continue;
            }
            const bool closes = key.scancode == input::kKeyTab || key.scancode == input::kKeyEscape;
            disarm_construct(); // any other key closes it too, and then does its own job
            if (still_constructor && closes) {
                continue;
            }
        }
        if (key.scancode == input::kKeyG && !key.ctrl && !key.alt) {
            go_armed_ = true;
        } else if (const int target = input::shift_function_key(key); target != 0) {
            // Tested BEFORE the bare F-key: `function_key` answers a shifted press too.
            move_selection_to_key(sim, options, target);
        } else if (const int group = input::function_key(key); group != 0) {
            select_roster_key(options, group);
        } else if (input::nav_menu_pressed(key)) {
            // Reached only when the nav menu did not take the `~` (see `tilde_steps`).
            open_unit_list(sim, options);
        } else if (key.scancode == input::kKeyEscape || key.scancode == input::kKeyTab) {
            // Tab clears, as Escape does.
            selection_.clear();
            stepping_ = false;
        } else if (const int digit = input::mark_digit(key); digit >= 0) {
            // Reached only with no prompt open: G and the attack list settle a digit above and
            // `continue`. A digit is the OPEN MENU's verb when a selection owns one; with nothing
            // selected it is the TOP MENU's, and there `1` and `2` are the buildings (the menu
            // spec: a left menu owns its keys, the top menu is reachable only without one).
            if (selection_.empty()) {
                bring_up_building(sim, digit);
            } else {
                menu_digit(sim, options, digit, key.shift);
            }
        } else if (key.scancode == input::kKeyT && !key.ctrl && !key.alt) {
            // THE ASSIST. The hostile nearest the crosshair within the cone is latched for the dot
            // (replacing a latch already held); with nothing in the cone the latch is let go, and
            // with none held nothing happens at all -- no order, no prompt, no message. Nothing
            // here is a command: the dot draws, it never steers the shot.
            assist_ = assist_pick(sim, options);
            // The readout lock rides the same key on ANY asset: the one pointed at, his own or
            // not. The assist above is untouched by it.
            const sim::UnitId pointed
                = options.aim.has_value()
                      ? pointed_asset(sim, options,
                                      pick(sim, options, options.aim->x, options.aim->y))
                      : sim::UnitId{};
            readout_lock_ = pointed == readout_lock_ ? sim::UnitId{} : pointed;
        } else if (key.scancode == input::kKeyX) {
            swap_to_crosshair(sim, options);
        } else if (key.scancode == input::kKeyM) {
            move_to_crosshair_ground(sim, options);
        } else if (key.scancode == input::kKeyF) {
            follow_crosshair_unit(sim, options);
        } else if (key.scancode == input::kKeySpace) {
            if (!options.aim.has_value()) {
                continue; // no mark, nothing to point at; see the click loop
            }
            // The crosshair is read fresh on every press, so SPACE after a kill retargets for free.
            const Pick picked = pick(sim, options, options.aim->x, options.aim->y);
            const sim::Unit* hit = picked.has_unit() ? sim.find(picked.unit) : nullptr;
            const bool hit_is_ours = hit != nullptr && hit->alive && hit->side == sim::Side::Player;

            // Space is the SELECTOR, and its meaning depends on what is already selected and
            // what the mark is on. The menu on the left tells the player which one is live.
            // It never ADDS to a selection: a selection comes from a function key (a whole
            // group) or from Space over one vehicle starting from empty.
            if (selection_.empty()) {
                // Nothing selected: select the single own unit under the mark. What the mark can
                // be on is already settled by `pick` (the drawn radius, VIS). Ground, sky,
                // hostiles and the driven tank (nothing to order) are ignored: a stray press
                // does nothing.
                if (hit_is_ours && hit->id != driven_) {
                    selection_.clear();
                    selection_.add(hit->id);
                    message_ = "SPACE Select";
                }
                continue;
            }
            // Something is selected. FIRST the attack, because it is what the menu offers: the
            // same function that draws `SPACE Attack` and the red line names the target, so the
            // HUD and the key can never disagree. Weapons only -- see `attack_target`.
            if (const sim::UnitId target = attack_target(sim, options, selection_.units(), driven_);
                target.valid()) {
                attack(target);
                continue;
            }
            // Then the REPAIR, the attack's mirror (`repair_target`, the function the menu's
            // `SPACE Repair` row and the green line are drawn from): a truck pointed at a damaged
            // friendly.
            if (const sim::UnitId patient
                = repair_target(sim, options, selection_.units(), driven_);
                patient.valid()) {
                repair(sim, patient);
                continue;
            }
            // Then the DEPLOY, which is offered only where it is true (`deploy_offer`, the same
            // function the menu's `SPACE Deploy` row is drawn from): a lone scavenger pointing at
            // a pool, or turrets pointing at bare ground.
            if (const std::optional<DeployOffer> offer
                = deploy_offer(sim, options, selection_.units(), driven_);
                offer.has_value()) {
                deploy_at(*offer);
                continue;
            }
            // Then GO HERE, offered where the mark is on bare ground (`go_here_offer`, the function
            // the menu's `SPACE Go Here` row is drawn from): any vehicle, or a whole function-key
            // group, drives to the tile.
            if (const std::optional<GoHereOffer> go
                = go_here_offer(sim, options, selection_.units(), driven_);
                go.has_value()) {
                go_here(*go);
                continue;
            }
            // Otherwise follow a vehicle of ours under the mark.
            if (hit_is_ours && sim::is_tank(hit->kind)) {
                follow_crosshair_unit(sim, options);
                message_ = "SPACE Follow";
            }
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

bool Interaction::deploy_armed() const noexcept { return deploy_armed_; }

std::optional<Placing> Interaction::placing() const noexcept { return placing_; }

bool Interaction::tilde_steps() const noexcept { return stepping_ && selection_.empty(); }

const std::vector<TargetRow>& Interaction::attack_targets() const noexcept { return targets_; }

bool Interaction::construct_armed() const noexcept { return construct_armed_; }

bool Interaction::repair_armed() const noexcept { return repair_armed_; }

const std::vector<TargetRow>& Interaction::repair_targets() const noexcept { return repairs_; }

bool Interaction::nav_list_armed() const noexcept { return nav_list_armed_; }

const std::vector<NavRow>& Interaction::nav_list() const noexcept { return nav_rows_; }

bool Interaction::unit_list_armed() const noexcept { return unit_list_armed_; }

const std::vector<TargetRow>& Interaction::unit_list() const noexcept { return unit_rows_; }

bool Interaction::unit_key_holds_several(const sim::Sim& sim, const SceneOptions& options) const {
    if (group_key_ < 1 || group_key_ > kRowGroups || options.row == nullptr
        || !(stepping_ || selection_in_group(sim, options))) {
        return false;
    }
    const std::optional<SlotReading> reading
        = read_slot(sim, options.row->slots.at(group_slot(group_key_)));
    return reading.has_value() && reading->count >= 2;
}

const cmd::Selection& Interaction::selection() const noexcept { return selection_; }

sim::UnitId Interaction::driven() const noexcept { return driven_; }

sim::UnitId Interaction::assist_target() const noexcept { return assist_; }

sim::UnitId Interaction::readout_lock() const noexcept { return readout_lock_; }

Weapon Interaction::weapon() const noexcept { return weapon_; }

void Interaction::trigger_refused(const sim::Sim& sim) noexcept {
    const sim::Unit* gun = sim.find(driven_);
    if (gun != nullptr && gun->alive) {
        message_ = trigger_refusal(weapon_, *gun);
    }
}

std::string_view Interaction::message() const noexcept {
    // The composed refusal shows only while no later message has replaced it: a later `message_ =`
    // (non-empty) wins, as it always did, and `refuse_owned` blanks `message_` so it wins in turn.
    return owned_ && message_.empty() ? std::string_view{owned_message_} : message_;
}

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
        // Not "armed": `is_drivable` asks whether the hull MOVES, and the scavenger and an
        // undeployed recycler are unarmed and drivable. The refusal must name the rule that ran.
        refuse("That cannot be driven -- you can only swap into your own vehicles.");
    } else if (hit->id == driven_) {
        refuse("You are already driving that unit.");
    } else if (options.row != nullptr && !options.row->swap_driven(sim, driven_, hit->id)) {
        // The row is re-derived with the swap (the vehicle left takes a key, the one entered gives
        // its own up) and a full row has no key to give: the swap is DISALLOWED, nothing changed.
        refuse("No function key is free for the vehicle you would leave.");
    } else {
        // The old tank would otherwise trundle off on its last MoveTo with nobody at the helm: Stop
        // idles it and the sim's steering zeroes its velocity. It is an ordinary unit from now on.
        const sim::UnitId vacated = driven_;
        const std::vector<sim::UnitId> roster{vacated};
        const std::vector<sim::Order> stop = cmd::stop(roster);
        remembered_.erase(vacated); // a Stop forgets; remember() skips the driven tank, as it was
        pending_.insert(pending_.end(), stop.begin(), stop.end());
        driven_ = hit->id;
        // A hull may be blocked from the weapon the old one had up: the selection must never name
        // a weapon this kind cannot carry, so it falls to the first the new kind allows. Nothing is
        // blocked today, so this is a no-op until the first row lands in `kWeaponBlocks`.
        if (!weapon_allowed(hit->kind, weapon_)) {
            weapon_ = weapon_lineup(hit->kind).weapons.front();
        }
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
    selection_.clear(); // the sequence is complete: the menu closes (see `menu_digit`)
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
    selection_.clear(); // the sequence is complete: the menu closes (see `menu_digit`)
}

void Interaction::menu_digit(const sim::Sim& sim, const SceneOptions& options, int digit,
                             bool shift) {
    MenuInput in;
    in.subject = menu_subject_of(sim, selection_.units());
    in.faction = faction_of(options.factions, sim::Side::Player);
    in.ore = static_cast<int>(sim.ore(sim::Side::Player));
    in.group = selection_.units().size() > 1;
    in.deployed = any_deployed_turret(sim, selection_.units());
    const std::vector<MenuRow> rows = menu_rows(in);
    const char label = static_cast<char>('0' + digit);
    const auto row = std::find_if(rows.begin(), rows.end(), [&label](const MenuRow& candidate) {
        return candidate.key == std::string_view{&label, 1};
    });
    // A build row obeys the model's dim like every other row (the owner's ruling, round 16): a row
    // drawn Unaffordable is not a command, and the press is refused HERE, at the order, by reading
    // the row's own `enabled` -- the one predicate (`append_build_list`'s `ore >= cost`) that also
    // draws the dim, so the two cannot drift apart. What is NOT refused is a queue already ordered
    // that runs short of scrap later: the sim still pauses and resumes it (`production_step`).
    // So this runs before the enabled test below only because a build row queues instead of acting.
    if ((in.subject == MenuSubject::Deployment || in.subject == MenuSubject::Factory)
        && row != rows.end()) {
        const std::span<const BuildItem> items = build_items(in.faction, in.subject);
        const auto item
            = std::find_if(items.begin(), items.end(), [&row](const BuildItem& candidate) {
                  return candidate.key == row->key;
              });
        if (item != items.end()) {
            if (!row->enabled) {
                // Refused WHOLE: a shifted batch holds its earlier presses back until shift goes
                // up, and those go too, so letting go cannot order part of a refused batch.
                held_.clear();
                shift_batch_ = 0;
                // The sentence is the ROW's own reason made into words (`refusal_message`), so the
                // dim and the refusal cannot disagree about why.
                refuse_owned(refusal_message(*row));
                return;
            }
            queue_build(sim, *item, shift);
            return;
        }
    }
    shift_batch_ = 0; // a digit that is not a build row ends a shift gesture
    // Harri: a COMPLETED menu sequence closes the menu and un-selects, and so does every shortcut
    // that gives an order (SPACE attack, F, M, G + a mark, the deploy click: "the command is done,
    // and the menu should go away, as should the selection", 2026-11-01). The menu is derived from
    // the selection (`menu_subject_of`), so the handlers that POST orders -- Follow Me, Hold, Go to
    // Nav, Recycle, Scavenge, and the attack list's row (Attack only arms that list; the row
    // completes it) -- end by clearing it. Two kinds of row deliberately do not: the build rows
    // (above) keep the factory selected so the player can keep queueing, and Deploy only ARMS a
    // click, so its sequence is not complete until the click lands. A row the model dims, or draws
    // no digit for, is not a command: the digit does nothing, as it did before the menu was routed.
    // Only the rows below can act yet; the rest stay inert.
    if (row == rows.end() || !row->enabled) {
        return;
    }
    if (row->verb == kVerbFollowMe) {
        follow_driven(sim);
    } else if (row->verb == kVerbHold) {
        hold_selection(sim);
    } else if (row->verb == kVerbScavenge) {
        scavenge_selection();
    } else if (row->verb == kVerbDeploy) {
        arm_deploy();
    } else if (row->verb == kVerbGoToNav) {
        arm_nav_list(options);
    } else if (row->verb == kVerbAttack) {
        attack_from_menu(sim, options);
    } else if (row->verb == kVerbRepair) {
        arm_repair_list(sim);
    } else if (row->verb == kVerbConstruct) {
        arm_construct();
    } else if (row->verb == kVerbRecycle) {
        recycle_selection(sim);
    } else if (row->verb == kVerbDefend) {
        defend_selection(sim, options);
    } else if (row->verb == kVerbUndeploy) {
        undeploy_selection(sim);
    }
}

void Interaction::queue_build(const sim::Sim& sim, const BuildItem& item, bool shift) {
    // The producer is chosen by KIND among the selection (the lowest id, as a deploy's recycler
    // is), never as "the factory": a side may have several, and each queue is its own.
    const std::span<const sim::UnitId> held = selection_.units();
    std::vector<sim::UnitId> chosen(held.begin(), held.end());
    std::sort(chosen.begin(), chosen.end());
    const sim::Unit* producer = nullptr;
    for (const sim::UnitId id : chosen) {
        const sim::Unit* unit = sim.find(id);
        if (unit != nullptr && unit->alive && unit->side == sim::Side::Player
            && sim::is_producer(unit->kind)) {
            producer = unit;
            break;
        }
    }
    if (producer == nullptr || !item.kind.has_value()) {
        shift_batch_ = 0; // an item no kind names is drawn and does nothing
        return;
    }
    // The queue as the sim will find it: what it holds, and what this frame has already asked for.
    const auto asked = static_cast<std::size_t>(
        std::count_if(pending_.begin(), pending_.end(), is_build_for(producer->id))
        + std::count_if(held_.begin(), held_.end(), is_build_for(producer->id)));
    const std::size_t queued = producer->queue.count + asked;
    // A press CONTINUES a run when it is shifted into the run already open at this producer; any
    // other press would start a batch of its own (THE RULING, Harri 2026-10-28).
    const bool continues = shift && shift_batch_ != 0 && shift_producer_ == producer->id;
    if (continues && *item.kind != shift_kind_) {
        // A gesture is bound to ONE kind: the run stays open for its own kind.
        refuse("A batch is one kind of thing. Let go of shift to build something else.");
        return;
    }
    if (!continues) {
        shift_batch_ = 0; // a press that does not continue the run ends it, accepted or not
        if (queued > 0) {
            refuse("This producer is busy until its batch is done.");
            return;
        }
    }
    if (queued >= sim::kBuildQueueDepth) {
        refuse("The build queue is full.");
        return;
    }
    std::uint32_t batch = 0; // a bare press is a lone build: the sim numbers it
    if (shift) {
        if (!continues) {
            batch_tokens_ = batch_tokens_ % sim::kMaxBatchToken + 1;
            shift_batch_ = batch_tokens_;
            shift_producer_ = producer->id;
            shift_kind_ = *item.kind;
        }
        batch = shift_batch_;
    }
    // A shifted press waits for shift to go up; a bare one is a lone build and goes at once.
    (shift ? held_ : pending_)
        .push_back(sim::Order{producer->id, sim::orders::Build{*item.kind, batch}});
}

void Interaction::release_shift() {
    shift_batch_ = 0; // the next shifted press is a gesture of its own
    pending_.insert(pending_.end(), held_.begin(), held_.end());
    held_.clear();
}

void Interaction::cancel_selected_builds(const sim::Sim& sim) {
    const std::span<const sim::UnitId> held = selection_.units();
    std::vector<sim::UnitId> chosen(held.begin(), held.end());
    std::sort(chosen.begin(), chosen.end());
    for (const sim::UnitId id : chosen) {
        const sim::Unit* unit = sim.find(id);
        if (unit != nullptr && unit->alive && unit->side == sim::Side::Player
            && sim::is_producer(unit->kind)
            && (unit->queue.count > 0
                || std::any_of(pending_.begin(), pending_.end(), is_build_for(id)))) {
            pending_.push_back(sim::Order{id, sim::orders::CancelBuild{}});
        }
    }
}

void Interaction::bring_up_building(const sim::Sim& sim, int key) {
    // THE CAUSE of "1 and 2 do not bring up the buildings": the row DREW them (`RowState::update`
    // refills slots 1 and 2 from the sim every frame) but nothing ever READ them back. The digit
    // path went only to `menu_digit`, which looks the digit up in the SELECTED subject's menu --
    // and with nothing selected there is no subject and no menu, so the press fell through to
    // nothing. Pointing at the building and pressing SPACE was the only way to select one.
    const sim::UnitId building = building_key_holder(sim, sim::Side::Player, key);
    if (building.valid()) {
        select_only(building);
        return;
    }
    // Key 3 (the armory) has no kind yet and stays silent, as it always was; 1 and 2 say why.
    if (key == 1) {
        refuse("You have no recycler.");
    } else if (key == 2) {
        refuse("You have no factory.");
    }
}

void Interaction::attack(sim::UnitId target) {
    const std::vector<sim::UnitId> roster = orderable();
    if (roster.empty()) {
        refuse("You are driving that one; select another unit to give orders.");
        return;
    }
    const std::vector<sim::Order> orders = cmd::attack(roster, target);
    if (orders.empty()) {
        refuse("That order cannot be carried out.");
        return;
    }
    remember(orders);
    pending_.insert(pending_.end(), orders.begin(), orders.end());
    message_ = "SPACE Attack";
    selection_.clear(); // the sequence is complete: the menu closes (see `menu_digit`)
}

void Interaction::repair(const sim::Sim& sim, sim::UnitId patient) {
    std::vector<sim::UnitId> trucks;
    for (const sim::UnitId id : orderable()) {
        const sim::Unit* unit = sim.find(id);
        if (unit != nullptr && unit->kind == sim::UnitKind::ServiceTruck && id != patient) {
            trucks.push_back(id);
        }
    }
    const std::vector<sim::Order> orders = cmd::repair(trucks, patient);
    if (orders.empty()) {
        refuse("That order cannot be carried out.");
        return;
    }
    remember(orders);
    pending_.insert(pending_.end(), orders.begin(), orders.end());
    message_ = "SPACE Repair";
    selection_.clear(); // the sequence is complete: the menu closes (see `menu_digit`)
}

void Interaction::arm_deploy() {
    if (orderable().empty()) {
        refuse("You are driving that one; select another unit to give orders.");
        return;
    }
    deploy_armed_ = true;
}

void Interaction::deploy_at(const DeployOffer& offer) {
    std::vector<sim::UnitId> roster = orderable();
    if (roster.empty()) {
        refuse("You are driving that one; select another unit to give orders.");
        return;
    }
    const std::vector<sim::Order> orders = cmd::deploy(roster, offer.tile_x, offer.tile_y);
    if (orders.empty()) {
        refuse("That order cannot be carried out.");
        return;
    }
    remember(orders);
    pending_.insert(pending_.end(), orders.begin(), orders.end());
    message_ = "deploying.";
    selection_.clear(); // the sequence is complete: the menu closes (see `menu_digit`)
}

void Interaction::go_here(const GoHereOffer& offer) {
    const std::vector<sim::UnitId> roster = orderable();
    if (roster.empty()) {
        refuse("You are driving that one; select another unit to give orders.");
        return;
    }
    const std::vector<sim::Order> orders = cmd::move_to(roster, sim::Fixed::from_int(offer.tile_x),
                                                        sim::Fixed::from_int(offer.tile_y));
    if (orders.empty()) {
        refuse("That order cannot be carried out.");
        return;
    }
    remember(orders);
    pending_.insert(pending_.end(), orders.begin(), orders.end());
    message_ = "SPACE Go Here";
    selection_.clear(); // the sequence is complete: the menu closes (see `menu_digit`)
}

void Interaction::defend_selection(const sim::Sim& sim, const SceneOptions& options) {
    const std::vector<sim::UnitId> roster = orderable();
    if (roster.empty()) {
        refuse("You are driving that one; select another unit to give orders.");
        return;
    }
    // Each turret defends against the hostile asset NEAREST IT that the player can see (the
    // radar's reach and the drawn window: `available_to_viewer`), ties to the lower id. The sim
    // does the rest: it drives the turret to within its weapon range of that unit and deploys it.
    std::vector<sim::Order> orders;
    for (const sim::UnitId id : roster) {
        const sim::Unit* turret = sim.find(id);
        if (turret == nullptr) {
            continue;
        }
        const sim::Unit* nearest = nullptr;
        std::int64_t best = 0;
        for (const sim::Unit& other : sim.units()) {
            if (!other.alive || other.side == sim::Side::Player
                || !available_to_viewer(sim, options, other)) {
                continue;
            }
            const std::int64_t dx = static_cast<std::int64_t>(other.x.raw) - turret->x.raw;
            const std::int64_t dy = static_cast<std::int64_t>(other.y.raw) - turret->y.raw;
            const std::int64_t d2 = (dx * dx) + (dy * dy);
            if (nearest == nullptr || d2 < best) { // strict: ties keep the lower id
                nearest = &other;
                best = d2;
            }
        }
        if (nearest != nullptr) {
            const std::vector<sim::UnitId> one{id};
            const std::vector<sim::Order> mine = cmd::defend(one, nearest->id);
            orders.insert(orders.end(), mine.begin(), mine.end());
        }
    }
    if (orders.empty()) {
        refuse("no enemies within radar range.");
        return;
    }
    remember(orders);
    pending_.insert(pending_.end(), orders.begin(), orders.end());
    message_ = "defending.";
    selection_.clear(); // the sequence is complete: the menu closes (see `menu_digit`)
}

void Interaction::undeploy_selection(const sim::Sim& sim) {
    std::vector<sim::UnitId> deployed;
    for (const sim::UnitId id : orderable()) {
        const sim::Unit* unit = sim.find(id);
        if (unit != nullptr && unit->kind == sim::UnitKind::DeployedTurret) {
            deployed.push_back(id);
        }
    }
    if (deployed.empty()) {
        refuse("Nothing selected is deployed.");
        return;
    }
    const std::vector<sim::Order> orders = cmd::undeploy(deployed);
    remember(orders);
    pending_.insert(pending_.end(), orders.begin(), orders.end());
    message_ = "undeploying.";
    selection_.clear(); // the sequence is complete: the menu closes (see `menu_digit`)
}

bool Interaction::deploy_key(const input::KeyEvent& key, const sim::Sim& sim,
                             const SceneOptions& options) {
    if (key.ctrl || key.alt) {
        return false;
    }
    if (key.scancode == input::kKeySpace) {
        deploy_at_crosshair(sim, options);
        return true;
    }
    if (key.scancode == input::kKeyTab || key.scancode == input::kKeyEscape) {
        deploy_armed_ = false;
        return false; // and on to its usual job: the selection, and so the menu, closes
    }
    if (key.scancode == input::kKeyGrave) {
        // `~ ROTATE BUILDINGS`. The footprint is a 2x2 square about its own centre, and a square
        // turned a quarter is the same square on the same four tiles: there is nothing to rotate
        // yet, so no rotation is kept. The key is the deployment menu's, and it does nothing else.
        return true;
    }
    // The top menu is shown but not reachable while this left menu is open (MENU-NAV's rule): the
    // F keys, shifted or not, and the G prompt, which would open a panel of its own.
    return input::function_key(key) != 0 || key.scancode == input::kKeyG;
}

void Interaction::deploy_at_crosshair(const sim::Sim& sim, const SceneOptions& options) {
    if (!options.aim.has_value()) {
        return; // no reticle, no ground: the arming waits for one
    }
    const Pick picked = pick(sim, options, options.aim->x, options.aim->y);
    if (!picked.has_tile()) {
        return; // the reticle is off the ground: SPACE deploys nothing, and the arming survives
    }
    if (!sim::deploy_site_ok(sim.seed(), sim.units(), picked.tile_x, picked.tile_y)) {
        refuse("There is no room to deploy there.");
        return;
    }
    std::vector<sim::UnitId> roster = orderable();
    if (roster.empty()) {
        refuse("You are driving that one; select another unit to give orders.");
        return;
    }
    std::sort(roster.begin(), roster.end());
    const std::vector<sim::Order> orders{
        sim::Order{roster.front(), sim::orders::Deploy{picked.tile_x, picked.tile_y}}};
    remember(orders);
    pending_.insert(pending_.end(), orders.begin(), orders.end());
    deploy_armed_ = false;
    message_ = "deploying.";
    selection_.clear(); // the sequence is complete: the menu closes (see `menu_digit`)
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
    selection_.clear(); // the sequence is complete: the menu closes (see `menu_digit`)
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
    selection_.clear(); // the sequence is complete: the menu closes (see `menu_digit`)
}

void Interaction::scavenge_selection() {
    const std::vector<sim::UnitId> roster = orderable();
    if (roster.empty()) {
        refuse("You are driving that one; select another unit to give orders.");
        return;
    }
    // `3 Scavenge` is a Stop and nothing more: an idle scavenger takes the scrap inside its radar
    // where it stands, so the order carries no destination and names no pile. Leading it to new
    // litter is `1 Follow Me`, and the player's.
    const std::vector<sim::Order> orders = cmd::stop(roster);
    if (orders.empty()) {
        refuse("That order cannot be carried out.");
        return;
    }
    remember(orders);
    pending_.insert(pending_.end(), orders.begin(), orders.end());
    message_ = "scavenging here.";
    selection_.clear(); // the sequence is complete: the menu closes (see `menu_digit`)
}

void Interaction::arm_nav_list(const SceneOptions& options) {
    // `marks()` returns its list by value, so it is walked, never pointed into.
    std::vector<NavRow> rows;
    if (options.nav_marks != nullptr) {
        const std::vector<NavMarks::Mark> marks = options.nav_marks->marks();
        rows.reserve(marks.size());
        for (const NavMarks::Mark& mark : marks) {
            rows.push_back(NavRow{nav_slot_digit(mark.slot),
                                  nav_metres(options.camera, mark.tile_x, mark.tile_y)});
        }
    }
    if (rows.empty()) {
        refuse("no nav points are set.");
        return;
    }
    if (orderable().empty()) {
        refuse("You are driving that one; select another unit to give orders.");
        return;
    }
    nav_rows_ = std::move(rows);
    nav_list_armed_ = true;
}

void Interaction::disarm_nav_list() noexcept {
    nav_list_armed_ = false;
    nav_rows_.clear();
}

void Interaction::attack_from_menu(const sim::Sim& sim, const SceneOptions& options) {
    // The list appears and the next digit names the row to attack. This row is the only door onto
    // it: T was a second and now has another job, so there is ONE target list and one numbering.
    arm_attack_list(sim, options);
}

void Interaction::recycle_selection(const sim::Sim& sim) {
    const std::vector<sim::UnitId> roster = orderable();
    if (roster.empty()) {
        refuse("You are driving that one; select another unit to give orders.");
        return;
    }
    // The side's own recycler: the deployed base first (a side has one ever), else one still on
    // wheels. Lowest id wins if a test rig somehow has several.
    sim::UnitId recycler{};
    for (const sim::Unit& unit : sim.units()) {
        if (!unit.alive || unit.side != sim::Side::Player) {
            continue;
        }
        if (unit.kind == sim::UnitKind::DeployedRecycler) {
            recycler = unit.id;
            break;
        }
        if (unit.kind == sim::UnitKind::Recycler && !recycler.valid()) {
            recycler = unit.id;
        }
    }
    if (!recycler.valid()) {
        refuse("There is no recycler to recycle at.");
        return;
    }
    std::vector<sim::UnitId> units_to_recycle;
    for (const sim::UnitId id : roster) {
        if (id != recycler) {
            units_to_recycle.push_back(id);
        }
    }
    if (units_to_recycle.empty()) {
        refuse("The recycler cannot recycle itself.");
        return;
    }
    const std::vector<sim::Order> orders = cmd::recycle(units_to_recycle, recycler);
    if (orders.empty()) {
        refuse("That order cannot be carried out.");
        return;
    }
    remember(orders);
    pending_.insert(pending_.end(), orders.begin(), orders.end());
    message_ = "returning to the recycler.";
    selection_.clear(); // the sequence is complete: the menu closes (see `menu_digit`)
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
        if (nav_slot_digit(mark.slot) == number) {
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
    selection_.clear(); // the sequence is complete: the menu closes (see `menu_digit`)
}

void Interaction::arm_attack_list(const sim::Sim& sim, const SceneOptions& options) {
    // Captured once, here: the numbers on the screen are the numbers the next key means, however
    // the board moves in between. The driven tank is the centre, as it is the radar's, and the
    // reach is the radar's too -- and nothing the world view does not draw is named (VIS).
    std::vector<TargetRow> rows = nearest_enemies(sim, sim::Side::Player, driven_,
                                                  viewer_radar_radius(sim, driven_), &options);
    if (rows.empty()) {
        refuse("no enemies within radar range.");
        return;
    }
    // No need to clear the G arming here: the only way in is the `3` row, and a G arming eats the
    // `3` that would have got here (interaction.cpp's go_armed_ block runs first).
    targets_ = std::move(rows);
    attack_armed_ = true;
}

void Interaction::disarm_attack() noexcept {
    attack_armed_ = false;
    targets_.clear();
}

void Interaction::arm_repair_list(const sim::Sim& sim) {
    std::vector<sim::UnitId> trucks;
    for (const sim::UnitId id : orderable()) {
        const sim::Unit* unit = sim.find(id);
        if (unit != nullptr && unit->kind == sim::UnitKind::ServiceTruck) {
            trucks.push_back(id);
        }
    }
    if (trucks.empty()) {
        refuse("You are driving that one; select another unit to give orders.");
        return;
    }
    // Captured once, here, so the numbers on the screen are the numbers the next key means. The
    // centre is the driven craft and the reach is ITS radar (Harri: "whichever vehicle I'm in at
    // the time sets the radar range"), the same `viewer_radar_radius` the attack list uses.
    std::vector<TargetRow> rows = nearest_wounded(sim, sim::Side::Player, driven_,
                                                  viewer_radar_radius(sim, driven_), trucks);
    if (rows.empty()) {
        refuse("nothing within radar range needs repair.");
        return;
    }
    repairs_ = std::move(rows);
    repair_armed_ = true;
}

void Interaction::arm_construct() {
    if (orderable().empty()) {
        refuse("You are driving that one; select another unit to give orders.");
        return;
    }
    construct_armed_ = true;
}

void Interaction::disarm_construct() noexcept {
    construct_armed_ = false;
    placing_.reset(); // the armed building goes with the panel it was armed in
}

void Interaction::rotate_placing() noexcept {
    if (!placing_.has_value()) {
        return;
    }
    // A quarter turn: the same rectangle with its two sides swapped (a 4x2 becomes a 2x4), the
    // footprint the sim will test and stand the building on.
    placing_->turned = !placing_->turned;
    placing_->size = sim::FootprintSize{placing_->size.height, placing_->size.width};
    message_ = "Turned a quarter.";
}

void Interaction::place_at_crosshair(const sim::Sim& sim, const SceneOptions& options) {
    // A copy, checked once: what is placed is what was armed when SPACE was pressed.
    const std::optional<Placing> armed = placing_;
    if (!armed.has_value() || !options.aim.has_value()) {
        return; // no reticle, no ground: the arming waits for one
    }
    const Pick picked = pick(sim, options, options.aim->x, options.aim->y);
    if (!picked.has_tile()) {
        return; // the reticle is off the ground: SPACE places nothing, and the arming survives
    }
    // The SIM'S verdict, the one the preview is coloured from: a refusal here is out loud, charges
    // nothing and leaves the building armed so the player can aim elsewhere.
    if (!sim::place_ok(sim.seed(), sim.units(), armed->kind, armed->turned, picked.tile_x,
                       picked.tile_y)) {
        refuse("There is no room to build there.");
        return;
    }
    if (static_cast<int>(sim.ore(sim::Side::Player)) < armed->cost) {
        refuse_owned("Add "
                     + std::to_string(armed->cost - static_cast<int>(sim.ore(sim::Side::Player)))
                     + " scrap (have " + std::to_string(sim.ore(sim::Side::Player)) + ", costs "
                     + std::to_string(armed->cost) + ")");
        return;
    }
    std::vector<sim::UnitId> roster = orderable();
    if (roster.empty()) {
        refuse("You are driving that one; select another unit to give orders.");
        return;
    }
    std::sort(roster.begin(), roster.end());
    pending_.push_back(
        sim::Order{roster.front(),
                   sim::orders::Place{armed->kind, picked.tile_x, picked.tile_y, armed->turned}});
    placing_.reset(); // placed: the panel stays up, and the next row arms the next building
    message_ = "Building.";
}

void Interaction::construct_row(const sim::Sim& sim, const SceneOptions& options, int digit) {
    MenuInput in;
    in.subject = MenuSubject::Constructor;
    in.faction = faction_of(options.factions, sim::Side::Player);
    in.ore = static_cast<int>(sim.ore(sim::Side::Player));
    in.construct_panel = true;
    in.standing = standing_buildings(sim, sim::Side::Player);
    const std::vector<MenuRow> rows = menu_rows(in);
    const char label = static_cast<char>('0' + digit);
    const auto row = std::find_if(rows.begin(), rows.end(), [&label](const MenuRow& candidate) {
        return !candidate.heading && candidate.key == std::string_view{&label, 1};
    });
    if (row == rows.end()) {
        refuse("There is no building on that key.");
        return;
    }
    if (!row->enabled) {
        // Dim means inert, and the refusal names the obstacle: the ROW's own reason in words, so
        // the dim and the sentence cannot disagree (`refusal_message`, as slice K built it).
        refuse_owned(refusal_message(*row));
        return;
    }
    // A row that names a placeable building ARMS it: nothing is charged and no order is issued
    // until SPACE places it. The rows whose buildings are later slices carry no kind and still
    // refuse, said plainly so the press is never mistaken for a missed key.
    const std::span<const BuildItem> items = construct_items();
    const auto item
        = std::find_if(items.begin(), items.end(), [&label](const BuildItem& candidate) {
              return candidate.key == std::string_view{&label, 1};
          });
    // The row's kind, copied out once: a row with none is a later slice's name row.
    const sim::UnitKind row_kind
        = item != items.end() ? item->kind.value_or(sim::UnitKind::Tank) : sim::UnitKind::Tank;
    if (item != items.end() && item->kind.has_value() && sim::is_placeable(row_kind)) {
        // Re-arming the same building keeps the turn it already has.
        const bool kept_turn = placing_.has_value() && placing_.value_or(Placing{}).kind == row_kind
                               && placing_.value_or(Placing{}).turned;
        const sim::FootprintSize base = sim::footprint_of(row_kind);
        placing_ = Placing{row_kind, kept_turn, sim::spec_of(row_kind).cost,
                           kept_turn ? sim::FootprintSize{base.height, base.width} : base};
        message_ = "SPACE places it, ~ turns it.";
        return;
    }
    refuse("Nothing can be built yet. Placing a building is not done.");
}

void Interaction::disarm_repair() noexcept {
    repair_armed_ = false;
    repairs_.clear();
}

void Interaction::repair_row(const sim::Sim& sim, const std::vector<TargetRow>& rows, int key) {
    const auto row = std::find_if(rows.begin(), rows.end(), [key](const TargetRow& candidate) {
        return candidate.key == key;
    });
    if (row == rows.end()) {
        refuse("no patient on that key.");
        return;
    }
    // The board has moved since the list opened: a patient that has died, or been mended, is no
    // longer one.
    const sim::Unit* patient = sim.find(row->id);
    if (patient == nullptr || !patient->alive || patient->hp >= sim::spec_of(patient->kind).hp) {
        refuse("that one no longer needs repair.");
        return;
    }
    repair(sim, row->id);
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
    // The selection is read afresh: a unit may have died since the list was armed.
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
    selection_.clear(); // the sequence is complete: the menu closes (see `menu_digit`)
}

void Interaction::select_roster_key(const SceneOptions& options, int group_key) {
    // The key names the group the command row holds under it, and replaces whatever the mouse had
    // built. A key whose group is gone and a key that never held one are the same empty slot, so
    // one message covers both. Digits do not select: they are the mark and attack-list row keys
    // and the command menu's.
    if (options.row == nullptr || group_key < 1 || group_key > kRowGroups
        || !options.row->occupied(group_slot(group_key))) {
        refuse(empty_key_message(group_key));
        return;
    }
    selection_.clear();
    for (const sim::UnitId id : options.row->slots.at(group_slot(group_key)).units) {
        selection_.add(id);
    }
    group_key_ = group_key;
    stepping_ = false;
}

bool Interaction::selection_in_group(const sim::Sim& sim, const SceneOptions& options) const {
    if (selection_.empty() || options.row == nullptr || group_key_ < 1 || group_key_ > kRowGroups) {
        return false;
    }
    const std::vector<sim::UnitId>& units = options.row->slots.at(group_slot(group_key_)).units;
    return std::all_of(selection_.units().begin(), selection_.units().end(),
                       [&units, &sim](sim::UnitId id) {
                           const sim::Unit* unit = sim.find(id);
                           return unit == nullptr || !unit->alive
                                  || std::find(units.begin(), units.end(), id) != units.end();
                       });
}

void Interaction::open_unit_list(const sim::Sim& sim, const SceneOptions& options) {
    // Silent in every way it cannot act: no group, no row, a selection that is not that group's,
    // and a key with a single living unit (a group of one is already the one).
    if (!unit_key_holds_several(sim, options)) {
        return;
    }
    unit_rows_ = unit_list_rows(sim, options.row->slots.at(group_slot(group_key_)));
    unit_list_armed_ = !unit_rows_.empty();
}

void Interaction::unit_list_row(const sim::Sim& sim, const SceneOptions& options,
                                const std::vector<TargetRow>& rows, int key) {
    const auto row = std::find_if(rows.begin(), rows.end(), [key](const TargetRow& candidate) {
        return candidate.key == key;
    });
    if (row == rows.end()) {
        refuse("no unit on that key.");
        return;
    }
    // The board has moved since the list opened: a unit that has died, or left the key, is gone.
    const sim::Unit* unit = sim.find(row->id);
    const std::vector<sim::UnitId>& held = options.row->slots.at(group_slot(group_key_)).units;
    if (unit == nullptr || !unit->alive
        || std::find(held.begin(), held.end(), row->id) == held.end()) {
        refuse("that unit is gone.");
        return;
    }
    select_only(row->id);
    stepping_ = true;
}

void Interaction::disarm_unit_list() noexcept {
    unit_list_armed_ = false;
    unit_rows_.clear();
}

void Interaction::move_selection_to_key(const sim::Sim& sim, const SceneOptions& options,
                                        int group_key) {
    // Only a refusal speaks: the row redraws, so the moved key is its own feedback. The selection
    // is left as it is -- the units are the same units, they just sit under another key.
    const RowMove outcome = options.row == nullptr
                                ? RowMove::NothingToMove
                                : options.row->move_into(sim, selection_.units(), group_key);
    if (outcome != RowMove::Moved) {
        refuse(move_refusal_message(outcome));
    }
}

void Interaction::select_only(sim::UnitId id) noexcept {
    selection_.clear();
    selection_.add(id);
}

void Interaction::toggle(sim::UnitId id) noexcept { selection_.toggle(id); }

void Interaction::refuse(std::string_view why) noexcept {
    message_ = why;
    owned_ = false;
}

void Interaction::refuse_owned(std::string why) {
    owned_message_ = std::move(why);
    owned_ = true;
    message_ = std::string_view{};
}

} // namespace protekt::game
