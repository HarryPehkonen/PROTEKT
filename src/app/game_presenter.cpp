#include "protekt/app/game_presenter.hpp"

#include <ostream>
#include <utility>

#ifdef PROTEKT_HAVE_WINDOW
#include "protekt/game/atlas.hpp"
#include "protekt/input/bindings.hpp"
#include "protekt/render/window.hpp"
#endif

namespace protekt::app {

GamePresenter::~GamePresenter() = default;

std::unique_ptr<GamePresenter> GamePresenter::create(const PresenterOptions& options,
                                                     std::ostream& out, std::ostream& err) {
    std::unique_ptr<GamePresenter> presenter{new GamePresenter};
    presenter->extent_ = options.extent;
    std::string error;
#ifdef PROTEKT_HAVE_WINDOW
    if (!options.headless) {
        render::WindowOptions window_options;
        window_options.extent = options.extent;
        window_options.device_name = options.gpu;
        window_options.fullscreen = options.fullscreen;
        window_options.atlas_rgba = game::load_game_atlas(err).pixels;
        window_options.atlas_side = game::kAtlasSide;
        presenter->window_ = render::Window::create(window_options, error);
        if (presenter->window_ == nullptr) {
            err << "protekt-game: " << error << "\n"
                << "protekt-game: no window, no game; try --headless to run without a display\n";
            return nullptr;
        }
        // The controls, where the player is actually looking: the terminal the game was started
        // from. One line and a pointer rather than the whole table, because the table lives in one
        // place -- README.md, "How to play" -- and a second copy of it here is a second copy to go
        // stale.
        out << "protekt-game: W/S throttle, mouse and A/D look | left click a unit to select it, a "
               "HOSTILE "
               "to "
               "fire at it | right click to order what is selected | Tab or Escape clears | O "
               "opens the world map\n"
            << "protekt-game: the full table, and what each control does not do: README.md "
               "(How to play)\n";
    }
#endif
    if (options.headless) {
        render::CreateOptions create_options;
        create_options.device_name = options.gpu;
        create_options.validation = true;
        create_options.extent = options.extent;
        create_options.atlas_rgba = game::load_game_atlas(err).pixels;
        create_options.atlas_side = game::kAtlasSide;
        presenter->offscreen_ = render::create(create_options, error);
        if (presenter->offscreen_ == nullptr) {
            err << "protekt-game: " << error << "\n";
            return nullptr;
        }
        out << "protekt-game: headless on " << presenter->offscreen_->device_name() << "\n";
    }
    if (presenter->offscreen_ == nullptr && presenter->window_ == nullptr) {
        err << "protekt-game: no presenter was created; refusing to run\n";
        return nullptr;
    }
    return presenter;
}

bool GamePresenter::windowed() const noexcept { return window_ != nullptr; }

bool GamePresenter::poll(input::InputFrame& hands, bool scripted) {
#ifdef PROTEKT_HAVE_WINDOW
    if (window_ != nullptr) {
        if (!window_->pump_events()) {
            return false;
        }
        // Measured before the scripted run dropped its hands: 1 divergent run in 8 against the
        // headless path; after: the windowed world is a function of the seed and the flags alone.
        const input::InputFrame from_hands = window_->input();
        if (!scripted) {
            hands = from_hands;
        }
    }
#else
    (void)hands;
    (void)scripted;
#endif
    return true;
}

game::Extent GamePresenter::follow(const input::InputFrame& hands) {
#ifdef PROTEKT_HAVE_WINDOW
    if (window_ != nullptr) {
        // The game follows the window rather than stretching a fixed 1280x720 over it, and the aim
        // point's middle moves with it.
        extent_
            = input::fullscreen_requested(hands) ? window_->toggle_fullscreen() : window_->extent();
    }
#else
    (void)hands;
#endif
    return extent_;
}

bool GamePresenter::present(const game::Scene& scene, std::uint64_t seed, sim::Tick tick) {
#ifdef PROTEKT_HAVE_WINDOW
    if (window_ != nullptr) {
        window_->present(scene);
        return true;
    }
#endif
    render::FrameStats stats{};
    render::FrameRequest request;
    request.extent = extent_;
    request.seed = seed;
    request.tick = tick;
    request.scene = &scene;
    last_frame_ = offscreen_->render(request, stats);
    return !last_frame_.empty();
}

std::vector<std::uint8_t> GamePresenter::finish() {
#ifdef PROTEKT_HAVE_WINDOW
    if (window_ != nullptr) {
        last_frame_ = window_->capture();
    }
#endif
    return std::move(last_frame_);
}

} // namespace protekt::app
