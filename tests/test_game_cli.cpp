// The game loop, headless.
//
// The loop does not know whether it is presenting to a window or to a buffer, so everything except
// the window itself is testable here -- on a machine with no display and no GPU.
#include <gtest/gtest.h>

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

TEST_F(GameCliTest, ARefusedKeyNameNamesTheAcceptedOnes) {
    // `--key` takes space, tab and f1-f10, in lower case: anything else is a refusal that says so.
    for (const std::string_view bad : {"f11", "f0", "f01", "F3", "enter", "3"}) {
        std::string err;
        EXPECT_EQ(run({"--headless", "--key", "1", bad}, err), 2) << bad;
        EXPECT_NE(err.find("space, tab, f1"), std::string::npos) << bad << ": " << err;
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
    // F3 selects the factory group: the roster strip lights it and the command menu opens. Both
    // are drawing, so the picture must change while the state hash does not move.
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

TEST_F(GameCliTest, TheMenuIsOnScreenWhenTheRosterSelectsAFactory) {
    if (kUnderThreadSanitizer) {
        GTEST_SKIP() << kTsanReason;
    }
    // F3 selects the factory group, and that selection's command menu is drawn down the left of the
    // frame. Scrap is 0 at seed 42, so all SEVEN build rows are unaffordable and drawn in the dim
    // ink while the state row, TAB CLOSE MENU and ~ DROP OFF stay live: ten rows, three of them
    // live, and not one of them before the key is pressed. RB5 fills the build list, and this count
    // changes with it.
    const std::string before = temp_path("menu-before.ppm");
    const std::string after = temp_path("menu-after.ppm");
    std::string err;
    ASSERT_EQ(run({"--headless", "--seed", "42", "--frames", "6", "--capture", before}, err), 0)
        << err;
    ASSERT_EQ(
        run({"--headless", "--seed", "42", "--frames", "6", "--key", "1", "f3", "--capture", after},
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
    EXPECT_EQ(menu.live, 3) << "the state row, TAB CLOSE MENU and ~ DROP OFF are the live rows";
    EXPECT_EQ(menu.dim, 7) << "no scrap is banked, so every build row is dim";
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
