#include "protekt/game/session.hpp"

#include <algorithm>

#include "protekt/game/navigate.hpp"
#include "protekt/sim/commander.hpp"

namespace protekt::game {

Session::Session(sim::UnitId driven) noexcept : interaction_(driven) {}

void Session::frame(const input::InputFrame& frame, const sim::Sim& sim,
                    const SceneOptions& options, std::vector<sim::Order>& out) {
    if (!headed_) {
        // Face the open ground before the first frame: at seed 42 the player's tank spawns looking
        // straight at four crater tiles, and the simulation is right to refuse to drive into them.
        const sim::Unit* self = sim.find(interaction_.driven());
        if (self != nullptr) {
            player_.set_heading(
                clearest_heading(options.seed, self->x.floor_int(), self->y.floor_int()));
            headed_ = true;
        }
    }
    // A destroyed tank explains the one report a player made that looked like a stuck game: the
    // throttle did nothing and turning still worked. Say it plainly, once.
    if (!destroyed_ && sim.find(interaction_.driven()) == nullptr) {
        destroyed_ = true;
        destroyed_message_ = "your hover tank was destroyed -- the view is a spectator's now";
    }
    // The keyboard is a STATE, not a stream: a tank keeps moving while W is held, and the window
    // sends a key-down for every OS repeat, which must change nothing.
    for (const input::KeyEvent& key : frame.keys) {
        input::apply(keys_, key);
    }

    InputState input{};
    // The movement half of the control set, in one line: W/S is the throttle and A/D turns the
    // BODY; the mouse turns the turret on it (see PlayerCommander). The
    // clicks are in game/interaction.cpp and the table a player reads is README.md.
    input.throttle = destroyed_ ? 0.0F : (keys_.forward ? 1.0F : 0.0F) - (keys_.back ? 1.0F : 0.0F);
    // Increasing the yaw turns toward +X, which is the RIGHT of the screen, so A subtracts.
    input.turn = (keys_.right ? 1.0F : 0.0F) - (keys_.left ? 1.0F : 0.0F);
    // The same keys strafe a hover tank; the commander reads whichever fits the unit it drives.
    input.strafe = input.turn;
    // The mouse looks. Moving it right looks right, and moving it down looks down.
    input.look_yaw = frame.mouse_dx * kLookRadiansPerPixel;
    input.look_pitch = -frame.mouse_dy * kLookRadiansPerPixel;
    player_.set_input(input);

    // The driven tank's own order goes first, so the tank the player is sitting in has moved before
    // anything else is asked to. A destroyed tank is still thought through, for the view's sake;
    // it issues no order.
    const sim::Observation seen = sim::observe(sim, sim::Side::Player);
    player_.think(interaction_.driven(), seen, out);

    if (!destroyed_) {
        const sim::UnitId driven_before = interaction_.driven();
        interaction_.handle(frame, sim, options);
        if (interaction_.driven() != driven_before) {
            player_.reset_helm(); // a new tank starts from a standstill; the view is kept
        }
    }
    // The wounded go home after the clicks, so a retreat outranks an order given this frame. It
    // runs for a spectator too: the player's tanks do not stop looking after themselves when he
    // dies.
    interaction_.tend(seen);
    for (const sim::Order& order : interaction_.take_orders()) {
        out.push_back(order);
    }
}

bool Session::take_shot_voice() noexcept { return interaction_.take_shot_voice(); }

sim::UnitId Session::driven() const noexcept { return interaction_.driven(); }

float Session::heading() const noexcept { return player_.heading(); }

float Session::view_yaw() const noexcept { return player_.view_yaw(); }

float Session::view_pitch() const noexcept { return player_.view_pitch(); }

const cmd::Selection& Session::selection() const noexcept { return interaction_.selection(); }

std::string_view Session::message() const noexcept {
    if (!destroyed_message_.empty()) {
        return destroyed_message_;
    }
    return interaction_.message();
}

bool Session::destroyed() const noexcept { return destroyed_; }

const input::HeldKeys& Session::keys() const noexcept { return keys_; }

} // namespace protekt::game
