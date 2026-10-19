#include "protekt/game/session.hpp"

#include <algorithm>

#include "protekt/game/navigate.hpp"
#include "protekt/sim/commander.hpp"

namespace protekt::game {

Session::Session(sim::UnitId driven) noexcept : player_(driven), interaction_(driven) {}

void Session::frame(const input::InputFrame& frame, const sim::Sim& sim,
                    const SceneOptions& options, std::vector<sim::Order>& out) {
    if (!headed_) {
        // Face the open ground before the first frame: at seed 42 the player's tank spawns looking
        // straight at four crater tiles, and the simulation is right to refuse to drive into them.
        const sim::Unit* self = sim.find(player_.unit());
        if (self != nullptr) {
            player_.set_heading(
                clearest_heading(options.seed, self->x.floor_int(), self->y.floor_int()));
            headed_ = true;
        }
    }
    // A destroyed tank explains the one report a player made that looked like a stuck game: the
    // throttle did nothing and turning still worked. Say it plainly, once.
    if (!destroyed_ && sim.find(player_.unit()) == nullptr) {
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
    // VIEW. The hull follows the view only while the throttle is held (see PlayerCommander). The
    // clicks are in game/interaction.cpp and the table a player reads is README.md.
    input.throttle = destroyed_ ? 0.0F : (keys_.forward ? 1.0F : 0.0F) - (keys_.back ? 1.0F : 0.0F);
    // Sign matters, and it is not the obvious one: A must turn the view to the LEFT ON SCREEN.
    // `look_at` builds its right vector as cross(forward, up), which at yaw 0 (forward = +z, up =
    // +Y) is -X -- so tile +x is drawn on the left of the screen, and increasing the yaw is a
    // SCREEN-left turn. Pressing A therefore adds to the yaw. The real fix is to stop mirroring
    // the world (see docs/PLAN.md, "The world is drawn mirrored"), which would flip this line back.
    input.turn = (keys_.left ? 1.0F : 0.0F) - (keys_.right ? 1.0F : 0.0F);
    // The mouse looks. Moving it right looks right, and moving it down looks down.
    // The yaw sign is the same non-obvious one as A/D above: the world is drawn mirrored, so
    // increasing the yaw is a SCREEN-left turn and a rightward mouse move must SUBTRACT. This is
    // the line `docs/PLAN.md` "The world is drawn mirrored" flips back when the mirror goes.
    input.look_yaw = -frame.mouse_dx * kLookRadiansPerPixel;
    input.look_pitch = -frame.mouse_dy * kLookRadiansPerPixel;
    player_.set_input(input);

    // The driven tank's own order goes first, so the tank the player is sitting in has moved before
    // anything else is asked to. A destroyed tank is still thought through, for the view's sake;
    // it issues no order.
    player_.think(sim::observe(sim, sim::Side::Player), out);
    if (destroyed_) {
        return;
    }

    interaction_.handle(frame, sim, options);
    for (const sim::Order& order : interaction_.take_orders()) {
        out.push_back(order);
    }
}

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
