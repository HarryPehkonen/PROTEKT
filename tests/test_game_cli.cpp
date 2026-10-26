// The game loop, headless.
//
// The loop does not know whether it is presenting to a window or to a buffer, so everything except
// the window itself is testable here -- on a machine with no display and no GPU.
#include <gtest/gtest.h>

#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "protekt/app/game.hpp"

namespace {

using protekt::app::run_game_cli;

/// llvmpipe -- the only device on the gate box -- spawns worker threads and races inside itself.
/// The driver is not built with ThreadSanitizer and cannot be instrumented, so its internal races
/// are noise this project cannot fix (docs/PLAN.md states the resulting gap). The tests below that
/// actually RASTERISE therefore skip under TSan; the ones that only parse or refuse an argument
/// still run, because they do not touch the driver.
#if defined(__SANITIZE_THREAD__)
constexpr bool kUnderThreadSanitizer = true;
#else
constexpr bool kUnderThreadSanitizer = false;
#endif

constexpr const char* kTsanReason
    = "skipped under ThreadSanitizer: the Vulkan driver (lavapipe) races inside itself and cannot "
      "be instrumented; see docs/PLAN.md";

class GameCliTest : public ::testing::Test {
protected:
    void TearDown() override {
        std::error_code ignored;
        for (const auto& path : written_) {
            std::filesystem::remove(path, ignored);
        }
    }

    [[nodiscard]] std::string temp_path(const std::string& stem) {
        static int counter = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto path = std::filesystem::temp_directory_path()
                          / ("protekt-game-" + std::to_string(stamp) + "-"
                             + std::to_string(++counter) + "-" + stem);
        written_.push_back(path);
        return path.string();
    }

    [[nodiscard]] static std::string slurp(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    /// Runs the loop headlessly; returns the exit code and fills both streams.
    static int run(std::vector<std::string_view> args, std::string& err,
                   std::string* out = nullptr) {
        std::ostringstream out_stream;
        std::ostringstream err_stream;
        const int code = run_game_cli(args, out_stream, err_stream);
        err = err_stream.str();
        if (out != nullptr) {
            *out = out_stream.str();
        }
        return code;
    }

    [[nodiscard]] static bool display_available() {
        const char* display = std::getenv("DISPLAY");
        const char* wayland = std::getenv("WAYLAND_DISPLAY");
        return (display != nullptr && *display != '\0') || (wayland != nullptr && *wayland != '\0');
    }

    std::vector<std::filesystem::path> written_;
};

TEST_F(GameCliTest, HeadlessWritesAPpmOfExactlyTheRequestedSize) {
    if (kUnderThreadSanitizer) {
        GTEST_SKIP() << kTsanReason;
    }
    const std::string path = temp_path("frame.ppm");
    std::string err;
    const std::vector<std::string_view> args{"--headless", "--frames", "3",         "--width", "96",
                                             "--height",   "64",       "--capture", path};
    ASSERT_EQ(run(args, err), 0) << err;
    const std::string bytes = slurp(path);
    const std::string header = "P6\n96 64\n255\n";
    ASSERT_GE(bytes.size(), header.size());
    EXPECT_EQ(bytes.compare(0, header.size(), header), 0) << "header: " << bytes.substr(0, 20);
    EXPECT_EQ(bytes.size(), header.size() + static_cast<std::size_t>(96U * 64U * 3U));
}

TEST_F(GameCliTest, TheSameSeedAndTheSameInputWriteTheSameBytes) {
    if (kUnderThreadSanitizer) {
        GTEST_SKIP() << kTsanReason;
    }
    const std::string first = temp_path("a.ppm");
    const std::string second = temp_path("b.ppm");
    std::string err;
    ASSERT_EQ(run({"--headless", "--seed", "1234", "--frames", "20", "--width", "64", "--height",
                   "48", "--capture", first},
                  err),
              0)
        << err;
    ASSERT_EQ(run({"--headless", "--seed", "1234", "--frames", "20", "--width", "64", "--height",
                   "48", "--capture", second},
                  err),
              0)
        << err;
    EXPECT_EQ(slurp(first), slurp(second))
        << "a game that renders the same frames differently each run is not deterministic";
}

TEST_F(GameCliTest, TheWorldMovesBetweenFrames) {
    if (kUnderThreadSanitizer) {
        GTEST_SKIP() << kTsanReason;
    }
    // The IMAGE is the wrong witness here: at seed 42 the enemy starts a hundred tiles away and
    // the camera follows the player, so a stationary player sees an unchanged frame for hundreds
    // of ticks even though the simulation is running. The world's state hash is the honest
    // evidence -- and that the two frames are byte-identical is asserted deliberately, because it
    // is true and it documents the reason a screenshot cannot be the oracle.
    const std::string early = temp_path("early.ppm");
    const std::string later = temp_path("later.ppm");
    std::string early_err;
    std::string later_err;
    std::string early_out;
    std::string later_out;
    ASSERT_EQ(run({"--headless", "--seed", "42", "--frames", "1", "--width", "96", "--height", "64",
                   "--capture", early},
                  early_err, &early_out),
              0)
        << early_err;
    ASSERT_EQ(run({"--headless", "--seed", "42", "--frames", "400", "--width", "96", "--height",
                   "64", "--capture", later},
                  later_err, &later_out),
              0)
        << later_err;
    ASSERT_NE(early_out.find("hash"), std::string::npos) << early_out;
    ASSERT_NE(later_out.find("hash"), std::string::npos) << later_out;
    EXPECT_NE(early_out.substr(early_out.find("hash")), later_out.substr(later_out.find("hash")))
        << "400 ticks left the world in the same state as one: nothing is being simulated";
    EXPECT_EQ(slurp(early), slurp(later))
        << "if the picture DID change with no input, the camera or the scene is drifting";
}

TEST_F(GameCliTest, RejectsAnUnknownFlag) {
    std::string err;
    const std::vector<std::string_view> args{"--headless", "--wat"};
    EXPECT_EQ(run(args, err), 2);
    EXPECT_NE(err.find("--wat"), std::string::npos) << err;
}

// --- --scenario: the composition from a file ----------------------------------------------------

TEST_F(GameCliTest, AScenarioFileThatCannotBeReadRefusesToStart) {
    std::string err;
    const std::string path = temp_path("missing.toml");
    EXPECT_EQ(run({"--headless", "--frames", "1", "--scenario", path}, err), 2);
    EXPECT_NE(err.find(path), std::string::npos) << err;
}

TEST_F(GameCliTest, ABadScenarioFileRefusesToStartAndNamesTheLine) {
    const std::string path = temp_path("bad.toml");
    {
        std::ofstream file(path);
        file << "[[group]]\nkind = \"tank\"\ncount = 0\ndx = 0\ndy = 0\n";
    }
    std::string err;
    std::string out;
    EXPECT_EQ(run({"--headless", "--frames", "1", "--scenario", path}, err, &out), 2);
    EXPECT_NE(err.find("--scenario " + path), std::string::npos) << err;
    EXPECT_NE(err.find("line 3"), std::string::npos) << err;
    EXPECT_TRUE(out.empty()) << "no game was played: " << out;
}

TEST_F(GameCliTest, AScenarioFileChangesTheBattleAndNoFlagPlaysTheBuiltInOne) {
    if (kUnderThreadSanitizer) {
        GTEST_SKIP() << kTsanReason;
    }
    const std::string path = temp_path("two_tanks.toml");
    {
        std::ofstream file(path);
        file << "[[group]]\nkind = \"tank\"\ncount = 2\ndx = 8\ndy = 2\n\n"
                "[[group]]\nkind = \"factory\"\ncount = 1\ndx = 0\ndy = 0\n";
    }
    const std::string builtin = temp_path("builtin.toml");
    {
        // The built-in composition written out: a file that says what the default says plays it.
        std::ofstream file(builtin);
        struct Row {
            const char* kind;
            int dx;
            int dy;
        };
        const std::array<Row, 10> rows{{{"tank", 8, 2},
                                        {"tank", 8, 8},
                                        {"tank", 2, 8},
                                        {"hover_tank", 5, 5},
                                        {"factory", 0, 0},
                                        {"turret", 5, -3},
                                        {"turret", -3, 5},
                                        {"mortar", -4, -1},
                                        {"rocket", -1, -4},
                                        {"recycler", -3, -3}}};
        for (const Row& row : rows) {
            file << "[[group]]\nkind = \"" << row.kind << "\"\ncount = 1\ndx = " << row.dx
                 << "\ndy = " << row.dy << "\n\n";
        }
    }
    const std::string frame = temp_path("frame.ppm");
    const auto hash_of = [&](std::vector<std::string_view> args) {
        args.insert(args.begin(), {"--headless", "--frames", "40", "--width", "64", "--height",
                                   "48", "--capture", frame});
        std::string err;
        std::string out;
        EXPECT_EQ(run(args, err, &out), 0) << err;
        EXPECT_NE(out.find("hash"), std::string::npos) << out;
        return out.substr(out.find("hash"));
    };
    const std::string plain = hash_of({});
    EXPECT_EQ(hash_of({"--scenario", builtin}), plain)
        << "the default, spelled out, is the default";
    EXPECT_NE(hash_of({"--scenario", path}), plain)
        << "a different composition is a different battle";
}

// --- --log-spawns: the composition, read back ------------------------------------------------

TEST_F(GameCliTest, LogSpawnsPrintsTheCompositionInTheBattleLogGrammar) {
    if (kUnderThreadSanitizer) {
        GTEST_SKIP() << kTsanReason;
    }
    const std::string path = temp_path("log_two_tanks.toml");
    {
        std::ofstream file(path);
        file << "[[group]]\nkind = \"tank\"\ncount = 2\ndx = 8\ndy = 2\n\n"
                "[[group]]\nkind = \"factory\"\ncount = 1\ndx = 0\ndy = 0\n";
    }
    std::string err;
    std::string out;
    EXPECT_EQ(run({"--headless", "--frames", "1", "--width", "32", "--height", "24", "--scenario",
                   path, "--log-spawns"},
                  err, &out),
              0)
        << err;

    // One frozen line per spawned unit: "tick 0 status unit <id> <kind> side <side> pos <x> <y>
    // hp <hp> stance <stance>". The file names two tanks and one factory per side, so six units.
    std::vector<std::string> spawns;
    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.rfind("tick 0 status unit ", 0) == 0) {
            spawns.push_back(line);
        }
    }
    EXPECT_EQ(spawns.size(), 6U) << "one line per spawned unit: " << out;
    for (const std::string& spawn : spawns) {
        EXPECT_NE(spawn.find(" side "), std::string::npos) << spawn;
        EXPECT_NE(spawn.find(" pos "), std::string::npos) << spawn;
        EXPECT_NE(spawn.find(" hp "), std::string::npos) << spawn;
        EXPECT_NE(spawn.find(" stance "), std::string::npos) << spawn;
        // A position prints as tiles with one decimal digit, e.g. "24.0" -- never a raw Q16.16.
        const std::size_t pos = spawn.find(" pos ");
        const std::size_t hp = spawn.find(" hp ", pos);
        ASSERT_NE(pos, std::string::npos);
        ASSERT_NE(hp, std::string::npos);
        const std::string position = spawn.substr(pos + 5, hp - (pos + 5));
        const std::size_t space = position.find(' ');
        ASSERT_NE(space, std::string::npos) << position;
        for (const std::string_view value : {std::string_view(position).substr(0, space),
                                             std::string_view(position).substr(space + 1)}) {
            const std::size_t dot = value.find('.');
            ASSERT_NE(dot, std::string::npos) << value;
            EXPECT_EQ(value.size() - dot, 2U) << "one decimal digit: " << value;
        }
    }
    EXPECT_NE(out.find("tank"), std::string::npos) << out;
    EXPECT_NE(out.find("factory"), std::string::npos) << out;
    EXPECT_NE(out.find("player"), std::string::npos) << out;
    EXPECT_NE(out.find("enemy"), std::string::npos) << out;
}

TEST_F(GameCliTest, WithoutTheFlagNoSpawnLinesArePrinted) {
    if (kUnderThreadSanitizer) {
        GTEST_SKIP() << kTsanReason;
    }
    std::string err;
    std::string out;
    EXPECT_EQ(run({"--headless", "--frames", "1", "--width", "32", "--height", "24"}, err, &out), 0)
        << err;
    EXPECT_EQ(out.find("tick 0 status unit "), std::string::npos) << out;
}

TEST_F(GameCliTest, LogSpawnsPrintsTheSameCompositionTwiceForTheSameSeed) {
    if (kUnderThreadSanitizer) {
        GTEST_SKIP() << kTsanReason;
    }
    std::string err;
    std::string first;
    std::string second;
    const std::vector<std::string_view> args{"--headless", "--frames", "1",  "--width",
                                             "32",         "--height", "24", "--log-spawns"};
    EXPECT_EQ(run(args, err, &first), 0) << err;
    EXPECT_EQ(run(args, err, &second), 0) << err;
    EXPECT_EQ(first, second) << "the spawn log is a pure function of the seed and the scenario";
}

TEST_F(GameCliTest, ARefusedKeyNameNamesTheAcceptedOnes) {
    // `--key` takes space, tab, tilde and f1-f10, in lower case: anything else is a refusal that
    // says so.
    for (const std::string_view bad : {"f11", "f0", "f01", "F3", "enter", "3", "~", "grave"}) {
        std::string err;
        EXPECT_EQ(run({"--headless", "--key", "1", bad}, err), 2) << bad;
        EXPECT_NE(err.find("space, tab, tilde, f1"), std::string::npos) << bad << ": " << err;
    }
    std::string err;
    EXPECT_EQ(run({"--headless", "--key", "0", "f3"}, err), 2) << "frames are 1-based";
    EXPECT_EQ(run({"--headless", "--key", "1"}, err), 2) << "a key with no name";
    EXPECT_EQ(run({"--headless", "--key"}, err), 2) << "a key with nothing at all";
}

TEST_F(GameCliTest, AScriptedKeyReachesTheFrameAndLeavesTheSimAlone) {
    if (kUnderThreadSanitizer) {
        GTEST_SKIP() << kTsanReason;
    }
    // F3 selects the third vehicle: the command row lights its key and, for a unit with a menu, the
    // command menu opens. Both are drawing, so the picture must change while the state hash does
    // not move.
    const std::string before = temp_path("before.ppm");
    const std::string after = temp_path("after.ppm");
    std::string before_err;
    std::string after_err;
    std::string before_out;
    std::string after_out;
    ASSERT_EQ(run({"--headless", "--seed", "42", "--frames", "6", "--capture", before}, before_err,
                  &before_out),
              0)
        << before_err;
    ASSERT_EQ(
        run({"--headless", "--seed", "42", "--frames", "6", "--key", "1", "f3", "--capture", after},
            after_err, &after_out),
        0)
        << after_err;
    ASSERT_NE(before_out.find("hash"), std::string::npos) << before_out;
    EXPECT_EQ(before_out.substr(before_out.find("hash")), after_out.substr(after_out.find("hash")))
        << "selecting is drawing: it must not move the sim";
    EXPECT_NE(slurp(before), slurp(after)) << "--key emitted nothing the frame could show";
}

TEST_F(GameCliTest, TheNavChainPlacesAndHighlightsAndLeavesTheSimAlone) {
    if (kUnderThreadSanitizer) {
        GTEST_SKIP() << kTsanReason;
    }
    // MENU-NAV end to end: `n` arms, SPACE places on the ground under the reticle, `~` opens the
    // nav menu and `1` toggles the first slot's highlight. The marks are the app's, never the
    // sim's, so the state hash is the one a run with no keys at all ends on.
    const std::string plain = temp_path("plain.ppm");
    const std::string navs = temp_path("navs.ppm");
    std::string err;
    std::string plain_out;
    std::string navs_out;
    ASSERT_EQ(
        run({"--headless", "--seed", "42", "--frames", "5", "--capture", plain}, err, &plain_out),
        0)
        << err;
    ASSERT_EQ(run({"--headless", "--seed", "42", "--frames", "5", "--nav", "1", "--key", "2",
                   "space", "--key", "3", "tilde", "--digit", "4", "1", "--capture", navs},
                  err, &navs_out),
              0)
        << err;
    EXPECT_NE(navs_out.find("Nav 1 placed."), std::string::npos) << navs_out;
    EXPECT_NE(navs_out.find("Nav 1 highlighted."), std::string::npos) << navs_out;
    ASSERT_NE(plain_out.find("hash"), std::string::npos) << plain_out;
    EXPECT_EQ(plain_out.substr(plain_out.find("hash")), navs_out.substr(navs_out.find("hash")))
        << "a nav mark must not move the sim";
    EXPECT_NE(slurp(plain), slurp(navs));
}

/// The PPM the harness writes: "P6", the size, 255, then three bytes a pixel.
struct Frame {
    int width{};
    int height{};
    std::vector<unsigned char> rgb{};
};

[[nodiscard]] Frame read_ppm(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string bytes = buffer.str();
    std::size_t at = 0;
    const auto token = [&]() {
        while (at < bytes.size() && std::isspace(static_cast<unsigned char>(bytes.at(at))) != 0) {
            ++at;
        }
        const std::size_t start = at;
        while (at < bytes.size() && std::isspace(static_cast<unsigned char>(bytes.at(at))) == 0) {
            ++at;
        }
        return bytes.substr(start, at - start);
    };
    Frame frame;
    EXPECT_EQ(token(), "P6");
    frame.width = std::stoi(token());
    frame.height = std::stoi(token());
    EXPECT_EQ(token(), "255");
    ++at; // the one whitespace byte after the maximum, then the pixels
    frame.rgb.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.end());
    EXPECT_EQ(frame.rgb.size(),
              static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) * 3U);
    return frame;
}

/// How many ROWS of text a frame draws down its left side, and how many of them are the dim ink.
///
/// A row is a run of inked scanlines: one row of text is ten-odd lines of ink and the gap to the
/// next row is blank. Only the left 40% of the frame is searched -- that is where the command menu
/// lives -- and a pixel counts as ink if it is the LIVE ink (all three channels at 235 or above) or
/// the DIM ink: one grey, the channels equal within a step of each other and around 102. The dim
/// ink is what RB0.5 draws an unaffordable or inapplicable row in, so it has to be counted here or
/// a menu of dimmed build rows reads as no menu at all.
struct MenuRows {
    int live{};
    int dim{};
    [[nodiscard]] int total() const { return live + dim; }
};

[[nodiscard]] MenuRows menu_rows_in_band(const Frame& frame, double top_fraction,
                                         double bottom_fraction) {
    const int top = static_cast<int>(top_fraction * static_cast<double>(frame.height));
    const int bottom = static_cast<int>(bottom_fraction * static_cast<double>(frame.height));
    const int limit = static_cast<int>(0.4 * static_cast<double>(frame.width));
    MenuRows rows;
    bool was_live = false;
    bool was_dim = false;
    for (int y = top; y < bottom; ++y) {
        bool live_here = false;
        bool dim_here = false;
        for (int x = 0; x < limit; ++x) {
            const std::size_t at
                = (static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width)
                   + static_cast<std::size_t>(x))
                  * 3U;
            const int r = frame.rgb.at(at);
            const int g = frame.rgb.at(at + 1U);
            const int b = frame.rgb.at(at + 2U);
            if (r >= 235 && g >= 235 && b >= 235) {
                live_here = true;
            } else if (std::abs(r - g) <= 2 && std::abs(g - b) <= 2 && r >= 95 && r <= 110) {
                dim_here = true;
            }
        }
        if (live_here && !was_live) {
            ++rows.live;
        }
        if (dim_here && !was_dim) {
            ++rows.dim;
        }
        was_live = live_here;
        was_dim = dim_here;
    }
    return rows;
}

TEST_F(GameCliTest, TheMenuIsOnScreenWhenTheRowSelectsTheRecycler) {
    if (kUnderThreadSanitizer) {
        GTEST_SKIP() << kTsanReason;
    }
    // F9 selects the recycler (the ninth vehicle; the factory is a building and sits on key 2, not
    // under an F key), and that selection's command menu is drawn down the left of the frame: six
    // rows, five of them live (SPACE, TAB, ~, 1 FOLLOW ME, 2 GO TO NAV) and the sixth, 3 DEPLOY,
    // dim because deploying does not exist yet (RB3) -- and not one of them before the key is
    // pressed.
    const std::string before = temp_path("menu-before.ppm");
    const std::string after = temp_path("menu-after.ppm");
    std::string err;
    ASSERT_EQ(run({"--headless", "--seed", "42", "--frames", "6", "--capture", before}, err), 0)
        << err;
    ASSERT_EQ(
        run({"--headless", "--seed", "42", "--frames", "6", "--key", "1", "f9", "--capture", after},
            err),
        0)
        << err;

    // The band the menu lives in: below the roster strip and the notice, above the SCRAP label and
    // the radar. Both frames are measured the same way, which is what makes the pair a proof.
    constexpr double kTopFraction = 45.0 / 720.0;
    constexpr double kBottomFraction = 560.0 / 720.0;
    const MenuRows empty = menu_rows_in_band(read_ppm(before), kTopFraction, kBottomFraction);
    const MenuRows menu = menu_rows_in_band(read_ppm(after), kTopFraction, kBottomFraction);
    EXPECT_EQ(empty.total(), 0) << "a frame with no menu must draw no rows in that band";
    EXPECT_EQ(menu.live, 5) << "SPACE, TAB, ~, 1 FOLLOW ME and 2 GO TO NAV are the live rows";
    EXPECT_EQ(menu.dim, 1) << "3 DEPLOY is dim: nothing deploys yet";
}

TEST_F(GameCliTest, FailsWithAReasonWhenTheNamedGpuDoesNotExist) {
    if (kUnderThreadSanitizer) {
        GTEST_SKIP() << kTsanReason;
    }
    std::string err;
    const std::vector<std::string_view> args{"--headless", "--gpu", "not-a-real-gpu"};
    EXPECT_EQ(run(args, err), 4) << err;
    EXPECT_NE(err.find("not-a-real-gpu"), std::string::npos) << err;
}

TEST_F(GameCliTest, WithoutAHeadlessFlagAndWithNoDisplayItRefusesCleanly) {
    if (display_available()) {
        GTEST_SKIP() << "this machine has a display, so the no-display path cannot be exercised";
    }
    std::string err;
    const std::vector<std::string_view> args{"--frames", "1"};
    EXPECT_EQ(run(args, err), 4) << "a window that cannot be opened must be a clean refusal, not a "
                                    "crash and not a hang";
    EXPECT_FALSE(err.empty()) << "the refusal must say why";
}

} // namespace
