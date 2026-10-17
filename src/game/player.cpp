#include "protekt/game/player.hpp"

#include <cmath>
#include <cstdint>

#include "protekt/game/scene.hpp"

namespace protekt::game {

namespace {
constexpr float kTurnRatePerTick = 0.05F; ///< radians per tick at full lock: a tank, not a spin
constexpr float kLookaheadTiles = 8.0F;
} // namespace

PlayerCommander::PlayerCommander(sim::UnitId unit) : unit_(unit) {}

void PlayerCommander::set_input(const InputState& input) noexcept { input_ = input; }
const InputState& PlayerCommander::input() const noexcept { return input_; }
float PlayerCommander::heading() const noexcept { return heading_; }
sim::UnitId PlayerCommander::unit() const noexcept { return unit_; }

void PlayerCommander::set_heading(float radians) noexcept {
    heading_ = Camera::wrap_angle(radians);
}

void PlayerCommander::think(const sim::Observation& observation, std::vector<sim::Order>& out) {
    const sim::UnitView* self = nullptr;
    for (const auto& view : observation.own) {
        if (view.id == unit_) {
            self = &view;
            break;
        }
    }
    if (self == nullptr) {
        // The tank is gone -- destroyed. Turning the camera here is what made a destroyed player
        // believe the game had stopped responding to the throttle but not to the wheel: the heading
        // moved, the hull no longer existed, so nothing else did.
        return;
    }

    heading_ = Camera::wrap_angle(heading_ + kTurnRatePerTick * input_.turn);

    // Yaw 0 points at +y in tile space; throttle scales the lookahead distance and flips it for
    // reverse.
    const float distance = kLookaheadTiles * input_.throttle;
    const float target_x = static_cast<float>(self->x) + std::sin(heading_) * distance;
    const float target_y = static_cast<float>(self->y) + std::cos(heading_) * distance;

    out.push_back(
        sim::Order{unit_, sim::orders::MoveTo{sim::Fixed::from_raw(static_cast<std::int32_t>(
                                                  std::lround(target_x * sim::Fixed::kOne))),
                                              sim::Fixed::from_raw(static_cast<std::int32_t>(
                                                  std::lround(target_y * sim::Fixed::kOne)))}});
}

} // namespace protekt::game
