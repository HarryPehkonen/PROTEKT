// N1, corrected by MENU-NAV: navigational marks. Ten slots filled round-robin (drawn 1-9, then 0),
// an overwrite destroys the occupant, nothing ever deletes a mark, and every slot carries a
// highlight toggle. `n` arms placing, SPACE places, `~` opens the nav menu, and SPACE over a mark
// and the mark's digit are the same toggle. None of it touches the sim.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "haze.hpp"
#include "protekt/game/aim.hpp"
#include "protekt/game/interaction.hpp"
#include "protekt/game/menu.hpp"
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

/// A press of `scancode`, bare.
[[nodiscard]] input::KeyEvent press(std::int32_t scancode) {
    return input::KeyEvent{scancode, true};
}

/// The digit key `n` (0-9) as the window reports it: HID 30 is "1" ... 38 is "9", 39 is "0".
[[nodiscard]] input::KeyEvent digit(int n) {
    return press(n == 0 ? input::kKey0 : input::kKey1 + n - 1);
}

/// A reticle on the ground at `tile`, `metres` away.
[[nodiscard]] NavAim on(int tile_x, int tile_y, int metres = 17) {
    return NavAim{tile_x, tile_y, metres};
}

/// A frame's scene with the nav marks as given.
[[nodiscard]] Scene scene_with(const Rig& rig, const NavMarks* marks, bool map = false) {
    SceneOptions options = rig.options;
    options.nav_marks = marks;
    options.show_world_map = map;
    return build_scene(rig.sim, options);
}

[[nodiscard]] bool has_colour(const SceneVertex& v, const math::Vec3& rgb) {
    return v.colour[0] == rgb[0] && v.colour[1] == rgb[1] && v.colour[2] == rgb[2];
}

[[nodiscard]] std::size_t vertices_in(const Scene& scene, const math::Vec3& rgb) {
    return static_cast<std::size_t>(
        std::count_if(scene.vertices.begin(), scene.vertices.end(),
                      [&rgb](const SceneVertex& v) { return has_colour(v, rgb); }));
}

/// The tiles of placements 1..`count`, each its own: placement p is at (100 + p, 7).
[[nodiscard]] NavMarks placed(int count) {
    NavMarks marks;
    for (int p = 1; p <= count; ++p) {
        EXPECT_EQ(marks.place(100 + p, 7).outcome, Outcome::Placed);
    }
    return marks;
}

// --- the model: slots, the wrap, the highlight ---------------------------------------------------

TEST(NavMarks, TheSlotsAreDrawnOneToNineThenZero) {
    for (int slot = 0; slot < kNavSlots; ++slot) {
        EXPECT_EQ(nav_slot_digit(slot), slot == 9 ? 0 : slot + 1);
        EXPECT_EQ(nav_digit_slot(nav_slot_digit(slot)), slot);
    }
}

TEST(NavMarks, PlacementPLandsInSlotPMinusOneModTen) {
    NavMarks marks;
    for (int p = 1; p <= 25; ++p) {
        const NavMarks::Result result = marks.place(100 + p, 7);
        ASSERT_EQ(result.outcome, Outcome::Placed);
        EXPECT_EQ(result.slot, (p - 1) % kNavSlots) << "placement " << p;
    }
}

TEST(NavMarks, TheEleventhLandsInSlotOneAndTheFirstIsGone) {
    NavMarks marks = placed(10);
    ASSERT_EQ(marks.marks().size(), 10U);
    const NavMarks::Result eleventh = marks.place(111, 7);
    EXPECT_EQ(eleventh.outcome, Outcome::Placed) << "a full table is not a refusal";
    EXPECT_EQ(nav_slot_digit(eleventh.slot), 1);
    const std::vector<NavMarks::Mark> now = marks.marks();
    ASSERT_EQ(now.size(), 10U);
    EXPECT_EQ(now.at(0).tile_x, 111);
    for (const NavMarks::Mark& mark : now) {
        EXPECT_NE(mark.tile_x, 101) << "the first placement is gone from every slot";
    }
}

TEST(NavMarks, TwelvePlacementsAreHisWorkedExample) {
    // "1 = 11th, 2 = 12th, 3 = 3rd, ... 0 = 10th. The first and second nav beacon have disappeared
    // completely from any record."
    const NavMarks marks = placed(12);
    const std::vector<NavMarks::Mark> now = marks.marks();
    ASSERT_EQ(now.size(), 10U);
    const std::array<int, kNavSlots> placement{11, 12, 3, 4, 5, 6, 7, 8, 9, 10};
    for (int slot = 0; slot < kNavSlots; ++slot) {
        const NavMarks::Mark& mark = now.at(static_cast<std::size_t>(slot));
        EXPECT_EQ(mark.slot, slot) << "listed in slot order";
        EXPECT_EQ(mark.tile_x, 100 + placement.at(static_cast<std::size_t>(slot)))
            << "row " << nav_slot_digit(slot);
    }
}

TEST(NavMarks, AnOverwriteDestroysTheOccupantAndItsHighlight) {
    NavMarks marks = placed(10);
    ASSERT_TRUE(marks.toggle(0));
    ASSERT_TRUE(marks.marks().at(0).highlighted);
    static_cast<void>(marks.place(55, 66));
    const NavMarks::Mark fresh = marks.marks().at(0);
    EXPECT_EQ(fresh.tile_x, 55);
    EXPECT_EQ(fresh.tile_y, 66);
    EXPECT_FALSE(fresh.highlighted) << "a freshly placed mark is always plain";
    EXPECT_EQ(marks.slot_at(101, 7), -1) << "the old occupant is nowhere";
}

TEST(NavMarks, AFreshMarkIsNeverHighlighted) {
    NavMarks marks;
    for (int p = 1; p <= 23; ++p) {
        const NavMarks::Result result = marks.place(p, p);
        for (const NavMarks::Mark& mark : marks.marks()) {
            if (mark.slot == result.slot) {
                EXPECT_FALSE(mark.highlighted) << "placement " << p;
            }
        }
        marks.toggle(result.slot); // so the slot the wrap comes back to is highlighted
    }
}

TEST(NavMarks, AToggleFlipsOnlyItsOwnSlotAndSeveralCoexist) {
    NavMarks marks = placed(5);
    ASSERT_TRUE(marks.toggle(1));
    ASSERT_TRUE(marks.toggle(3));
    for (const NavMarks::Mark& mark : marks.marks()) {
        EXPECT_EQ(mark.highlighted, mark.slot == 1 || mark.slot == 3) << "slot " << mark.slot;
    }
    ASSERT_TRUE(marks.toggle(1));
    for (const NavMarks::Mark& mark : marks.marks()) {
        EXPECT_EQ(mark.highlighted, mark.slot == 3) << "slot " << mark.slot;
    }
}

TEST(NavMarks, ToggleOnAnEmptySlotChangesNothing) {
    NavMarks marks = placed(2);
    const std::vector<NavMarks::Mark> before = marks.marks();
    EXPECT_FALSE(marks.toggle(2));
    EXPECT_FALSE(marks.toggle(-1));
    EXPECT_FALSE(marks.toggle(kNavSlots));
    const std::vector<NavMarks::Mark> after = marks.marks();
    ASSERT_EQ(after.size(), before.size());
    for (std::size_t i = 0; i < after.size(); ++i) {
        EXPECT_EQ(after.at(i).highlighted, before.at(i).highlighted);
    }
}

TEST(NavMarks, NoGroundNeverPlacesAndDoesNotTurnTheCycle) {
    NavMarks marks;
    EXPECT_EQ(marks.place(-1, -1).outcome, Outcome::NoGround);
    EXPECT_EQ(marks.place(5, -1).outcome, Outcome::NoGround);
    EXPECT_TRUE(marks.marks().empty());
    EXPECT_EQ(marks.place(5, 5).slot, 0) << "the refusal spent no slot";
}

TEST(NavMarks, AimingAtAMarkNeverRemovesIt) {
    // R2: the shipped "N on a mark clears it" is gone. The same tile placed twice is two marks.
    NavMarks marks;
    static_cast<void>(marks.place(10, 10));
    static_cast<void>(marks.place(10, 10));
    const std::vector<NavMarks::Mark> now = marks.marks();
    ASSERT_EQ(now.size(), 2U);
    EXPECT_EQ(now.at(0).tile_x, 10);
    EXPECT_EQ(now.at(1).tile_x, 10);
    // And through the keys: SPACE while placing, aimed at a mark, is a placement too.
    NavMenu menu;
    const std::array keys{press(input::kKeyN), press(input::kKeySpace)};
    static_cast<void>(menu.handle(keys, marks, on(10, 10), true));
    EXPECT_EQ(marks.marks().size(), 3U);
}

// --- the keys: `top menu -> n`, `top menu -> ~` --------------------------------------------------

TEST(NavMenu, NArmsPlacingOnlyFromTheTopMenu) {
    NavMarks marks;
    NavMenu menu;
    const std::array n{press(input::kKeyN)};
    EXPECT_TRUE(menu.handle(n, marks, on(5, 5), false).empty()) << "n is the nav's key";
    EXPECT_EQ(menu.state(), NavState::Closed) << "another left menu is open";
    static_cast<void>(menu.handle(n, marks, on(5, 5), true));
    EXPECT_EQ(menu.state(), NavState::Placing);
    EXPECT_TRUE(marks.marks().empty()) << "n only arms; nothing is placed yet";
}

TEST(NavMenu, PlacingIsOneRowGetNavWithTheLiveDistance) {
    NavMarks marks;
    NavMenu menu;
    static_cast<void>(menu.handle(std::array{press(input::kKeyN)}, marks, on(5, 5), true));
    const std::vector<MenuRow> rows = nav_menu_rows(menu.state(), marks, on(5, 5, 17), Camera{});
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows.at(0).key, "SPACE");
    EXPECT_EQ(rows.at(0).verb, "GET NAV");
    EXPECT_EQ(rows.at(0).right, "17m");
    EXPECT_EQ(nav_menu_rows(menu.state(), marks, on(5, 5, 42), Camera{}).at(0).right, "42m")
        << "live";
}

TEST(NavMenu, SpacePlacesUnderTheReticleAndCloses) {
    NavMarks marks;
    NavMenu menu;
    const std::array keys{press(input::kKeyN), press(input::kKeySpace)};
    EXPECT_TRUE(menu.handle(keys, marks, on(12, 34), true).empty());
    ASSERT_EQ(marks.marks().size(), 1U);
    EXPECT_EQ(marks.marks().at(0).tile_x, 12);
    EXPECT_EQ(marks.marks().at(0).tile_y, 34);
    EXPECT_EQ(menu.state(), NavState::Closed);
    EXPECT_EQ(menu.message(), "Nav 1 placed.");
}

TEST(NavMenu, OffTheGroundPlacingIsDrawnAsNothingAndSurvives) {
    NavMarks marks;
    NavMenu menu;
    static_cast<void>(menu.handle(std::array{press(input::kKeyN)}, marks, on(5, 5), true));
    const NavAim sky{};
    EXPECT_TRUE(nav_menu_rows(menu.state(), marks, sky, Camera{}).empty())
        << "no menu, no distance, no trace";
    static_cast<void>(menu.handle(std::array{press(input::kKeySpace)}, marks, sky, true));
    EXPECT_TRUE(marks.marks().empty()) << "a ray that meets no ground never places";
    EXPECT_EQ(menu.state(), NavState::Placing) << "the arming survives being hidden";
    EXPECT_EQ(nav_menu_rows(menu.state(), marks, on(5, 5), Camera{}).size(), 1U)
        << "and returns unchanged with the reticle back on the ground";
    static_cast<void>(menu.handle(std::array{press(input::kKeySpace)}, marks, on(5, 5), true));
    EXPECT_EQ(marks.marks().size(), 1U);
}

TEST(NavMenu, TabClosesPlacingWithoutPlacing) {
    NavMarks marks;
    NavMenu menu;
    const std::array keys{press(input::kKeyN), press(input::kKeyTab), press(input::kKeySpace)};
    const std::vector<input::KeyEvent> rest = menu.handle(keys, marks, on(5, 5), true);
    EXPECT_TRUE(marks.marks().empty());
    EXPECT_EQ(menu.state(), NavState::Closed);
    ASSERT_EQ(rest.size(), 1U) << "once closed, SPACE is the session's selector again";
    EXPECT_EQ(rest.at(0).scancode, input::kKeySpace);
}

TEST(NavMenu, TildeOpensTheNavMenuOnlyFromTheTopMenuAndOnlyWithAMark) {
    NavMarks marks;
    NavMenu menu;
    const std::array tilde{press(input::kKeyGrave)};
    static_cast<void>(menu.handle(tilde, marks, on(5, 5), true));
    EXPECT_EQ(menu.state(), NavState::Closed) << "no mark, no ~";
    static_cast<void>(marks.place(5, 5));
    static_cast<void>(menu.handle(tilde, marks, on(5, 5), false));
    EXPECT_EQ(menu.state(), NavState::Closed) << "another left menu is open";
    static_cast<void>(menu.handle(tilde, marks, on(5, 5), true));
    EXPECT_EQ(menu.state(), NavState::Menu);
    static_cast<void>(menu.handle(std::array{press(input::kKeyTab)}, marks, on(5, 5), true));
    EXPECT_EQ(menu.state(), NavState::Closed);
}

TEST(NavMenu, TheNavMenuRowsAreSpaceTabThenEverySlotInSlotOrder) {
    NavMarks marks = placed(12);
    Camera from{};
    from.x = (101.5F * kMetresPerTile);
    from.z = (7.5F * kMetresPerTile);
    const std::vector<MenuRow> rows = nav_menu_rows(NavState::Menu, marks, NavAim{}, from);
    ASSERT_EQ(rows.size(), 12U);
    EXPECT_EQ(rows.at(0).key, "SPACE");
    EXPECT_EQ(rows.at(0).verb, "") << "the reticle is on no mark";
    EXPECT_EQ(rows.at(1).key, "TAB");
    EXPECT_EQ(rows.at(1).verb, "Close Menu");
    const std::array<std::string_view, kNavSlots> keys{"1", "2", "3", "4", "5",
                                                       "6", "7", "8", "9", "0"};
    const std::array<int, kNavSlots> placement{11, 12, 3, 4, 5, 6, 7, 8, 9, 10};
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const MenuRow& row = rows.at(i + 2);
        EXPECT_EQ(row.key, keys.at(i));
        EXPECT_EQ(row.verb, "NAV");
        // Placement p sits at tile 100 + p, two metres a tile from tile 101.
        EXPECT_EQ(row.right, std::to_string((placement.at(i) - 1) * 2) + "m") << "row " << i;
    }
}

TEST(NavMenu, SelectReadsOnlyWithTheReticleOverAMark) {
    NavMarks marks = placed(3);
    EXPECT_EQ(nav_menu_rows(NavState::Menu, marks, on(102, 7), Camera{}).at(0).verb, "Select");
    EXPECT_EQ(nav_menu_rows(NavState::Menu, marks, on(150, 7), Camera{}).at(0).verb, "");
    EXPECT_EQ(nav_menu_rows(NavState::Menu, marks, NavAim{}, Camera{}).at(0).verb, "");
}

TEST(NavMenu, SpaceOverAMarkAndItsDigitAreTheSameToggle) {
    const auto open = [](NavMarks& marks, NavMenu& menu) {
        static_cast<void>(menu.handle(std::array{press(input::kKeyGrave)}, marks, NavAim{}, true));
        ASSERT_EQ(menu.state(), NavState::Menu);
    };
    // The mark of placement 3 is slot 2, drawn as `3`, at tile (103, 7).
    NavMarks by_space = placed(5);
    NavMarks by_digit = placed(5);
    NavMenu a;
    NavMenu b;
    open(by_space, a);
    open(by_digit, b);
    static_cast<void>(a.handle(std::array{press(input::kKeySpace)}, by_space, on(103, 7), true));
    static_cast<void>(b.handle(std::array{digit(3)}, by_digit, NavAim{}, true));
    for (NavMarks* marks : {&by_space, &by_digit}) {
        for (const NavMarks::Mark& mark : marks->marks()) {
            EXPECT_EQ(mark.highlighted, mark.slot == 2) << "slot " << mark.slot;
        }
    }
    EXPECT_EQ(a.message(), "Nav 3 highlighted.");
    EXPECT_EQ(b.message(), a.message());
    // Each verb undoes the other: one press per mark, no other state.
    static_cast<void>(a.handle(std::array{digit(3)}, by_space, NavAim{}, true));
    static_cast<void>(b.handle(std::array{press(input::kKeySpace)}, by_digit, on(103, 7), true));
    for (NavMarks* marks : {&by_space, &by_digit}) {
        for (const NavMarks::Mark& mark : marks->marks()) {
            EXPECT_FALSE(mark.highlighted) << "slot " << mark.slot;
        }
    }
    EXPECT_EQ(a.message(), "Nav 3 plain.");
    EXPECT_EQ(a.state(), NavState::Menu) << "the menu stays open";
}

TEST(NavMenu, ZeroIsTheTenthSlotAndADigitWithNoMarkDoesNothing) {
    NavMarks marks = placed(10);
    NavMenu menu;
    static_cast<void>(
        menu.handle(std::array{press(input::kKeyGrave), digit(0)}, marks, NavAim{}, true));
    EXPECT_TRUE(marks.marks().at(9).highlighted);
    NavMarks few = placed(2);
    NavMenu other;
    static_cast<void>(
        other.handle(std::array{press(input::kKeyGrave), digit(7)}, few, NavAim{}, true));
    for (const NavMarks::Mark& mark : few.marks()) {
        EXPECT_FALSE(mark.highlighted);
    }
    EXPECT_TRUE(other.message().empty());
}

TEST(NavMenu, WhileOpenTheTopMenuIsShutOutAndTheRestPasses) {
    NavMarks marks = placed(2);
    for (const std::int32_t opener : {input::kKeyN, input::kKeyGrave}) {
        NavMenu menu;
        static_cast<void>(menu.handle(std::array{press(opener)}, marks, NavAim{}, true));
        ASSERT_NE(menu.state(), NavState::Closed);
        input::KeyEvent shifted_f2 = press(input::kKeyF1 + 1);
        shifted_f2.shift = true;
        const std::array shut{press(input::kKeyF1),
                              shifted_f2,
                              press(input::kKeyG),
                              press(input::kKeyT),
                              press(input::kKeyN),
                              press(input::kKeyGrave),
                              digit(1),
                              digit(0)};
        EXPECT_TRUE(menu.handle(shut, marks, NavAim{}, true).empty());
        EXPECT_NE(menu.state(), NavState::Closed) << "none of those closes it";
        const std::array pass{press(input::kKeyW), press(input::kKeyR), press(input::kKeyX),
                              press(input::kKeyM), input::KeyEvent{input::kKeySpace, false}};
        const std::vector<input::KeyEvent> rest = menu.handle(pass, marks, NavAim{}, true);
        ASSERT_EQ(rest.size(), pass.size());
        for (std::size_t i = 0; i < pass.size(); ++i) {
            EXPECT_EQ(rest.at(i).scancode, pass.at(i).scancode);
            EXPECT_EQ(rest.at(i).pressed, pass.at(i).pressed);
        }
    }
}

TEST(NavMenu, ClosedItTakesOnlyNAndTilde) {
    NavMarks marks = placed(2);
    NavMenu menu;
    const std::array keys{press(input::kKeySpace), digit(1), press(input::kKeyF1),
                          press(input::kKeyTab), press(input::kKeyG)};
    const std::vector<input::KeyEvent> rest = menu.handle(keys, marks, on(101, 7), true);
    ASSERT_EQ(rest.size(), keys.size());
    EXPECT_EQ(menu.state(), NavState::Closed);
    for (const NavMarks::Mark& mark : marks.marks()) {
        EXPECT_FALSE(mark.highlighted) << "SPACE in the top menu is the session's, not a toggle";
    }
}

// --- the reticle, through the real picker -------------------------------------------------------

TEST(NavAim, IsTheTileTheCrosshairRayMeetsAndItsDistance) {
    Rig rig = make_rig();
    aim_at_pixel(rig, 640.0F, 560.0F);
    const Pick ray = ray_tile(rig, 640.0F, 560.0F);
    ASSERT_TRUE(ray.has_tile()) << "the test needs a ray that reaches the ground";
    const NavAim aim = nav_aim(rig.sim, rig.options);
    EXPECT_EQ(aim.tile_x, ray.tile_x);
    EXPECT_EQ(aim.tile_y, ray.tile_y);
    EXPECT_EQ(aim.metres, nav_metres(rig.options.camera, ray.tile_x, ray.tile_y));
    EXPECT_GT(aim.metres, 0);
    to_the_sky(rig);
    EXPECT_FALSE(nav_aim(rig.sim, rig.options).on_ground());
}

TEST(NavAim, MetresAreTheHorizontalDistanceToTheTilesMiddle) {
    Camera from{};
    from.x = 40.5F * kMetresPerTile;
    from.z = 40.5F * kMetresPerTile;
    EXPECT_EQ(nav_metres(from, 40, 40), 0);
    EXPECT_EQ(nav_metres(from, 50, 40), 20);
    EXPECT_EQ(nav_metres(from, 43, 44), 10); // a 3-4-5 triangle of tiles
}

// --- the keys themselves
// --------------------------------------------------------------------------

TEST(NavKeys, NAndTildeArePlainPresses) {
    EXPECT_TRUE(input::nav_arm_pressed(press(input::kKeyN)));
    EXPECT_FALSE(input::nav_arm_pressed(input::KeyEvent{input::kKeyN, false}))
        << "a release is not a press";
    EXPECT_FALSE(input::nav_arm_pressed(input::KeyEvent{input::kKeyN, true, false, true, false}));
    EXPECT_TRUE(input::nav_menu_pressed(press(input::kKeyGrave)));
    EXPECT_FALSE(input::nav_menu_pressed(press(input::kKeyN)));
}

// --- the radar and the world map
// --------------------------------------------------------------------

TEST(NavMarksRadar, ListsExactlyThePlacedMarksInSlotOrder) {
    Rig rig = make_rig();
    NavMarks marks;
    marks.place(44, 40);
    marks.place(40, 44);
    marks.toggle(1);
    const RadarPanel panel = radar_panel(kExtent);
    const std::vector<RadarNav> shown
        = radar_nav_marks(rig.sim.units(), rig.tank_a, marks.marks(), 0.0F, panel);
    ASSERT_EQ(shown.size(), 2U);
    EXPECT_EQ(shown.at(0).slot, 0);
    EXPECT_EQ(shown.at(1).slot, 1);
    EXPECT_FALSE(shown.at(0).highlighted);
    EXPECT_TRUE(shown.at(1).highlighted);
    EXPECT_FALSE(shown.at(0).at_rim);
    EXPECT_FLOAT_EQ(shown.at(0).half, radar_nav_half(panel));
    // Heading-up at yaw 0: east of the tank is to the right of the centre, north of it is above.
    const float middle = panel.width * 0.5F;
    const float middle_y = panel.height * 0.5F;
    EXPECT_GT(shown.at(0).px, middle);
    EXPECT_NEAR(shown.at(0).py, middle_y, 3.0F) << "the same row, within a tile";
    EXPECT_LT(shown.at(1).py, middle_y);
}

TEST(NavMarksRadar, AMarkBeyondTheRadiusIsPinnedToTheRimInItsDirection) {
    Rig rig = make_rig();
    NavMarks marks;
    marks.place(40 + (3 * kRadarRadiusTiles), 40);
    const RadarPanel panel = radar_panel(kExtent);
    const std::vector<RadarNav> shown
        = radar_nav_marks(rig.sim.units(), rig.tank_a, marks.marks(), 0.0F, panel);
    ASSERT_EQ(shown.size(), 1U);
    EXPECT_TRUE(shown.at(0).at_rim);
    const float middle = panel.width * 0.5F;
    EXPECT_GT(shown.at(0).px, middle * 1.5F);
    EXPECT_NEAR(shown.at(0).py, panel.height * 0.5F, 0.5F);
    EXPECT_LE(shown.at(0).px + shown.at(0).half, panel.width);
}

TEST(NavMarksRadar, EveryMarkLiesInsideTheOvalAtAnyBearing) {
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
        marks.place(tile.at(0), tile.at(1));
        const std::vector<RadarNav> shown
            = radar_nav_marks(rig.sim.units(), rig.tank_a, marks.marks(), 0.4F, panel);
        ASSERT_EQ(shown.size(), 1U);
        const RadarNav& nav = shown.at(0);
        for (const float dx : {-nav.half, nav.half}) {
            for (const float dy : {-nav.half, nav.half}) {
                const float ex = (nav.px + dx - (panel.width * 0.5F)) / (panel.width * 0.5F);
                const float ey = (nav.py + dy - (panel.height * 0.5F)) / (panel.height * 0.5F);
                EXPECT_LE((ex * ex) + (ey * ey), 1.0F) << tile.at(0) << "," << tile.at(1);
            }
        }
    }
}

TEST(NavMarksRadar, NoViewerMeansNoMarks) {
    Rig rig = make_rig();
    NavMarks marks;
    marks.place(44, 40);
    EXPECT_TRUE(
        radar_nav_marks(rig.sim.units(), sim::UnitId{}, marks.marks(), 0.0F, radar_panel(kExtent))
            .empty());
}

/// R6, on both surfaces: a plain mark is a green plus and nothing yellow; the same mark highlighted
/// is a yellow plus with a yellow diamond and nothing green. The mark is 30 tiles from the camera,
/// past the scene's 24-tile radius, so no world beacon shares its colours with the surface's mark.
void expect_highlight_is_drawn_differently(bool map) {
    Rig rig = make_rig();
    NavMarks marks;
    static_cast<void>(marks.place(70, 30));
    const Scene plain = scene_with(rig, &marks, map);
    marks.toggle(0);
    const Scene lit = scene_with(rig, &marks, map);
    const std::size_t plus = vertices_in(plain, kNavPlainColour);
    EXPECT_EQ(plus, 12U) << "two crossed bars";
    EXPECT_EQ(vertices_in(plain, kNavHighlightColour), 0U);
    EXPECT_EQ(vertices_in(lit, kNavPlainColour), 0U);
    EXPECT_EQ(vertices_in(lit, kNavHighlightColour), plus + 24U)
        << "the plus and four diamond sides";
    EXPECT_EQ(plain.vertices.size() + 24U, lit.vertices.size()) << "nothing else moved";
}

TEST(NavMarksRadar, AHighlightedMarkIsAYellowPlusInADiamond) {
    expect_highlight_is_drawn_differently(false);
}

TEST(NavMarksMap, AHighlightedMarkIsAYellowPlusInADiamond) {
    expect_highlight_is_drawn_differently(true);
}

TEST(NavMarksMap, TheMapsMarkIsTheRadarsMark) {
    // "It's EXACTLY the same as in the radar": the same glyph at the same size, only placed
    // elsewhere. Every vertex of the map's mark is the radar's, shifted by one offset.
    Rig rig = make_rig();
    NavMarks marks;
    static_cast<void>(marks.place(70, 30));
    marks.toggle(0);
    const auto glyph = [&](bool map) {
        std::vector<SceneVertex> out;
        for (const SceneVertex& v : scene_with(rig, &marks, map).vertices) {
            if (has_colour(v, kNavHighlightColour)) {
                const Projected p = project(rig.options.camera, kExtent, v.position[0],
                                            v.position[1], v.position[2]);
                out.push_back(SceneVertex{{p.x, p.y, 0.0F}, v.colour});
            }
        }
        return out;
    };
    const std::vector<SceneVertex> radar = glyph(false);
    const std::vector<SceneVertex> map = glyph(true);
    ASSERT_EQ(radar.size(), map.size());
    ASSERT_FALSE(radar.empty());
    const float dx = map.at(0).position[0] - radar.at(0).position[0];
    const float dy = map.at(0).position[1] - radar.at(0).position[1];
    // A quarter of a pixel: the two layers sit at different depths, and a float round trip through
    // `ray_point` and `project` is good to a few hundredths of a pixel there, not to zero.
    constexpr float kPixel = 0.25F;
    for (std::size_t i = 0; i < radar.size(); ++i) {
        EXPECT_NEAR(map.at(i).position[0] - radar.at(i).position[0], dx, kPixel) << i;
        EXPECT_NEAR(map.at(i).position[1] - radar.at(i).position[1], dy, kPixel) << i;
    }
}

TEST(NavMarksWorld, AMarkDrawsABeaconInItsOwnColourAndNoMarkDrawsNone) {
    Rig rig = make_rig();
    NavMarks marks;
    EXPECT_EQ(vertices_in(scene_with(rig, &marks), kNavPlainColour), 0U);
    EXPECT_EQ(vertices_in(scene_with(rig, nullptr), kNavPlainColour), 0U);
    marks.place(40, 50);
    const auto standing = [&](const Scene& scene, const math::Vec3& rgb) {
        const float cx = 40.5F * kMetresPerTile;
        const float cz = 50.5F * kMetresPerTile;
        for (const SceneVertex& v : scene.vertices) {
            if (protekt::game::haze::is_ink(rig.options, v, rgb)
                && std::abs(v.position[0] - cx) < 1.0F && std::abs(v.position[2] - cz) < 1.0F
                && v.position[1] > height_metres(kSeed, 40, 50) + 3.0F) {
                return true;
            }
        }
        return false;
    };
    EXPECT_TRUE(standing(scene_with(rig, &marks), kNavPlainColour))
        << "a green beacon stands at the tile, well above the ground";
    marks.toggle(0);
    EXPECT_TRUE(standing(scene_with(rig, &marks), kNavHighlightColour)) << "yellow, highlighted";
}

TEST(NavMarksSurvive, ATickACameraMoveAndAnXSwap) {
    Rig rig = make_rig();
    NavMarks marks;
    marks.place(44, 40);
    marks.place(30, 30);
    marks.toggle(1);
    const std::vector<NavMarks::Mark> before = marks.marks();
    const auto same = [&] {
        const std::vector<NavMarks::Mark> now = marks.marks();
        ASSERT_EQ(now.size(), before.size());
        for (std::size_t i = 0; i < now.size(); ++i) {
            EXPECT_EQ(now.at(i).slot, before.at(i).slot);
            EXPECT_EQ(now.at(i).tile_x, before.at(i).tile_x);
            EXPECT_EQ(now.at(i).tile_y, before.at(i).tile_y);
            EXPECT_EQ(now.at(i).highlighted, before.at(i).highlighted);
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

/// `n`, then SPACE, at three spots under the crosshair, the second one highlighted.
void place_three(Rig& rig, NavMarks& marks) {
    NavMenu menu;
    for (const float y : {500.0F, 560.0F, 640.0F}) {
        aim_at_pixel(rig, 640.0F, y);
        const std::array keys{press(input::kKeyN), press(input::kKeySpace)};
        static_cast<void>(menu.handle(keys, marks, nav_aim(rig.sim, rig.options), true));
        rig.sim.step();
    }
    static_cast<void>(menu.handle(std::array{press(input::kKeyGrave), digit(2)}, marks,
                                  nav_aim(rig.sim, rig.options), true));
}

[[nodiscard]] std::vector<std::uint8_t> run_once() {
    Rig rig = make_rig();
    NavMarks marks;
    place_three(rig, marks);
    const Scene scene = scene_with(rig, &marks);
    std::vector<std::uint8_t> bytes(scene.vertices.size() * sizeof(SceneVertex));
    std::memcpy(bytes.data(), scene.vertices.data(), bytes.size());
    for (const NavMarks::Mark& m : marks.marks()) {
        bytes.push_back(static_cast<std::uint8_t>(m.slot));
        bytes.push_back(static_cast<std::uint8_t>(m.tile_x));
        bytes.push_back(static_cast<std::uint8_t>(m.tile_y));
        bytes.push_back(static_cast<std::uint8_t>(m.highlighted ? 1 : 0));
    }
    return bytes;
}

TEST(NavMarksDeterminism, TwoSameSeedRunsAreByteIdentical) {
    const std::vector<std::uint8_t> a = run_once();
    const std::vector<std::uint8_t> b = run_once();
    ASSERT_FALSE(a.empty());
    EXPECT_EQ(a, b);
}

TEST(NavMarksSim, PlacingAndHighlightingDoNotChangeTheSimHash) {
    Rig with = make_rig();
    Rig without = make_rig();
    NavMarks marks;
    place_three(with, marks);
    ASSERT_EQ(marks.marks().size(), 3U);
    ASSERT_TRUE(marks.marks().at(1).highlighted);
    for (int i = 0; i < 3; ++i) {
        without.sim.step();
    }
    for (int i = 0; i < 20; ++i) {
        with.sim.step();
        without.sim.step();
    }
    EXPECT_EQ(with.sim.state_hash(), without.sim.state_hash());
}

} // namespace
} // namespace protekt::game
