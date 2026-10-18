#include "protekt/app/game.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "protekt/ai/scripted.hpp"
#include "protekt/game/aim.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/session.hpp"
#include "protekt/input/bindings.hpp"
#include "protekt/input/input.hpp"
#include "protekt/render/renderer.hpp"
#include "protekt/sim/commander.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/world/world.hpp"
#ifdef PROTEKT_HAVE_WINDOW
#include "protekt/render/window.hpp"
#endif

namespace protekt::app {

namespace {

struct Options {
    std::uint64_t seed{42};
    /// 0 means `until the window is closed`, which is what playing is. A positive count is a
    /// SCRIPTED run: exactly that many frames, no pacing, so a capture is reproducible.
    std::uint32_t frames{0};
    std::uint32_t width{1280};
    std::uint32_t height{720};
    std::uint32_t ticks_per_frame{1};
    std::string capture{};
    std::string gpu{};
    bool headless{false};
    /// Holds W for the whole run. For verifying that the world moves under the hull on a machine
    /// with no keyboard -- the window itself needs a pair of hands.
    bool autodrive{false};
    /// A pointer placed on the command line, for a frame that has no mouse: a run that wants to
    /// show (or capture) a reticle without a pair of hands. Absent is the default, and it is what
    /// keeps a verification frame identical to what it drew before there was a reticle.
    int cursor_x{};
    int cursor_y{};
    bool cursor_placed{false};
    /// Start in full screen. The window is the authority on its size either way; this only says
    /// which size to start at.
    bool fullscreen{false};
    /// One key pressed on the first frame, for a run nobody is sitting at: `--press f11` exercises
    /// the full-screen toggle, `--press ctrl+q` the quit, and a bug report can name the key that
    /// did it. The same family as `--cursor`: a scripted frame of input, so what happened is
    /// repeatable.
    std::int32_t press_scancode{-1};
    bool press_ctrl{false};
    bool press_alt{false};
    /// Turn the radar off, for comparing two frames pixel by pixel. The game always has one.
    bool no_radar{false};
};

template <typename T> bool parse_number(std::string_view text, T& value) {
    const char* end = text.data() + text.size();
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): from_chars takes a length.
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    return ec == std::errc{} && ptr == end;
}

/// Returns 0 on success, or 2 after printing why the command line was refused.
int parse_args(std::span<const std::string_view> args, Options& options, std::ostream& err) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        const auto next = [&](std::string_view& value) {
            if (i + 1 >= args.size()) {
                err << "protekt-game: " << arg << " needs a value\n";
                return false;
            }
            value = args[++i];
            return true;
        };
        std::string_view value;
        if (arg == "--headless") {
            options.headless = true;
        } else if (arg == "--no-radar") {
            options.no_radar = true;
        } else if (arg == "--press") {
            std::string_view name;
            if (!next(name) || (name != "f11" && name != "ctrl+q" && name != "alt+enter")) {
                err << "protekt-game: --press wants one of f11, ctrl+q, alt+enter\n";
                return 2;
            }
            options.press_scancode = name == "f11" ? input::kKeyF11 : input::kKeyQ;
            options.press_ctrl = name == "ctrl+q";
            options.press_alt = name == "alt+enter";
            if (options.press_alt) {
                options.press_scancode = input::kKeyEnter;
            }
        } else if (arg == "--fullscreen") {
            if (options.headless) {
                err << "protekt-game: --fullscreen needs a window; --headless has none\n";
                return 2;
            }
            options.fullscreen = true;
        } else if (arg == "--autodrive") {
            options.autodrive = true;
        } else if (arg == "--seed") {
            if (!next(value)) {
                return 2;
            }
            if (!parse_number(value, options.seed)) {
                err << "protekt-game: --seed wants a number, got '" << value << "'\n";
                return 2;
            }
        } else if (arg == "--frames") {
            if (!next(value) || !parse_number(value, options.frames)) {
                err << "protekt-game: --frames wants a number\n";
                return 2;
            }
        } else if (arg == "--width") {
            if (!next(value) || !parse_number(value, options.width) || options.width == 0) {
                err << "protekt-game: --width wants a positive number\n";
                return 2;
            }
        } else if (arg == "--height") {
            if (!next(value) || !parse_number(value, options.height) || options.height == 0) {
                err << "protekt-game: --height wants a positive number\n";
                return 2;
            }
        } else if (arg == "--ticks-per-frame") {
            if (!next(value) || !parse_number(value, options.ticks_per_frame)
                || options.ticks_per_frame == 0) {
                err << "protekt-game: --ticks-per-frame wants a positive number\n";
                return 2;
            }
        } else if (arg == "--cursor") {
            std::string_view x_text;
            std::string_view y_text;
            if (!next(x_text) || !next(y_text) || !parse_number(x_text, options.cursor_x)
                || !parse_number(y_text, options.cursor_y)) {
                err << "protekt-game: --cursor wants two numbers: pixels from the left and the "
                       "top\n";
                return 2;
            }
            options.cursor_placed = true;
        } else if (arg == "--capture") {
            if (!next(value)) {
                return 2;
            }
            options.capture = std::string(value);
        } else if (arg == "--gpu") {
            if (!next(value)) {
                return 2;
            }
            options.gpu = std::string(value);
        } else {
            err << "protekt-game: unknown option '" << arg << "'\n";
            return 2;
        }
    }
    return 0;
}

int write_ppm(const std::string& path, const std::vector<std::uint8_t>& rgba,
              const render::Extent& extent) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return 1;
    }
    out << "P6\n" << extent.width << ' ' << extent.height << "\n255\n";
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        out.put(static_cast<char>(rgba[i]));
        out.put(static_cast<char>(rgba[i + 1]));
        out.put(static_cast<char>(rgba[i + 2]));
    }
    return out ? 0 : 1;
}

/// The player's eye: kEyeHeight metres above the ground under the tank, looking along its heading.
/// The height comes from `world::sample`, the same generator the simulation drives on, so the
/// camera neither floats over a ridge nor sinks into a hollow.
game::Camera camera_for(const sim::Sim& sim, sim::UnitId id, float yaw, std::uint64_t seed) {
    constexpr float kEyeHeight = 3.0F;
    game::Camera camera;
    for (const sim::Unit& unit : sim.units()) {
        if (unit.id != id) {
            continue;
        }
        camera.x = static_cast<float>(unit.x.raw) / 65536.0F * game::kMetresPerTile;
        camera.z = static_cast<float>(unit.y.raw) / 65536.0F * game::kMetresPerTile;
        const auto tile = world::sample(seed, unit.x.floor_int(), unit.y.floor_int());
        camera.y = static_cast<float>(tile.height) * game::kMetresPerHeightUnit + kEyeHeight;
        break;
    }
    camera.yaw = yaw;
    camera.pitch = -0.12F; // a slight downward tilt: the ground ahead beats empty sky
    return camera;
}

} // namespace

int run_game_cli(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
    Options options;
    if (const int code = parse_args(args, options, err); code != 0) {
        return code;
    }

    render::Extent extent{options.width, options.height};

    // The presentation seam: a window if there is one, an offscreen buffer if not.
    std::string error;
    std::unique_ptr<render::Renderer> offscreen;
#ifdef PROTEKT_HAVE_WINDOW
    std::unique_ptr<render::Window> window;
#endif
    bool headless = options.headless;
#ifdef PROTEKT_HAVE_WINDOW
    if (!headless) {
        render::WindowOptions window_options;
        window_options.extent = extent;
        window_options.device_name = options.gpu;
        window_options.fullscreen = options.fullscreen;
        window = render::Window::create(window_options, error);
        if (window == nullptr) {
            err << "protekt-game: " << error << "\n"
                << "protekt-game: no window, no game; try --headless to run without a display\n";
            return 4;
        }
        // The controls, where the player is actually looking: the terminal the game was started
        // from. One line and a pointer rather than the whole table, because the table lives in one
        // place -- README.md, "How to play" -- and a second copy of it here is a second copy to go
        // stale.
        out << "protekt-game: W/S throttle, A/D turn | left click a unit to select it, a HOSTILE "
               "to "
               "fire at it | right click to order what is selected | Tab cycles, Escape clears\n"
            << "protekt-game: the full table, and what each control does not do: README.md "
               "(How to play)\n";
    }
#endif
    if (headless) {
        render::CreateOptions create_options;
        create_options.device_name = options.gpu;
        create_options.validation = true;
        create_options.extent = extent;
        offscreen = render::create(create_options, error);
        if (offscreen == nullptr) {
            err << "protekt-game: " << error << "\n";
            return 4;
        }
        out << "protekt-game: headless on " << offscreen->device_name() << "\n";
    }

    if (offscreen == nullptr
#ifdef PROTEKT_HAVE_WINDOW
        && window == nullptr
#endif
    ) {
        err << "protekt-game: no presenter was created; refusing to run\n";
        return 4;
    }

    sim::Sim sim = sim::Sim::create(sim::skirmish(options.seed));
    sim::UnitId player_id{};
    for (const sim::Unit& unit : sim.units()) {
        if (unit.side == sim::Side::Player && unit.kind == sim::UnitKind::Tank) {
            player_id = unit.id;
            break;
        }
    }
    if (!player_id.valid()) {
        err << "protekt-game: the scenario has no player tank to drive\n";
        return 1;
    }

    // One object for the player's whole side of the frame: the tank he is in and the units he
    // commands. `app` decides what to present; `game::Session` decides what the input MEANS.
    game::Session session{player_id};
    ai::ScriptedCommander enemy;

    // 30 Hz is the rate the sim is designed and balanced at: 6 tiles/s at 30 ticks/s is the 12 m/s
    // hover tank in docs/PLAN.md.
    // Checked here rather than at parse time so that a SPECIFIC request error -- a device name that
    // does not exist -- is still the error a caller sees first.
    if (options.frames == 0) {
#ifdef PROTEKT_HAVE_WINDOW
        if (options.headless) {
            err << "protekt-game: --headless needs --frames N (there is no window to close)\n";
            return 2;
        }
#else
        err << "protekt-game: --frames N is required: this build has no window\n";
        return 2;
#endif
    }

    constexpr std::chrono::nanoseconds play_tick_period{1'000'000'000LL / 30};
    constexpr std::chrono::nanoseconds play_max_catch_up{250'000'000LL};
    auto last_step_time = std::chrono::steady_clock::now();
    std::chrono::nanoseconds tick_backlog{0};

    std::vector<std::uint8_t> last_frame;
    std::string last_message;
    for (std::uint32_t frame = 0; options.frames == 0 || frame < options.frames; ++frame) {
        input::InputFrame hands{};
#ifdef PROTEKT_HAVE_WINDOW
        if (window != nullptr) {
            if (!window->pump_events()) {
                out << "protekt-game: window closed after " << frame << " frames, tick "
                    << sim.tick() << '\n';
                break;
            }
            // Autodrive is a SCRIPTED run, so a real key or click must not be able to change its
            // world: the windowed and headless paths are compared hash for hash, and a stray event
            // arriving at a window that has just taken focus made one such comparison disagree.
            // The events are still pumped (the window stays responsive and its internal state stays
            // coherent) and their input frame is dropped. Measured before this: 1 divergent run in
            // 8; after: the windowed world is a function of the seed and the flags alone.
            const input::InputFrame from_hands = window->input();
            if (!options.autodrive) {
                hands = from_hands;
            }
        }
#endif
        if (options.autodrive) {
            hands.keys.push_back(input::KeyEvent{input::kKeyW, true, false});
        }
        if (options.press_scancode >= 0) {
            hands.keys.push_back(input::KeyEvent{options.press_scancode, true, false,
                                                 options.press_ctrl, options.press_alt});
        }
        if (options.cursor_placed) {
            hands.cursor_x = options.cursor_x;
            hands.cursor_y = options.cursor_y;
            hands.cursor_known = true;
        }

#ifdef PROTEKT_HAVE_WINDOW
        if (window != nullptr) {
            // The window is the authority on its own size. A full-screen switch or a resize changes
            // the frame the projection has to fill -- and the aim point's middle with it -- so the
            // game follows the window rather than stretching a fixed 1280x720 over it.
            extent = input::fullscreen_requested(hands) ? window->toggle_fullscreen()
                                                        : window->extent();
        }
#endif
        if (input::quit_requested(hands)) {
            out << "protekt-game: Ctrl+Q, so quitting at frame " << frame << ", tick " << sim.tick()
                << '\n';
            break;
        }

        // What the player can see, as of the end of the last frame. The picking needs the camera,
        // and one frame of staleness is invisible: the tank does not teleport.
        game::SceneOptions scene_options;
        scene_options.seed = options.seed;
        scene_options.camera = camera_for(sim, player_id, session.heading(), options.seed);
        scene_options.extent = extent;
        scene_options.radius_tiles = 24;
        scene_options.aim = game::aim_from(hands, extent);
        scene_options.viewer_unit = options.no_radar ? sim::UnitId{} : player_id;

        std::vector<sim::Order> orders;
        session.frame(hands, sim, scene_options, orders);
        enemy.think(sim::observe(sim, sim::Side::Enemy), orders);
        for (const sim::Order& order : orders) {
            sim.enqueue(order);
        }
        if (!session.message().empty() && session.message() != last_message) {
            out << "protekt-game: " << session.message() << '\n';
            last_message = std::string(session.message());
        }
        // PLAY mode advances the sim on the wall clock at 30 Hz, so the tank drives at the speed
        // the sim is designed for whatever the renderer manages. A scripted run advances exactly
        // ticks_per_frame times and never reads a clock, so its capture stays reproducible.
        std::uint32_t steps = options.ticks_per_frame;
        if (options.frames == 0) {
            const auto now = std::chrono::steady_clock::now();
            auto elapsed = now - last_step_time;
            last_step_time = now;
            // A stall must not turn into a catch-up burst that teleports the tank.
            if (elapsed > play_max_catch_up) {
                elapsed = play_max_catch_up;
            }
            tick_backlog += elapsed;
            steps = 0;
            while (tick_backlog >= play_tick_period) {
                tick_backlog -= play_tick_period;
                ++steps;
            }
        }
        for (std::uint32_t tick = 0; tick < steps; ++tick) {
            sim.step();
        }

        scene_options.camera = camera_for(sim, player_id, session.heading(), options.seed);
        const render::Scene scene
            = game::build_scene(sim, scene_options, session.selection().units());

        render::FrameStats stats{};
        render::FrameRequest request;
        request.extent = extent;
        request.seed = options.seed;
        request.tick = sim.tick();
        request.scene = &scene;
#ifdef PROTEKT_HAVE_WINDOW
        if (window != nullptr) {
            window->present(scene);
            continue;
        }
#endif
        last_frame = offscreen->render(request, stats);
        if (last_frame.empty()) {
            err << "protekt-game: frame " << frame << " could not be rendered\n";
            return 1;
        }
    }

#ifdef PROTEKT_HAVE_WINDOW
    if (window != nullptr) {
        last_frame = window->capture();
    }
#endif

    if (!options.capture.empty()) {
        if (last_frame.empty()) {
            err << "protekt-game: nothing was rendered, so there is nothing to capture\n";
            return 1;
        }
        if (const int code = write_ppm(options.capture, last_frame, extent); code != 0) {
            err << "protekt-game: could not write " << options.capture << "\n";
            return code;
        }
        out << "protekt-game: wrote " << options.capture << " (" << extent.width << 'x'
            << extent.height << ", tick " << sim.tick() << ", hash " << sim.state_hash() << ")\n";
    }
    return 0;
}

} // namespace protekt::app
