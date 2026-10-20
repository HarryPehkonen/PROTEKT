#pragma once

#include <cstdint>

#include "protekt/input/input.hpp"

/// Which keys are down, and what they mean. Movement only: the clicks and the command keys are in
/// `game/interaction.cpp`, the throttle rule in `game/session.cpp`, and the player-facing table in
/// README.md, "How to play". The first half is here (SDL scancodes in, a set of held
/// keys out); the second half lives in `game/session.cpp`, because "W means go forward" is a game
/// rule, not an input rule.
namespace protekt::input {

/// USB HID usage codes, which is what SDL scancodes are. Named so the game, the tests and the
/// window agree about a key without any of them including SDL.
inline constexpr std::int32_t kKeyA = 4;
inline constexpr std::int32_t kKeyC = 6;
inline constexpr std::int32_t kKeyD = 7;
inline constexpr std::int32_t kKeyE = 8;
inline constexpr std::int32_t kKeyS = 22;
inline constexpr std::int32_t kKeyW = 26;
inline constexpr std::int32_t kKeyX = 27;

/// The movement keys, as a state rather than as a stream of events: a tank keeps going while a key
/// is held, and a missed key-up would otherwise drive it into a wall forever.
struct HeldKeys {
    bool forward{};
    bool back{};
    bool left{};
    bool right{};
    /// E, held: the hover lifts while it is down. Presentation only, so `any()` -- "is the player
    /// driving" -- does not count it.
    bool boost{};

    [[nodiscard]] bool any() const noexcept { return forward || back || left || right; }
};

/// Fold one key event into the held state. Keys the game does not use are ignored, and a key-down
/// for a key that is already down (the window sends those: the OS repeats them) changes nothing.
inline void apply(HeldKeys& keys, const KeyEvent& event) noexcept {
    switch (event.scancode) {
    case kKeyW:
        keys.forward = event.pressed;
        break;
    case kKeyS:
        keys.back = event.pressed;
        break;
    case kKeyA:
        keys.left = event.pressed;
        break;
    case kKeyD:
        keys.right = event.pressed;
        break;
    case kKeyE:
        keys.boost = event.pressed;
        break;
    default:
        break; // not a movement key: the game does not care
    }
}

/// The keys that are COMMANDS rather than states: pressing one asks for something once, and holding
/// it must not ask again. Both are pure predicates over a frame, so the app can be tested without a
/// window.
///
/// Ctrl+Q quits, and it is deliberately a MODIFIED key: a bare `Q` would end the game on a slipped
/// hand, and the usual escape hatches (a menu, a close button) stop being usable the moment the
/// pointer is captured for mouse look. F11 and Alt+Enter both ask for full screen.
[[nodiscard]] inline bool quit_requested(const InputFrame& frame) noexcept {
    for (const KeyEvent& key : frame.keys) {
        if (key.pressed && key.ctrl && key.scancode == kKeyQ) {
            return true;
        }
    }
    return false;
}

/// F11, or Alt+Enter: two keys, one request, because both are reflexes.
[[nodiscard]] inline bool fullscreen_requested(const InputFrame& frame) noexcept {
    for (const KeyEvent& key : frame.keys) {
        if (!key.pressed) {
            continue;
        }
        if (key.scancode == kKeyF11 || (key.alt && key.scancode == kKeyEnter)) {
            return true;
        }
    }
    return false;
}

/// C: toggle the colour calibration view. A press asks once; release and OS repeat are ignored by
/// the caller seeing only `pressed` events, as with the other commands.
[[nodiscard]] inline bool calibration_toggled(const InputFrame& frame) noexcept {
    for (const KeyEvent& key : frame.keys) {
        if (key.pressed && key.scancode == kKeyC) {
            return true;
        }
    }
    return false;
}

} // namespace protekt::input
