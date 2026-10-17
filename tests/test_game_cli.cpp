// The game loop, headless.
//
// The loop does not know whether it is presenting to a window or to a buffer, so everything except
// the window itself is testable here -- on a machine with no display and no GPU.
#include <gtest/gtest.h>

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
