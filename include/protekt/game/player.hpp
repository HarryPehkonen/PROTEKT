#pragma once

#include <vector>

#include "protekt/sim/commander.hpp"
#include "protekt/sim/sim.hpp"

/// The player, as a commander.
///
/// This is the architecture claim made concrete: a human driving a tank enters the simulation
/// through the SAME interface and the same orders as the scripted opponent. Nothing in the sim
/// knows a human is present.
namespace protekt::game {

/// The helm's rates, all measured in SIM TICKS. Exposed here rather than hidden in the .cpp so
/// that a test asserts the property instead of restating the number.
inline constexpr float kTurnRatePerTick
    = 0.05F; ///< radians per tick at full lock: a tank, not a spin
inline constexpr float kLookaheadTiles = 8.0F;
/// The helm eases: the turn rate goes to the stop it is asked for, and comes off it, over a SECOND
/// rather than in one tick. The sim runs at 30 ticks per second (kTankSpeedRaw is 0.2 tiles/tick,
/// documented as 6 tiles/second), so 1/30 of full lock per tick is 30 ticks -- one second -- to
/// full lock and the same back to centre.
inline constexpr float kTurnEasePerTick = 1.0F / 30.0F;
/// The throttle eases the same way, over the same second.
inline constexpr float kThrottleEasePerTick = 1.0F / 30.0F;

/// Mouse look: radians of view per pixel of relative mouse movement. Exposed so a test asserts the
/// property (right turns the yaw up, down looks down) and not the number.
inline constexpr float kLookRadiansPerPixel = 0.0025F;
/// The view's pitch is held within +-this many radians: far enough to see the ground under the nose
/// and the sky above the horizon, and no further, so the view cannot roll over the top.
inline constexpr float kLookPitchLimit = 1.0F;
/// The pitch the view starts at: a slight downward tilt, the ground ahead beating empty sky.
inline constexpr float kLookPitchStart = -0.12F;

struct InputState {
    float throttle{}; ///< -1 (reverse) to +1 (full ahead)
    float turn{};     ///< -1 (left) to +1 (right)
    float look_yaw{}; ///< radians to add to the view's yaw this frame; spent by the next `think`
    float
        look_pitch{}; ///< radians to add to the view's pitch this frame; spent by the next `think`
    bool quit{};
};

class PlayerCommander final : public sim::ICommander {
public:
    explicit PlayerCommander(sim::UnitId unit);

    void set_input(const InputState& input) noexcept;
    [[nodiscard]] const InputState& input() const noexcept;
    /// The HULL's heading: where the tank points. It follows the view only while the throttle is
    /// non-zero.
    [[nodiscard]] float heading() const noexcept;
    /// Where the player is LOOKING, in radians. Free of the hull: A/D and the mouse turn this, and
    /// it keeps answering after the tank is destroyed.
    [[nodiscard]] float view_yaw() const noexcept;
    [[nodiscard]] float view_pitch() const noexcept;
    /// The eased helm state, -1..+1: what the turn and throttle have reached, as against what the
    /// keys are asking for. Exposed so a test can see the ease rather than infer it from a
    /// waypoint.
    [[nodiscard]] float turn_rate() const noexcept;
    [[nodiscard]] float throttle_rate() const noexcept;

    /// Point the tank somewhere before the player takes the wheel. Used once, at spawn, to choose a
    /// heading with ground ahead of it: the sim will not drive into a crater, and a player whose
    /// first keypress does nothing has been told the game is broken.
    void set_heading(float radians) noexcept;
    [[nodiscard]] sim::UnitId unit() const noexcept;

    /// One order per TICK, for its own unit and no other: a `MoveTo` to a waypoint ahead of the
    /// tank. Turning is applied here, because the sim moves a unit toward a destination and has no
    /// throttle -- the tank follows the waypoint.
    ///
    /// Everything here is clocked by the sim tick read from the observation, never by the call. The
    /// frame loop steps the sim zero, one or two times per frame depending on the wall clock, so a
    /// value advanced once per call is advanced once per FRAME: a turn rate written as radians per
    /// tick became radians per frame, which at 120 frames per second is four times the tank's turn
    /// rate and exactly the "aiming is difficult, the turn happens so quickly" report. A frame in
    /// which no tick ran therefore turns nothing -- the order is still issued, because the tank's
    /// intent is a property of the frame.
    ///
    /// It never issues an `Attack`. An explicit attack target outranks auto-engagement and makes
    /// the unit PURSUE, which would take the steering wheel out of the player's hands; the player's
    /// weapons still fire in range, because that is the sim's job. Attack-on-command is the
    /// commander layer's (M4).
    void think(const sim::Observation& observation, std::vector<sim::Order>& out) override;

private:
    sim::UnitId unit_;
    InputState input_{};
    float heading_{};
    float view_yaw_{};
    float view_pitch_{kLookPitchStart};
    float turn_rate_{};     ///< eased -1..+1, what the helm has reached
    float throttle_rate_{}; ///< eased -1..+1, what the throttle has reached
    sim::Tick last_tick_{};
    bool clocked_{false}; ///< false until the first observation gives the clock a starting point
};

} // namespace protekt::game
