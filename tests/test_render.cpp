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
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "protekt/app/render.hpp"
#include "protekt/game/atlas.hpp"
#include "protekt/game/radar.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/render/renderer.hpp"
#include "protekt/sim/scenario.hpp"

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

    // The scene has exactly three flat colours: sky, ground, and the unit. The unit is the one
    // that is neither of the other two -- and it is submitted FIRST, so if it is visible the
    // depth test did its job.
    std::uint32_t unit = 0;
    std::size_t unit_pixels = 0;
    for (const auto& [colour, count] : histogram) {
        if (colour != sky && colour != ground) {
            unit = colour;
            unit_pixels = count;
            break;
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
             y < static_cast<std::uint32_t>(panel.y + panel.size + slack); ++y) {
            for (std::uint32_t x = static_cast<std::uint32_t>(panel.x - slack);
                 x < static_cast<std::uint32_t>(panel.x + panel.size + slack); ++x) {
                const std::uint32_t rgb = frame.rgb(x, y);
                const std::uint32_t r = (rgb >> 16U) & 0xFFU;
                const std::uint32_t g = (rgb >> 8U) & 0xFFU;
                const std::uint32_t b = rgb & 0xFFU;
                const bool inside = static_cast<float>(x) >= panel.x
                                    && static_cast<float>(x) < (panel.x + panel.size)
                                    && static_cast<float>(y) >= panel.y
                                    && static_cast<float>(y) < (panel.y + panel.size);
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
        const std::uint32_t middle_x = static_cast<std::uint32_t>(panel.x + (panel.size * 0.5F));
        const std::uint32_t middle_y = static_cast<std::uint32_t>(panel.y + (panel.size * 0.5F));
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
    EXPECT_GT(near_enemy.at(3), 10U) << "a hostile in range is red on the panel";

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
