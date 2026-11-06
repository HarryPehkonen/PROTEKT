// AC, the parts under the service truck's repair list (tests/test_repair_list.cpp drives the whole
// of it through a session): `nearest_wounded` on its own, the list as menu rows, and the green
// leader each row draws to the asset it names.
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "protekt/game/attack_aim.hpp"
#include "protekt/game/menu.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/targets.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"

namespace protekt::game {
namespace {

using sim::Side;
using sim::UnitId;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr std::uint16_t kHurt = 10;
constexpr int kReach = 40;

[[nodiscard]] sim::Sim make(const std::vector<sim::Spawn>& spawns) {
    return sim::Sim::create(sim::Scenario{kSeed, spawns});
}

[[nodiscard]] std::vector<UnitId> ids_of(const std::vector<TargetRow>& rows) {
    std::vector<UnitId> ids;
    ids.reserve(rows.size());
    for (const TargetRow& row : rows) {
        ids.push_back(row.id);
    }
    return ids;
}

[[nodiscard]] UnitId nth(const sim::Sim& sim, std::size_t index) {
    return sim.units().at(index).id;
}

} // namespace

// ------------------------------------------------------------------------------ nearest_wounded

TEST(NearestWounded, OrdersByDistanceAndNotBySpawnOrOrIdAndTiesGoToTheLowerId) {
    // 0 centre, 1 truck, then wounded at 9 (id 3), 3 (id 4), 6 north (id 5), 6 east (id 6).
    const sim::Sim sim = make({{UnitKind::Tank, Side::Player, 60, 60},
                               {UnitKind::ServiceTruck, Side::Player, 60, 61},
                               {UnitKind::Tank, Side::Player, 69, 60, kHurt},
                               {UnitKind::Tank, Side::Player, 63, 60, kHurt},
                               {UnitKind::Tank, Side::Player, 60, 66, kHurt},
                               {UnitKind::Tank, Side::Player, 66, 60, kHurt}});
    const std::vector<UnitId> trucks{nth(sim, 1)};
    const std::vector<TargetRow> rows
        = nearest_wounded(sim, Side::Player, nth(sim, 0), kReach, trucks);
    EXPECT_EQ(ids_of(rows),
              (std::vector<UnitId>{nth(sim, 3), nth(sim, 4), nth(sim, 5), nth(sim, 2)}));
    EXPECT_EQ(rows.at(0).key, 1);
    EXPECT_EQ(rows.at(3).key, 4);
}

TEST(NearestWounded, AWholeUnitAnEnemyAndTheCentreAreNeverListed) {
    const sim::Sim sim = make({{UnitKind::Tank, Side::Player, 60, 60, kHurt}, // the centre, hurt
                               {UnitKind::ServiceTruck, Side::Player, 60, 61},
                               {UnitKind::Tank, Side::Player, 61, 60},          // whole
                               {UnitKind::Tank, Side::Enemy, 62, 60, kHurt},    // hostile
                               {UnitKind::Tank, Side::Player, 63, 60, kHurt}}); // the one
    const std::vector<UnitId> trucks{nth(sim, 1)};
    EXPECT_EQ(ids_of(nearest_wounded(sim, Side::Player, nth(sim, 0), kReach, trucks)),
              (std::vector<UnitId>{nth(sim, 4)}));
}

TEST(NearestWounded, ALoneTruckIsNotItsOwnPatientButOneOfTwoIs) {
    const sim::Sim sim = make({{UnitKind::Tank, Side::Player, 60, 60},
                               {UnitKind::ServiceTruck, Side::Player, 60, 62, kHurt},
                               {UnitKind::ServiceTruck, Side::Player, 60, 64, kHurt}});
    EXPECT_TRUE(
        nearest_wounded(sim, Side::Player, nth(sim, 0), kReach, std::vector<UnitId>{nth(sim, 1)})
            .size()
        == 1U)
        << "the other truck can be sent to it, but the lone one cannot mend itself";
    EXPECT_EQ(ids_of(nearest_wounded(sim, Side::Player, nth(sim, 0), kReach,
                                     std::vector<UnitId>{nth(sim, 1)})),
              (std::vector<UnitId>{nth(sim, 2)}))
        << "selected: truck 1 alone, so only truck 2 is a patient";
    EXPECT_EQ(nearest_wounded(sim, Side::Player, nth(sim, 0), kReach,
                              std::vector<UnitId>{nth(sim, 1), nth(sim, 2)})
                  .size(),
              2U)
        << "two trucks selected: each can mend the other";
}

TEST(NearestWounded, ADeadCentreOrNoTruckGivesNoList) {
    const sim::Sim sim = make({{UnitKind::Tank, Side::Player, 60, 60},
                               {UnitKind::ServiceTruck, Side::Player, 60, 61},
                               {UnitKind::Tank, Side::Player, 62, 60, kHurt}});
    EXPECT_TRUE(
        nearest_wounded(sim, Side::Player, UnitId{}, kReach, std::vector<UnitId>{nth(sim, 1)})
            .empty());
    EXPECT_TRUE(nearest_wounded(sim, Side::Player, nth(sim, 0), kReach, {}).empty());
    EXPECT_TRUE(
        nearest_wounded(sim, Side::Player, nth(sim, 0), kReach, std::vector<UnitId>{nth(sim, 0)})
            .empty())
        << "a tank is not a service truck";
}

// ------------------------------------------------------------------------------ the rows

TEST(RepairListRows, EachRowNamesItsAssetItsDistanceAndWhichUnitItIs) {
    const std::vector<TargetRow> list{TargetRow{1, UnitKind::Tank, UnitId{7}, 8},
                                      TargetRow{2, UnitKind::Factory, UnitId{9}, 130}};
    MenuInput in;
    in.subject = MenuSubject::ServiceTruck;
    in.repair_list = &list;
    const std::vector<MenuRow> rows = menu_rows(in);
    ASSERT_EQ(rows.size(), 3U);
    EXPECT_EQ(rows.at(0).key, "TAB");
    EXPECT_EQ(rows.at(1).key, "1");
    EXPECT_EQ(rows.at(1).right, "8m");
    EXPECT_EQ(rows.at(1).unit, UnitId{7});
    EXPECT_EQ(rows.at(2).key, "2");
    EXPECT_EQ(rows.at(2).right, "130m");
    EXPECT_EQ(rows.at(2).unit, UnitId{9});
    EXPECT_FALSE(rows.at(0).unit.valid()) << "the way out names no asset";
}

TEST(RepairListRows, TheTenthRowIsOnZero) {
    std::vector<TargetRow> list;
    list.reserve(10);
    for (int i = 0; i < 10; ++i) {
        list.push_back(
            TargetRow{(i + 1) % 10, UnitKind::Tank, UnitId{static_cast<std::uint32_t>(i + 1)}, i});
    }
    MenuInput in;
    in.repair_list = &list;
    const std::vector<MenuRow> rows = menu_rows(in);
    ASSERT_EQ(rows.size(), 11U);
    EXPECT_EQ(rows.back().key, "0");
}

TEST(RepairListRows, AnEmptyListIsTheOrdinaryTrucksMenu) {
    const std::vector<TargetRow> none;
    MenuInput in;
    in.subject = MenuSubject::ServiceTruck;
    in.repair_list = &none;
    EXPECT_EQ(menu_rows(in).front().key, "SPACE");
}

// ------------------------------------------------------------------------------ the leaders

namespace {

constexpr Extent kFrame{800, 450};

struct Watch {
    sim::Sim sim;
    SceneOptions options;
    UnitId near_one;
    UnitId far_one;
};

[[nodiscard]] Watch watch() {
    Watch w{};
    w.sim = make({{UnitKind::Tank, Side::Player, 50, 50},
                  {UnitKind::Tank, Side::Player, 50, 62, kHurt},
                  {UnitKind::Tank, Side::Player, 54, 66, kHurt}});
    w.near_one = nth(w.sim, 1);
    w.far_one = nth(w.sim, 2);
    w.options.seed = kSeed;
    w.options.radius_tiles = 20;
    w.options.extent = kFrame;
    w.options.viewer_unit = nth(w.sim, 0);
    w.options.camera = Camera{100.0F, 14.0F, 76.0F, 0.0F, -0.4F};
    w.options.show_ledger = false;
    return w;
}

[[nodiscard]] std::vector<MenuRow> list_rows(const Watch& w) {
    const std::vector<TargetRow> list{TargetRow{1, UnitKind::Tank, w.near_one, 24},
                                      TargetRow{2, UnitKind::Tank, w.far_one, 32}};
    MenuInput in;
    in.subject = MenuSubject::ServiceTruck;
    in.repair_list = &list;
    return menu_rows(in);
}

[[nodiscard]] std::vector<std::size_t> green_vertices(const Scene& scene) {
    std::vector<std::size_t> found;
    for (std::size_t i = 0; i < scene.vertices.size(); ++i) {
        if (scene.vertices.at(i).colour == kRepairLineInk) {
            found.push_back(i);
        }
    }
    return found;
}

[[nodiscard]] std::array<float, 2> pixel_of(const SceneOptions& options, const SceneVertex& v) {
    const Projected p
        = project(options.camera, options.extent, v.position[0], v.position[1], v.position[2]);
    return {p.x, p.y};
}

} // namespace

TEST(RepairListLeaders, EveryRowDrawsAGreenLineEndingOnItsOwnAsset) {
    Watch w = watch();
    const std::vector<MenuRow> rows = list_rows(w);
    w.options.show_menu = true;
    w.options.menu = &rows;
    const Scene scene = build_scene(w.sim, w.options);

    const std::vector<std::size_t> at = green_vertices(scene);
    // Two assets, each a quad (6 vertices) and a dot (a whole number of triangles).
    ASSERT_GT(at.size(), 2U * 6U);
    // The first line's quad is the first six green vertices; its far edge is b and c.
    const std::array<float, 2> b = pixel_of(w.options, scene.vertices.at(at.at(1)));
    const std::array<float, 2> c = pixel_of(w.options, scene.vertices.at(at.at(2)));
    const std::array<float, 2> end{(b.at(0) + c.at(0)) * 0.5F, (b.at(1) + c.at(1)) * 0.5F};
    const Projected first = attack_point(w.options, *w.sim.find(w.near_one));
    EXPECT_NEAR(end.at(0), first.x, 0.05F);
    EXPECT_NEAR(end.at(1), first.y, 0.05F);

    // The second asset's quad follows the first's dot: find a green triangle centred on it.
    const Projected second = attack_point(w.options, *w.sim.find(w.far_one));
    bool centred_on_second = false;
    for (std::size_t i = 6; i + 2 < at.size(); i += 3) {
        const std::array<float, 2> centre = pixel_of(w.options, scene.vertices.at(at.at(i)));
        centred_on_second = centred_on_second
                            || (std::abs(centre.at(0) - second.x) < 0.5F
                                && std::abs(centre.at(1) - second.y) < 0.5F);
    }
    EXPECT_TRUE(centred_on_second) << "a dot sits on the second row's asset too";
}

TEST(RepairListLeaders, ALineLeavesTheRowItBelongsToPastItsMetres) {
    Watch w = watch();
    const std::vector<MenuRow> rows = list_rows(w);
    w.options.show_menu = true;
    w.options.menu = &rows;
    const Scene scene = build_scene(w.sim, w.options);
    const std::vector<std::size_t> at = green_vertices(scene);
    ASSERT_GT(at.size(), 6U);

    const float px = static_cast<float>(kFrame.height) / 720.0F;
    const float cell = 2.0F * px; // the menu's layout cell, as the scene lays it out
    const MenuLayout layout
        = menu_layout(rows, 6.0F * px, protekt::game::kMenuTopPixels * px, cell);
    const std::array<float, 2> origin = row_line_origin(layout, 1, rows.at(1).right.size(), cell);
    const std::array<float, 2> a = pixel_of(w.options, scene.vertices.at(at.at(0)));
    const std::array<float, 2> d = pixel_of(w.options, scene.vertices.at(at.at(5)));
    EXPECT_NEAR((a.at(0) + d.at(0)) * 0.5F, origin.at(0), 0.05F);
    EXPECT_NEAR((a.at(1) + d.at(1)) * 0.5F, origin.at(1), 0.05F);
    EXPECT_GT(origin.at(0), layout.rows.at(1).right_left) << "clear of the metres it leaves";
}

TEST(RepairListLeaders, TheOrdinaryTrucksMenuDrawsNoGreenAtAll) {
    Watch w = watch();
    MenuInput in;
    in.subject = MenuSubject::ServiceTruck;
    const std::vector<MenuRow> rows = menu_rows(in);
    w.options.show_menu = true;
    w.options.menu = &rows;
    EXPECT_TRUE(green_vertices(build_scene(w.sim, w.options)).empty());
}

TEST(RepairListLeaders, AnAssetThatIsNotInTheSimDrawsNothing) {
    Watch w = watch();
    std::vector<MenuRow> rows = list_rows(w);
    rows.at(1).unit = UnitId{999}; // not in the sim
    rows.at(2).unit = UnitId{};
    w.options.show_menu = true;
    w.options.menu = &rows;
    EXPECT_TRUE(green_vertices(build_scene(w.sim, w.options)).empty());
}

} // namespace protekt::game
