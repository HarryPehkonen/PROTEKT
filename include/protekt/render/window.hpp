#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "protekt/input/input.hpp"
#include "protekt/render/renderer.hpp"

/// The window, and the swapchain behind it.
///
/// Deliberately NOT unit-testable: it needs a display, and the gate box has none. Its tests are
/// the shrinking handful that can run headless (a missing display must be a clean refusal, not a
/// crash); its real verification is a frame the game captures of what it presented.
///
/// The game loop does not know which presenter it has. That is the point: the same loop runs
/// against `Renderer` (offscreen, on the gate box, in the suite) and against this (on a GPU,
/// with a swapchain).
namespace protekt::render {

struct WindowOptions {
    std::string title{"PROTEKT"};
    game::Extent extent{1280, 720};
    std::string device_name;
    bool validation{true};
    /// Start in desktop fullscreen. A command-line flag rather than only a key, because that is
    /// what makes a full-screen frame reproducible on a machine nobody is sitting at.
    bool fullscreen{false};
};

class Window {
public:
    Window() = default;
    virtual ~Window() = default;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    /// Never throws. No display, no SDL, no surface support: nullptr and a reason.
    [[nodiscard]] static std::unique_ptr<Window> create(const WindowOptions& options,
                                                        std::string& error);

    /// Pump the event queue. Returns false once the user has asked to close the window.
    ///
    /// Everything the user did lands in `input()`. The frame is CLEARED as the queue is pumped, so
    /// a caller sees each event exactly once and does not have to remember what it already handled.
    virtual bool pump_events() = 0;

    /// What the user did since the last pump. Window-free by type: the game loop consumes this and
    /// the suite constructs the same struct by hand, which is why the picking can be tested with no
    /// display.
    [[nodiscard]] virtual const input::InputFrame& input() const noexcept = 0;

    [[nodiscard]] virtual game::Extent extent() const noexcept = 0;
    [[nodiscard]] virtual std::string_view device_name() const noexcept = 0;

    /// Draw one scene and present it. A window whose extent changed is resized here.
    virtual void present(const game::Scene& scene) = 0;

    /// Ask for full screen, or leave it, and report the pixel extent the window has afterwards.
    ///
    /// DESKTOP fullscreen (borderless), never exclusive: `Alt+Tab` and the desktop shortcuts keep
    /// working, and a game that has captured the pointer can still be killed from the window
    /// manager. The extent is returned rather than left to the resize event because the caller's
    /// projection has to know the new shape in the SAME frame -- a full-screen frame is not
    /// 1280x720.
    [[nodiscard]] virtual game::Extent toggle_fullscreen() = 0;

    /// The frame that was last presented: RGBA8, row-major, top-left first. What the game writes
    /// when it is asked to capture, so that what you see is what is proved.
    [[nodiscard]] virtual std::vector<std::uint8_t> capture() const = 0;

    [[nodiscard]] virtual std::uint32_t validation_errors() const noexcept = 0;
};

} // namespace protekt::render
