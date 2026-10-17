#pragma once

#include <vector>

#include "protekt/ai/commander.hpp"
#include "protekt/sim/sim.hpp"

/// The player, as a commander.
///
/// This is the architecture claim made concrete: a human driving a tank enters the simulation
/// through the SAME interface and the same orders as the scripted opponent. Nothing in the sim
/// knows a human is present.
namespace protekt::game {

struct InputState {
    float throttle{};   ///< -1 (reverse) to +1 (full ahead)
    float turn{};       ///< -1 (left) to +1 (right)
    float look_yaw{};   ///< radians to add to the camera's yaw this frame
    float look_pitch{}; ///< radians to add to the camera's pitch this frame
    bool quit{};
};

class PlayerCommander final : public ai::ICommander {
public:
    explicit PlayerCommander(sim::UnitId unit);

    void set_input(const InputState& input) noexcept;
    [[nodiscard]] const InputState& input() const noexcept;
    [[nodiscard]] float heading() const noexcept;
    [[nodiscard]] sim::UnitId unit() const noexcept;

    /// One order per tick, for its own unit and no other: a `MoveTo` to a waypoint ahead of the
    /// tank. Turning is applied here, because the sim moves a unit toward a destination and has no
    /// throttle -- the tank follows the waypoint.
    ///
    /// It never issues an `Attack`. An explicit attack target outranks auto-engagement and makes
    /// the unit PURSUE, which would take the steering wheel out of the player's hands; the player's
    /// weapons still fire in range, because that is the sim's job. Attack-on-command is the
    /// commander layer's (M4).
    void think(const ai::Observation& observation, std::vector<sim::Order>& out) override;

private:
    sim::UnitId unit_;
    InputState input_{};
    float heading_{};
};

} // namespace protekt::game
