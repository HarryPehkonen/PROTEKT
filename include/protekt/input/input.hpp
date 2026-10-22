#pragma once

#include <cstdint>
#include <vector>

/// What the window collected since the last frame, in a form that knows nothing about the window.
///
/// This header is the seam. `render::Window` fills it from SDL, `app::run_game` consumes it, and
/// neither has to know about the other -- which is what lets the picking and the selection be
/// tested on a machine with no display at all.
namespace protekt::input {

/// MouseButton and the key scancodes below are the whole vocabulary of input. What each one DOES is
/// decided in three places -- `bindings.hpp` folds the movement keys, `game/session.cpp` gives held
/// keys a throttle, and `game/interaction.cpp` takes the clicks -- and the table a player reads is
/// README.md, "How to play".
enum class MouseButton : std::uint8_t { Left, Right, Middle };

/// SDL3 scancodes for the keys the game acts on. Named here so the game layer and its tests agree
/// about a key without either of them including SDL (these are USB HID usage codes, which is what
/// SDL scancodes are).
inline constexpr std::int32_t kKeyM = 16;
inline constexpr std::int32_t kKeyN = 17;
inline constexpr std::int32_t kKeyQ = 20;
inline constexpr std::int32_t kKeyT = 23;
inline constexpr std::int32_t kKeyEscape = 41;
inline constexpr std::int32_t kKeyTab = 43;
inline constexpr std::int32_t kKeySpace = 44;
inline constexpr std::int32_t kKeyEnter = 40;
inline constexpr std::int32_t kKeyF11 = 68;

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
    bool ctrl{};  ///< Ctrl was held: Ctrl+Q quits, and nothing else uses it yet
    bool alt{};   ///< Alt was held, for Alt+Enter as a second way to ask for full screen
};

struct InputFrame {
    std::vector<Click> clicks;
    std::vector<KeyEvent> keys;
    float mouse_dx{}; ///< relative motion this frame, pixels (positive = right)
    float mouse_dy{}; ///< relative motion this frame, pixels (positive = down)
    /// Mouse-wheel detents this frame, whole notches: positive is wheel-up (away from the user, as
    /// SDL reports `y`), negative wheel-down. A fractional scroll (a trackpad) is accumulated by
    /// the window until it makes a whole notch, so one notch is exactly one count here.
    int wheel{};
    /// Where the pointer is, in framebuffer pixels -- from the left, from the TOP -- and whether
    /// anything has ever said. A POSITION, not an event, so `clear()` leaves it alone: a frame with
    /// no mouse event must not forget where the pointer is. It is false for a run that has no
    /// pointer at all, which is how a headless or `--autodrive` frame stays byte-identical to what
    /// it drew before there was anything to draw here.
    int cursor_x{};
    int cursor_y{};
    bool cursor_known{};
    bool quit{};

    void clear() noexcept {
        clicks.clear();
        keys.clear();
        mouse_dx = 0.0F;
        mouse_dy = 0.0F;
        wheel = 0;
        quit = false;
    }
};

} // namespace protekt::input
