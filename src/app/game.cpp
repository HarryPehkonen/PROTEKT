#include "protekt/app/game.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "protekt/ai/scripted.hpp"
#include "protekt/app/args.hpp"
#include "protekt/app/audio.hpp"
#include "protekt/app/audio_pipeline.hpp"
#include "protekt/app/game_presenter.hpp"
#include "protekt/app/player_view.hpp"
#include "protekt/app/scenario_toml.hpp"
#include "protekt/app/time_accumulator.hpp"
#include "protekt/game/aim.hpp"
#include "protekt/game/menu.hpp"
#include "protekt/game/nav_marks.hpp"
#include "protekt/game/player.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/session.hpp"
#include "protekt/game/sounds.hpp"
#include "protekt/input/bindings.hpp"
#include "protekt/input/input.hpp"
#include "protekt/render/renderer.hpp"
#include "protekt/sim/commander.hpp"
#include "protekt/sim/observer.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/world/world.hpp"

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
    /// The reticle's pulse held at a percentage, for a frame that has no firefight to time: the
    /// same family as `--cursor`. Absent (-1) is the default, and the pulse is the real one.
    int pulse_percent{-1};
    /// Start in full screen. The window is the authority on its size either way; this only says
    /// which size to start at.
    bool fullscreen{false};
    /// One key pressed on the first frame, for a run nobody is sitting at: `--press f11` exercises
    /// the full-screen toggle, `--press ctrl+q` the quit, and a bug report can name the key that
    /// did it. The same family as `--cursor`: a scripted frame of input, so what happened is
    /// repeatable.
    std::int32_t press_scancode{-1};
    /// `--switch N`: the weapon switched once on each of the first N frames -- a right CLICK, which
    /// is the only thing that changes weapons since 2026-10-23 -- so a capture can show the gun
    /// (0), the rocket (1) or the mortar (2) with nobody at the mouse.
    std::uint32_t switches{0};
    /// `--look-up N`: the view tilted up N hundredths of a radian on the first frame, and
    /// `--fire N`: the trigger pulled on frame N (1-based; 0 is never), so a capture can show a
    /// shell in the air with nobody at the mouse. The same family as `--switch`.
    std::uint32_t look_up_centirad{0};
    std::uint32_t fire_frame{0};
    /// `--look-level N`: on frame N (1-based; 0 is never) the view comes back down by exactly what
    /// `--look-up` raised it, so a capture can watch where a high lob landed.
    std::uint32_t level_frame{0};
    /// `--nav FRAME` (repeatable): N pressed on that frame (1-based), so a capture can show nav
    /// marks with nobody at the keyboard. The same family as `--fire`.
    std::vector<std::uint32_t> nav_frames{};
    /// `--digit FRAME N` (repeatable): the number key N (0-9) pressed on that frame (1-based), so
    /// a capture can show a roster selection, or a dead key, with nobody at the keyboard.
    /// `--go FRAME` (repeatable): G pressed on that frame (1-based); the number that answers it is
    /// a `--digit` on a later frame. Lets a capture show the go prompt with nobody at the keyboard.
    std::vector<std::uint32_t> go_frames{};
    /// `--map FRAME` (repeatable): R pressed on that frame (1-based), which opens the map view and
    /// on a second frame closes it, so a capture can show the map with nobody at the keyboard.
    std::vector<std::uint32_t> map_frames{};
    /// `--targets FRAME` (repeatable): T pressed on that frame (1-based), so a capture can show the
    /// attack list with nobody at the keyboard. A hostile under the crosshair is still attacked;
    /// aim at the sky to arm the list instead.
    std::vector<std::uint32_t> target_frames{};
    struct DigitPress {
        std::uint32_t frame{};
        int digit{};
    };
    std::vector<DigitPress> digit_presses{};
    /// `--key FRAME NAME` (repeatable): SPACE, TAB or F1-F10 pressed on that frame (1-based). The
    /// digits cannot make a selection -- they are mark and menu-row keys -- so this is the scripted
    /// selection (`--key 1 f3` picks the factory group), and a capture can show the command menu
    /// with nobody at the keyboard.
    struct KeyPress {
        std::uint32_t frame{};
        std::int32_t scancode{};
    };
    std::vector<KeyPress> key_presses{};
    bool press_ctrl{false};
    bool press_alt{false};
    /// Turn the radar off, for comparing two frames pixel by pixel. The game always has one.
    bool no_radar{false};
    /// Suppress the radar's terrain wireframe (RADAR3D-2) while everything else about the radar
    /// stays: the same family as `--no-radar`, and what the slice's pixel acceptance compares
    /// against -- two frames that must differ ONLY inside the oval.
    bool no_radar_mesh{false};
    /// Never open the sound device, for a run that must be silent whatever the machine has: the
    /// same family as `--no-radar`, a flag for verification. Sound never touches the simulation,
    /// so it changes nothing but what the speakers do.
    bool no_sound{false};
    /// `--scenario FILE`: the composition to play, in place of the built-in one. Empty means the
    /// built-in; there is no search path, so a named file is the only way another one is read.
    std::string scenario{};
    /// `--log-spawns`: print every unit the scenario spawned, once, after the world is built and
    /// before the first frame. A verification flag: it changes nothing in the simulation, only
    /// what the run prints.
    bool log_spawns{false};
};

/// The scancode `--key` names: `space`, `tab`, `f1` to `f10`, in lower case and nothing else.
bool parse_key_name(std::string_view name, std::int32_t& scancode) {
    if (name == "space") {
        scancode = input::kKeySpace;
        return true;
    }
    if (name == "tab") {
        scancode = input::kKeyTab;
        return true;
    }
    int number = 0;
    if (name.size() < 2 || name.front() != 'f' || !parse_number(name.substr(1), number)
        || number < 1 || number > 10 || name.size() != (number == 10 ? 3U : 2U)) {
        return false;
    }
    scancode = input::kKeyF1 + number - 1;
    return true;
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
        } else if (arg == "--no-radar-mesh") {
            options.no_radar_mesh = true;
        } else if (arg == "--no-sound") {
            options.no_sound = true;
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
        } else if (arg == "--switch") {
            if (!next(value) || !parse_number(value, options.switches)) {
                err << "protekt-game: --switch wants a number of weapon-key presses\n";
                return 2;
            }
        } else if (arg == "--look-up") {
            if (!next(value) || !parse_number(value, options.look_up_centirad)) {
                err << "protekt-game: --look-up wants hundredths of a radian\n";
                return 2;
            }
        } else if (arg == "--look-level") {
            if (!next(value) || !parse_number(value, options.level_frame)) {
                err << "protekt-game: --look-level wants a frame number\n";
                return 2;
            }
        } else if (arg == "--fire") {
            if (!next(value) || !parse_number(value, options.fire_frame)) {
                err << "protekt-game: --fire wants a frame number\n";
                return 2;
            }
        } else if (arg == "--nav") {
            std::uint32_t frame = 0;
            if (!next(value) || !parse_number(value, frame) || frame == 0) {
                err << "protekt-game: --nav wants a frame number (1-based)\n";
                return 2;
            }
            options.nav_frames.push_back(frame);
        } else if (arg == "--go") {
            std::uint32_t frame = 0;
            if (!next(value) || !parse_number(value, frame) || frame == 0) {
                err << "protekt-game: --go wants a frame number (1-based)\n";
                return 2;
            }
            options.go_frames.push_back(frame);
        } else if (arg == "--map") {
            std::uint32_t frame = 0;
            if (!next(value) || !parse_number(value, frame) || frame == 0) {
                err << "protekt-game: --map wants a frame number (1-based)\n";
                return 2;
            }
            options.map_frames.push_back(frame);
        } else if (arg == "--targets") {
            std::uint32_t frame = 0;
            if (!next(value) || !parse_number(value, frame) || frame == 0) {
                err << "protekt-game: --targets wants a frame number (1-based)\n";
                return 2;
            }
            options.target_frames.push_back(frame);
        } else if (arg == "--digit") {
            std::string_view digit_text;
            Options::DigitPress press;
            if (!next(value) || !next(digit_text) || !parse_number(value, press.frame)
                || !parse_number(digit_text, press.digit) || press.frame == 0 || press.digit < 0
                || press.digit > 9) {
                err << "protekt-game: --digit wants a frame number (1-based) and a key 0-9\n";
                return 2;
            }
            options.digit_presses.push_back(press);
        } else if (arg == "--key") {
            std::string_view name;
            Options::KeyPress press;
            if (!next(value) || !next(name) || !parse_number(value, press.frame) || press.frame == 0
                || !parse_key_name(name, press.scancode)) {
                err << "protekt-game: --key wants a frame number (1-based) and one of space, tab, "
                       "f1, f2, f3, f4, f5, f6, f7, f8, f9, f10\n";
                return 2;
            }
            options.key_presses.push_back(press);
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
        } else if (arg == "--pulse") {
            if (!next(value) || !parse_number(value, options.pulse_percent)
                || options.pulse_percent < 0 || options.pulse_percent > 100) {
                err << "protekt-game: --pulse wants a percentage from 0 to 100\n";
                return 2;
            }
        } else if (arg == "--scenario") {
            if (!next(value)) {
                return 2;
            }
            options.scenario = std::string(value);
        } else if (arg == "--log-spawns") {
            options.log_spawns = true;
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
              const game::Extent& extent) {
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

/// The player's side of a frame and the world it plays in, in the order a frame needs them. `app`
/// decides what to present; `game::Session` decides what the input MEANS.
class Game {
public:
    Game(Options options, sim::Sim sim, const game::Session& session, const AudioPipeline& pipeline)
        : options_(std::move(options)), sim_(std::move(sim)), session_(session),
          terrain_cache_(options_.seed), pipeline_(pipeline),
          // 30 Hz is the rate the sim is designed and balanced at: 6 tiles/s at 30 ticks/s is the
          // 12 m/s hover tank in docs/PLAN.md. A stall is capped at 250 ms of catch-up.
          play_clock_(std::chrono::nanoseconds{1'000'000'000LL / 30},
                      std::chrono::nanoseconds{250'000'000LL}, TimeAccumulator::Clock::now()) {
        refresh_row();
    }

    [[nodiscard]] sim::Tick tick() const { return sim_.tick(); }
    [[nodiscard]] std::uint64_t state_hash() const { return sim_.state_hash(); }
    void toggle_calibration() { calibration_ = !calibration_; }
    void toggle_world_map() { world_map_ = !world_map_; }

    /// What the player does with this frame's hands: the orders, the enemy's reply, and the
    /// message to say. Returns the scene options as of the end of the last frame -- the picking
    /// needs the camera, and one frame of staleness is invisible: the tank does not teleport.
    game::SceneOptions command(const input::InputFrame& hands, const game::Extent& extent,
                               std::ostream& out) {
        game::SceneOptions scene_options;
        scene_options.seed = options_.seed;
        scene_options.camera = camera_for(sim_, session_, options_.seed, hover_.z());
        scene_options.extent = extent;
        scene_options.radius_tiles = 24;
        scene_options.aim = game::aim_from(hands, extent);
        scene_options.viewer_unit = radar_viewer(session_, options_.no_radar);
        scene_options.nav_marks = &nav_marks_; // G-then-a-number reads the marks
        scene_options.row = &row_;             // F1-F10 select what the row holds
        player_alive_before_ = is_alive(sim_, session_.driven());

        std::vector<sim::Order> orders;
        session_.frame(hands, sim_, scene_options, orders);
        if (input::nav_mark_pressed(hands)) {
            // The player's own pin, held here and never given to the sim.
            const std::string said = game::nav_press(nav_marks_, sim_, scene_options);
            out << "protekt-game: " << said << '\n';
        }
        enemy_.think(sim::observe(sim_, sim::Side::Enemy), orders);
        for (const sim::Order& order : orders) {
            sim_.enqueue(order);
        }
        // A roster key the roster refused: keep its sentence on screen a few seconds, since the
        // session's own message lasts one frame.
        for (const input::KeyEvent& key : hands.keys) {
            if ((input::function_key(key) != 0 || input::mark_digit(key) >= 0)
                && !session_.message().empty()) {
                notice_ = std::string(session_.message());
                notice_until_ = sim_.tick() + kNoticeTicks;
            }
        }
        announce(out);
        return scene_options;
    }

    /// Says the session's message once, when it changes. The sim's answer to a trigger arrives in
    /// `advance`, after `command`, so this runs again after the step.
    void announce(std::ostream& out) {
        if (!session_.message().empty() && session_.message() != last_message_) {
            out << "protekt-game: " << session_.message() << '\n';
            last_message_ = std::string(session_.message());
        }
    }

    /// Steps the sim, and hears it. PLAY mode advances on the wall clock at 30 Hz, so the tank
    /// drives at the speed the sim is designed for whatever the renderer manages. A scripted run
    /// advances exactly ticks_per_frame times and never reads a clock, so its capture stays
    /// reproducible.
    void advance(const game::SceneOptions& scene_options) {
        const std::uint32_t steps = options_.frames == 0
                                        ? play_clock_.steps_at(TimeAccumulator::Clock::now())
                                        : options_.ticks_per_frame;
        // The trigger waits for a step to be judged on: with none this frame, the pull stays
        // pending (the flag is drained only when the sim is stepped).
        const bool trigger_pulled = steps > 0 && session_.take_shot_voice();
        const AdvanceResult result
            = pipeline_.advance(sim_, steps,
                                audio_frame_for(sim_, session_, scene_options.camera,
                                                trigger_pulled, player_alive_before_),
                                hit_flash_, hit_mark_);
        // The sim's own answer to the trigger: it launched nothing, so say why.
        if (result.trigger_refused) {
            session_.trigger_refused(sim_);
        }
        for (const AdvanceResult::Touchdown& down : result.touchdowns) {
            landing_marks_.land(down.tick, down.landing.tile_x, down.landing.tile_y);
        }
        refresh_row();
    }

    /// The frame, as of now. The hover advances once per tick the sim has run, off the sim's own
    /// counter, from the ground under the tank where it now stands. A dead player has no hull to
    /// float.
    game::Scene scene(game::SceneOptions scene_options) {
        const sim::UnitId driven = session_.driven();
        for (const sim::Unit& unit : sim_.units()) {
            if (unit.id == driven) {
                hover_.advance(sim_.tick(), game::ground_metres(options_.seed, unit.x, unit.y),
                               session_.keys().boost);
                // `advance` has just given the hover a height.
                if (const std::optional<float> z = hover_.z(); z.has_value()) {
                    scene_options.hover = game::SceneOptions::Hovering{driven, *z};
                }
                break;
            }
        }
        scene_options.camera = camera_for(sim_, session_, options_.seed, hover_.z());
        // The turret's angle relative to the hull: `Session` exposes the hull's heading and the
        // turret's world yaw (`view_yaw`), but not `PlayerCommander::turret_yaw` itself, so it is
        // their wrapped difference -- the same number, without adding an accessor.
        scene_options.facing = game::SceneOptions::Facing{
            driven, session_.heading(),
            game::Camera::wrap_angle(session_.view_yaw() - session_.heading())};
        headings_.observe(sim_);
        scene_options.headings = &headings_;
        scene_options.terrain = &terrain_cache_; // the per-seed memo the mesh reads through
        scene_options.hit_flash = hit_flash_.level(sim_.tick());
        scene_options.weapon = session_.weapon();
        scene_options.show_weapon = !session_.destroyed();
        scene_options.reloading = game::weapon_reloading(sim_, driven);
        scene_options.landing_marks = &landing_marks_;
        scene_options.nav_marks = &nav_marks_;
        scene_options.row = &row_;
        scene_options.show_radar_mesh = !options_.no_radar_mesh;
        scene_options.show_world_map = world_map_;
        if (session_.attack_armed()) {
            // The list lives in the Session's own vector and is captured when T arms it; pointing
            // at it is enough, and nothing here is owned. A frame that is not arming stays null.
            scene_options.attack_list = &session_.attack_targets();
        }
        if (session_.go_armed()) {
            // Never expires: the prompt is on screen for exactly as long as the mode lasts.
            scene_options.notice = std::string(game::kGoPrompt);
        } else if (sim_.tick() < notice_until_) {
            scene_options.notice = notice_;
        }
        scene_options.reticle_pulse = options_.pulse_percent >= 0
                                          ? static_cast<float>(options_.pulse_percent) / 100.0F
                                          : hit_mark_.level(sim_.tick());
        // The rows live in a NAMED LOCAL that outlives the build below: SceneOptions holds a
        // pointer and nothing owned, so pointing it at a temporary would leave a dangling menu in
        // the frame.
        const std::vector<game::MenuRow> menu
            = menu_for(sim_, session_, scene_options, session_.attack_armed());
        scene_options.show_menu = true;
        scene_options.menu = menu.empty() ? nullptr : &menu;
        return calibration_ ? game::build_calibration_scene(scene_options)
                            : game::build_scene(sim_, scene_options, session_.selection().units());
    }

private:
    Options options_;
    sim::Sim sim_;
    game::Session session_;
    game::Hover hover_;
    game::HitFlash hit_flash_;
    game::HitMark hit_mark_;
    game::LandingMarks landing_marks_;
    game::NavMarks nav_marks_;
    game::Headings headings_;
    /// The world's per-tile answers for this session's seed, remembered (game/terrain_cache.hpp):
    /// the terrain mesh asks the world five times a tile and is rebuilt every frame, so this is the
    /// difference between 12.1 ms and ~2.5 ms of a frame at radius 24. One cache, one seed: the
    /// seed is fixed for the session, so the cache is never stale.
    game::TerrainCache terrain_cache_;
    bool world_map_{false};   // R toggles the map view; a view, it never touches the sim
    bool calibration_{false}; // C toggles the colour calibration view; it never touches the sim
    bool player_alive_before_{false};
    ai::ScriptedCommander enemy_;
    AudioPipeline pipeline_;
    TimeAccumulator play_clock_;
    std::string last_message_;
    /// What a roster key just said, held for `kNoticeTicks` of sim time (tick-based, so a scripted
    /// capture shows it identically every run).
    static constexpr sim::Tick kNoticeTicks = 3 * sim::kTicksPerSecond;
    std::string notice_;
    sim::Tick notice_until_{0};
    /// The command row (ROW-SLOTS): which key holds which units. Refreshed once per frame, right
    /// after the sim steps, so the input of the next frame and the picture of this one agree.
    game::RowState row_;

    void refresh_row() { row_.update(sim_, sim::Side::Player, nav_marks_.marks().size()); }
};

/// The keys and pointer a run puts in the player's hands on its own, for a frame nobody is
/// sitting at.
void script_hands(const Options& options, std::uint32_t frame, input::InputFrame& hands) {
    if (options.autodrive) {
        hands.keys.push_back(input::KeyEvent{input::kKeyW, true, false});
    }
    if (std::find(options.nav_frames.begin(), options.nav_frames.end(), frame + 1)
        != options.nav_frames.end()) {
        hands.keys.push_back(input::KeyEvent{input::kKeyN, true, false});
    }
    if (std::find(options.go_frames.begin(), options.go_frames.end(), frame + 1)
        != options.go_frames.end()) {
        hands.keys.push_back(input::KeyEvent{input::kKeyG, true, false});
    }
    if (std::find(options.map_frames.begin(), options.map_frames.end(), frame + 1)
        != options.map_frames.end()) {
        hands.keys.push_back(input::KeyEvent{input::kKeyR, true, false});
    }
    if (std::find(options.target_frames.begin(), options.target_frames.end(), frame + 1)
        != options.target_frames.end()) {
        hands.keys.push_back(input::KeyEvent{input::kKeyT, true, false});
    }
    for (const Options::DigitPress& press : options.digit_presses) {
        if (press.frame == frame + 1) {
            hands.keys.push_back(input::KeyEvent{
                press.digit == 0 ? input::kKey0 : input::kKey1 + press.digit - 1, true, false});
        }
    }
    for (const Options::KeyPress& press : options.key_presses) {
        if (press.frame == frame + 1) {
            hands.keys.push_back(input::KeyEvent{press.scancode, true, false});
        }
    }
    if (options.press_scancode >= 0) {
        hands.keys.push_back(input::KeyEvent{options.press_scancode, true, false,
                                             options.press_ctrl, options.press_alt});
    }
    if (frame < options.switches) {
        // The weapon switch is a right CLICK now, so a scripted switch is a real click: the aim
        // point is what the interaction reads, and the pixel is not.
        hands.clicks.push_back(input::Click{0, 0, input::MouseButton::Right, true, false});
    }
    if (frame == 0 && options.look_up_centirad > 0) {
        hands.mouse_dy
            = -static_cast<float>(options.look_up_centirad) / 100.0F / game::kLookRadiansPerPixel;
    }
    if (options.level_frame > 0 && frame + 1 == options.level_frame) {
        hands.mouse_dy
            = static_cast<float>(options.look_up_centirad) / 100.0F / game::kLookRadiansPerPixel;
    }
    if (options.fire_frame > 0 && frame + 1 == options.fire_frame) {
        hands.clicks.push_back(input::Click{0, 0, input::MouseButton::Left, true, false});
    }
    if (options.cursor_placed) {
        hands.cursor_x = options.cursor_x;
        hands.cursor_y = options.cursor_y;
        hands.cursor_known = true;
    }
}

sim::UnitId player_tank(const sim::Sim& sim) {
    for (const sim::Unit& unit : sim.units()) {
        if (unit.side == sim::Side::Player && unit.kind == sim::UnitKind::Tank) {
            return unit.id;
        }
    }
    return {};
}

/// Plays frames until the count runs out, the window closes or the player quits. Returns 0, or 1
/// if a frame could not be rendered.
int play(Game& game, GamePresenter& presenter, const Options& options, std::ostream& out,
         std::ostream& err) {
    for (std::uint32_t frame = 0; options.frames == 0 || frame < options.frames; ++frame) {
        input::InputFrame hands{};
        if (!presenter.poll(hands, options.autodrive)) {
            out << "protekt-game: window closed after " << frame << " frames, tick " << game.tick()
                << '\n';
            break;
        }
        script_hands(options, frame, hands);
        const game::Extent extent = presenter.follow(hands);
        if (input::calibration_toggled(hands)) {
            game.toggle_calibration();
        }
        if (input::world_map_toggled(hands)) {
            game.toggle_world_map();
        }
        if (input::quit_requested(hands)) {
            out << "protekt-game: Ctrl+Q, so quitting at frame " << frame << ", tick "
                << game.tick() << '\n';
            break;
        }
        game::SceneOptions scene_options = game.command(hands, extent, out);
        game.advance(scene_options);
        game.announce(out);
        if (!presenter.present(game.scene(scene_options), options.seed, game.tick())) {
            err << "protekt-game: frame " << frame << " could not be rendered\n";
            return 1;
        }
    }
    return 0;
}

} // namespace

int run_game_cli(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
    Options options;
    if (const int code = parse_args(args, options, err); code != 0) {
        return code;
    }

    // A named scenario that cannot be read or parsed refuses to start, before any window opens: a
    // bad data file must never quietly play a different game.
    sim::ScenarioData scenario_data = sim::default_data();
    if (!options.scenario.empty()) {
        ScenarioParse parsed = load_scenario_file(options.scenario);
        if (!parsed.ok) {
            err << "protekt-game: --scenario " << options.scenario << ": " << parsed.error << "\n";
            return 2;
        }
        scenario_data = std::move(parsed.data);
    }

    PresenterOptions presenter_options;
    presenter_options.extent = game::Extent{options.width, options.height};
    presenter_options.gpu = options.gpu;
    presenter_options.headless = options.headless;
    presenter_options.fullscreen = options.fullscreen;
    const std::unique_ptr<GamePresenter> presenter
        = GamePresenter::create(presenter_options, out, err);
    if (presenter == nullptr) {
        return 4;
    }

    // Sound is the window's: a headless run has no speakers to be wrong about, and a machine with
    // no device must still play. ONE line says so, because a silent game with no explanation looks
    // like a bug.
    std::unique_ptr<AudioOut> audio_out;
    if (presenter->windowed() && !options.no_sound) {
        std::string audio_error;
        audio_out = AudioOut::open(audio_error);
        if (audio_out == nullptr) {
            out << "protekt-game: " << audio_error << "; the game runs silent\n";
        }
    }
    std::optional<AudioOutSink> audio_sink;
    if (audio_out != nullptr) {
        audio_sink.emplace(*audio_out);
    }

    sim::Sim sim = sim::Sim::create(sim::skirmish(options.seed, scenario_data));
    const sim::UnitId player_id = player_tank(sim);
    if (!player_id.valid()) {
        err << "protekt-game: the scenario has no player tank to drive\n";
        return 1;
    }

    // `--log-spawns`: one line per spawned unit, after the world is built and before the first
    // frame, in the same frozen grammar the battle log prints on tick 0, so a scenario file's
    // composition can be read back without asserting on how the battle turns out. Positions print
    // through sim::append_tiles -- the ONE place that formatting rule lives. Nothing here touches
    // the simulation; it only prints what the scenario produced.
    if (options.log_spawns) {
        for (const sim::Unit& unit : sim.units()) {
            std::string line = "tick 0 status unit ";
            line += std::to_string(unit.id.value);
            line += ' ';
            line += sim::to_string(unit.kind);
            line += " side ";
            line += sim::to_string(unit.side);
            line += " pos ";
            sim::append_tiles(line, unit.x.raw);
            line += ' ';
            sim::append_tiles(line, unit.y.raw);
            line += " hp ";
            line += std::to_string(unit.hp);
            line += " stance ";
            line += sim::to_string(unit.stance);
            line += '\n';
            out << line;
        }
    }

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

    Game game{options, std::move(sim), game::Session{player_id},
              AudioPipeline{audio_sink ? &*audio_sink : nullptr}};
    if (const int code = play(game, *presenter, options, out, err); code != 0) {
        return code;
    }

    if (!options.capture.empty()) {
        const game::Extent extent = presenter->extent();
        const std::vector<std::uint8_t> last_frame = presenter->finish();
        if (last_frame.empty()) {
            err << "protekt-game: nothing was rendered, so there is nothing to capture\n";
            return 1;
        }
        if (const int code = write_ppm(options.capture, last_frame, extent); code != 0) {
            err << "protekt-game: could not write " << options.capture << "\n";
            return code;
        }
        out << "protekt-game: wrote " << options.capture << " (" << extent.width << 'x'
            << extent.height << ", tick " << game.tick() << ", hash " << game.state_hash() << ")\n";
    }
    return 0;
}

} // namespace protekt::app
