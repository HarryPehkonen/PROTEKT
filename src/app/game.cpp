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

    const render::Extent extent{options.width, options.height};

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
        window = render::Window::create(window_options, error);
        if (window == nullptr) {
            err << "protekt-game: " << error << "\n"
                << "protekt-game: no window, no game; try --headless to run without a display\n";
            return 4;
        }
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

        // What the player can see, as of the end of the last frame. The picking needs the camera,
        // and one frame of staleness is invisible: the tank does not teleport.
        game::SceneOptions scene_options;
        scene_options.seed = options.seed;
        scene_options.camera = camera_for(sim, player_id, session.heading(), options.seed);
        scene_options.extent = extent;
        scene_options.radius_tiles = 24;

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
