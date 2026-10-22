#include "protekt/game/nav_marks.hpp"

#include <string>

#include "protekt/game/pick.hpp"

namespace protekt::game {

NavMarks::Result NavMarks::press(int tile_x, int tile_y) {
    if (tile_x < 0 || tile_y < 0) {
        return Result{Outcome::NoGround, -1};
    }
    // Aimed at a tile that already holds a mark: that is a clear, never a second mark.
    for (int number = 0; number < kNavSlots; ++number) {
        Slot& slot = slots_.at(static_cast<std::size_t>(number));
        if (slot.used && slot.tile_x == tile_x && slot.tile_y == tile_y) {
            slot.used = false;
            return Result{Outcome::Cleared, number};
        }
    }
    for (int number = 0; number < kNavSlots; ++number) {
        Slot& slot = slots_.at(static_cast<std::size_t>(number));
        if (!slot.used) {
            slot = Slot{true, tile_x, tile_y};
            return Result{Outcome::Dropped, number};
        }
    }
    return Result{Outcome::Full, -1};
}

std::vector<NavMarks::Mark> NavMarks::marks() const {
    std::vector<Mark> out;
    for (int number = 0; number < kNavSlots; ++number) {
        const Slot& slot = slots_.at(static_cast<std::size_t>(number));
        if (slot.used) {
            out.push_back(Mark{number, slot.tile_x, slot.tile_y});
        }
    }
    return out;
}

std::string nav_press(NavMarks& marks, const sim::Sim& sim, const SceneOptions& options) {
    NavMarks::Result result{};
    if (options.aim.has_value()) {
        const Pick ray = pick(sim, options, options.aim->x, options.aim->y);
        result = marks.press(ray.tile_x, ray.tile_y);
    }
    switch (result.outcome) {
    case NavMarks::Outcome::Dropped:
        return "Nav mark " + std::to_string(result.number) + " dropped.";
    case NavMarks::Outcome::Cleared:
        return "Nav mark " + std::to_string(result.number) + " cleared.";
    case NavMarks::Outcome::Full:
        return "All ten nav marks are in use -- aim at one and press N to clear it first.";
    case NavMarks::Outcome::NoGround:
        break;
    }
    return "No ground under the crosshair in range -- no nav mark dropped.";
}

} // namespace protekt::game
