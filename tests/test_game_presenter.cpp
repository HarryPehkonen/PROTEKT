// The presentation seam of the game loop: a window if there is one, an offscreen buffer if not.
// The window half needs a display, so what is pinned here is the headless half and the refusals.
#include <gtest/gtest.h>

#include <cstddef>
#include <sstream>
#include <string>

#include "protekt/app/game_presenter.hpp"
#include "protekt/input/input.hpp"
#include "protekt/render/renderer.hpp"

namespace {

using protekt::app::GamePresenter;
using protekt::app::PresenterOptions;
using protekt::game::Extent;

/// llvmpipe -- the only device on the gate box -- spawns worker threads and races inside itself.
/// The driver is not built with ThreadSanitizer and cannot be instrumented, so its internal races
/// are noise this project cannot fix (docs/PLAN.md states the resulting gap). Every test below
/// creates a real presenter, which opens the driver, so each skips under TSan, as the other
/// rendering tests do (test_render.cpp, test_game_cli.cpp).
#if defined(__SANITIZE_THREAD__)
constexpr bool kUnderThreadSanitizer = true;
#else
constexpr bool kUnderThreadSanitizer = false;
#endif

constexpr const char* kTsanReason
    = "skipped under ThreadSanitizer: the Vulkan driver (lavapipe) races inside itself and cannot "
      "be instrumented; see docs/PLAN.md";

PresenterOptions headless(Extent extent) {
    PresenterOptions options;
    options.extent = extent;
    options.headless = true;
    return options;
}

TEST(GamePresenter, HeadlessHasNoWindowAndSaysWhichDeviceItIsOn) {
    if (kUnderThreadSanitizer) {
        GTEST_SKIP() << kTsanReason;
    }
    std::ostringstream out;
    std::ostringstream err;
    const auto presenter = GamePresenter::create(headless(Extent{64, 48}), out, err);
    if (presenter == nullptr) {
        GTEST_SKIP() << "no Vulkan device here: " << err.str();
    }
    EXPECT_FALSE(presenter->windowed());
    EXPECT_NE(out.str().find("protekt-game: headless on "), std::string::npos);
}

TEST(GamePresenter, HeadlessPollsAnEmptyHandsAndNeverCloses) {
    if (kUnderThreadSanitizer) {
        GTEST_SKIP() << kTsanReason;
    }
    std::ostringstream out;
    std::ostringstream err;
    const auto presenter = GamePresenter::create(headless(Extent{64, 48}), out, err);
    if (presenter == nullptr) {
        GTEST_SKIP() << "no Vulkan device here";
    }
    protekt::input::InputFrame hands{};
    EXPECT_TRUE(presenter->poll(hands, false));
    EXPECT_TRUE(hands.keys.empty());
    EXPECT_FALSE(hands.cursor_known);
}

TEST(GamePresenter, HeadlessKeepsItsExtentWhateverTheHandsAsk) {
    if (kUnderThreadSanitizer) {
        GTEST_SKIP() << kTsanReason;
    }
    std::ostringstream out;
    std::ostringstream err;
    const auto presenter = GamePresenter::create(headless(Extent{64, 48}), out, err);
    if (presenter == nullptr) {
        GTEST_SKIP() << "no Vulkan device here";
    }
    const protekt::input::InputFrame hands{};
    const Extent extent = presenter->follow(hands);
    EXPECT_EQ(extent.width, 64U);
    EXPECT_EQ(extent.height, 48U);
}

TEST(GamePresenter, PresentKeepsTheLastFrameForCapture) {
    if (kUnderThreadSanitizer) {
        GTEST_SKIP() << kTsanReason;
    }
    std::ostringstream out;
    std::ostringstream err;
    const auto presenter = GamePresenter::create(headless(Extent{64, 48}), out, err);
    if (presenter == nullptr) {
        GTEST_SKIP() << "no Vulkan device here";
    }
    EXPECT_TRUE(presenter->finish().empty()) << "nothing has been presented yet";
    const protekt::game::Scene scene{};
    EXPECT_TRUE(presenter->present(scene, 7, 0));
    EXPECT_TRUE(presenter->present(scene, 7, 1));
    EXPECT_EQ(presenter->finish().size(), std::size_t{64} * 48 * 4);
}

TEST(GamePresenter, AnUnknownDeviceIsRefusedWithItsNameAndNoPresenter) {
    if (kUnderThreadSanitizer) {
        GTEST_SKIP() << kTsanReason;
    }
    std::ostringstream out;
    std::ostringstream err;
    PresenterOptions options = headless(Extent{64, 48});
    options.gpu = "no-such-gpu-anywhere";
    EXPECT_EQ(GamePresenter::create(options, out, err), nullptr);
    EXPECT_NE(err.str().find("no-such-gpu-anywhere"), std::string::npos);
}

} // namespace

namespace {
TEST(GamePresenter, WithNoReadbackAFrameStillPresentsAndFinishIsEmpty) {
    if (kUnderThreadSanitizer) {
        GTEST_SKIP() << kTsanReason;
    }
    std::ostringstream out;
    std::ostringstream err;
    PresenterOptions options = headless(Extent{64, 48});
    options.readback = false;
    const auto presenter = GamePresenter::create(options, out, err);
    if (presenter == nullptr) {
        GTEST_SKIP() << "no Vulkan device here";
    }
    const protekt::game::Scene scene{};
    EXPECT_TRUE(presenter->present(scene, 7, 0)) << "a frame with no readback still presented";
    EXPECT_TRUE(presenter->present(scene, 7, 1));
    EXPECT_TRUE(presenter->finish().empty()) << "nobody asked for pixels, so none were kept";
}
} // namespace
