#pragma once

#include <string_view>
#include <vector>

#include "protekt/audio/mixer.hpp"
#include "protekt/cmd/selection.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/orders.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::game {

/// The gun's rate limit on the player's BUTTON: at most one manual shot per this many sim ticks (8
/// is just over a quarter of a second). A shot the sim fires itself -- auto-engagement, a FocusFire
/// already ordered -- is not on this timer.
inline constexpr sim::Tick kManualShotIntervalTicks = 8;

/// The voice of a shot fired at nothing: the player's own gun, full gain, centred. The same voice
/// `sounds_for` gives a shot the sim fires from the player's side.
[[nodiscard]] audio::Voice shot_at_nothing_voice() noexcept;

/// The player's hands, as a pure function of input and what he can see.
///
/// Everything the mouse and the keyboard mean lives here and NOWHERE else: which unit a click
/// selects, what a right click orders, what Tab cycles. No window, no SDL, no clock, no GPU -- so
/// the whole of "what does that click do" is tested on a machine with no display, and a recorded
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
    ///    `kManualShotIntervalTicks`. On any unit, hostile or ours, it is a FocusFire from the
    ///    driven tank; on the ground or the sky it is a FireTowards along the turret's aim, and the
    ///    shot is also to be heard (`take_shot_at_nothing`). It never changes the selection.
    ///  * Space / Shift-Space: select the own unit under the mark / toggle it
    ///  * right click on the ground with a selection: MoveTo that tile, for every selected unit
    ///  that
    ///    is not the driven one
    ///  * right click on a hostile unit with a selection: Attack it, same roster rule
    ///  * right click with nothing selected, or with only the driven unit selected: refused
    ///  * Tab / Shift-Tab: cycle the selection through our units, ascending id, wrapping
    ///  * Escape: clear the selection
    ///
    /// Refusals never throw and never half-apply: either the whole intent is carried out or none of
    /// it is, and `message()` says why.
    void handle(const input::InputFrame& frame, const sim::Sim& sim, const SceneOptions& options);

    /// Drain the orders this input implied. Empty when nothing was asked, or when the last thing
    /// that was asked could not be done. Units appear in ascending id order, because `cmd::` verbs
    /// build them that way and the order the player clicked in must not change what the sim sees.
    [[nodiscard]] std::vector<sim::Order> take_orders() noexcept;

    /// Whether the last `handle` pulled the trigger at nothing: a bolt is on the order queue
    /// for it (a FireTowards), and this is the shot to play `shot_at_nothing_voice` for. Drained by
    /// reading, like the orders.
    [[nodiscard]] bool take_shot_at_nothing() noexcept;

    [[nodiscard]] const cmd::Selection& selection() const noexcept;
    [[nodiscard]] sim::UnitId driven() const noexcept;

    /// Why the last input was refused, for the HUD. Empty when it was carried out. Cleared at the
    /// start of every `handle`, so a message describes the frame that just happened and nothing
    /// else.
    [[nodiscard]] std::string_view message() const noexcept;

private:
    /// Units in the selection that may be ordered: everything except the driven tank, and nothing
    /// at all if that is all there is.
    [[nodiscard]] std::vector<sim::UnitId> orderable() const;

    void select_only(sim::UnitId id) noexcept;
    void toggle(sim::UnitId id) noexcept;
    void cycle(bool backwards, const sim::Sim& sim);
    void refuse(std::string_view why) noexcept;

    sim::UnitId driven_;
    cmd::Selection selection_;
    std::vector<sim::Order> pending_;
    bool shot_at_nothing_{false};
    bool has_fired_{false};
    sim::Tick last_shot_tick_{};
    std::string_view message_;
};

} // namespace protekt::game
