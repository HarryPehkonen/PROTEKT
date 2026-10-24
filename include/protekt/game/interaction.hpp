#pragma once

#include <map>
#include <set>
#include <string_view>
#include <vector>

#include "protekt/audio/mixer.hpp"
#include "protekt/cmd/selection.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/targets.hpp"
#include "protekt/game/weapon.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/orders.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::game {

/// The gun's rate limit on the player's BUTTON: at most one manual shot per this many sim ticks (8
/// is just over a quarter of a second). A shot the sim fires itself -- auto-engagement, a FocusFire
/// already ordered -- is not on this timer.
inline constexpr sim::Tick kManualShotIntervalTicks = 8;

/// The beat the idle tanks think their defence on: every 15th sim tick, the cadence the AI's
/// controllers use. Between beats nothing is emitted; the sim's own aim and cooldown carry on.
inline constexpr sim::Tick kDefendCadence = 15;

/// What the HUD says while G is armed. Shown the whole time `Interaction::go_armed()` is true.
inline constexpr std::string_view kGoPrompt = "go where? 0-9 for a mark.";

/// The voice of a manual shot: the player's own gun, full gain, centred. Every trigger pull makes
/// this sound, whatever it was aimed at, because the sim reports no `Fired` event for a shot the
/// player asked for. The same voice `sounds_for` gives a shot the sim fires from the player's side.
[[nodiscard]] audio::Voice shot_voice() noexcept;

/// The player's hands, as a pure function of input and what he can see.
///
/// Everything the mouse and the keyboard mean lives here and NOWHERE else: which unit a click
/// selects, what a right click orders, what Escape clears. No window, no SDL, no clock, no GPU --
/// so the whole of "what does that click do" is tested on a machine with no display, and a recorded
/// frame of input replays exactly.
///
/// The one thing it will not do is order the tank the player is sitting in. That unit is driven by
/// `PlayerCommander` from the keyboard every tick; an order from a click would fight it, and the
/// player would be fighting his own autopilot. Selecting it is allowed -- the player should see
/// what he is in -- and an order that would have moved it is refused, out loud, with a reason.
class Interaction {
public:
    /// `driven` is the unit the player is in. Selecting it is allowed; ordering it is not.
    explicit Interaction(sim::UnitId driven) noexcept;

    /// Consume one frame of input against the world as it is now.
    ///
    ///  * left click: THE TRIGGER, whatever is under the mark, at most once per
    ///    `kManualShotIntervalTicks`. It is always one FireTowards from the driven tank, along the
    ///    turret's aim: a unit under the mark is named in `aimed_at` as intent and does not bend
    ///    the path. The shot is also to be heard (`take_shot_voice`). It never changes the
    ///    selection.
    ///  * Space / Shift-Space: select the own unit under the mark / toggle it
    ///  * right click: THE WEAPON SWITCH, one step of `next_weapon`'s wrap. It needs no mark, and
    ///    it is the ONLY switch since 2026-10-23 (`Q` and the mouse wheel stopped switching). The
    ///    orders this button used to give -- MoveTo the ground, Attack a hostile -- live on `M`,
    ///    `T` and `G` below
    ///  * M with a selection: MoveTo the ground under the mark for every selected unit that is not
    ///    the driven one, even with a unit under the mark; nothing selected: refused
    ///  * X: swap the driven tank to the own tank under the mark; the vacated one is told to Stop
    ///  * F1-F10: select the whole roster GROUP that key belongs to, wherever its units are, on
    ///    screen or not (the roster strip, `game/roster.hpp`), replacing whatever the mouse had
    ///    built; a key no group holds is FREE -- the pool keeps no memory of a returned key -- and
    ///    one message covers "that group is gone" and "you never had that group" alike. The key is
    ///    a GROUP's, not a unit's (N2); digits stopped selecting in 160dec4 and are now the mark
    ///    row, the attack-list row, and -- with no prompt open -- the command menu's rows
    ///  * G, then 0-9 (N3): send the selection to that nav mark's tile -- the same MoveTo `M`
    ///    makes. G arms; the next key settles it. A digit is then a MARK number, never N2's
    ///    selection key: a number with no mark is refused and orders nothing. Any digit uses the
    ///    arming up; Escape cancels it (and is spent doing so: the selection stays); any other key
    ///    cancels it and then does its own job; an order by key (`M`) cancels it. The trigger
    ///    leaves it armed, and so does a weapon click: neither is an order.
    ///  * T with a hostile under the mark: Attack it, as above. T with none: arm the attack list of
    ///    the driven tank's nearest hostiles within the radar's radius (`nearest_enemies`), unless
    ///    there are none (refused). The next 0-9 attacks that row for the selection.
    ///  * a digit with NO prompt open (RB-DIGITS): the row of the selected subject's command menu
    ///    that key names, but only the rows that can act yet, and only when `menu_rows` says the
    ///    row is enabled. `FOLLOW ME` orders the selection to follow the craft being driven;
    ///    `HOLD` posts each selected unit where it stands (GuardAt at its own position -- a bare
    ///    Stop would not hold a harvester, whose fetch starts again the same tick). Every other
    ///    row, and every digit with no menu, does nothing. A `G` or `T` prompt, armed, swallows
    ///    the digit before the menu sees it.
    ///  * Tab or Escape: clear the selection
    ///
    /// Refusals never throw and never half-apply: either the whole intent is carried out or none of
    /// it is, and `message()` says why.
    void handle(const input::InputFrame& frame, const sim::Sim& sim, const SceneOptions& options);

    /// The retreat -> repair -> return loop, and nothing else. Every own, living, non-driven tank
    /// at or below `UtilityTuning::retreat_hp` is ordered to its home factory (none alive: it keeps
    /// fighting), every frame, over whatever it was doing; once it is back at or above `return_hp`
    /// it is re-issued the last order the player gave it (none remembered: it stays idle). The
    /// driven tank is never touched: the player is its judgement. Appends to the pending orders, so
    /// call it after `handle` -- a retreat outranks an order given the same frame.
    ///
    /// And the defence: on every `kDefendCadence`th tick an own, living, non-driven tank with NO
    /// remembered player order, not retreating and above the retreat line is told to FocusFire the
    /// nearest hostile within weapon range it has a line to (squared distance in Q16.16, ties to
    /// the lower id). FocusFire sets no stance and no destination: the tank fires from where it
    /// stands, never pursues, and a target that leaves range is simply not shot at. It never
    /// overrides an order the player gave.
    void tend(const sim::Observation& own_side);

    /// Drain the orders this input implied. Empty when nothing was asked, or when the last thing
    /// that was asked could not be done. Units appear in ascending id order, because `cmd::` verbs
    /// build them that way and the order the player clicked in must not change what the sim sees.
    [[nodiscard]] std::vector<sim::Order> take_orders() noexcept;

    /// Whether the last `handle` pulled the trigger: a bolt is on the order queue for it (a
    /// FireTowards), and this is the shot to play `shot_voice` for. The sim reports no
    /// Fired event for a manual shot, so this is the only sound it makes. Drained by reading, like
    /// the orders.
    [[nodiscard]] bool take_shot_voice() noexcept;

    /// The sim launched no shot for the trigger the last frame pulled: say why (`trigger_refusal`).
    /// Called by the app after the step, once the sim has answered; the message then stands until
    /// the next `handle` clears it.
    void trigger_refused(const sim::Sim& sim) noexcept;

    [[nodiscard]] const cmd::Selection& selection() const noexcept;
    [[nodiscard]] sim::UnitId driven() const noexcept;
    /// The weapon the driven craft has up. The craft carries the set, so an X swap keeps it.
    [[nodiscard]] Weapon weapon() const noexcept;
    /// G has been pressed and the digit that names its mark has not: the next 0-9 is a MARK number
    /// (N3), and the HUD shows `kGoPrompt` for as long as this is true.
    [[nodiscard]] bool go_armed() const noexcept;
    /// T has been pressed with no hostile under the crosshair and the key that names a row has not:
    /// the next 0-9 is a ROW of `attack_targets()` (BZ2A), and the HUD shows the list for as long
    /// as this is true. Settled like G: a digit uses it up, Escape cancels it (and is spent doing
    /// so), any other key cancels it and then does its own job, a right-click order cancels it.
    [[nodiscard]] bool attack_armed() const noexcept;
    /// The rows the armed list offers, nearest first, captured when T armed it so the numbers
    /// cannot shift under the player's finger. Empty whenever the list is not armed.
    [[nodiscard]] const std::vector<TargetRow>& attack_targets() const noexcept;

    /// Why the last input was refused, for the HUD. Empty when it was carried out. Cleared at the
    /// start of every `handle`, so a message describes the frame that just happened and nothing
    /// else.
    [[nodiscard]] std::string_view message() const noexcept;

private:
    /// Units in the selection that may be ordered: everything except the driven tank, and nothing
    /// at all if that is all there is.
    [[nodiscard]] std::vector<sim::UnitId> orderable() const;

    /// X: drive the own, living, other tank under the crosshair; refuse with a reason otherwise.
    void swap_to_crosshair(const sim::Sim& sim, const SceneOptions& options);
    void move_to_crosshair_ground(const sim::Sim& sim, const SceneOptions& options);
    void follow_crosshair_unit(const sim::Sim& sim, const SceneOptions& options);
    /// F1-F10: select the whole roster group that key belongs to (N2). The key names a GROUP, not a
    /// unit, and a key no group holds is refused with `empty_key_message`.
    void select_roster_key(const sim::Sim& sim, int group_key);
    /// A digit with no prompt open: the menu row it names, if that row acts and the model says it
    /// is enabled; otherwise nothing at all (RB-DIGITS).
    void menu_digit(const sim::Sim& sim, int digit);
    /// `1 FOLLOW ME`: every orderable selected unit follows the driven craft. Refused when there is
    /// no living craft, or nothing but that craft is selected.
    void follow_driven(const sim::Sim& sim);
    /// `4 HOLD`: every orderable selected unit is posted where it stands, one GuardAt each.
    void hold_selection(const sim::Sim& sim);
    /// G then a digit: MoveTo the tile of mark `number` for every orderable selected unit (N3).
    void send_to_mark(int number, const SceneOptions& options);
    /// T with nothing hostile under the crosshair: capture the nearest hostiles and arm the list,
    /// or refuse when there are none. A refusal leaves nothing armed.
    void arm_attack_list(const sim::Sim& sim);
    void disarm_attack() noexcept;
    /// The key that settles the armed list: Attack the row it names for every orderable selected
    /// unit, or refuse when no row has that key.
    void attack_row(const std::vector<TargetRow>& rows, int key);
    void select_only(sim::UnitId id) noexcept;
    void toggle(sim::UnitId id) noexcept;
    void refuse(std::string_view why) noexcept;

    /// Record what the player ordered `orders` to do, for the return leg: a Stop forgets, and the
    /// driven tank is never recorded.
    void remember(const std::vector<sim::Order>& orders);

    sim::UnitId driven_;
    std::map<sim::UnitId, sim::OrderPayload> remembered_; ///< each unit's last player order
    std::set<sim::UnitId> retreating_;                    ///< tanks sent home and not yet healed
    cmd::Selection selection_;
    Weapon weapon_{Weapon::Gun};
    std::vector<sim::Order> pending_;
    bool shot_voice_{false};
    bool go_armed_{false};
    bool attack_armed_{false};
    std::vector<TargetRow> targets_{};
    bool has_fired_{false};
    sim::Tick last_shot_tick_{};
    std::string_view message_;
};

} // namespace protekt::game
