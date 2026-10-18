#include "protekt/game/aim.hpp"

#include <algorithm>

namespace protekt::game {

std::optional<Aim> aim_from(const input::InputFrame& frame, render::Extent extent) noexcept {
    if (!frame.cursor_known || extent.width <= 0 || extent.height <= 0) {
        return std::nullopt;
    }
    const float middle = static_cast<float>(extent.width) / 2.0F;
    const float lowest = static_cast<float>(extent.height - 1);
    return Aim{middle, std::clamp(static_cast<float>(frame.cursor_y), 0.0F, lowest)};
}

} // namespace protekt::game
