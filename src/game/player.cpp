#include "protekt/game/player.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "protekt/game/scene.hpp"

namespace protekt::game {

namespace {

/// One tick of an ease: moves `value` toward `target` by at most `step`, and settles exactly on the
/// target rather than overshooting it. Used once per tick -- see the loop in `think`, which is what
/// keeps the helm independent of the frame rate.
float eased(float value, float target, float step) noexcept {
    const float delta = target - value;
    if (delta > step) {
        return value + step;
    }
    if (delta < -step) {
        return value - step;
    }
    return target;
}

/// The signed shortest way round from `from` to `to`, in radians.
float angle_between(float from, float to) noexcept { return Camera::wrap_angle(to - from); }

} // namespace

PlayerCommander::PlayerCommander(sim::UnitId unit) : unit_(unit) {}

void PlayerCommander::set_input(const InputState& input) noexcept { input_ = input; }
const InputState& PlayerCommander::input() const noexcept { return input_; }
float PlayerCommander::heading() const noexcept { return heading_; }
float PlayerCommander::view_yaw() const noexcept { return view_yaw_; }
float PlayerCommander::view_pitch() const noexcept { return view_pitch_; }
float PlayerCommander::turn_rate() const noexcept { return turn_rate_; }
float PlayerCommander::throttle_rate() const noexcept { return throttle_rate_; }
sim::UnitId PlayerCommander::unit() const noexcept { return unit_; }

void PlayerCommander::set_heading(float radians) noexcept {
    heading_ = Camera::wrap_angle(radians);
    view_yaw_ = heading_;
}

void PlayerCommander::think(const sim::Observation& observation, std::vector<sim::Order>& out) {
    // The view first, and for a dead tank too: looking around from a wreck is the point of a
    // spectator. The mouse's radians for this frame are spent once, here.
    view_yaw_ = Camera::wrap_angle(view_yaw_ + input_.look_yaw);
    view_pitch_ = std::clamp(view_pitch_ + input_.look_pitch, -kLookPitchLimit, kLookPitchLimit);
    input_.look_yaw = 0.0F;
    input_.look_pitch = 0.0F;

    const sim::UnitView* self = nullptr;
    for (const auto& view : observation.own) {
        if (view.id == unit_) {
            self = &view;
            break;
        }
    }

    // How many ticks this frame is worth, read from the sim's own clock. The first observation has
    // no earlier tick to measure from, so it is worth one -- which is also what the old
    // once-per-call code did, and keeps a capture that steps one tick per frame identical.
    const sim::Tick elapsed = observation.tick > last_tick_ ? observation.tick - last_tick_ : 0;
    const std::uint32_t ticks = clocked_ ? static_cast<std::uint32_t>(elapsed) : 1U;
    clocked_ = true;
    last_tick_ = observation.tick;

    // One step of the helm per tick, IN THE LOOP. Advancing by `ticks * rate` instead would use the
    // rate the helm ended the frame at rather than the rate it had on each tick, and two ticks in
    // one frame would land the tank somewhere two single-tick frames would not -- a smaller version
    // of the same frame-rate dependence this is here to remove. A frame in which no tick ran turns
    // nothing: the loop does not run. The order below is still issued, because the tank's intent is
    // a property of the frame, and a waypoint re-stated is a no-op for the sim.
    for (std::uint32_t tick = 0; tick < ticks; ++tick) {
        // The helm eases toward the key state and the VIEW turns at the rate it has reached, so a
        // tap is a nudge rather than a full-lock lurch and letting go eases back to centre.
        turn_rate_ = eased(turn_rate_, input_.turn, kTurnEasePerTick);
        view_yaw_ = Camera::wrap_angle(view_yaw_ + (kTurnRatePerTick * turn_rate_));
        if (self == nullptr) {
            continue; // the hull is gone: it cannot be throttled or steered
        }
        throttle_rate_ = eased(throttle_rate_, input_.throttle, kThrottleEasePerTick);
        // The hull follows the view only while the tank is driven; a stopped tank sits still while
        // the view turns freely. Steering by looking is how a driver steers.
        if (throttle_rate_ != 0.0F) {
            heading_ = Camera::wrap_angle(heading_
                                          + std::clamp(angle_between(heading_, view_yaw_),
                                                       -kTurnRatePerTick, kTurnRatePerTick));
        }
    }
    if (self == nullptr) {
        // The tank is gone -- destroyed. The view above still turns; nothing else does, and no
        // order is issued.
        return;
    }

    // Yaw 0 points at +y in tile space; throttle scales the lookahead distance and flips it for
    // reverse.
    const float distance = kLookaheadTiles * throttle_rate_;
    const float target_x = static_cast<float>(self->x) + std::sin(heading_) * distance;
    const float target_y = static_cast<float>(self->y) + std::cos(heading_) * distance;

    out.push_back(
        sim::Order{unit_, sim::orders::MoveTo{sim::Fixed::from_raw(static_cast<std::int32_t>(
                                                  std::lround(target_x * sim::Fixed::kOne))),
                                              sim::Fixed::from_raw(static_cast<std::int32_t>(
                                                  std::lround(target_y * sim::Fixed::kOne)))}});
}

} // namespace protekt::game
