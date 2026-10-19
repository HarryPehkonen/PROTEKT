#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "protekt/input/input.hpp"
#include "protekt/render/renderer.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::render {
class Window;
}

namespace protekt::app {

struct PresenterOptions {
    game::Extent extent{};
    std::string gpu{};
    bool headless{false};
    bool fullscreen{false};
};

/// The presentation seam of the game loop: a window if there is one, an offscreen buffer if not.
/// The loop does not care which, which is what makes it testable on a machine with no display.
class GamePresenter {
public:
    /// nullptr means refused, and the reason is already on `err`. A windowed presenter prints the
    /// controls line to `out`; a headless one names its device there.
    [[nodiscard]] static std::unique_ptr<GamePresenter>
    create(const PresenterOptions& options, std::ostream& out, std::ostream& err);

    GamePresenter(const GamePresenter&) = delete;
    GamePresenter& operator=(const GamePresenter&) = delete;
    GamePresenter(GamePresenter&&) = delete;
    GamePresenter& operator=(GamePresenter&&) = delete;
    ~GamePresenter();

    [[nodiscard]] bool windowed() const noexcept;

    /// Pumps the window's events and, unless `scripted`, takes its input frame into `hands`.
    /// Autodrive is a SCRIPTED run, so a real key or click must not be able to change its world;
    /// the events are still pumped (the window stays responsive) and the frame is dropped. False
    /// when the window was closed. A headless presenter has no hands and never closes.
    [[nodiscard]] bool poll(input::InputFrame& hands, bool scripted);

    /// The window is the authority on its own size: a full-screen switch or a resize changes the
    /// frame the projection has to fill. Returns the extent this frame is drawn at.
    [[nodiscard]] game::Extent follow(const input::InputFrame& hands);

    [[nodiscard]] game::Extent extent() const noexcept { return extent_; }

    /// Draws one frame. False when an offscreen frame could not be rendered.
    [[nodiscard]] bool present(const game::Scene& scene, std::uint64_t seed, sim::Tick tick);

    /// The last frame as RGBA, for a capture: the window's own if there is one. Empty if nothing
    /// was presented.
    [[nodiscard]] std::vector<std::uint8_t> finish();

private:
    GamePresenter() = default;

    game::Extent extent_{};
    std::unique_ptr<render::Renderer> offscreen_;
    std::unique_ptr<render::Window> window_;
    std::vector<std::uint8_t> last_frame_;
};

} // namespace protekt::app
