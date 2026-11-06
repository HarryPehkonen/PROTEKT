// AC: the SERVICE TRUCK's own menu. Harri, playtest round 11: "The left side menu should be
// different for service truckes. '3 Repair'. If I select a service truck and choose 3 from the
// menu, it should come up with the nearest ten assets of mine that need repair, in order of
// vicinity. If something does not need repair, it is not listed. You don't always see ten. The max
// range for anything to be repaired on the list is within radar range." And, ruled the same day:
// "Whichever vehicle I'm in at the time sets the radar range."
//
// Everything here goes through the app's own door -- a `Session` fed key frames, and `menu_for`,
// the rows the frame draws -- so what is pinned is what the player sees and presses.
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "protekt/app/player_view.hpp"
#include "protekt/game/menu.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/session.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"

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

struct Yard {
    sim::Sim sim;
    SceneOptions options;
    RowState row;
    UnitId driven;
    UnitId truck;
};

/// The driven craft (`driven_kind`) and a service truck beside it, at (60,60), then `others` in the
/// order given -- spawn order is deliberately NOT distance order in the tests below, so a list that
/// is merely in the sim's order cannot pass for one that is sorted.
[[nodiscard]] Yard make_yard(const std::vector<sim::Spawn>& others,
                             UnitKind driven_kind = UnitKind::Tank) {
    std::vector<sim::Spawn> spawns{{driven_kind, Side::Player, kHomeX, kHomeY},
                                   {UnitKind::ServiceTruck, Side::Player, kHomeX + 1, kHomeY}};
    spawns.insert(spawns.end(), others.begin(), others.end());
    Yard y{};
    y.sim = sim::Sim::create(sim::Scenario{kSeed, spawns});
    y.driven = y.sim.units().at(0).id;
    y.truck = y.sim.units().at(1).id;
    y.options.seed = kSeed;
    y.options.extent = Extent{1280, 720};
    y.options.camera = Camera{120.0F, 20.0F, 120.0F, 0.0F, 0.0F};
    y.options.viewer_unit = y.driven;
    y.row.update(y.sim, Side::Player, 0);
    y.options.row = &y.row;
    return y;
}

[[nodiscard]] InputFrame key_frame(std::int32_t scancode) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{scancode, true, false});
    return frame;
}

/// A wounded friendly tank `tiles` straight east of home.
[[nodiscard]] sim::Spawn hurt_tank(int tiles) {
    return {UnitKind::Tank, Side::Player, kHomeX + tiles, kHomeY, kHurt};
}

/// Press whichever function key holds the lone truck, so the truck is the whole selection.
void select_truck(const Yard& y, Session& session, std::vector<sim::Order>& orders) {
    for (int slot = 0; slot < 10; ++slot) {
        if (session.selection().size() == 1U && session.selection().units().front() == y.truck) {
            break;
        }
        session.frame(key_frame(input::kKeyF1 + slot), y.sim, y.options, orders);
    }
    ASSERT_EQ(session.selection().size(), 1U);
    ASSERT_EQ(session.selection().units().front(), y.truck);
}

[[nodiscard]] std::vector<MenuRow> rows_now(const Yard& y, const Session& session) {
    return app::menu_for(y.sim, session, y.options, session.attack_armed());
}

[[nodiscard]] const MenuRow* row_keyed(const std::vector<MenuRow>& rows, std::string_view key) {
    for (const MenuRow& row : rows) {
        if (row.key == key) {
            return &row;
        }
    }
    return nullptr;
}

/// The key of the first row, or nothing when there are no rows: an assertion on a menu that came
/// out wrong must fail, not read past the end of an empty one.
[[nodiscard]] std::string first_key(const std::vector<MenuRow>& rows) {
    return rows.empty() ? std::string{} : std::string{rows.front().key};
}

/// The list rows only: everything under the `TAB Close Menu` row that opens a list.
[[nodiscard]] std::vector<MenuRow> list_of(const std::vector<MenuRow>& rows) {
    return rows.size() < 2 ? std::vector<MenuRow>{}
                           : std::vector<MenuRow>(rows.begin() + 1, rows.end());
}

[[nodiscard]] std::string metres_of(int tiles) {
    return metres_text(
        static_cast<int>(std::floor((static_cast<float>(tiles) * kMetresPerTile) + 0.5F)));
}

/// What the frame ordered the TRUCK to do. The session also issues the driven tank's own order on
/// every frame, which is not the list's business.
[[nodiscard]] std::vector<sim::Order> trucks_orders(const Yard& y,
                                                    const std::vector<sim::Order>& orders) {
    std::vector<sim::Order> mine;
    for (const sim::Order& order : orders) {
        if (order.unit == y.truck) {
            mine.push_back(order);
        }
    }
    return mine;
}

void open_list(const Yard& y, Session& session, std::vector<sim::Order>& orders) {
    select_truck(y, session, orders);
    session.frame(key_frame(input::kKey1 + 2), y.sim, y.options, orders); // the menu's `3`
}

} // namespace

// ------------------------------------------------------------------------------ the menu

TEST(RepairList, AServiceTrucksMenuCarriesThreeRepair) {
    Yard y = make_yard({});
    Session session{y.driven};
    std::vector<sim::Order> orders;
    select_truck(y, session, orders);

    const std::vector<MenuRow> rows = rows_now(y, session);
    const MenuRow* three = row_keyed(rows, "3");
    ASSERT_NE(three, nullptr);
    EXPECT_EQ(three->verb, "Repair");
    EXPECT_TRUE(three->enabled);
    for (const MenuRow& row : rows) {
        EXPECT_NE(row.verb, "Attack")
            << "a truck is unarmed: its menu is not the ordinary vehicle's";
    }
}

TEST(RepairList, AnOrdinaryVehiclesMenuStillCarriesThreeAttack) {
    Yard y = make_yard({{UnitKind::Tank, Side::Player, kHomeX + 2, kHomeY}});
    const std::vector<UnitId> tank{y.sim.units().at(2).id};
    MenuInput in;
    in.subject = menu_subject_of(y.sim, tank);
    in.faction = Faction::Human;
    const std::vector<MenuRow> rows = menu_rows(in);
    const MenuRow* three = row_keyed(rows, "3");
    ASSERT_NE(three, nullptr);
    EXPECT_EQ(three->verb, "Attack");
}

// ------------------------------------------------------------------------------ the list

TEST(RepairList, ChoosingItListsOnlyTheWoundedNearestFirst) {
    // Spawned far-to-near-to-mid on purpose. A whole tank at 3 tiles and a wounded ENEMY at 2 are
    // nearer than anything listed and must not be.
    Yard y = make_yard({hurt_tank(20),
                        {UnitKind::Tank, Side::Player, kHomeX + 3, kHomeY},
                        hurt_tank(10),
                        {UnitKind::Tank, Side::Enemy, kHomeX + 2, kHomeY, kHurt},
                        hurt_tank(4)});
    Session session{y.driven};
    std::vector<sim::Order> orders;
    open_list(y, session, orders);

    const std::vector<MenuRow> rows = rows_now(y, session);
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(first_key(rows), "TAB") << "the list replaces the menu; it is left by TAB";
    const std::vector<MenuRow> list = list_of(rows);
    ASSERT_EQ(list.size(), 3U) << "three wounded friends, not the whole one and not the enemy";
    EXPECT_EQ(list.at(0).key, "1");
    EXPECT_EQ(list.at(1).key, "2");
    EXPECT_EQ(list.at(2).key, "3");
    EXPECT_EQ(list.at(0).right, metres_of(4)) << "nearest first";
    EXPECT_EQ(list.at(1).right, metres_of(10));
    EXPECT_EQ(list.at(2).right, metres_of(20));
    EXPECT_EQ(list.at(0).verb, std::string{kind_label(UnitKind::Tank, Faction::Human)})
        << "each row names its asset";
    for (const MenuRow& row : list) {
        EXPECT_TRUE(row.enabled);
    }
}

TEST(RepairList, AnUninjuredAssetIsNotListedHoweverNearItIs) {
    Yard y = make_yard({{UnitKind::Tank, Side::Player, kHomeX + 1, kHomeY + 1},
                        {UnitKind::Factory, Side::Player, kHomeX - 8, kHomeY},
                        hurt_tank(12)});
    Session session{y.driven};
    std::vector<sim::Order> orders;
    open_list(y, session, orders);

    const std::vector<MenuRow> list = list_of(rows_now(y, session));
    ASSERT_EQ(list.size(), 1U);
    EXPECT_EQ(list.at(0).right, metres_of(12));
}

TEST(RepairList, ABuildingThatIsHurtIsListedLikeAnythingElse) {
    Yard y = make_yard({{UnitKind::Factory, Side::Player, kHomeX - 9, kHomeY,
                         static_cast<std::uint16_t>(sim::spec_of(UnitKind::Factory).hp / 2)}});
    Session session{y.driven};
    std::vector<sim::Order> orders;
    open_list(y, session, orders);

    const std::vector<MenuRow> list = list_of(rows_now(y, session));
    ASSERT_EQ(list.size(), 1U);
    EXPECT_EQ(list.at(0).verb, std::string{kind_label(UnitKind::Factory, Faction::Human)});
}

TEST(RepairList, FewerThanTenWhenFewerQualify) {
    Yard y = make_yard({hurt_tank(5), hurt_tank(6)});
    Session session{y.driven};
    std::vector<sim::Order> orders;
    open_list(y, session, orders);
    EXPECT_EQ(list_of(rows_now(y, session)).size(), 2U);
}

TEST(RepairList, NeverMoreThanTenAndTheTenAreTheNearestTen) {
    std::vector<sim::Spawn> hurt;
    for (int tiles = 14; tiles >= 2; --tiles) { // twelve, farthest first: 2..14 tiles
        hurt.push_back(hurt_tank(tiles));
    }
    Yard y = make_yard(hurt);
    Session session{y.driven};
    std::vector<sim::Order> orders;
    open_list(y, session, orders);

    const std::vector<MenuRow> list = list_of(rows_now(y, session));
    ASSERT_EQ(list.size(), 10U);
    const std::array<std::string_view, 10> keys{"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
    for (std::size_t i = 0; i < list.size(); ++i) {
        EXPECT_EQ(list.at(i).key, keys.at(i)) << "ten rows on the ten digits, the tenth on 0";
        EXPECT_EQ(list.at(i).right, metres_of(2 + static_cast<int>(i)));
    }
}

TEST(RepairList, NothingBeyondTheDrivenCraftsRadarIsListed) {
    const int reach = sim::radar_radius_tiles(UnitKind::Tank);
    Yard y = make_yard({hurt_tank(reach), hurt_tank(reach + 1), hurt_tank(reach + 20)});
    Session session{y.driven};
    std::vector<sim::Order> orders;
    open_list(y, session, orders);

    const std::vector<MenuRow> list = list_of(rows_now(y, session));
    ASSERT_EQ(list.size(), 1U) << "the rim is in, one tile past it is out";
    EXPECT_EQ(list.at(0).right, metres_of(reach));
}

TEST(RepairList, TheReachIsTheDrivenCraftsNotTheTrucks) {
    // A scout sees twice as far as a tank or a truck. The same wounded friend, 60 tiles out, is on
    // the list when the player sits in a scout and off it when the player sits in a tank: the truck
    // is the same truck either way, so only the driven craft can have made the difference.
    const int out = sim::radar_radius_tiles(UnitKind::Tank) + 20;
    ASSERT_LT(out, sim::radar_radius_tiles(UnitKind::Scout));
    {
        Yard y = make_yard({hurt_tank(out)}, UnitKind::Scout);
        Session session{y.driven};
        std::vector<sim::Order> orders;
        open_list(y, session, orders);
        EXPECT_EQ(list_of(rows_now(y, session)).size(), 1U) << "driving a scout";
    }
    {
        Yard y = make_yard({hurt_tank(out)}, UnitKind::Tank);
        Session session{y.driven};
        std::vector<sim::Order> orders;
        open_list(y, session, orders);
        EXPECT_EQ(first_key(rows_now(y, session)), "SPACE")
            << "nothing in reach: the list does not open, the truck's menu stays";
    }
}

TEST(RepairList, WithNothingToMendTheListDoesNotOpen) {
    Yard y = make_yard({{UnitKind::Tank, Side::Player, kHomeX + 4, kHomeY}});
    Session session{y.driven};
    std::vector<sim::Order> orders;
    open_list(y, session, orders);

    const std::vector<MenuRow> rows = rows_now(y, session);
    EXPECT_EQ(first_key(rows), "SPACE") << "still the truck's own menu";
    const MenuRow* three = row_keyed(rows, "3");
    ASSERT_NE(three, nullptr);
    EXPECT_EQ(three->verb, "Repair");
    EXPECT_FALSE(session.selection().empty()) << "and the truck is still selected";
}

// ------------------------------------------------------------------------------ the choice

/// Three patients spawned 20, 4 and 10 tiles out, so spawn order, id order and distance order are
/// three different orders: the ids of the farthest, the nearest and the middle one are units 2, 3
/// and 4 of the yard.
[[nodiscard]] Yard three_patients() {
    return make_yard({hurt_tank(20), hurt_tank(4), hurt_tank(10)});
}

/// Open the list, press the digit `key_offset` past `1`, and return the repair orders the truck was
/// given.
[[nodiscard]] std::vector<sim::Order> choose_row(const Yard& y, int key_offset) {
    Session session{y.driven};
    std::vector<sim::Order> orders;
    open_list(y, session, orders);
    orders.clear();
    session.frame(key_frame(input::kKey1 + key_offset), y.sim, y.options, orders);
    EXPECT_TRUE(session.selection().empty()) << "a finished command closes the menu";
    EXPECT_TRUE(rows_now(y, session).empty());
    return trucks_orders(y, orders);
}

[[nodiscard]] UnitId repair_target_of(const std::vector<sim::Order>& mine) {
    if (mine.size() != 1U || mine.front().kind() != sim::OrderKind::Repair) {
        return UnitId{};
    }
    return std::get<sim::orders::Repair>(mine.front().payload).target;
}

TEST(RepairList, ChoosingTheFirstRowSendsTheTruckToTheNearestPatient) {
    const Yard y = three_patients();
    EXPECT_EQ(repair_target_of(choose_row(y, 0)), y.sim.units().at(3).id);
}

TEST(RepairList, ChoosingTheSecondRowSendsTheTruckToTheNextNearest) {
    const Yard y = three_patients();
    EXPECT_EQ(repair_target_of(choose_row(y, 1)), y.sim.units().at(4).id);
}

TEST(RepairList, ChoosingTheLastRowSendsTheTruckToTheFarthestPatient) {
    const Yard y = three_patients();
    EXPECT_EQ(repair_target_of(choose_row(y, 2)), y.sim.units().at(2).id);
}

TEST(RepairList, AKeyWithNoRowOrdersNothingAndTheListIsLeft) {
    Yard y = make_yard({hurt_tank(4)});
    Session session{y.driven};
    std::vector<sim::Order> orders;
    open_list(y, session, orders);
    orders.clear();

    session.frame(key_frame(input::kKey1 + 6), y.sim, y.options, orders); // row 7: none
    EXPECT_TRUE(trucks_orders(y, orders).empty());
    EXPECT_EQ(first_key(rows_now(y, session)), "SPACE") << "back on the truck's own menu";
}

TEST(RepairList, EscapeLeavesTheListWithoutOrderingAnything) {
    Yard y = make_yard({hurt_tank(4)});
    Session session{y.driven};
    std::vector<sim::Order> orders;
    open_list(y, session, orders);
    ASSERT_EQ(first_key(rows_now(y, session)), "TAB") << "the list is open";
    orders.clear();

    session.frame(key_frame(input::kKeyEscape), y.sim, y.options, orders);
    EXPECT_TRUE(trucks_orders(y, orders).empty());
    EXPECT_EQ(first_key(rows_now(y, session)), "SPACE") << "the truck's own menu again";
}

} // namespace protekt::game
