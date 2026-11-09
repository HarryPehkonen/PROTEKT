// AW: THE FOUR UNIT LISTS LOOK THE SAME. Harri, 2026-11-08: "The lists of units, including in Fx ->
// '~ Select Unit', service vehicle selection -> '3 Repair', any vehicle selection -> '2 Go to
// Nav', as well as offensive vehicle selection (i.e. tank) -> '3 Attack', should all be displayed
// the same way, which should be in the same format and colour as the regular left side menu:
// rounded sides, blue with highlights, number on left, slight gap, and name of vehicle if
// appropriate, and a thin green and blue lines indicating the health and ammunition capacity for
// each."
//
// Each test opens ONE list through the app's own door (`Session` keys and `menu_for`, or the nav
// chain's `nav_menu_rows`), draws the frame, and asks the frame for what the menu's drawer puts
// there: the blue face of every bar and, on a row that names a unit, the two thin lines. The blue
// ammunition line's colour is a literal on purpose -- a test comparing against the drawing's own
// constant would pass whatever it was -- and no other drawing on the frame uses it.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "protekt/app/player_view.hpp"
#include "protekt/game/menu.hpp"
#include "protekt/game/nav_marks.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/session.hpp"
#include "protekt/game/targets.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::game {
namespace {

using input::InputFrame;
using input::KeyEvent;
using sim::Side;
using sim::UnitId;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr std::uint16_t kHurt = 10;
constexpr int kHomeX = 60;
constexpr int kHomeY = 60;
constexpr std::array<float, 3> kAmmoLine{0.35F, 0.70F, 1.0F};
constexpr std::array<float, 3> kGreenLine{0.30F, 0.85F, 0.35F}; // the radar's "yours"

struct Rig {
    sim::Sim sim;
    SceneOptions options;
    RowState row;
    UnitId driven;
    UnitId truck;
};

/// A driven tank, a service truck, a wounded friendly tank and two more tanks that make F1 a
/// group, and an enemy tank close enough to be on the attack list.
[[nodiscard]] Rig make_rig() {
    const std::vector<sim::Spawn> spawns{{UnitKind::Tank, Side::Player, kHomeX, kHomeY},
                                         {UnitKind::ServiceTruck, Side::Player, kHomeX + 1, kHomeY},
                                         {UnitKind::Tank, Side::Player, kHomeX + 4, kHomeY, kHurt},
                                         {UnitKind::Tank, Side::Player, kHomeX + 2, kHomeY + 2},
                                         {UnitKind::Tank, Side::Enemy, kHomeX + 8, kHomeY + 3},
                                         {UnitKind::Tank, Side::Enemy, kHomeX + 6, kHomeY - 3}};
    Rig rig{sim::Sim::create(sim::Scenario{kSeed, spawns}), {}, {}, {}, {}};
    rig.driven = rig.sim.units().at(0).id;
    rig.truck = rig.sim.units().at(1).id;
    rig.options.seed = kSeed;
    rig.options.extent = Extent{1280, 720};
    rig.options.camera = Camera{120.0F, 20.0F, 120.0F, 0.0F, 0.0F};
    rig.options.viewer_unit = rig.driven;
    rig.options.driven_unit = rig.driven;
    rig.row.update(rig.sim, Side::Player, 0);
    rig.options.row = &rig.row;
    return rig;
}

[[nodiscard]] InputFrame key_frame(std::int32_t scancode) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{scancode, true, false});
    return frame;
}

void press(Session& session, Rig& rig, std::int32_t scancode) {
    std::vector<sim::Order> orders;
    session.frame(key_frame(scancode), rig.sim, rig.options, orders);
}

/// Press function keys until `wanted` is the whole selection.
void select_only(Session& session, Rig& rig, UnitId wanted) {
    for (int slot = 0; slot < 10; ++slot) {
        if (session.selection().size() == 1U && session.selection().units().front() == wanted) {
            return;
        }
        press(session, rig, input::kKeyF1 + slot);
    }
}

[[nodiscard]] std::vector<MenuRow> rows_now(const Rig& rig, const Session& session) {
    return app::menu_for(rig.sim, session, rig.options, session.attack_armed());
}

[[nodiscard]] Scene draw(Rig& rig, const std::vector<MenuRow>& rows) {
    rig.options.show_menu = true;
    rig.options.menu = &rows;
    return build_scene(rig.sim, rig.options);
}

[[nodiscard]] std::size_t count_of(const Scene& scene, const std::array<float, 3>& rgb) {
    return static_cast<std::size_t>(
        std::count_if(scene.vertices.begin(), scene.vertices.end(),
                      [&](const SceneVertex& v) { return v.colour == rgb; }));
}

[[nodiscard]] std::size_t unit_rows(const std::vector<MenuRow>& rows) {
    return static_cast<std::size_t>(std::count_if(
        rows.begin(), rows.end(), [](const MenuRow& row) { return row.unit.valid(); }));
}

/// What the shared drawer must put on the frame for `rows`: one blue ammunition line (a quad, six
/// vertices) per row that names a unit, and the bar faces -- the menu's own blue -- on all of them.
void expect_drawn_by_the_menu_drawer(const Scene& scene, const std::vector<MenuRow>& rows) {
    ASSERT_GT(unit_rows(rows), 0U);
    EXPECT_EQ(count_of(scene, kAmmoLine), 6U * unit_rows(rows))
        << "every row that names a unit carries the blue ammunition line";
    EXPECT_GE(count_of(scene, menu_face_colour(true, 0)), rows.size())
        << "the bar faces are the left menu's blue";
}

} // namespace

TEST(ListLook, SelectUnitGoesThroughTheSharedDrawer) {
    // F1 holds every tank here: the driven one and three more, seated by the opening row.
    Rig rig{sim::Sim::create(sim::Scenario{kSeed,
                                           {{UnitKind::Tank, Side::Player, 4, 8},
                                            {UnitKind::Tank, Side::Player, 4, 4},
                                            {UnitKind::Tank, Side::Player, 7, 4},
                                            {UnitKind::Tank, Side::Player, 10, 4}}}),
            {},
            {},
            {},
            {}};
    rig.driven = rig.sim.units().at(0).id;
    rig.row.seed_from_start(rig.sim, Side::Player, 0, rig.driven);
    rig.options.seed = kSeed;
    rig.options.extent = Extent{1280, 720};
    rig.options.driven_unit = rig.driven;
    rig.options.viewer_unit = rig.driven;
    rig.options.row = &rig.row;
    Session session{rig.driven};
    press(session, rig, input::kKeyF1);
    press(session, rig, input::kKeyGrave);
    ASSERT_TRUE(session.unit_list_armed());
    const std::vector<MenuRow> rows = rows_now(rig, session);
    expect_drawn_by_the_menu_drawer(draw(rig, rows), rows);
}

TEST(ListLook, RepairGoesThroughTheSharedDrawer) {
    Rig rig = make_rig();
    Session session{rig.driven};
    select_only(session, rig, rig.truck);
    ASSERT_EQ(session.selection().size(), 1U);
    press(session, rig, input::kKey1 + 2);
    ASSERT_TRUE(session.repair_armed());
    const std::vector<MenuRow> rows = rows_now(rig, session);
    expect_drawn_by_the_menu_drawer(draw(rig, rows), rows);
}

TEST(ListLook, AttackGoesThroughTheSharedDrawer) {
    Rig rig = make_rig();
    Session session{rig.driven};
    press(session, rig, input::kKeyF1);
    press(session, rig, input::kKey1 + 2);
    ASSERT_TRUE(session.attack_armed());
    const std::vector<MenuRow> rows = rows_now(rig, session);
    ASSERT_GE(rows.size(), 2U) << "the attack list is menu rows now, `TAB Close Menu` first";
    EXPECT_EQ(rows.front().key, "TAB");
    expect_drawn_by_the_menu_drawer(draw(rig, rows), rows);
}

TEST(ListLook, GoToNavGoesThroughTheSharedDrawerAndHasNoUnitToMeasure) {
    Rig rig = make_rig();
    NavMarks marks;
    static_cast<void>(marks.place(20, 20));
    static_cast<void>(marks.place(30, 25));
    const NavAim aim{};
    const std::vector<MenuRow> rows = nav_menu_rows(NavState::Menu, marks, aim, rig.options.camera);
    ASSERT_GE(rows.size(), 4U);
    const Scene scene = draw(rig, rows);
    EXPECT_GE(count_of(scene, menu_face_colour(true, 0)), rows.size())
        << "the same blue bars as every other list";
    // A nav mark is a place, not a unit: it has no health and no ammunition, so no lines.
    EXPECT_EQ(unit_rows(rows), 0U);
    EXPECT_EQ(count_of(scene, kAmmoLine), 0U);
}

TEST(ListLook, TheAttackListIsDrawnExactlyAsTheRepairListIs) {
    // The same entries, one list named by `attack_list` and the other by `repair_list`, drawn with
    // the same names: the frames are byte-identical once the repair list's green leader (which the
    // attack list does not draw, a hostile not being yours) is switched off. One drawer, so they
    // cannot drift apart.
    Rig rig = make_rig();
    const std::vector<TargetRow> entries{{1, UnitKind::Tank, rig.sim.units().at(4).id, 40},
                                         {2, UnitKind::Tank, rig.sim.units().at(5).id, 30}};
    MenuInput in;
    in.attack_list = &entries;
    const std::vector<MenuRow> attack = menu_rows(in);
    in.attack_list = nullptr;
    in.repair_list = &entries;
    std::vector<MenuRow> repair = menu_rows(in);
    ASSERT_EQ(attack.size(), repair.size());
    for (MenuRow& row : repair) {
        row.leads = false;
    }
    const Scene a = draw(rig, attack);
    const Scene b = draw(rig, repair);
    ASSERT_EQ(a.vertices.size(), b.vertices.size());
    EXPECT_EQ(
        std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(SceneVertex)),
        0);
}

TEST(ListLook, TheAttackListHasNoDrawingOfItsOwnAndTheCrosshairReadoutIsAnotherThing) {
    // A frame with an attack target under the crosshair draws the floating name and bar over it
    // (`append_attack_readout`) -- that is a readout of ONE target, not the list -- and an armed
    // attack list adds nothing to a frame but its menu rows.
    Rig rig = make_rig();
    Session session{rig.driven};
    press(session, rig, input::kKeyF1);
    press(session, rig, input::kKey1 + 2);
    ASSERT_TRUE(session.attack_armed());
    const std::vector<MenuRow> rows = rows_now(rig, session);
    const std::vector<MenuRow> none;
    const Scene with = draw(rig, rows);
    const Scene without = draw(rig, none);
    EXPECT_EQ(count_of(with, kAmmoLine) - count_of(without, kAmmoLine), 6U * unit_rows(rows));
    EXPECT_EQ(count_of(with, kGreenLine) > count_of(without, kGreenLine), true)
        << "the health lines of a full-health list are the ramp's green";
}

} // namespace protekt::game
