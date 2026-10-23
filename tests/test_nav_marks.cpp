// N1: navigational marks. N drops a numbered mark on the ground under the crosshair, N on a mark
// clears it, the radar lists the marks, and none of it touches the sim.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "protekt/game/aim.hpp"
#include "protekt/game/interaction.hpp"
#include "protekt/game/nav_marks.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/radar.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/input/bindings.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::game {
namespace {

using Outcome = NavMarks::Outcome;
using sim::Side;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr Extent kExtent{1280, 720};

struct Rig {
    sim::Sim sim;
    SceneOptions options;
    sim::UnitId tank_a{};
    sim::UnitId tank_b{};
};

/// The interaction tests' layout: the camera at (80, ground + 3, 60) looking down +Z, tanks at
/// tiles (40,40) and (44,41).
[[nodiscard]] Rig make_rig() {
    Rig rig{};
    rig.sim = sim::Sim::create(sim::Scenario{kSeed,
                                             {{UnitKind::Tank, Side::Player, 40, 40},
                                              {UnitKind::Tank, Side::Player, 44, 41},
                                              {UnitKind::Tank, Side::Enemy, 38, 70}}});
    for (const sim::Unit& unit : rig.sim.units()) {
        if (unit.side == Side::Player && unit.x.floor_int() == 40) {
            rig.tank_a = unit.id;
        } else if (unit.side == Side::Player) {
            rig.tank_b = unit.id;
        }
    }
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    rig.options.camera = Camera{80.0F, height_metres(kSeed, 40, 40) + 3.0F, 60.0F, 0.0F, -0.12F};
    rig.options.viewer_unit = rig.tank_a;
    rig.options.aim = Aim{640.0F, 360.0F};
    return rig;
}

void aim_at_pixel(Rig& rig, float x, float y) { rig.options.aim = Aim{x, y}; }

/// The ground tile the ray under pixel (x, y) meets, from the picker alone.
[[nodiscard]] Pick ray_tile(const Rig& rig, float x, float y) {
    return pick(rig.sim, rig.options, x, y);
}

void to_the_sky(Rig& rig) {
    rig.options.camera.y += 500.0F; // well above every hill, looking up: no ground in 200 tiles
    rig.options.camera.pitch = 0.6F;
}

/// A frame's scene with the nav marks as given.
[[nodiscard]] Scene scene_with(const Rig& rig, const NavMarks* marks) {
    SceneOptions options = rig.options;
    options.nav_marks = marks;
    return build_scene(rig.sim, options);
}

[[nodiscard]] bool is_beacon(const SceneVertex& v) {
    return v.colour[0] == kNavColour[0] && v.colour[1] == kNavColour[1]
           && v.colour[2] == kNavColour[2];
}

[[nodiscard]] std::size_t beacon_vertices(const Scene& scene) {
    return static_cast<std::size_t>(
        std::count_if(scene.vertices.begin(), scene.vertices.end(),
                      [](const SceneVertex& v) { return is_beacon(v); }));
}

TEST(NavMarks, NDropsAMarkOnTheTileTheCrosshairRayMeets) {
    Rig rig = make_rig();
    const Pick ray = ray_tile(rig, 640.0F, 560.0F);
    ASSERT_TRUE(ray.has_tile()) << "the test needs a ray that reaches the ground";
    aim_at_pixel(rig, 640.0F, 560.0F);
    NavMarks marks;
    const std::string said = nav_press(marks, rig.sim, rig.options);
    const std::vector<NavMarks::Mark> got = marks.marks();
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(got.at(0).tile_x, ray.tile_x);
    EXPECT_EQ(got.at(0).tile_y, ray.tile_y);
    EXPECT_EQ(got.at(0).number, 0);
    EXPECT_EQ(said, "Nav mark 0 dropped.");
}

TEST(NavMarks, NumbersAreTheLowestFreeSlotAndAClearedNumberIsReused) {
    NavMarks marks;
    EXPECT_EQ(marks.press(10, 10).number, 0);
    EXPECT_EQ(marks.press(11, 10).number, 1);
    EXPECT_EQ(marks.press(12, 10).number, 2);
    const NavMarks::Result cleared = marks.press(10, 10);
    EXPECT_EQ(cleared.outcome, Outcome::Cleared);
    EXPECT_EQ(cleared.number, 0);
    const NavMarks::Result again = marks.press(20, 20);
    EXPECT_EQ(again.outcome, Outcome::Dropped);
    EXPECT_EQ(again.number, 0) << "the freed 0 is the lowest free number";
    EXPECT_EQ(marks.press(21, 20).number, 3);
}

TEST(NavMarks, TheEleventhDropIsRefusedWithAMessageAndTouchesNothing) {
    Rig rig = make_rig();
    NavMarks marks;
    for (int i = 0; i < kNavSlots; ++i) {
        ASSERT_EQ(marks.press(10 + i, 10).outcome, Outcome::Dropped);
    }
    const std::vector<NavMarks::Mark> before = marks.marks();
    ASSERT_EQ(before.size(), 10U);
    aim_at_pixel(rig, 640.0F, 560.0F);
    ASSERT_TRUE(ray_tile(rig, 640.0F, 560.0F).has_tile());
    const std::string said = nav_press(marks, rig.sim, rig.options);
    EXPECT_EQ(said, "All ten nav marks are in use -- aim at one and press N to clear it first.");
    const std::vector<NavMarks::Mark> after = marks.marks();
    ASSERT_EQ(after.size(), before.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(after.at(i).number, before.at(i).number);
        EXPECT_EQ(after.at(i).tile_x, before.at(i).tile_x);
        EXPECT_EQ(after.at(i).tile_y, before.at(i).tile_y);
    }
}

TEST(NavMarks, NAimedAtAMarkClearsItAndFreesTheSlot) {
    Rig rig = make_rig();
    aim_at_pixel(rig, 640.0F, 560.0F);
    NavMarks marks;
    EXPECT_EQ(nav_press(marks, rig.sim, rig.options), "Nav mark 0 dropped.");
    ASSERT_EQ(marks.marks().size(), 1U);
    EXPECT_EQ(nav_press(marks, rig.sim, rig.options), "Nav mark 0 cleared.");
    EXPECT_TRUE(marks.marks().empty());
    EXPECT_EQ(nav_press(marks, rig.sim, rig.options), "Nav mark 0 dropped.")
        << "the slot is free again";
}

TEST(NavMarks, NWithNoGroundInRangeIsRefusedAndCreatesNothing) {
    Rig rig = make_rig();
    to_the_sky(rig);
    ASSERT_FALSE(ray_tile(rig, 640.0F, 360.0F).has_tile());
    NavMarks marks;
    const std::string said = nav_press(marks, rig.sim, rig.options);
    EXPECT_EQ(said, "No ground under the crosshair in range -- no nav mark dropped.");
    EXPECT_TRUE(marks.marks().empty());
}

TEST(NavMarks, TheKeyIsAPlainNPress) {
    input::InputFrame frame{};
    EXPECT_FALSE(input::nav_mark_pressed(frame));
    frame.keys.push_back(input::KeyEvent{input::kKeyN, false});
    EXPECT_FALSE(input::nav_mark_pressed(frame)) << "a release is not a press";
    frame.keys.push_back(input::KeyEvent{input::kKeyN, true});
    EXPECT_TRUE(input::nav_mark_pressed(frame));
}

TEST(NavMarksRadar, ListsExactlyTheDroppedMarksWithTheirNumbers) {
    Rig rig = make_rig();
    NavMarks marks;
    marks.press(44, 40);
    marks.press(36, 40);
    marks.press(40, 44);
    marks.press(36, 40); // clear number 1
    const RadarPanel panel = radar_panel(kExtent);
    const std::vector<RadarNav> shown
        = radar_nav_marks(rig.sim.units(), rig.tank_a, marks.marks(), 0.0F, panel);
    ASSERT_EQ(shown.size(), 2U);
    EXPECT_EQ(shown.at(0).number, 0);
    EXPECT_EQ(shown.at(1).number, 2);
    EXPECT_FALSE(shown.at(0).at_rim);
    // Heading-up at yaw 0: east of the tank is to the right of the centre, north of it is above.
    const float middle = panel.size * 0.5F;
    const float middle_y = panel.height * 0.5F;
    EXPECT_GT(shown.at(0).px, middle);
    EXPECT_NEAR(shown.at(0).py, middle_y, 3.0F) << "the same row, within a tile";
    EXPECT_LT(shown.at(1).py, middle_y);
}

TEST(NavMarksRadar, AMarkBeyondTheRadiusIsPinnedToTheRimInItsDirection) {
    Rig rig = make_rig();
    NavMarks marks;
    marks.press(40 + (3 * kRadarRadiusTiles), 40);
    const RadarPanel panel = radar_panel(kExtent);
    const std::vector<RadarNav> shown
        = radar_nav_marks(rig.sim.units(), rig.tank_a, marks.marks(), 0.0F, panel);
    ASSERT_EQ(shown.size(), 1U);
    EXPECT_TRUE(shown.at(0).at_rim);
    const float middle = panel.size * 0.5F;
    EXPECT_GT(shown.at(0).px, middle * 1.5F);
    EXPECT_NEAR(shown.at(0).py, panel.height * 0.5F, 0.5F);
    EXPECT_LE(shown.at(0).px + shown.at(0).half, panel.size);
}

TEST(NavMarksRadar, EveryMarkPlateLiesInsideTheOvalAtAnyBearing) {
    Rig rig = make_rig();
    const RadarPanel panel = radar_panel(kExtent);
    for (const std::array<int, 2>& tile : {std::array<int, 2>{40, 140},
                                           {140, 40},
                                           {140, 140},
                                           {1, 140},
                                           {1, 1},
                                           {79, 40},
                                           {40, 79},
                                           {40, 1},
                                           {1, 40}}) {
        NavMarks marks;
        marks.press(tile.at(0), tile.at(1));
        const std::vector<RadarNav> shown
            = radar_nav_marks(rig.sim.units(), rig.tank_a, marks.marks(), 0.4F, panel);
        ASSERT_EQ(shown.size(), 1U);
        const RadarNav& nav = shown.at(0);
        for (const float dx : {-nav.half, nav.half}) {
            for (const float dy : {-nav.half, nav.half}) {
                const float ex = (nav.px + dx - (panel.size * 0.5F)) / (panel.size * 0.5F);
                const float ey = (nav.py + dy - (panel.height * 0.5F)) / (panel.height * 0.5F);
                EXPECT_LE((ex * ex) + (ey * ey), 1.0F) << tile.at(0) << "," << tile.at(1);
            }
        }
    }
}

TEST(NavMarksRadar, NoViewerMeansNoMarks) {
    Rig rig = make_rig();
    NavMarks marks;
    marks.press(44, 40);
    EXPECT_TRUE(
        radar_nav_marks(rig.sim.units(), sim::UnitId{}, marks.marks(), 0.0F, radar_panel(kExtent))
            .empty());
}

TEST(NavMarksWorld, AMarkDrawsABeaconAndNoMarkDrawsNone) {
    Rig rig = make_rig();
    NavMarks marks;
    EXPECT_EQ(beacon_vertices(scene_with(rig, &marks)), 0U);
    EXPECT_EQ(beacon_vertices(scene_with(rig, nullptr)), 0U);
    // The radar plate and digit for the mark are drawn too, so count the world's share alone by
    // looking for vertices above the ground at the tile.
    marks.press(40, 50);
    const Scene with = scene_with(rig, &marks);
    EXPECT_GT(beacon_vertices(with), 0U);
    const float cx = 40.5F * kMetresPerTile;
    const float cz = 50.5F * kMetresPerTile;
    bool standing = false;
    for (const SceneVertex& v : with.vertices) {
        const bool beacon = is_beacon(v);
        if (beacon && std::abs(v.position[0] - cx) < 1.0F && std::abs(v.position[2] - cz) < 1.0F
            && v.position[1] > height_metres(kSeed, 40, 50) + 3.0F) {
            standing = true;
        }
    }
    EXPECT_TRUE(standing) << "a beacon stands at the tile, well above the ground";
}

TEST(NavMarksSurvive, ATickACameraMoveAndAnXSwap) {
    Rig rig = make_rig();
    NavMarks marks;
    marks.press(44, 40);
    marks.press(30, 30);
    const std::vector<NavMarks::Mark> before = marks.marks();
    const auto same = [&] {
        const std::vector<NavMarks::Mark> now = marks.marks();
        ASSERT_EQ(now.size(), before.size());
        for (std::size_t i = 0; i < now.size(); ++i) {
            EXPECT_EQ(now.at(i).number, before.at(i).number);
            EXPECT_EQ(now.at(i).tile_x, before.at(i).tile_x);
            EXPECT_EQ(now.at(i).tile_y, before.at(i).tile_y);
        }
    };
    for (int i = 0; i < 30; ++i) {
        rig.sim.step();
    }
    same();
    rig.options.camera.x += 40.0F;
    rig.options.camera.yaw += 1.0F;
    same();
    // An X swap into the other own tank: the crosshair on it, X pressed.
    Interaction player(rig.tank_a);
    const sim::Unit* other = rig.sim.find(rig.tank_b);
    rig.options.camera.x -= 40.0F;
    rig.options.camera.yaw -= 1.0F;
    const Projected q
        = project(rig.options.camera, kExtent,
                  static_cast<float>(other->x.raw) / sim::Fixed::kOne * kMetresPerTile,
                  height_metres(kSeed, other->x.floor_int(), other->y.floor_int()) + 0.9F,
                  static_cast<float>(other->y.raw) / sim::Fixed::kOne * kMetresPerTile);
    rig.options.aim = Aim{q.x, q.y};
    input::InputFrame x{};
    x.keys.push_back(input::KeyEvent{input::kKeyX, true});
    player.handle(x, rig.sim, rig.options);
    ASSERT_EQ(player.driven(), rig.tank_b) << "the swap really happened";
    same();
}

[[nodiscard]] std::vector<std::uint8_t> run_once() {
    Rig rig = make_rig();
    NavMarks marks;
    for (const float y : {500.0F, 560.0F, 640.0F}) {
        aim_at_pixel(rig, 640.0F, y);
        static_cast<void>(nav_press(marks, rig.sim, rig.options));
        rig.sim.step();
    }
    const Scene scene = scene_with(rig, &marks);
    std::vector<std::uint8_t> bytes(scene.vertices.size() * sizeof(SceneVertex));
    std::memcpy(bytes.data(), scene.vertices.data(), bytes.size());
    for (const NavMarks::Mark& m : marks.marks()) {
        bytes.push_back(static_cast<std::uint8_t>(m.number));
        bytes.push_back(static_cast<std::uint8_t>(m.tile_x));
        bytes.push_back(static_cast<std::uint8_t>(m.tile_y));
    }
    return bytes;
}

TEST(NavMarksDeterminism, TwoSameSeedRunsAreByteIdentical) {
    const std::vector<std::uint8_t> a = run_once();
    const std::vector<std::uint8_t> b = run_once();
    ASSERT_FALSE(a.empty());
    EXPECT_EQ(a, b);
}

TEST(NavMarksSim, DroppingMarksDoesNotChangeTheSimHash) {
    Rig with = make_rig();
    Rig without = make_rig();
    NavMarks marks;
    aim_at_pixel(with, 640.0F, 560.0F);
    static_cast<void>(nav_press(marks, with.sim, with.options));
    ASSERT_EQ(marks.marks().size(), 1U);
    for (int i = 0; i < 20; ++i) {
        with.sim.step();
        without.sim.step();
    }
    EXPECT_EQ(with.sim.state_hash(), without.sim.state_hash());
}

} // namespace
} // namespace protekt::game
