#pragma once

#include <cstdint>

#include "protekt/input/input.hpp"

/// Which keys are down, and what they mean. Two things live here: the held movement keys (SDL
/// scancodes in, a set of held keys out), and the command predicates that say which key event is
/// which request (quit, full screen, calibration view, the nav keys, world map, and the roster,
/// function-key and mark-digit decoders). What a command then DOES is elsewhere: the clicks and
/// the other command keys are in `game/interaction.cpp`, and the throttle rule is in
/// `game/session.cpp`, because "W means go forward" is a game rule, not an input rule. The
/// player-facing table is README.md, "How to play".
namespace protekt::input {

/// USB HID usage codes, which is what SDL scancodes are. Named so the game, the tests and the
/// window agree about a key without any of them including SDL.
inline constexpr std::int32_t kKeyA = 4;
inline constexpr std::int32_t kKeyC = 6;
inline constexpr std::int32_t kKeyD = 7;
inline constexpr std::int32_t kKeyE = 8;
inline constexpr std::int32_t kKeyF = 9;
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

/// The digit 1-9 a key event is, or 0. Bare only: Ctrl/Alt+digit are not the roster's.
[[nodiscard]] inline int roster_digit(const KeyEvent& key) noexcept {
    if (!key.pressed || key.ctrl || key.alt || key.scancode < kKey1 || key.scancode > kKey1 + 8) {
        return 0;
    }
    return key.scancode - kKey1 + 1;
}

/// The command-row group an F-key event asks for, 1-10, or 0 for any other key. Takes a BARE
/// press: Ctrl/Alt+F1 are not the row's. It does not look at Shift, so a shifted press answers
/// too; the shifted press means something else (a move), and that is `shift_function_key`, which
/// the caller must test first. F1..F10 are the vehicle groups' keys; the digit row is the
/// command/mark row.
[[nodiscard]] inline int function_key(const KeyEvent& key) noexcept {
    if (!key.pressed || key.ctrl || key.alt || key.scancode < kKeyF1 || key.scancode > kKeyF1 + 9) {
        return 0;
    }
    return key.scancode - kKeyF1 + 1;
}

/// The command-row group a SHIFT + F-key event asks to move the selection into, 1-10, or 0 for any
/// other key, and for an F-key with no Shift. The shifted sibling of `function_key`.
[[nodiscard]] inline int shift_function_key(const KeyEvent& key) noexcept {
    return key.shift ? function_key(key) : 0;
}

/// The mark number 0-9 a key event is, or -1. Bare only. "0" follows "9" in the HID table, so it
/// is not in `roster_digit`'s run: the roster has no 0.
[[nodiscard]] inline int mark_digit(const KeyEvent& key) noexcept {
    if (!key.pressed || key.ctrl || key.alt) {
        return -1;
    }
    if (key.scancode == kKey0) {
        return 0;
    }
    const int roster = roster_digit(key);
    return roster != 0 ? roster : -1;
}

/// Whether `key` is a bare press of `scancode`: Ctrl/Alt+key is not the key.
[[nodiscard]] inline bool bare_press(const KeyEvent& key, std::int32_t scancode) noexcept {
    return key.pressed && !key.ctrl && !key.alt && key.scancode == scancode;
}

/// N: arm nav placement (`top menu -> n`). What it then does is `game::NavMenu`'s.
[[nodiscard]] inline bool nav_arm_pressed(const KeyEvent& key) noexcept {
    return bare_press(key, kKeyN);
}

/// `~`: open the nav menu (`top menu -> ~`). What it then does is `game::NavMenu`'s.
[[nodiscard]] inline bool nav_menu_pressed(const KeyEvent& key) noexcept {
    return bare_press(key, kKeyGrave);
}

/// R: toggle the map view (N4, BZ2R -- BZ2 puts the map on R). Bare only; a pure view, so the
/// caller flips a flag and nothing else. (Tab clears the selection, M orders a move, so neither is
/// free.) O no longer does this: O is BZ2's OBJECTIVES key and stays unbound until objectives
/// exist.
[[nodiscard]] inline bool world_map_toggled(const InputFrame& frame) noexcept {
    for (const KeyEvent& key : frame.keys) {
        if (key.pressed && !key.ctrl && !key.alt && key.scancode == kKeyR) {
            return true;
        }
    }
    return false;
}

} // namespace protekt::input
