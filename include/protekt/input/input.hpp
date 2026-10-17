#pragma once

#include <cstdint>
#include <vector>

/// What the window collected since the last frame, in a form that knows nothing about the window.
///
/// This header is the seam. `render::Window` fills it from SDL, `app::run_game` consumes it, and
/// neither has to know about the other -- which is what lets the picking and the selection be
/// tested on a machine with no display at all.
namespace protekt::input {

enum class MouseButton : std::uint8_t { Left, Right, Middle };

/// SDL3 scancodes for the keys the game acts on. Named here so the game layer and its tests agree
/// about a key without either of them including SDL (these are USB HID usage codes, which is what
/// SDL scancodes are).
inline constexpr std::int32_t kKeyEscape = 41;
inline constexpr std::int32_t kKeyTab = 43;

struct Click {
    int x{}; ///< pixels from the left of the framebuffer
    int y{}; ///< pixels from the TOP of the framebuffer
    MouseButton button{MouseButton::Left};
    bool pressed{};
    bool shift{}; ///< Shift was held: a click that adds to a selection rather than replacing it
};

/// A physical key, by SDL scancode. Opaque here on purpose: this header must not include SDL, or
/// every test that touches input needs a display stack to compile.
struct KeyEvent {
    std::int32_t scancode{};
    bool pressed{};
    bool shift{}; ///< Shift was held: Shift-Tab cycles the other way
};

struct InputFrame {
    std::vector<Click> clicks;
    std::vector<KeyEvent> keys;
    float mouse_dx{}; ///< relative motion this frame, pixels (positive = right)
    float mouse_dy{}; ///< relative motion this frame, pixels (positive = down)
    bool quit{};

    void clear() noexcept {
        clicks.clear();
        keys.clear();
        mouse_dx = 0.0F;
        mouse_dy = 0.0F;
        quit = false;
    }
};

} // namespace protekt::input
