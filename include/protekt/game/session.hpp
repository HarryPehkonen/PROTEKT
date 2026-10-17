#pragma once

#include <string_view>
#include <vector>

#include "protekt/game/interaction.hpp"
#include "protekt/game/player.hpp"
#include "protekt/input/bindings.hpp"

namespace protekt::game {

/// The player's whole side of one frame: the tank he is sitting in, and the units he is commanding.
///
/// This exists because of a gap that mattered: the window opened, the simulation ran, and the tank
/// never moved, because nothing ever turned a keystroke into `PlayerCommander::set_input`. The loop
/// in `app/game.cpp` now pumps events, calls `frame`, and presents -- nothing else -- so the part
/// that used to be untestable has shrunk to the pumping.
///
/// One entry point, one ordering rule: the driven tank's own order comes first, then the commanded
/// units in ascending id order.
class Session {
public:
    explicit Session(sim::UnitId driven) noexcept;

    /// Turn one frame of input into orders, appended to `out`.
    void frame(const input::InputFrame& frame, const sim::Sim& sim, const SceneOptions& options,
               std::vector<sim::Order>& out);

    [[nodiscard]] sim::UnitId driven() const noexcept;
    [[nodiscard]] float heading() const noexcept;
    [[nodiscard]] const cmd::Selection& selection() const noexcept;
    /// The interaction's refusal, for a HUD: "you are driving that one", "nothing selected".
    [[nodiscard]] std::string_view message() const noexcept;
    [[nodiscard]] const input::HeldKeys& keys() const noexcept;

private:
    PlayerCommander player_;
    Interaction interaction_;
    input::HeldKeys keys_{};
    bool headed_{false}; ///< the spawn heading is chosen once, from the terrain, on the first frame
};

} // namespace protekt::game
