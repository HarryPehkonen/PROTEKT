#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "protekt/game/interaction.hpp"
#include "protekt/game/player.hpp"
#include "protekt/input/bindings.hpp"

namespace protekt::game {

/// The player's whole side of one frame: the tank he is sitting in, and the units he is commanding.
///
/// This exists because of a gap that mattered: the window opened, the simulation ran, and the tank
/// pumped by `app/game.cpp`, which calls `frame` between events and presentation -- so everything
/// that decides anything is here, under test, and the untestable part is only the pumping.
///
/// One entry point, one ordering rule: the driven tank's own order comes first, then the commanded
/// units in ascending id order.
class Session {
public:
    explicit Session(sim::UnitId driven) noexcept;

    /// Turn one frame of input into orders, appended to `out`.
    void frame(const input::InputFrame& frame, const sim::Sim& sim, const SceneOptions& options,
               std::vector<sim::Order>& out);

    /// Whether the last frame pulled the trigger: a bolt is queued as a FireTowards order, and the
    /// shot is heard through `shot_voice` -- the sim reports no Fired event for it.
    /// Drained by reading.
    [[nodiscard]] bool take_shot_voice() noexcept;

    [[nodiscard]] sim::UnitId driven() const noexcept;
    /// The hull's heading: where the tank points and drives. Only A/D turn it. Presentation state,
    /// never read by the sim.
    [[nodiscard]] float heading() const noexcept;
    /// Where the turret is looking, in radians: the hull's heading plus the mouse's turret angle.
    /// Alive after death, from the wreck's last heading.
    [[nodiscard]] float view_yaw() const noexcept;
    [[nodiscard]] float view_pitch() const noexcept;
    [[nodiscard]] const cmd::Selection& selection() const noexcept;
    /// The interaction's refusal, for a HUD: "you are driving that one", "nothing selected".
    [[nodiscard]] std::string_view message() const noexcept;
    [[nodiscard]] const input::HeldKeys& keys() const noexcept;
    /// True once the driven tank has been destroyed: the view is a spectator's from then on.
    [[nodiscard]] bool destroyed() const noexcept;

private:
    PlayerCommander player_;
    Interaction interaction_;
    input::HeldKeys keys_{};
    bool headed_{false}; ///< the spawn heading is chosen once, from the terrain, on the first frame
    bool destroyed_{false};
    std::string_view destroyed_message_{};
};

} // namespace protekt::game
