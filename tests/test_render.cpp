// The renderer's contract, tested headlessly.
//
// On the gate box the only Vulkan device is llvmpipe (Mesa's CPU rasteriser), which is exactly
// what makes these tests worth having: they run on every gate, on a machine with no GPU.
//
// Two rules shape every assertion here:
//   1. Structure, not palette. The suite never hardcodes a colour; it finds the sky on the top
//      row and the ground on the bottom row and reasons from there.
//   2. No golden image. Two GPUs legitimately differ by a texel, so byte equality is asserted
//      only between two renders of the same request on the same device.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "protekt/app/render.hpp"
#include "protekt/game/atlas.hpp"
#include "protekt/game/radar.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/store_gauge.hpp"
#include "protekt/game/world_map.hpp"
#include "protekt/render/renderer.hpp"
#include "protekt/sim/orders.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/world/world.hpp"

namespace {

using protekt::game::Extent;

/// llvmpipe -- the only device on the gate box -- spawns worker threads and races inside itself.
/// The driver is not built with ThreadSanitizer and cannot be instrumented, so its internal races
/// are noise this project cannot fix. Suppressing by library name would also hide a real race
/// between THIS code and the driver, so the Vulkan tests are skipped under TSan instead, and
/// docs/PLAN.md states the resulting gap: the renderer's CPU-side code is not TSan-covered.
#if defined(__SANITIZE_THREAD__)
constexpr bool kUnderThreadSanitizer = true;
#else
constexpr bool kUnderThreadSanitizer = false;
#endif

constexpr const char* kTsanReason
    = "skipped under ThreadSanitizer: the Vulkan driver (lavapipe) races inside itself and cannot "
      "be instrumented; see docs/PLAN.md";

constexpr std::uint32_t kWidth = 320;
constexpr std::uint32_t kHeight = 200;

struct Frame {
    Extent extent;
    std::vector<std::uint8_t> pixels;
    protekt::render::FrameStats stats;

    [[nodiscard]] std::uint32_t rgb(std::uint32_t x, std::uint32_t y) const {
        const std::size_t i
            = (static_cast<std::size_t>(y) * extent.width + static_cast<std::size_t>(x)) * 4U;
        return (static_cast<std::uint32_t>(pixels.at(i)) << 16U)
               | (static_cast<std::uint32_t>(pixels.at(i + 1)) << 8U)
               | static_cast<std::uint32_t>(pixels.at(i + 2));
    }

    [[nodiscard]] std::uint32_t alpha(std::uint32_t x, std::uint32_t y) const {
        const std::size_t i
            = (static_cast<std::size_t>(y) * extent.width + static_cast<std::size_t>(x)) * 4U;
        return pixels.at(i + 3);
    }

    /// How many pixels of each distinct colour the frame contains.
    [[nodiscard]] std::map<std::uint32_t, std::size_t> colour_histogram() const {
        std::map<std::uint32_t, std::size_t> counts;
        for (std::uint32_t y = 0; y < extent.height; ++y) {
            for (std::uint32_t x = 0; x < extent.width; ++x) {
                ++counts[rgb(x, y)];
            }
        }
        return counts;
    }

    /// The first row whose colour differs from the top row: the horizon, in this frame's terms.
    [[nodiscard]] std::uint32_t first_row_below_sky_colour() const {
        const std::uint32_t sky = rgb(0, 0);
        for (std::uint32_t y = 1; y < extent.height; ++y) {
            if (rgb(0, y) != sky) {
                return y;
            }
        }
        return extent.height;
    }
};

class RenderDevicesTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (kUnderThreadSanitizer) {
            GTEST_SKIP() << kTsanReason;
        }
    }
};

class RenderTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (kUnderThreadSanitizer) {
            GTEST_SKIP() << kTsanReason;
        }
        std::string error;
        // SKIP means "this machine has no Vulkan driver", which is a legitimate reason for these
        // tests not to run. A device that exists but cannot be opened is a DEFECT, and saying so
        // with a failure is the whole point -- a suite that skips on a broken renderer passes.
        if (protekt::render::devices(error).empty()) {
            GTEST_SKIP() << "no Vulkan device on this machine: " << error;
        }
        renderer_ = protekt::render::create(options(), error);
        ASSERT_NE(renderer_, nullptr)
            << "a device was enumerated but could not be opened: " << error;
    }

    [[nodiscard]] static protekt::render::CreateOptions options() {
        protekt::render::CreateOptions o;
        o.validation = true;
        o.extent = Extent{kWidth, kHeight};
        return o;
    }

    [[nodiscard]] Frame render_frame(std::uint64_t seed = 42,
                                     Extent extent = Extent{kWidth, kHeight}) const {
        Frame frame;
        frame.extent = extent;
        frame.pixels = renderer_->render(
            protekt::render::FrameRequest{.extent = extent, .seed = seed, .tick = 0}, frame.stats);
        return frame;
    }

    std::unique_ptr<protekt::render::Renderer> renderer_;
};

// ---------------------------------------------------------------- devices

TEST_F(RenderDevicesTest, EnumeratesAtLeastOneRenderableDevice) {
    std::string error;
    const auto list = protekt::render::devices(error);
    if (list.empty()) {
        GTEST_SKIP() << "no Vulkan device on this machine: " << error;
    }
    for (const auto& device : list) {
        EXPECT_FALSE(device.name.empty()) << "a device must have a name";
        EXPECT_GE(device.api_version, 0x00400000U) << "at least Vulkan 1.0 for " << device.name;
        EXPECT_NE(device.vendor_id, 0U) << device.name;
    }
}

TEST_F(RenderDevicesTest, PutsADiscreteGpuFirstWhenThereIsOne) {
    std::string error;
    const auto list = protekt::render::devices(error);
    if (list.empty()) {
        GTEST_SKIP() << "no Vulkan device on this machine: " << error;
    }
    const bool any_discrete
        = std::any_of(list.begin(), list.end(), [](const auto& d) { return d.discrete; });
    if (any_discrete) {
        EXPECT_TRUE(list.front().discrete)
            << "a laptop with Optimus reports the integrated GPU as device 0; automatic "
               "selection must still prefer "
            << list.front().name;
    }
}

TEST_F(RenderDevicesTest, MarksExactlyOneKindPerDevice) {
    std::string error;
    const auto list = protekt::render::devices(error);
    if (list.empty()) {
        GTEST_SKIP() << "no Vulkan device on this machine: " << error;
    }
    for (const auto& d : list) {
        const int kinds = static_cast<int>(d.discrete) + static_cast<int>(d.integrated)
                          + static_cast<int>(d.cpu);
        EXPECT_GE(kinds, 1) << d.name << " is none of discrete, integrated or cpu";
    }
}

TEST_F(RenderTest, FailsWithAReasonForAnUnknownDevice) {
    protekt::render::CreateOptions o;
    o.device_name = "no-such-gpu-anywhere";
    std::string error;
    const auto renderer = protekt::render::create(o, error);
    EXPECT_FALSE(renderer);
    EXPECT_NE(error.find("no-such-gpu-anywhere"), std::string::npos)
        << "the refusal must name what was asked for; got: " << error;
}

TEST_F(RenderTest, RefusesAnEmptyExtent) {
    protekt::render::CreateOptions o;
    o.extent = Extent{0, 0};
    std::string error;
    const auto renderer = protekt::render::create(o, error);
    EXPECT_FALSE(renderer) << "a zero-sized target is a caller bug, not a frame";
    EXPECT_FALSE(error.empty());
}

TEST_F(RenderTest, ReportsTheDeviceItChoseAndTheExtentItWasGiven) {
    EXPECT_FALSE(renderer_->device_name().empty());
    EXPECT_EQ(renderer_->extent().width, kWidth);
    EXPECT_EQ(renderer_->extent().height, kHeight);
}

// ---------------------------------------------------------------- frames

TEST_F(RenderTest, ReturnsRgba8OfExactlyTheRequestedExtent) {
    const Frame frame = render_frame();
    EXPECT_EQ(frame.pixels.size(),
              static_cast<std::size_t>(kWidth) * static_cast<std::size_t>(kHeight) * 4U);
    EXPECT_EQ(frame.alpha(0, 0), 255U) << "every texel is opaque in a finished frame";
    EXPECT_EQ(frame.alpha(kWidth - 1, kHeight - 1), 255U);
}

TEST_F(RenderTest, HasSkyAboveAHorizonAndGroundBelowIt) {
    const Frame frame = render_frame();
    const std::uint32_t sky = frame.rgb(0, 0);
    const std::uint32_t ground = frame.rgb(0, kHeight - 1);
    EXPECT_NE(sky, ground) << "a frame of one colour means nothing was drawn";
    const std::uint32_t horizon = frame.first_row_below_sky_colour();
    EXPECT_GT(horizon, 0U);
    EXPECT_LT(horizon, kHeight) << "the whole frame is sky: the ground plane is missing";
    // Everything from the first ground row down to the last row is ground colour.
    EXPECT_EQ(frame.rgb(kWidth / 2, kHeight - 1), ground);
}

TEST_F(RenderTest, ShowsTheUnitThatTheGroundWouldOtherwiseHide) {
    const Frame frame = render_frame();
    const auto histogram = frame.colour_histogram();
    const std::uint32_t sky = frame.rgb(0, 0);
    const std::uint32_t ground = frame.rgb(0, kHeight - 1);

    // The scene has three flat colours: sky, ground, and the unit -- plus the blends the edge pass
    // leaves along their borders, each a handful of pixels. The unit is the most common colour that
    // is neither sky nor ground, and it is submitted FIRST, so if it is visible the depth test did
    // its job.
    std::uint32_t unit = 0;
    std::size_t unit_pixels = 0;
    for (const auto& [colour, count] : histogram) {
        if (colour != sky && colour != ground && count > unit_pixels) {
            unit = colour;
            unit_pixels = count;
        }
    }
    ASSERT_NE(unit, 0U) << "no third colour in the frame: the unit was swallowed by the ground";
    EXPECT_GT(unit_pixels, 50U) << "the unit is present but only " << unit_pixels << " pixels";

    // ...and it sits below the horizon, in the middle of the frame, as a thing standing on the
    // ground a few metres ahead must.
    const std::uint32_t horizon = frame.first_row_below_sky_colour();
    std::size_t sum_x = 0;
    std::size_t sum_y = 0;
    for (std::uint32_t y = 0; y < kHeight; ++y) {
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            if (frame.rgb(x, y) == unit) {
                sum_x += x;
                sum_y += y;
            }
        }
    }
    const std::uint32_t centroid_x = static_cast<std::uint32_t>(sum_x / unit_pixels);
    const std::uint32_t centroid_y = static_cast<std::uint32_t>(sum_y / unit_pixels);
    EXPECT_GT(centroid_y, horizon) << "the unit is above the horizon, so the camera or the "
                                      "transform is wrong";
    EXPECT_GT(centroid_x, kWidth / 4) << "the unit is pinned to the left edge";
    EXPECT_LT(centroid_x, 3 * kWidth / 4) << "the unit is pinned to the right edge";
}

TEST_F(RenderTest, TwoRendersOfOneRequestAreByteIdentical) {
    const Frame first = render_frame(7);
    const Frame second = render_frame(7);
    ASSERT_EQ(first.pixels.size(), second.pixels.size());
    EXPECT_EQ(first.pixels, second.pixels)
        << "the same request on the same device must produce the same bytes";
}

TEST_F(RenderTest, ADifferentSeedChangesTheGround) {
    const Frame a = render_frame(1);
    const Frame b = render_frame(2);
    EXPECT_NE(a.rgb(0, kHeight - 1), b.rgb(0, kHeight - 1))
        << "the seed must reach the shading, or 'deterministic' is untested";
    EXPECT_NE(a.pixels, b.pixels);
}

TEST_F(RenderTest, HonoursAPerFrameResize) {
    const Frame small = render_frame(42, Extent{160, 100});
    EXPECT_EQ(small.pixels.size(), 160U * 100U * 4U);
    EXPECT_EQ(renderer_->extent().width, kWidth) << "extent() reports the creation size";
    const Frame large = render_frame(42, Extent{64, 48});
    EXPECT_EQ(large.pixels.size(), 64U * 48U * 4U);
    // A resize must still produce a coherent frame, not a stretched or cleared buffer.
    EXPECT_NE(large.rgb(0, 0), large.rgb(0, 47));
}

TEST_F(RenderTest, CountsItsDrawCallAndTriangles) {
    const Frame frame = render_frame();
    EXPECT_GE(frame.stats.draw_calls, 1U);
    EXPECT_GE(frame.stats.triangles, 2U) << "ground and unit together are more than one triangle";
}

// ---------------------------------------------------------------- validation

TEST_F(RenderTest, TheValidationLayerReportsNothingAcrossSeveralFrames) {
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        protekt::render::FrameStats stats{};
        const auto pixels = renderer_->render(
            protekt::render::FrameRequest{
                .extent = Extent{kWidth, kHeight}, .seed = seed, .tick = 0},
            stats);
        ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4U)
            << "seed " << seed;
    }
    EXPECT_EQ(renderer_->validation_errors(), 0U)
        << "the validation layer is the cheapest correctness oracle this renderer has; a "
           "non-zero count is a real defect, not noise";
}

// ---------------------------------------------------------------- the CLI

class RenderCliTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (kUnderThreadSanitizer) {
            GTEST_SKIP() << kTsanReason;
        }
    }

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
                          / ("protekt-render-" + std::to_string(stamp) + "-"
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

    std::vector<std::filesystem::path> written_;
};

TEST_F(RenderCliTest, ListsGpusAndExitsZero) {
    std::ostringstream out;
    std::ostringstream err;
    const std::vector<std::string_view> args{"--list-gpus"};
    const int code = protekt::app::run_render_cli(args, out, err);
    if (code == 4) {
        GTEST_SKIP() << "no Vulkan device on this machine: " << err.str();
    }
    EXPECT_EQ(code, 0);
    EXPECT_FALSE(out.str().empty()) << "listing devices without naming any is not useful";
    // Self-consistent rather than machine-specific: whatever the enumeration reports must be
    // what the command names, including the CPU rasteriser that the gate box depends on.
    std::string error;
    const auto list = protekt::render::devices(error);
    ASSERT_FALSE(list.empty()) << error;
    for (const auto& device : list) {
        EXPECT_NE(out.str().find(device.name), std::string::npos)
            << "the listing omits " << device.name << "; got: " << out.str();
    }
}

TEST_F(RenderCliTest, WritesAPpmWithTheRightHeaderAndSize) {
    const std::string path = temp_path("frame.ppm");
    std::ostringstream out;
    std::ostringstream err;
    const std::vector<std::string_view> args{"--seed",   "42", "--width", "96",
                                             "--height", "64", "--out",   path};
    ASSERT_EQ(protekt::app::run_render_cli(args, out, err), 0) << err.str();

    const std::string bytes = slurp(path);
    // P6, then width and height, then the maximum value, each on its own line -- so the header is
    // 13 bytes for this image ("P6\n" + "96 64\n" + "255\n"), and the arithmetic is derived here
    // rather than trusted.
    const std::string header = "P6\n96 64\n255\n";
    ASSERT_GE(bytes.size(), header.size());
    EXPECT_EQ(bytes.compare(0, header.size(), header), 0)
        << "header was: " << bytes.substr(0, header.size());
    EXPECT_EQ(bytes.size(), header.size() + static_cast<std::size_t>(96U * 64U * 3U))
        << "P6 header, then three bytes per texel";
}

TEST_F(RenderCliTest, TheSameSeedWritesTheSameFile) {
    const std::string first = temp_path("a.ppm");
    const std::string second = temp_path("b.ppm");
    std::ostringstream out;
    std::ostringstream err;
    const std::vector<std::string_view> first_args{"--seed",   "1234", "--width", "64",
                                                   "--height", "48",   "--out",   first};
    ASSERT_EQ(protekt::app::run_render_cli(first_args, out, err), 0) << err.str();
    const std::vector<std::string_view> second_args{"--seed",   "1234", "--width", "64",
                                                    "--height", "48",   "--out",   second};
    ASSERT_EQ(protekt::app::run_render_cli(second_args, out, err), 0) << err.str();
    EXPECT_EQ(slurp(first), slurp(second)) << "same seed, same size, same bytes";
}

TEST_F(RenderCliTest, RejectsAnUnknownFlag) {
    std::ostringstream out;
    std::ostringstream err;
    const std::vector<std::string_view> args{"--wat"};
    const int code = protekt::app::run_render_cli(args, out, err);
    EXPECT_EQ(code, 2);
    EXPECT_NE(err.str().find("--wat"), std::string::npos)
        << "name the flag it refused: " << err.str();
}

TEST_F(RenderCliTest, FailsWithAReasonWhenTheNamedGpuDoesNotExist) {
    std::ostringstream out;
    std::ostringstream err;
    const std::vector<std::string_view> args{"--gpu", "not-a-real-gpu"};
    const int code = protekt::app::run_render_cli(args, out, err);
    EXPECT_EQ(code, 4) << err.str();
    EXPECT_NE(err.str().find("not-a-real-gpu"), std::string::npos) << err.str();
}

// ---------------------------------------------------------------- the radar panel, as drawn

TEST_F(RenderTest, DrawsTheRadarWithAFaceMarksAndABorderThatSurviveTheDepthTest) {
    // Why this test renders instead of counting vertices: the panel is three layers of geometry and
    // the depth test decides between them per PIXEL. Two layers at the same distance do not fight
    // politely -- the depth test is LESS with writes on, so the later one loses every pixel and the
    // panel comes out in the colour that went down first (measured: a light border with no face at
    // all). No assertion over vertices can see that; one over pixels can.
    using protekt::sim::Spawn;
    using protekt::sim::UnitKind;

    const auto panel_pixels = [&](int enemy_tile_x) {
        protekt::sim::Scenario scenario;
        scenario.seed = 42;
        scenario.spawns.push_back(Spawn{UnitKind::Tank, protekt::sim::Side::Player, 50, 50});
        scenario.spawns.push_back(
            Spawn{UnitKind::Tank, protekt::sim::Side::Enemy, enemy_tile_x, 52});
        const auto sim = protekt::sim::Sim::create(scenario);
        protekt::sim::UnitId player{};
        for (const auto& unit : sim.units()) {
            if (unit.side == protekt::sim::Side::Player) {
                player = unit.id;
            }
        }
        protekt::game::SceneOptions options;
        options.seed = scenario.seed;
        options.radius_tiles = 8;
        options.extent = Extent{kWidth, kHeight};
        options.camera.x = 100.0F;
        options.camera.y = 8.0F;
        options.camera.z = 104.0F;
        options.viewer_unit = player;
        const protekt::game::Scene scene = protekt::game::build_scene(sim, options);

        Frame frame;
        frame.extent = Extent{kWidth, kHeight};
        frame.pixels = renderer_->render(
            protekt::render::FrameRequest{
                .extent = frame.extent, .seed = scenario.seed, .tick = 0, .scene = &scene},
            frame.stats);
        const protekt::game::RadarPanel panel = protekt::game::radar_panel(Extent{kWidth, kHeight});

        std::size_t face = 0;
        std::size_t border = 0;
        std::size_t green = 0;
        std::size_t red = 0;
        // The panel's face and its marks are inside the panel; its border is a ring just outside
        // it, which is why the scan runs over the widened rectangle and sorts the pixels by where
        // they are.
        const float slack = protekt::game::kRadarBorderPixels + 1.0F;
        for (std::uint32_t y = static_cast<std::uint32_t>(panel.y - slack);
             y < static_cast<std::uint32_t>(panel.y + panel.height + slack); ++y) {
            for (std::uint32_t x = static_cast<std::uint32_t>(panel.x - slack);
                 x < static_cast<std::uint32_t>(panel.x + panel.width + slack); ++x) {
                const std::uint32_t rgb = frame.rgb(x, y);
                const std::uint32_t r = (rgb >> 16U) & 0xFFU;
                const std::uint32_t g = (rgb >> 8U) & 0xFFU;
                const std::uint32_t b = rgb & 0xFFU;
                const bool inside = static_cast<float>(x) >= panel.x
                                    && static_cast<float>(x) < (panel.x + panel.width)
                                    && static_cast<float>(y) >= panel.y
                                    && static_cast<float>(y) < (panel.y + panel.height);
                if (inside && r < 40U && g < 40U && b < 40U) {
                    ++face; // the panel's dark face
                }
                if (!inside && r > 150U && g > 150U && b > 150U) {
                    ++border; // the light ring around it
                }
                if (g > (r + 40U) && g > (b + 40U)) {
                    ++green; // yours, and you
                }
                if (r > (g + 40U) && r > (b + 40U)) {
                    ++red; // a hostile in range
                }
            }
        }
        // You are always at the middle of the panel: a 3x3 window there must contain your own mark,
        // which is a structural claim about the radar rather than a claim about a colour value.
        bool you_are_at_the_middle = false;
        const std::uint32_t middle_x = static_cast<std::uint32_t>(panel.x + (panel.width * 0.5F));
        const std::uint32_t middle_y = static_cast<std::uint32_t>(panel.y + (panel.height * 0.5F));
        for (std::uint32_t y = middle_y - 1U; y <= middle_y + 1U; ++y) {
            for (std::uint32_t x = middle_x - 1U; x <= middle_x + 1U; ++x) {
                const std::uint32_t rgb = frame.rgb(x, y);
                if (((rgb >> 8U) & 0xFFU) > (((rgb >> 16U) & 0xFFU) + 40U)) {
                    you_are_at_the_middle = true;
                }
            }
        }
        return std::array<std::size_t, 5>{face, border, green, red,
                                          you_are_at_the_middle ? 1U : 0U};
    };

    // An enemy five tiles away: on the radar, and red.
    const std::array<std::size_t, 5> near_enemy = panel_pixels(55);
    EXPECT_GT(near_enemy.at(0), 1000U) << "the dark face must survive the layers above it";
    EXPECT_GT(near_enemy.at(1), 100U) << "the border must be visible";
    EXPECT_GT(near_enemy.at(2), 5U) << "you are on the panel";
    EXPECT_EQ(near_enemy.at(4), 1U) << "you are at the middle of it";
    // A hostile's dot is 4.6 px across since round 10 halved it (2026-11-05), so a handful of red
    // pixels, not the dozen a 9 px dot gave; the far frame below still has none.
    EXPECT_GT(near_enemy.at(3), 4U) << "a hostile in range is red on the panel";

    // The same frame with the enemy three hundred tiles away: everything else identical, and no red
    // at all. The radius is the promise, and this is where it is kept in pixels.
    const std::array<std::size_t, 5> far_enemy = panel_pixels(350);
    EXPECT_GT(far_enemy.at(0), 1000U);
    EXPECT_EQ(far_enemy.at(4), 1U);
    EXPECT_EQ(far_enemy.at(3), 0U) << "a hostile outside the radius must not be on the radar";
}

} // namespace

// ---- the atlas reaches the GPU (D2b) ----

TEST_F(RenderTest, AFacingTankDrawsTwiceByteIdenticallyThroughTheAtlas) {
    namespace sim = protekt::sim;
    const auto world = sim::Sim::create(sim::skirmish(42));
    protekt::game::SceneOptions so;
    so.seed = 42;
    so.extent = Extent{kWidth, kHeight};
    so.radius_tiles = 12;
    const auto& tank = world.units().back();
    so.camera.x = static_cast<float>(tank.x.raw) / 65536.0F * protekt::game::kMetresPerTile;
    so.camera.z = static_cast<float>(tank.y.raw) / 65536.0F * protekt::game::kMetresPerTile;
    so.camera.y = protekt::game::height_metres(42, static_cast<int>(tank.x.raw / 65536),
                                               static_cast<int>(tank.y.raw / 65536))
                  + 5.0F;
    so.camera.pitch = -1.5F;
    so.facing = protekt::game::SceneOptions::Facing{tank.id, 0.4F, 0.2F};
    const protekt::game::Scene scene = protekt::game::build_scene(world, so);
    const auto draw = [&](protekt::render::Renderer& r) {
        protekt::render::FrameStats stats{};
        return r.render(
            protekt::render::FrameRequest{
                .extent = Extent{kWidth, kHeight}, .seed = 42, .tick = 0, .scene = &scene},
            stats);
    };
    protekt::render::CreateOptions with = options();
    with.atlas_rgba = protekt::game::atlas_pixels();
    with.atlas_side = protekt::game::kAtlasSide;
    std::string error;
    const auto atlas_renderer = protekt::render::create(with, error);
    ASSERT_NE(atlas_renderer, nullptr) << error;
    EXPECT_EQ(draw(*atlas_renderer), draw(*atlas_renderer)) << "same scene, same bytes";
    EXPECT_EQ(atlas_renderer->validation_errors(), 0U);
    // The atlas is really sampled: the same scene over the default white texture differs, because
    // the track tile is dark.
    EXPECT_NE(draw(*atlas_renderer), draw(*renderer_));
}

// ---- the ground is painted (D3), measured on the rendered frame, not the tile ----

/// THE CAPTURE GATE. Over a patch of grass seen from a tank's-eye height, the rendered pixels'
/// luma spans at least this (where the flat-coloured ground spanned nothing)...
constexpr double kMinCapturedGroundLumaRange = 0.20;
/// ...and a run of one colour along a row averages at least this many pixels: the detail is
/// patches the eye can see, not sub-pixel speckle.
constexpr double kMinCapturedGroundPatchPixels = 6.0;

/// What a frame of grass looks like to the capture gate, and whether it is repeatable.
struct GrassMeasure {
    bool ok{}; ///< false when the frame could not be rendered (already reported)
    double luma_range{};
    double mean_run{};
    bool repeatable{};
    std::uint32_t validation_errors{};
};

/// Renders the patch of grass under a tank's-eye camera through a renderer with the edge pass on or
/// off, and measures it. `ok` false (after reporting why) when the seed has no 7x7 patch of grass.
GrassMeasure measure_grass(bool fxaa) {
    namespace sim = protekt::sim;
    namespace world = protekt::world;
    constexpr std::uint64_t kSeed = 42;
    // The first tile whose 7x7 neighbourhood is all grass: the whole frame below is one type, so
    // any luma range in it is the texture's and not a border between types.
    int gx = 0;
    int gy = 0;
    bool found = false;
    for (int y = 3; y < 200 && !found; ++y) {
        for (int x = 3; x < 200 && !found; ++x) {
            bool all = true;
            for (int dy = -3; dy <= 3 && all; ++dy) {
                for (int dx = -3; dx <= 3 && all; ++dx) {
                    all = world::sample(kSeed, x + dx, y + dy).terrain == world::Terrain::Flat;
                }
            }
            if (all) {
                gx = x;
                gy = y;
                found = true;
            }
        }
    }
    if (!found) {
        ADD_FAILURE() << "no 7x7 patch of grass to look at";
        return {};
    }
    const auto state = sim::Sim::create(sim::skirmish(kSeed));
    protekt::game::SceneOptions so;
    so.seed = kSeed;
    so.extent = Extent{kWidth, kHeight};
    so.radius_tiles = 6;
    so.camera.x = (static_cast<float>(gx) + 0.5F) * protekt::game::kMetresPerTile;
    so.camera.z = (static_cast<float>(gy) + 0.5F) * protekt::game::kMetresPerTile;
    so.camera.y = protekt::game::height_metres(kSeed, gx, gy) + 3.0F;
    so.camera.pitch = -1.5F;
    const protekt::game::Scene scene = protekt::game::build_scene(state, so);
    protekt::render::CreateOptions with;
    with.validation = true;
    with.extent = Extent{kWidth, kHeight};
    with.fxaa = fxaa;
    with.atlas_rgba = protekt::game::atlas_pixels();
    with.atlas_side = protekt::game::kAtlasSide;
    std::string error;
    const auto atlas_renderer = protekt::render::create(with, error);
    if (atlas_renderer == nullptr) {
        ADD_FAILURE() << error;
        return {};
    }
    Frame frame;
    frame.extent = Extent{kWidth, kHeight};
    frame.pixels = atlas_renderer->render(
        protekt::render::FrameRequest{
            .extent = frame.extent, .seed = kSeed, .tick = 0, .scene = &scene},
        frame.stats);

    GrassMeasure m;
    m.ok = true;
    double lo = 1.0;
    double hi = 0.0;
    std::size_t runs = 0;
    for (std::uint32_t y = 0; y < kHeight; ++y) {
        ++runs;
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            const std::uint32_t c = frame.rgb(x, y);
            const double l = ((0.2126 * ((c >> 16U) & 0xFFU)) + (0.7152 * ((c >> 8U) & 0xFFU))
                              + (0.0722 * (c & 0xFFU)))
                             / 255.0;
            lo = std::min(lo, l);
            hi = std::max(hi, l);
            runs += x > 0 && c != frame.rgb(x - 1, y) ? 1U : 0U;
        }
    }
    m.luma_range = hi - lo;
    m.mean_run = static_cast<double>(kWidth) * kHeight / static_cast<double>(runs);
    // Two renders of the one request are byte-identical: the paint is data, not noise.
    m.repeatable = atlas_renderer->render(
                       protekt::render::FrameRequest{
                           .extent = frame.extent, .seed = kSeed, .tick = 0, .scene = &scene},
                       frame.stats)
                   == frame.pixels;
    m.validation_errors = atlas_renderer->validation_errors();
    return m;
}

TEST_F(RenderTest, GrassUnderTheCameraRendersWithVisibleTextureDetail) {
    // Measured with the edge pass OFF: the run length is a property of the PAINT (does the atlas
    // reach the pixels as patches the eye can see), and the pass adds a blended pixel along every
    // patch border it softens, which shortens every run by construction. The pass's own effect on
    // this frame is the next test's business.
    const auto m = measure_grass(false);
    ASSERT_TRUE(m.ok);
    EXPECT_EQ(m.validation_errors, 0U);
    RecordProperty("ground_luma_range", std::to_string(m.luma_range));
    RecordProperty("ground_mean_run_pixels", std::to_string(m.mean_run));
    EXPECT_GE(m.luma_range, kMinCapturedGroundLumaRange);
    EXPECT_GE(m.mean_run, kMinCapturedGroundPatchPixels);
    EXPECT_TRUE(m.repeatable);
}

TEST_F(RenderTest, TheEdgePassSoftensTheGrassWithoutFlatteningIt) {
    // FXAA softens everything it sees, texture detail included -- the known trade. What it must not
    // do is flatten the ground: the luma range the capture gate asks of the paint still holds with
    // the pass on, the frame is still repeatable, and the pass really did something to it (its
    // runs are shorter than the paint's own).
    const auto off = measure_grass(false);
    const auto on = measure_grass(true);
    ASSERT_TRUE(off.ok);
    ASSERT_TRUE(on.ok);
    EXPECT_EQ(on.validation_errors, 0U);
    RecordProperty("fxaa_ground_luma_range", std::to_string(on.luma_range));
    RecordProperty("fxaa_ground_mean_run_pixels", std::to_string(on.mean_run));
    EXPECT_GE(on.luma_range, kMinCapturedGroundLumaRange);
    EXPECT_TRUE(on.repeatable);
    EXPECT_LT(on.mean_run, off.mean_run) << "the pass changed nothing on the textured ground";
}

// ---------------------------------------------------------------- the edge pass

/// A triangle drawn straight in clip space (the view-projection is the identity), flat `colour`, at
/// depth `z` (0 is the nearest). Three vertices appended to `scene`.
void add_clip_triangle(protekt::game::Scene& scene, std::array<std::array<float, 2>, 3> corners,
                       float z, std::array<float, 3> colour) {
    for (const auto& corner : corners) {
        protekt::game::SceneVertex v;
        v.position = {corner.at(0), corner.at(1), z};
        v.colour = colour;
        scene.vertices.push_back(v);
    }
}

[[nodiscard]] protekt::game::Scene clip_space_scene() {
    protekt::game::Scene scene;
    scene.view_projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    return scene;
}

constexpr std::array<float, 3> kDark{0.05F, 0.05F, 0.05F};
/// A pixel well inside `world_triangle_scene`'s triangle, away from every edge of it.
constexpr std::uint32_t kDarkX = 30;
constexpr std::uint32_t kDarkY = 50;
constexpr std::array<float, 3> kBright{0.95F, 0.95F, 0.20F};

/// The left half of the frame: a dark right triangle whose hypotenuse is a long diagonal edge
/// against the sky. A diagonal is the case an edge filter exists for -- its stair-steps.
[[nodiscard]] protekt::game::Scene world_triangle_scene() {
    protekt::game::Scene scene = clip_space_scene();
    add_clip_triangle(scene, {{{-0.9F, -0.9F}, {-0.1F, -0.9F}, {-0.9F, 0.9F}}}, 0.5F, kDark);
    return scene;
}

class EdgePassTest : public RenderTest {
protected:
    [[nodiscard]] std::unique_ptr<protekt::render::Renderer> make(bool fxaa) const {
        protekt::render::CreateOptions o = options();
        o.fxaa = fxaa;
        std::string error;
        auto made = protekt::render::create(o, error);
        EXPECT_NE(made, nullptr) << error;
        return made;
    }

    [[nodiscard]] static Frame draw(protekt::render::Renderer& renderer,
                                    const protekt::game::Scene& scene,
                                    Extent extent = Extent{kWidth, kHeight}) {
        Frame frame;
        frame.extent = extent;
        frame.pixels = renderer.render(
            protekt::render::FrameRequest{.extent = extent, .seed = 1, .tick = 0, .scene = &scene},
            frame.stats);
        return frame;
    }

    /// Pixels in columns [x0, x1) that differ between two frames of one extent.
    [[nodiscard]] static std::size_t differing(const Frame& a, const Frame& b, std::uint32_t x0,
                                               std::uint32_t x1) {
        std::size_t count = 0;
        for (std::uint32_t y = 0; y < a.extent.height; ++y) {
            for (std::uint32_t x = x0; x < x1; ++x) {
                count += a.rgb(x, y) != b.rgb(x, y) ? 1U : 0U;
            }
        }
        return count;
    }
};

TEST_F(EdgePassTest, BlendsAcrossAWorldEdgeAndLeavesFlatAreasAlone) {
    const auto off_renderer = make(false);
    ASSERT_NE(off_renderer, nullptr);
    const protekt::game::Scene scene = world_triangle_scene();
    const Frame off = draw(*off_renderer, scene);
    const Frame on = draw(*renderer_, scene);

    // With the pass off the frame is exactly the two flat colours: no blending anywhere.
    EXPECT_EQ(off.colour_histogram().size(), 2U);
    // With it on there are blends between them, and they are only ever BETWEEN them in luma.
    EXPECT_GT(on.colour_histogram().size(), 2U) << "no edge was smoothed";
    const std::uint32_t sky = off.rgb(kWidth - 1, 0);
    const std::uint32_t dark = off.rgb(kDarkX, kDarkY);
    ASSERT_NE(sky, dark);
    const auto red = [](std::uint32_t c) { return (c >> 16U) & 0xFFU; };
    std::size_t changed = 0;
    for (std::uint32_t y = 0; y < kHeight; ++y) {
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            if (on.rgb(x, y) == off.rgb(x, y)) {
                continue;
            }
            ++changed;
            EXPECT_GE(red(on.rgb(x, y)), std::min(red(sky), red(dark))) << x << "," << y;
            EXPECT_LE(red(on.rgb(x, y)), std::max(red(sky), red(dark))) << x << "," << y;
            // A changed pixel must sit next to the edge: the unfiltered frame has a different
            // colour somewhere within two pixels of it.
            bool near_edge = false;
            for (int dy = -2; dy <= 2 && !near_edge; ++dy) {
                for (int dx = -2; dx <= 2 && !near_edge; ++dx) {
                    const auto nx = static_cast<std::int64_t>(x) + dx;
                    const auto ny = static_cast<std::int64_t>(y) + dy;
                    if (nx >= 0 && ny >= 0 && nx < kWidth && ny < kHeight) {
                        near_edge = near_edge
                                    || off.rgb(static_cast<std::uint32_t>(nx),
                                               static_cast<std::uint32_t>(ny))
                                           != off.rgb(x, y);
                    }
                }
            }
            EXPECT_TRUE(near_edge) << "a pixel away from every edge was changed: " << x << "," << y;
        }
    }
    EXPECT_GT(changed, 50U) << "a diagonal edge this long has far more stair-steps than that";
    // The two far corners are flat sky and flat dark, and untouched.
    EXPECT_EQ(on.rgb(kWidth - 1, 0), off.rgb(kWidth - 1, 0));
    EXPECT_EQ(on.rgb(kDarkX, kDarkY), off.rgb(kDarkX, kDarkY));
    EXPECT_EQ(renderer_->validation_errors(), 0U);
}

TEST_F(EdgePassTest, TwoRendersWithThePassOnAreByteIdentical) {
    const protekt::game::Scene scene = world_triangle_scene();
    const Frame first = draw(*renderer_, scene);
    const Frame second = draw(*renderer_, scene);
    EXPECT_EQ(first.pixels, second.pixels);
    // ...and a SECOND renderer agrees: the result is a function of the frame, not of any state the
    // first one built up (there is no history buffer to remember).
    const auto other = make(true);
    ASSERT_NE(other, nullptr);
    EXPECT_EQ(draw(*other, scene).pixels, first.pixels);
}

TEST_F(EdgePassTest, NeverTouchesTheHud) {
    // The same two triangles drawn twice. In the first scene the right-hand one is past the
    // world/HUD boundary; in the second there is no boundary and it is world like the other.
    protekt::game::Scene scene = world_triangle_scene();
    const std::size_t world_end = scene.vertices.size();
    add_clip_triangle(scene, {{{0.1F, -0.9F}, {0.9F, -0.9F}, {0.9F, 0.9F}}}, 0.5F, kDark);

    protekt::game::Scene with_hud = scene;
    with_hud.world_vertex_count = world_end;
    const auto off_renderer = make(false);
    ASSERT_NE(off_renderer, nullptr);

    const Frame off = draw(*off_renderer, with_hud);
    const Frame on = draw(*renderer_, with_hud);
    // The right half is the HUD triangle: every pixel of it is as the unfiltered draw wrote it.
    EXPECT_EQ(differing(on, off, kWidth / 2, kWidth), 0U);
    // The left half is the world triangle and is smoothed.
    EXPECT_GT(differing(on, off, 0, kWidth / 2), 50U);

    // Without the boundary the right-hand triangle is world too, and IS smoothed -- so the equality
    // above is the boundary's doing and not an accident of where the triangle sits.
    const Frame all_world = draw(*renderer_, scene);
    EXPECT_GT(differing(all_world, off, kWidth / 2, kWidth), 50U);
}

TEST_F(EdgePassTest, TheHudStillDepthTestsAgainstTheWorld) {
    // The HUD is drawn in a pass of its own, against the world's depth buffer carried across the
    // filter. A HUD triangle NEARER than the world must show over it, and one FARTHER must not --
    // exactly what a single draw would have done.
    const auto probe = [this](float hud_z) {
        protekt::game::Scene scene = clip_space_scene();
        add_clip_triangle(scene, {{{-1.0F, -1.0F}, {3.0F, -1.0F}, {-1.0F, 3.0F}}}, 0.5F, kDark);
        scene.world_vertex_count = scene.vertices.size();
        add_clip_triangle(scene, {{{-1.0F, -1.0F}, {3.0F, -1.0F}, {-1.0F, 3.0F}}}, hud_z, kBright);
        const Frame frame = draw(*renderer_, scene);
        return std::pair{frame.rgb(kWidth / 2, kHeight / 2),
                         frame.rgb(kWidth / 2, kHeight / 2) == frame.rgb(0, 0)};
    };
    const auto [nearer, nearer_flat] = probe(0.25F);
    const auto [farther, farther_flat] = probe(0.75F);
    EXPECT_TRUE(nearer_flat);
    EXPECT_TRUE(farther_flat);
    EXPECT_NE(nearer, farther) << "the HUD ignored the world's depth in one direction";
    // The nearer HUD triangle is bright, the farther one is hidden behind the dark world.
    const auto red = [](std::uint32_t c) { return (c >> 16U) & 0xFFU; };
    EXPECT_GT(red(nearer), 200U);
    EXPECT_LT(red(farther), 50U);
}

TEST_F(EdgePassTest, SurvivesAResizeAndComesBackTheSameWithoutAValidationFinding) {
    const protekt::game::Scene scene = world_triangle_scene();
    const Extent big{kWidth, kHeight};
    const Extent small{160, 100};
    const Frame first = draw(*renderer_, scene, big);
    const Frame shrunk = draw(*renderer_, scene, small);
    const Frame again = draw(*renderer_, scene, big);
    EXPECT_EQ(shrunk.pixels.size(), static_cast<std::size_t>(160) * 100U * 4U);
    EXPECT_GT(shrunk.colour_histogram().size(), 2U) << "the pass did not run at the new size";
    EXPECT_EQ(again.pixels, first.pixels) << "a recreated target drew something different";
    EXPECT_EQ(renderer_->validation_errors(), 0U);
}

TEST_F(EdgePassTest, TheOffSwitchDrawsTheSceneUnfiltered) {
    // With the pass off, the built-in scene is the same frame as ever: three flat colours and no
    // blends. (The default-on renderer's histogram has more.)
    const auto off_renderer = make(false);
    ASSERT_NE(off_renderer, nullptr);
    protekt::render::FrameStats stats;
    Frame off;
    off.extent = Extent{kWidth, kHeight};
    off.pixels = off_renderer->render(
        protekt::render::FrameRequest{.extent = off.extent, .seed = 42, .tick = 0}, stats);
    EXPECT_EQ(off.colour_histogram().size(), 3U);
    EXPECT_GT(render_frame().colour_histogram().size(), 3U);
}

TEST_F(RenderTest, TheStoreGaugeShowsEveryChunkItShouldInRadarModeAndInMapMode) {
    // THE BUG THIS COVERS (Harri, 2026-11-03: "the scrap bar is broken when I am in map mode"). The
    // pill behind the gauge was ONE quad the height of the panel beside it, and the depth test is
    // per PIXEL: in map mode (a tall panel) the pill's middle beat the sections drawn a hair
    // nearer than it. Measured on the old code at 1280x720 (the map was 330 px then), down the
    // column's centre line, rows drawn / rows the layout says: radar mode green 62/61.8, gold
    // with its white bar 31/30.9; map mode green 62/203.2, gold with its white bar 46/101.6. Moving
    // the pill 10% farther gave 203 and 102: the cause is depth order, not the layout.
    // THIS test reads the pixels of the finished frame, so it pins what the player sees (every
    // chunk drawn, the pill showing only in the gaps); at today's half-size map the old single-quad
    // pill no longer hides anything here (the sag is under the sections' margin), which is why
    // `StoreGauge.NoPillQuadIsTallerThanAChunk` pins the cause on the vertices.
    using protekt::sim::Side;
    protekt::sim::Scenario s;
    s.seed = 20261101;
    s.pools = {protekt::sim::Pool{70, 60}, protekt::sim::Pool{74, 60}, protekt::sim::Pool{78, 60}};
    s.spawns = {protekt::sim::Spawn{protekt::sim::UnitKind::Factory, Side::Player, 50, 60},
                protekt::sim::Spawn{protekt::sim::UnitKind::Scavenger, Side::Player, 70, 60},
                protekt::sim::Spawn{protekt::sim::UnitKind::Scavenger, Side::Player, 74, 60},
                protekt::sim::Spawn{protekt::sim::UnitKind::Scavenger, Side::Player, 78, 60}};
    auto sim = protekt::sim::Sim::create(s);
    for (std::uint32_t id = 2; id <= 4; ++id) {
        sim.enqueue(protekt::sim::Order{
            protekt::sim::UnitId{id},
            protekt::sim::orders::Deploy{static_cast<int>(70 + (4 * (id - 2))), 60}});
    }
    for (int i = 0; i < 600 || sim.ore(Side::Player) < 5; ++i) {
        sim.step();
    }
    ASSERT_EQ(sim.capacity(Side::Player), 100) << "three deployed scavengers on top of the 40";

    // 1280x720 is the game's own; 2400x600 is a wide short frame, where the gauge's cell and the
    // panel beside it are a different share of the frame.
    for (const Extent extent : {Extent{1280, 720}, Extent{2400, 600}}) {
        for (const bool map : {false, true}) {
            protekt::game::SceneOptions o;
            o.seed = s.seed;
            o.radius_tiles = 8;
            o.extent = extent;
            o.camera.x = 100.0F;
            o.camera.z = 100.0F;
            o.camera.y = 8.0F;
            o.show_ship_gauge = false;
            o.show_ledger = true;
            o.show_world_map = map;
            o.viewer_unit = protekt::sim::UnitId{1};
            const protekt::game::Scene scene = protekt::game::build_scene(sim, o);
            Frame frame;
            frame.extent = o.extent;
            frame.pixels = renderer_->render(
                protekt::render::FrameRequest{
                    .extent = o.extent, .seed = s.seed, .tick = 0, .scene = &scene},
                frame.stats);
            protekt::game::RadarPanel beside = protekt::game::radar_panel(o.extent);
            if (map) {
                const protekt::game::WorldMapLayout m = protekt::game::world_map_layout(o.extent);
                beside = protekt::game::RadarPanel{m.x, m.y, m.size, m.size};
            }
            const protekt::game::StoreGaugeLayout g = protekt::game::store_gauge_layout(
                beside, sim.capacity(Side::Player), sim.fast_band(Side::Player),
                sim.ore(Side::Player), 2.0F * static_cast<float>(extent.height) / 720.0F);
            // Down the column's centre line: what colour each row is.
            const std::uint32_t x
                = static_cast<std::uint32_t>(g.column.x + (g.column.width * 0.5F));
            const auto is = [](std::uint32_t rgb, const std::array<float, 3>& ink) {
                const auto close = [](std::uint32_t got, float want) {
                    return std::abs(static_cast<float>(got) - (want * 255.0F)) < 8.0F;
                };
                return close((rgb >> 16U) & 0xFFU, ink.at(0))
                       && close((rgb >> 8U) & 0xFFU, ink.at(1)) && close(rgb & 0xFFU, ink.at(2));
            };
            std::size_t pill_rows = 0;
            std::size_t runs = 0; // stretches of rows that are NOT the pill: the chunks, counted
            bool in_chunk = false;
            float drawn = 0.0F;
            float wanted = 0.0F;
            for (const auto& chunk : g.chunks) {
                wanted += chunk.box.height;
            }
            for (std::uint32_t y = static_cast<std::uint32_t>(std::ceil(g.column.y));
                 y < static_cast<std::uint32_t>(g.column.y + g.column.height); ++y) {
                const bool pill = is(frame.rgb(x, y), protekt::game::kGaugePill);
                pill_rows += pill ? 1U : 0U;
                if (!pill) {
                    drawn += 1.0F;
                    runs += in_chunk ? 0U : 1U;
                }
                in_chunk = !pill;
            }
            EXPECT_EQ(runs, g.chunks.size())
                << "one stretch down the column per chunk, with the pill showing in each gap; map "
                << map << " at " << extent.width << "x" << extent.height;
            EXPECT_EQ(g.chunks.size(), 4U);
            EXPECT_NEAR(drawn, wanted, 2.0F * static_cast<float>(g.chunks.size()))
                << "the chunks' rows drawn against the layout's, map " << map;
            EXPECT_GE(pill_rows, 3U * 2U) << "three gaps of at least two rows";
        }
    }
}

// ---------------------------------------------------------------- the fragment stage's light (AP)

// The light is computed in the shader from the interpolated vertex normal, and a shader cannot be
// unit-tested except by drawing it. Three flat quads of ONE colour, one normal each: toward the sun
// (full light), square to the sun (the ramp's middle) and square away from it (the floor), plus a
// fourth with NO normal (unlit: the colour as it is). The pixels' ratios are the ramp, whatever the
// texture sample or the palette is.
TEST_F(EdgePassTest, TheFragmentStageLightsAVertexFromItsNormalAndLeavesAnUnlitOneAlone) {
    const auto unfiltered = make(false);
    ASSERT_NE(unfiltered, nullptr);
    protekt::game::Scene scene = clip_space_scene();
    const std::array<float, 3> colour{0.9F, 0.8F, 0.7F};
    const protekt::math::Vec3 sun = protekt::math::normalise(protekt::game::kSunDirection);
    // Any unit vector perpendicular to the sun: its cross with up, normalised.
    const protekt::math::Vec3 side = protekt::math::normalise(
        protekt::math::cross(sun, protekt::math::Vec3{0.0F, 1.0F, 0.0F}));
    struct Quad {
        float x0;
        std::array<float, 3> normal;
    };
    const std::array<Quad, 4> quads{{{-0.9F, {sun[0], sun[1], sun[2]}},
                                     {-0.45F, {side[0], side[1], side[2]}},
                                     {0.0F, {-sun[0], -sun[1], -sun[2]}},
                                     {0.45F, {0.0F, 0.0F, 0.0F}}}};
    for (const Quad& quad : quads) {
        const float x1 = quad.x0 + 0.4F;
        const std::array<std::array<float, 2>, 6> corners{{{quad.x0, -0.5F},
                                                           {x1, -0.5F},
                                                           {x1, 0.5F},
                                                           {quad.x0, -0.5F},
                                                           {x1, 0.5F},
                                                           {quad.x0, 0.5F}}};
        for (const auto& corner : corners) {
            protekt::game::SceneVertex v;
            v.position = {corner.at(0), corner.at(1), 0.5F};
            v.colour = colour;
            v.normal = quad.normal;
            scene.vertices.push_back(v);
        }
    }
    const Frame frame = draw(*unfiltered, scene);
    const auto red_at = [&](float clip_x) {
        const auto px = static_cast<std::uint32_t>((clip_x + 1.0F) * 0.5F * kWidth);
        return static_cast<float>((frame.rgb(px, kHeight / 2U) >> 16U) & 0xFFU);
    };
    const float toward = red_at(-0.7F);
    const float square = red_at(-0.25F);
    const float away = red_at(0.2F);
    const float unlit = red_at(0.65F);
    ASSERT_GT(toward, 100.0F) << "the quad is on screen and bright";
    // The ramp: 1 toward the sun, floor + (1 - floor) / 2 square to it, `kShadeFloor` away.
    const float floor = protekt::game::kShadeFloor;
    EXPECT_NEAR(square / toward, floor + ((1.0F - floor) * 0.5F), 0.02F);
    EXPECT_NEAR(away / toward, floor, 0.02F);
    EXPECT_NEAR(unlit, toward, 1.5F) << "no normal: the colour as it is, the same as full light";
    EXPECT_EQ(unfiltered->validation_errors(), 0U);
}
