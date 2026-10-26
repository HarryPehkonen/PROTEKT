#include "protekt/game/nav_marks.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "protekt/game/pick.hpp"
#include "protekt/input/bindings.hpp"

namespace protekt::game {
namespace {

/// The key column of a nav row, by slot: a literal per slot, because a row's key is a view.
constexpr std::array<std::string_view, kNavSlots> kSlotKeys{"1", "2", "3", "4", "5",
                                                            "6", "7", "8", "9", "0"};
static_assert(
    [] {
        for (int slot = 0; slot < kNavSlots; ++slot) {
            if (kSlotKeys.at(static_cast<std::size_t>(slot)).at(0) - '0' != nav_slot_digit(slot)) {
                return false;
            }
        }
        return true;
    }(),
    "a slot's label is its digit");

constexpr std::string_view kVerbGetNav{"GET NAV"};
constexpr std::string_view kVerbNav{"NAV"};

[[nodiscard]] std::string metres_text(int metres) { return std::to_string(metres) + "m"; }

[[nodiscard]] std::string nav_name(int slot) {
    return "Nav " + std::string{kSlotKeys.at(static_cast<std::size_t>(slot))};
}

[[nodiscard]] bool lit(const NavMarks& marks, int slot) {
    for (const NavMarks::Mark& mark : marks.marks()) {
        if (mark.slot == slot) {
            return mark.highlighted;
        }
    }
    return false;
}

} // namespace

NavMarks::Result NavMarks::place(int tile_x, int tile_y) {
    if (tile_x < 0 || tile_y < 0) {
        return Result{Outcome::NoGround, -1};
    }
    const int slot = next_;
    // Whatever the slot held is overwritten whole: its tile and its highlight both go.
    slots_.at(static_cast<std::size_t>(slot)) = Slot{true, tile_x, tile_y, false};
    next_ = (next_ + 1) % kNavSlots;
    return Result{Outcome::Placed, slot};
}

bool NavMarks::toggle(int slot) {
    if (slot < 0 || slot >= kNavSlots) {
        return false;
    }
    Slot& held = slots_.at(static_cast<std::size_t>(slot));
    if (!held.used) {
        return false;
    }
    held.highlighted = !held.highlighted;
    return true;
}

int NavMarks::slot_at(int tile_x, int tile_y) const {
    for (int slot = 0; slot < kNavSlots; ++slot) {
        const Slot& held = slots_.at(static_cast<std::size_t>(slot));
        if (held.used && held.tile_x == tile_x && held.tile_y == tile_y) {
            return slot;
        }
    }
    return -1;
}

std::vector<NavMarks::Mark> NavMarks::marks() const {
    std::vector<Mark> out;
    for (int slot = 0; slot < kNavSlots; ++slot) {
        const Slot& held = slots_.at(static_cast<std::size_t>(slot));
        if (held.used) {
            out.push_back(Mark{slot, held.tile_x, held.tile_y, held.highlighted});
        }
    }
    return out;
}

int nav_metres(const Camera& from, int tile_x, int tile_y) noexcept {
    const float dx = ((static_cast<float>(tile_x) + 0.5F) * kMetresPerTile) - from.x;
    const float dz = ((static_cast<float>(tile_y) + 0.5F) * kMetresPerTile) - from.z;
    return static_cast<int>(std::floor(std::hypot(dx, dz) + 0.5F));
}

NavAim nav_aim(const sim::Sim& sim, const SceneOptions& options) {
    if (!options.aim.has_value()) {
        return NavAim{};
    }
    const Pick ray = pick(sim, options, options.aim->x, options.aim->y);
    if (!ray.has_tile()) {
        return NavAim{};
    }
    return NavAim{ray.tile_x, ray.tile_y, nav_metres(options.camera, ray.tile_x, ray.tile_y)};
}

bool NavMenu::owned_while_open(const input::KeyEvent& key) noexcept {
    if (key.ctrl || key.alt) {
        return false;
    }
    switch (key.scancode) {
    case input::kKeySpace:
    case input::kKeyTab:
    case input::kKeyEscape:
    case input::kKeyGrave:
    case input::kKeyN:
    case input::kKeyG:
    case input::kKeyT:
        return true;
    default:
        break;
    }
    return input::mark_digit(key) >= 0 || input::function_key(key) != 0;
}

std::vector<input::KeyEvent> NavMenu::handle(std::span<const input::KeyEvent> keys, NavMarks& marks,
                                             const NavAim& aim, bool top_menu) {
    message_.clear();
    std::vector<input::KeyEvent> rest;
    for (const input::KeyEvent& key : keys) {
        if (!key.pressed) {
            rest.push_back(key);
            continue;
        }
        if (state_ == NavState::Closed) {
            // `n` and `~` are the top menu's: with another left menu open they do nothing at all.
            if (input::nav_arm_pressed(key)) {
                if (top_menu) {
                    state_ = NavState::Placing;
                }
            } else if (input::nav_menu_pressed(key)) {
                if (top_menu && !marks.marks().empty()) {
                    state_ = NavState::Menu;
                }
            } else {
                rest.push_back(key);
            }
            continue;
        }
        if (!owned_while_open(key)) {
            rest.push_back(key);
            continue;
        }
        if (key.scancode == input::kKeyTab || key.scancode == input::kKeyEscape) {
            state_ = NavState::Closed;
            continue;
        }
        if (state_ == NavState::Placing) {
            // Off the ground the state is drawn as nothing, and SPACE places nothing: a ray that
            // meets no ground never makes a mark. The arming survives, for when it is back.
            if (key.scancode == input::kKeySpace && aim.on_ground()) {
                const NavMarks::Result placed = marks.place(aim.tile_x, aim.tile_y);
                message_ = nav_name(placed.slot) + " placed.";
                state_ = NavState::Closed;
            }
            continue;
        }
        // The nav menu: SPACE over a mark and that mark's digit are ONE verb, a highlight toggle.
        int slot = -1;
        if (key.scancode == input::kKeySpace) {
            slot = aim.on_ground() ? marks.slot_at(aim.tile_x, aim.tile_y) : -1;
        } else if (const int digit = input::mark_digit(key); digit >= 0) {
            slot = nav_digit_slot(digit);
        }
        if (slot >= 0 && marks.toggle(slot)) {
            message_ = nav_name(slot) + (lit(marks, slot) ? " highlighted." : " plain.");
        }
    }
    return rest;
}

NavState NavMenu::state() const noexcept { return state_; }

std::string_view NavMenu::message() const noexcept { return message_; }

std::vector<MenuRow> nav_menu_rows(NavState state, const NavMarks& marks, const NavAim& aim,
                                   const Camera& from) {
    std::vector<MenuRow> rows;
    switch (state) {
    case NavState::Closed:
        break;
    case NavState::Placing:
        if (aim.on_ground()) {
            MenuRow get{kRowKeySpace, std::string{kVerbGetNav}, true, MenuDim::None};
            get.right = metres_text(aim.metres);
            rows.push_back(std::move(get));
        }
        break;
    case NavState::Menu: {
        const bool over_mark = aim.on_ground() && marks.slot_at(aim.tile_x, aim.tile_y) >= 0;
        rows.push_back(MenuRow{kRowKeySpace, over_mark ? std::string{kVerbSelect} : std::string{},
                               true, MenuDim::None});
        rows.push_back(MenuRow{kRowKeyTab, std::string{kVerbCloseMenu}, true, MenuDim::None});
        for (const NavMarks::Mark& mark : marks.marks()) {
            MenuRow row{kSlotKeys.at(static_cast<std::size_t>(mark.slot)), std::string{kVerbNav},
                        true, MenuDim::None};
            row.right = metres_text(nav_metres(from, mark.tile_x, mark.tile_y));
            rows.push_back(std::move(row));
        }
        break;
    }
    }
    return rows;
}

} // namespace protekt::game
