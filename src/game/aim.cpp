#include "protekt/game/aim.hpp"

namespace protekt::game {

std::optional<Aim> aim_from(const input::InputFrame& /*frame*/, Extent extent) noexcept {
    // The pointer is captured for look and the player aims by looking, so the mark is the centre of
    // the frame and the cursor means nothing to it.
    if (extent.width <= 0 || extent.height <= 0) {
        return std::nullopt;
    }
    return Aim{static_cast<float>(extent.width) / 2.0F, static_cast<float>(extent.height) / 2.0F};
}

} // namespace protekt::game
