// N3: G arms a go, the next digit 0-9 sends the selection to the nav mark the nav menu numbers so
// (1-9, then 0 for the tenth). While armed the digits are MARK numbers, not N2's selection keys;
// the order is the same MoveTo a right click makes.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "protekt/game/interaction.hpp"
#include "protekt/game/nav_marks.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/row.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/input/bindings.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::game {
namespace {

using input::InputFrame;
using input::KeyEvent;
using sim::Side;
using sim::Spawn;
using sim::UnitId;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr Extent kExtent{1280, 720};

[[nodiscard]] InputFrame key_frame(std::int32_t scancode) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{scancode, true, false});
    return frame;
}

/// The digit `n` (0-9) as the window reports it: HID 30 is "1" ... 38 is "9", 39 is "0". After K2
/// a digit is the MARK and attack-list row key: it answers G or T and selects no vehicle.
[[nodiscard]] InputFrame digit(int n) {
    return key_frame(n == 0 ? input::kKey0 : input::kKey1 + n - 1);
}

/// The F-key `n` (1-10) as the window reports it: F1 is HID 58 ... F10 is 67. These are the
/// roster's group keys now: F1 selects every unit under group 1, wherever they are.
[[nodiscard]] InputFrame fkey(int n) { return key_frame(input::kKeyF1 + n - 1); }

struct Rig {
    sim::Sim sim;
    SceneOptions options;
    NavMarks marks;
    std::vector<UnitId> own;
    RowState row; ///< the command row over `sim`: F1 is the first vehicle, F2 the second...
};

[[nodiscard]] Rig make_rig() {
    Rig rig{};
    // One unit per KIND, so each roster key selects exactly one unit (K1 groups by kind): these
    // tests are about sending a known unit to a mark, not about group selection.
    const std::vector<Spawn> spawns{{UnitKind::Tank, Side::Player, 40, 40},
                                    {UnitKind::HoverTank, Side::Player, 44, 41},
                                    {UnitKind::Mortar, Side::Player, 40, 2},
                                    {UnitKind::Tank, Side::Enemy, 38, 60}};
    rig.sim = sim::Sim::create(sim::Scenario{kSeed, spawns});
    for (const sim::Unit& unit : rig.sim.units()) {
        if (unit.side == Side::Player) {
            rig.own.push_back(unit.id);
        }
    }
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    rig.options.camera = Camera{80.0F, height_metres(kSeed, 40, 40) + 3.0F, 60.0F, 0.0F, -0.12F};
    rig.options.viewer_unit = rig.own.at(0);
    rig.options.aim = Aim{640.0F, 360.0F};
    rig.options.nav_marks = &rig.marks;
    rig.row.update(rig.sim, Side::Player, 0);
    rig.options.row = &rig.row;
    return rig;
}

void aim_at_unit(Rig& rig, UnitId unit) {
    const sim::Unit* u = rig.sim.find(unit);
    const float x
        = static_cast<float>(u->x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(u->y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = height_metres(kSeed, u->x.floor_int(), u->y.floor_int());
    const Projected p = project(rig.options.camera, kExtent, x, ground + 0.9F, z);
    rig.options.aim = Aim{p.x, p.y};
}

[[nodiscard]] std::vector<UnitId> selected(const Interaction& player) {
    const auto units = player.selection().units();
    return {units.begin(), units.end()};
}

/// own[1] selected alone.
void select_second(Rig& rig, Interaction& player) {
    player.handle(fkey(2), rig.sim, rig.options);
    ASSERT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(1)}));
}

/// Mark 1 at (50,50), mark 2 at (52,50), mark 3 at (54,52): the first three slots of the cycle,
/// which the nav menu numbers 1, 2, 3 (MENU-NAV: 1-9, then 0 for the tenth).
void drop_marks(Rig& rig) {
    rig.marks.place(50, 50);
    rig.marks.place(52, 50);
    rig.marks.place(54, 52);
}

void expect_move_to(const sim::Order& order, UnitId unit, int x, int y) {
    EXPECT_EQ(order.unit, unit);
    const auto* move = std::get_if<sim::orders::MoveTo>(&order.payload);
    ASSERT_NE(move, nullptr);
    EXPECT_EQ(move->x, sim::Fixed::from_int(x));
    EXPECT_EQ(move->y, sim::Fixed::from_int(y));
}

TEST(SendToMark, GThenTwoSendsTheSelectionToMarkTwosTile) {
    Rig rig = make_rig();
    drop_marks(rig);
    Interaction player{rig.own.at(0)};
    select_second(rig, player);
    player.handle(key_frame(input::kKeyG), rig.sim, rig.options);
    EXPECT_TRUE(player.go_armed());
    EXPECT_TRUE(player.take_orders().empty()) << "G alone orders nothing";
    player.handle(digit(2), rig.sim, rig.options);
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    expect_move_to(orders.at(0), rig.own.at(1), 52, 50);
    EXPECT_FALSE(player.go_armed()) << "used up";
}

TEST(SendToMark, ZeroIsTheTenthMark) {
    Rig rig = make_rig();
    for (int p = 1; p <= kNavSlots; ++p) {
        rig.marks.place(60 + p, 70);
    }
    Interaction player{rig.own.at(0)};
    select_second(rig, player);
    player.handle(key_frame(input::kKeyG), rig.sim, rig.options);
    player.handle(digit(0), rig.sim, rig.options);
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    expect_move_to(orders.at(0), rig.own.at(1), 70, 70);
}

TEST(SendToMark, ADigitWithNoMarkIsRefusedAndOrdersNothing) {
    Rig rig = make_rig();
    drop_marks(rig);
    Interaction player{rig.own.at(0)};
    select_second(rig, player);
    player.handle(key_frame(input::kKeyG), rig.sim, rig.options);
    player.handle(digit(7), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_EQ(player.message(), "There is no nav mark 7.");
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(1)}));
    EXPECT_FALSE(player.go_armed()) << "a refused digit is still the answer to the prompt";
}

TEST(SendToMark, WhileArmedADigitIsAMarkNumberAndNeverChangesTheSelection) {
    Rig rig = make_rig();
    drop_marks(rig);
    Interaction player{rig.own.at(0)};
    select_second(rig, player);
    player.handle(key_frame(input::kKeyG), rig.sim, rig.options);
    player.handle(digit(4), rig.sim,
                  rig.options); // G eats the digit as mark 4; mark 4 does not exist
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(1)}));
    // And the very next key, with the arming used up, selects a group again: F3.
    player.handle(fkey(3), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(2)}));
}

TEST(SendToMark, EscapeCancelsTheGoAndOrdersNothing) {
    Rig rig = make_rig();
    drop_marks(rig);
    Interaction player{rig.own.at(0)};
    select_second(rig, player);
    player.handle(key_frame(input::kKeyG), rig.sim, rig.options);
    player.handle(key_frame(input::kKeyEscape), rig.sim, rig.options);
    EXPECT_FALSE(player.go_armed());
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(1)})) << "Escape only cancels";
    player.handle(fkey(1), rig.sim, rig.options); // an ordinary roster key again
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(1 - 1)}));
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(SendToMark, AnyOtherKeyDisarmsAndStillDoesItsOwnJob) {
    Rig rig = make_rig();
    drop_marks(rig);
    Interaction player{rig.own.at(0)};
    select_second(rig, player);
    player.handle(key_frame(input::kKeyG), rig.sim, rig.options);
    // Tab is the "other key" now: `Q` stopped being a weapon control on 2026-10-23, so the key
    // whose own job this test leans on is the one that clears the selection.
    player.handle(key_frame(input::kKeyTab), rig.sim, rig.options);
    EXPECT_FALSE(player.go_armed());
    EXPECT_TRUE(player.selection().empty()) << "the key did its own job as well";
    player.handle(digit(0), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(SendToMark, AnOrderByKeyDisarmsTheGo) {
    Rig rig = make_rig();
    drop_marks(rig);
    Interaction player{rig.own.at(0)};
    select_second(rig, player);
    player.handle(key_frame(input::kKeyG), rig.sim, rig.options);
    player.handle(key_frame(input::kKeyM), rig.sim, rig.options); // an order, by hand
    EXPECT_FALSE(player.go_armed());
    for (const sim::Order& order : player.take_orders()) {
        EXPECT_NE(order.unit, rig.own.at(0));
    }
    player.handle(digit(0), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty()) << "no stale arming survives the click";
}

TEST(SendToMark, NothingSelectedIsAMessageAndNoOrder) {
    Rig rig = make_rig();
    drop_marks(rig);
    Interaction player{rig.own.at(0)};
    player.handle(key_frame(input::kKeyG), rig.sim, rig.options);
    player.handle(digit(1), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_EQ(player.message(), "Nothing is selected to give an order to.");
    EXPECT_FALSE(player.go_armed());
}

TEST(SendToMark, ANonDrivenUnitGoesToTheMark) {
    // U3: a selection comes from a function key (a whole group) or from Space over one vehicle,
    // not from repeated Space presses. The rig has one unit per kind, so fkey(2) selects the hover
    // tank group. The driven tank is not in this selection; the hover tank takes the order.
    Rig rig = make_rig();
    drop_marks(rig);
    Interaction player{rig.own.at(0)};
    player.handle(fkey(2), rig.sim, rig.options); // the hover tank group
    ASSERT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(1)}));
    player.handle(key_frame(input::kKeyG), rig.sim, rig.options);
    player.handle(digit(1), rig.sim, rig.options);
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    expect_move_to(orders.at(0), rig.own.at(1), 50, 50);
}

TEST(SendToMark, OnlyTheDrivenUnitSelectedIsRefused) {
    Rig rig = make_rig();
    drop_marks(rig);
    Interaction player{rig.own.at(0)};
    player.handle(fkey(1), rig.sim, rig.options);
    player.handle(key_frame(input::kKeyG), rig.sim, rig.options);
    player.handle(digit(1), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_EQ(player.message(), "You are driving that one; select another unit to give orders.");
}

TEST(SendToMark, TwoSameSeedRunsProduceByteIdenticalOrders) {
    const auto run = [] {
        Rig rig = make_rig();
        drop_marks(rig);
        Interaction player{rig.own.at(0)};
        std::vector<sim::Order> all;
        for (const InputFrame& frame : {fkey(2), key_frame(input::kKeyG), digit(1)}) {
            player.handle(frame, rig.sim, rig.options);
            for (const sim::Order& order : player.take_orders()) {
                all.push_back(order);
            }
        }
        for (const sim::Order& order : all) {
            rig.sim.enqueue(order);
        }
        for (int i = 0; i < 60; ++i) {
            rig.sim.step();
        }
        return rig.sim.state_hash();
    };
    EXPECT_EQ(run(), run());
}

// BZ2A-1: T with nothing hostile under the crosshair arms the attack list, and the next key settles
// it the way G's does. The list is the driven tank's nearest hostiles; see game/targets.hpp.

/// The rig above, plus hostiles to list. From the driven tank at (40,40): the factory at (30,45) is
/// 11.2 tiles (22 m), the tank at (50,55) 18.0 (36 m), and the tank at (38,44) -- clear of the
/// driven tank's line from the camera, so the crosshair can be put on it -- 4.5 (9 m). Ids follow
/// spawn order.
[[nodiscard]] Rig make_listing_rig() {
    Rig rig{};
    const std::vector<Spawn> spawns{
        {UnitKind::Tank, Side::Player, 40, 40},  {UnitKind::HoverTank, Side::Player, 44, 41},
        {UnitKind::Mortar, Side::Player, 40, 2}, {UnitKind::Tank, Side::Enemy, 38, 44},
        {UnitKind::Tank, Side::Enemy, 50, 55},   {UnitKind::Factory, Side::Enemy, 30, 45}};
    rig.sim = sim::Sim::create(sim::Scenario{kSeed, spawns});
    for (const sim::Unit& unit : rig.sim.units()) {
        if (unit.side == Side::Player) {
            rig.own.push_back(unit.id);
        }
    }
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    rig.options.camera = Camera{80.0F, height_metres(kSeed, 40, 40) + 3.0F, 60.0F, 0.0F, -0.12F};
    rig.options.viewer_unit = rig.own.at(0);
    rig.options.aim = Aim{640.0F, 360.0F};
    rig.options.nav_marks = &rig.marks;
    rig.row.update(rig.sim, Side::Player, 0);
    rig.options.row = &rig.row;
    return rig;
}

constexpr UnitId kFrontTank{4};
constexpr UnitId kFarTank{5};
constexpr UnitId kFactory{6};

void expect_attack(const sim::Order& order, UnitId unit, UnitId target) {
    EXPECT_EQ(order.unit, unit);
    const auto* attack = std::get_if<sim::orders::Attack>(&order.payload);
    ASSERT_NE(attack, nullptr);
    EXPECT_EQ(attack->target, target);
}

TEST(AttackList, TWithAHostileUnderTheCrosshairAttacksItAndDoesNotArm) {
    Rig rig = make_listing_rig();
    Interaction player{rig.own.at(0)};
    select_second(rig, player);
    aim_at_unit(rig, kFrontTank);
    ASSERT_TRUE(rig.options.aim.has_value());
    // value_or, not value(): clang-tidy cannot see through gtest's ASSERT macro and reads a
    // dereference as unchecked access (see test_aim.cpp's mark_for). The failure is recorded above.
    const Aim aim = rig.options.aim.value_or(Aim{});
    ASSERT_EQ(pick(rig.sim, rig.options, aim.x, aim.y).unit, kFrontTank);
    player.handle(key_frame(input::kKeyT), rig.sim, rig.options);
    EXPECT_FALSE(player.attack_armed());
    EXPECT_TRUE(player.attack_targets().empty());
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    expect_attack(orders.at(0), rig.own.at(1), kFrontTank);
    EXPECT_EQ(player.message(), "attacking the target under the crosshair.");
}

TEST(AttackList, TWithNothingUnderTheCrosshairArmsAndADigitAttacksThatRow) {
    Rig rig = make_listing_rig();
    Interaction player{rig.own.at(0)};
    // U3: fkey(2) selects the hover tank group; the driven tank is not in this selection.
    player.handle(fkey(2), rig.sim, rig.options);
    rig.options.aim = Aim{640.0F, 360.0F};
    ASSERT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(1)}));
    const UnitId under = pick(rig.sim, rig.options, 640.0F, 360.0F).unit;
    ASSERT_TRUE(!under.valid() || rig.sim.find(under)->side == Side::Player)
        << "nothing hostile may be under the crosshair, or T would attack it";
    static_cast<void>(player.take_orders());

    player.handle(key_frame(input::kKeyT), rig.sim, rig.options);
    EXPECT_TRUE(player.attack_armed());
    EXPECT_TRUE(player.take_orders().empty()) << "T alone orders nothing";
    const std::vector<TargetRow>& rows = player.attack_targets();
    ASSERT_EQ(rows.size(), 3U);
    EXPECT_EQ(rows.at(0).id, kFrontTank);
    EXPECT_EQ(rows.at(0).key, 1);
    EXPECT_EQ(rows.at(0).metres, 9);
    EXPECT_EQ(rows.at(1).id, kFactory);
    EXPECT_EQ(rows.at(1).key, 2);
    EXPECT_EQ(rows.at(1).metres, 22);
    EXPECT_EQ(rows.at(2).id, kFarTank);
    EXPECT_EQ(rows.at(2).key, 3);
    EXPECT_EQ(rows.at(2).metres, 36);

    player.handle(digit(3), rig.sim, rig.options);
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U) << "the hover tank takes the order";
    expect_attack(orders.at(0), rig.own.at(1), kFarTank);
    EXPECT_EQ(player.message(), "attacking the target in row 3.");
    EXPECT_FALSE(player.attack_armed());
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(1)}))
        << "the digit was the list's, not a roster key";
}

TEST(AttackList, TheListIsCapturedAtArmingAndDoesNotShiftUnderTheFinger) {
    Rig rig = make_listing_rig();
    Interaction player{rig.own.at(0)};
    select_second(rig, player);
    player.handle(key_frame(input::kKeyT), rig.sim, rig.options);
    ASSERT_TRUE(player.attack_armed());
    const std::vector<TargetRow> captured = player.attack_targets();
    rig.sim.step();
    EXPECT_EQ(player.attack_targets().size(), captured.size());
    EXPECT_EQ(player.attack_targets().at(0).id, captured.at(0).id);
}

TEST(AttackList, ADigitNamingNoRowIsRefusedOrdersNothingAndEndsTheMode) {
    Rig rig = make_listing_rig();
    Interaction player{rig.own.at(0)};
    select_second(rig, player);
    player.handle(key_frame(input::kKeyT), rig.sim, rig.options);
    ASSERT_TRUE(player.attack_armed());
    player.handle(digit(7), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_EQ(player.message(), "no target 7 in the list.");
    EXPECT_FALSE(player.attack_armed());
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(1)}));
    player.handle(fkey(1), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(0)}))
        << "the mode is over: F1 is the roster's key again";
}

TEST(AttackList, ZeroNamesTheTenthRow) {
    Rig rig = make_listing_rig();
    Interaction player{rig.own.at(0)};
    select_second(rig, player);
    player.handle(key_frame(input::kKeyT), rig.sim, rig.options);
    player.handle(digit(0), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_EQ(player.message(), "no target 0 in the list.") << "three rows: the tenth is empty";
}

TEST(AttackList, EscapeCancelsAndIsSpentDoingSo) {
    Rig rig = make_listing_rig();
    Interaction player{rig.own.at(0)};
    select_second(rig, player);
    player.handle(key_frame(input::kKeyT), rig.sim, rig.options);
    ASSERT_TRUE(player.attack_armed());
    player.handle(key_frame(input::kKeyEscape), rig.sim, rig.options);
    EXPECT_FALSE(player.attack_armed());
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(1)})) << "Escape only cancels";
}

TEST(AttackList, AnyOtherKeyCancelsAndStillDoesItsOwnJob) {
    Rig rig = make_listing_rig();
    Interaction player{rig.own.at(0)};
    select_second(rig, player);
    player.handle(key_frame(input::kKeyT), rig.sim, rig.options);
    ASSERT_TRUE(player.attack_armed());
    player.handle(key_frame(input::kKeyM), rig.sim, rig.options);
    EXPECT_FALSE(player.attack_armed());
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U) << "M moved the selection to the ground under the crosshair";
    EXPECT_EQ(orders.at(0).unit, rig.own.at(1));
    EXPECT_NE(std::get_if<sim::orders::MoveTo>(&orders.at(0).payload), nullptr);
}

TEST(AttackList, AnOrderByKeyCancelsIt) {
    Rig rig = make_listing_rig();
    Interaction player{rig.own.at(0)};
    select_second(rig, player);
    player.handle(key_frame(input::kKeyT), rig.sim, rig.options);
    ASSERT_TRUE(player.attack_armed());
    player.handle(key_frame(input::kKeyM), rig.sim, rig.options); // an order, by hand
    EXPECT_FALSE(player.attack_armed());
}

TEST(AttackList, RefusalsNameTheirReasonAndLeaveTheModeUnarmed) {
    {
        Rig rig = make_listing_rig();
        Interaction player{rig.own.at(0)};
        player.handle(key_frame(input::kKeyT), rig.sim, rig.options);
        EXPECT_FALSE(player.attack_armed());
        EXPECT_EQ(player.message(), "Nothing is selected to give an order to.");
    }
    {
        Rig rig = make_listing_rig();
        Interaction player{rig.own.at(0)};
        player.handle(fkey(1), rig.sim, rig.options); // the driven tank, alone
        ASSERT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(0)}));
        player.handle(key_frame(input::kKeyT), rig.sim, rig.options);
        EXPECT_FALSE(player.attack_armed());
        EXPECT_EQ(player.message(),
                  "You are driving that one; select another unit to give orders.");
    }
    {
        // No hostile within the radius: the only enemy is 60 tiles away.
        Rig rig{};
        const std::vector<Spawn> spawns{{UnitKind::Tank, Side::Player, 40, 40},
                                        {UnitKind::HoverTank, Side::Player, 44, 41},
                                        {UnitKind::Tank, Side::Enemy, 100, 100}};
        rig.sim = sim::Sim::create(sim::Scenario{kSeed, spawns});
        for (const sim::Unit& unit : rig.sim.units()) {
            if (unit.side == Side::Player) {
                rig.own.push_back(unit.id);
            }
        }
        rig.options.seed = kSeed;
        rig.options.extent = kExtent;
        rig.options.camera
            = Camera{80.0F, height_metres(kSeed, 40, 40) + 3.0F, 60.0F, 0.0F, -0.12F};
        rig.options.viewer_unit = rig.own.at(0);
        rig.options.aim = Aim{640.0F, 360.0F};
        rig.row.update(rig.sim, Side::Player, 0);
        rig.options.row = &rig.row;
        Interaction player{rig.own.at(0)};
        select_second(rig, player);
        player.handle(key_frame(input::kKeyT), rig.sim, rig.options);
        EXPECT_FALSE(player.attack_armed());
        EXPECT_TRUE(player.attack_targets().empty());
        EXPECT_EQ(player.message(), "no enemies within radar range.");
    }
}

TEST(AttackList, ArmingEitherClearsTheOther) {
    Rig rig = make_listing_rig();
    drop_marks(rig);
    Interaction player{rig.own.at(0)};
    select_second(rig, player);
    player.handle(key_frame(input::kKeyG), rig.sim, rig.options);
    ASSERT_TRUE(player.go_armed());
    player.handle(key_frame(input::kKeyT), rig.sim, rig.options);
    EXPECT_FALSE(player.go_armed()) << "any other key cancels G, and T then arms the list";
    EXPECT_TRUE(player.attack_armed());
    player.handle(key_frame(input::kKeyG), rig.sim, rig.options);
    EXPECT_TRUE(player.go_armed());
    EXPECT_FALSE(player.attack_armed());
    EXPECT_TRUE(player.attack_targets().empty());
}

// RB-DIGITS: with no prompt open a digit is a row of the selected subject's menu. Two rows act --
// `1 FOLLOW ME` and `4 HOLD` -- and every other row stays exactly as inert as it was.

/// The driven tank, a recycler, two harvesters and one hostile far off. One unit of each own kind
/// except the harvesters, which come in a pair: a group key that selects TWO of the same kind.
struct MenuRig {
    sim::Sim sim;
    SceneOptions options;
    UnitId tank;
    UnitId recycler;
    UnitId harvester_a;
    UnitId harvester_b;
    RowState row;
};

[[nodiscard]] MenuRig make_menu_rig() {
    MenuRig rig{};
    rig.sim = sim::Sim::create(sim::Scenario{kSeed,
                                             {{UnitKind::Tank, Side::Player, 40, 40},
                                              {UnitKind::Recycler, Side::Player, 44, 41},
                                              {UnitKind::Scavenger, Side::Player, 46, 42},
                                              {UnitKind::Scavenger, Side::Player, 48, 43},
                                              {UnitKind::Factory, Side::Player, 36, 50},
                                              {UnitKind::Tank, Side::Enemy, 38, 60}}});
    for (const sim::Unit& unit : rig.sim.units()) {
        if (unit.kind == UnitKind::Tank && unit.side == Side::Player) {
            rig.tank = unit.id;
        } else if (unit.kind == UnitKind::Recycler) {
            rig.recycler = unit.id;
        } else if (unit.kind == UnitKind::Scavenger) {
            (rig.harvester_a.valid() ? rig.harvester_b : rig.harvester_a) = unit.id;
        }
    }
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    rig.options.camera = Camera{80.0F, height_metres(kSeed, 40, 40) + 3.0F, 60.0F, 0.0F, -0.12F};
    rig.options.aim = Aim{640.0F, 700.0F}; // bare ground: no hostile under the mark
    rig.row.update(rig.sim, Side::Player, 0);
    // The pair shares one key, as a shift-built batch would: no batch exists yet, so it is arranged
    // by hand.
    for (RowSlot& slot : rig.row.slots) {
        std::erase(slot.units, rig.harvester_b);
    }
    for (RowSlot& slot : rig.row.slots) {
        if (!slot.units.empty() && slot.units.front() == rig.harvester_a) {
            slot.units.push_back(rig.harvester_b);
        }
    }
    rig.options.row = &rig.row;
    return rig;
}

/// The mark on `unit`, as the rig's camera sees it.
void aim_menu(MenuRig& rig, UnitId unit) {
    const sim::Unit* u = rig.sim.find(unit);
    const float x
        = static_cast<float>(u->x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(u->y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = height_metres(kSeed, u->x.floor_int(), u->y.floor_int());
    const Projected p = project(rig.options.camera, kExtent, x, ground + 0.9F, z);
    rig.options.aim = Aim{p.x, p.y};
}

/// Select the first own unit of `kind` the way the player would: its F key when the row holds it
/// (a vehicle), the crosshair and Space when it does not (the factory is a building, so it has no
/// F key).
void select_kind(MenuRig& rig, Interaction& player, UnitKind kind) {
    for (std::size_t index = kRowFirstGroup; index < kRowSlots; ++index) {
        const RowSlot& slot = rig.row.slots.at(index);
        if (!slot.units.empty() && rig.sim.find(slot.units.front())->kind == kind) {
            player.handle(fkey(static_cast<int>(index - kRowFirstGroup) + 1), rig.sim, rig.options);
            return;
        }
    }
    for (const sim::Unit& unit : rig.sim.units()) {
        if (unit.kind == kind && unit.side == Side::Player) {
            aim_menu(rig, unit.id);
            player.handle(key_frame(input::kKeySpace), rig.sim, rig.options);
            return;
        }
    }
    FAIL() << "no unit of that kind";
}

TEST(MenuDigit, OneOnARecyclerOrdersItToFollowTheDrivenCraft) {
    MenuRig rig = make_menu_rig();
    Interaction player{rig.tank};
    select_kind(rig, player, UnitKind::Recycler);
    ASSERT_EQ(selected(player), (std::vector<UnitId>{rig.recycler}));
    player.handle(digit(1), rig.sim, rig.options);
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.at(0).unit, rig.recycler);
    const auto* follow = std::get_if<sim::orders::Follow>(&orders.at(0).payload);
    ASSERT_NE(follow, nullptr) << "FOLLOW ME is a Follow order";
    EXPECT_EQ(follow->target, rig.tank) << "and the one it shadows is the craft the player drives";
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.recycler}))
        << "a command keeps the selection";
}

TEST(MenuDigit, OneFollowsTheCraftTheXSwapMovedTheDrivingTo) {
    MenuRig rig = make_menu_rig();
    Interaction player{rig.tank};
    select_kind(rig, player, UnitKind::Scavenger);
    // Drive the second harvester: the mark on it, then X.
    aim_menu(rig, rig.harvester_b);
    player.handle(key_frame(input::kKeyX), rig.sim, rig.options);
    ASSERT_EQ(player.driven(), rig.harvester_b);
    static_cast<void>(player.take_orders()); // the vacated tank's Stop
    player.handle(digit(1), rig.sim, rig.options);
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U) << "the driven harvester is selected but is not ordered";
    EXPECT_EQ(orders.at(0).unit, rig.harvester_a);
    const auto* follow = std::get_if<sim::orders::Follow>(&orders.at(0).payload);
    ASSERT_NE(follow, nullptr);
    EXPECT_EQ(follow->target, rig.harvester_b);
}

TEST(MenuDigit, FourOnHarvestersPostsEachWhereItStandsInAscendingIdOrder) {
    MenuRig rig = make_menu_rig();
    Interaction player{rig.tank};
    select_kind(rig, player, UnitKind::Scavenger);
    ASSERT_EQ(selected(player).size(), 2U);
    player.handle(digit(4), rig.sim, rig.options);
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 2U);
    ASSERT_LT(orders.at(0).unit.value, orders.at(1).unit.value);
    for (const sim::Order& order : orders) {
        const auto* post = std::get_if<sim::orders::GuardAt>(&order.payload);
        ASSERT_NE(post, nullptr) << "HOLD is a GuardAt: a Stop does not hold a harvester";
        const sim::Unit* unit = rig.sim.find(order.unit);
        EXPECT_EQ(post->x, unit->x) << "each at its OWN position, not one shared point";
        EXPECT_EQ(post->y, unit->y);
    }
    EXPECT_NE(rig.sim.find(orders.at(0).unit)->x, rig.sim.find(orders.at(1).unit)->x)
        << "the two stand apart, so a shared post would be a wrong one";
}

TEST(MenuDigit, AHeldHarvesterIsStillHeldAfterTheSimHasRunOn) {
    MenuRig rig = make_menu_rig();
    Interaction player{rig.tank};
    select_kind(rig, player, UnitKind::Scavenger);
    player.handle(digit(4), rig.sim, rig.options);
    for (const sim::Order& order : player.take_orders()) {
        rig.sim.enqueue(order);
    }
    const sim::Unit before = *rig.sim.find(rig.harvester_a);
    for (int tick = 0; tick < 90; ++tick) {
        rig.sim.step();
    }
    const sim::Unit* after = rig.sim.find(rig.harvester_a);
    EXPECT_EQ(after->stance, sim::Stance::GuardAt);
    EXPECT_EQ(after->x, before.x);
    EXPECT_EQ(after->y, before.y);
}

TEST(MenuDigit, FourWithTheDrivenHarvesterSelectedHoldsOnlyTheOthers) {
    MenuRig rig = make_menu_rig();
    Interaction player{rig.harvester_b}; // driving the harvester
    select_kind(rig, player, UnitKind::Scavenger);
    ASSERT_EQ(selected(player).size(), 2U) << "the driven craft may be in the selection";
    player.handle(digit(4), rig.sim, rig.options);
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.at(0).unit, rig.harvester_a)
        << "ordering the driven craft would fight its driver";
    player.handle(digit(1), rig.sim, rig.options);
    const std::vector<sim::Order> follow = player.take_orders();
    ASSERT_EQ(follow.size(), 1U);
    EXPECT_EQ(follow.at(0).unit, rig.harvester_a);
}

TEST(MenuDigit, OnlyTheDrivenHarvesterSelectedIsRefusedAndOrdersNothing) {
    MenuRig rig = make_menu_rig();
    Interaction player{rig.tank};
    // Select the harvester by Space, then drive it with X: it is then selected AND driven.
    aim_menu(rig, rig.harvester_b);
    player.handle(key_frame(input::kKeySpace), rig.sim, rig.options);
    player.handle(key_frame(input::kKeyX), rig.sim, rig.options);
    ASSERT_EQ(player.driven(), rig.harvester_b);
    ASSERT_EQ(selected(player), (std::vector<UnitId>{rig.harvester_b}));
    static_cast<void>(player.take_orders()); // the vacated tank's Stop
    for (const int key : {1, 4}) {
        player.handle(digit(key), rig.sim, rig.options);
        EXPECT_TRUE(player.take_orders().empty()) << "digit " << key;
        EXPECT_EQ(player.message(),
                  "You are driving that one; select another unit to give orders.");
    }
}

TEST(MenuDigit, AnArmedGoStillOwnsTheDigit) {
    MenuRig rig = make_menu_rig();
    NavMarks marks;
    marks.place(50, 50);
    marks.place(52, 50); // marks 1 and 2
    rig.options.nav_marks = &marks;
    Interaction player{rig.tank};
    select_kind(rig, player, UnitKind::Recycler);
    player.handle(key_frame(input::kKeyG), rig.sim, rig.options);
    ASSERT_TRUE(player.go_armed());
    player.handle(digit(1), rig.sim, rig.options);
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    expect_move_to(orders.at(0), rig.recycler, 50, 50);
    EXPECT_FALSE(player.go_armed());
    // The prompt is spent, so the very next `1` is the menu's.
    player.handle(digit(1), rig.sim, rig.options);
    const std::vector<sim::Order> next = player.take_orders();
    ASSERT_EQ(next.size(), 1U);
    EXPECT_NE(std::get_if<sim::orders::Follow>(&next.at(0).payload), nullptr);
}

TEST(MenuDigit, AnArmedGoWithNoMarkRefusesAndIssuesNoFollowOrHold) {
    MenuRig rig = make_menu_rig();
    NavMarks marks;
    rig.options.nav_marks = &marks;
    Interaction player{rig.tank};
    select_kind(rig, player, UnitKind::Scavenger);
    for (const int key : {1, 4}) {
        player.handle(key_frame(input::kKeyG), rig.sim, rig.options);
        player.handle(digit(key), rig.sim, rig.options);
        EXPECT_TRUE(player.take_orders().empty()) << "digit " << key << " answered the G prompt";
        EXPECT_FALSE(player.go_armed());
    }
}

TEST(MenuDigit, AnArmedAttackListStillOwnsTheDigit) {
    MenuRig rig = make_menu_rig();
    Interaction player{rig.tank};
    select_kind(rig, player, UnitKind::Recycler);
    player.handle(key_frame(input::kKeyT), rig.sim, rig.options);
    ASSERT_TRUE(player.attack_armed());
    ASSERT_FALSE(player.attack_targets().empty());
    player.handle(digit(1), rig.sim, rig.options);
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_NE(std::get_if<sim::orders::Attack>(&orders.at(0).payload), nullptr)
        << "row 1 of the list, not FOLLOW ME";
    EXPECT_FALSE(player.attack_armed());
}

TEST(MenuDigit, EveryRowThatCannotActYetDoesNothingAndTheSimIsUntouched) {
    struct Case {
        UnitKind kind;
        std::vector<int> digits;
    };
    // The recycler has no 4 (HOLD is the harvester's row); GO TO NAV and SCAVENGE wait on rulings
    // and RB3, and the recycler's 3 only ARMS the deploy (DEPLOY: no order until SPACE); PICK ME
    // UP, BAILOUT and RECYCLE are not built; a deployment's digits are its build list (RB5); a lone
    // tank has no menu at all.
    const std::vector<Case> cases{{UnitKind::Recycler, {0, 2, 3, 4, 5, 9}},
                                  {UnitKind::Scavenger, {0, 2, 3, 5, 9}},
                                  {UnitKind::Factory, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}},
                                  {UnitKind::Tank, {0, 1, 2, 3, 4, 5, 9}}};
    for (const Case& c : cases) {
        MenuRig rig = make_menu_rig();
        Interaction player{rig.tank};
        select_kind(rig, player, c.kind);
        ASSERT_FALSE(player.selection().empty()) << "no selection means nothing is proved";
        const std::uint64_t hash = rig.sim.state_hash();
        for (const int key : c.digits) {
            player.handle(digit(key), rig.sim, rig.options);
            EXPECT_TRUE(player.take_orders().empty())
                << "digit " << key << " on kind " << static_cast<int>(c.kind);
            EXPECT_TRUE(player.message().empty());
        }
        EXPECT_EQ(rig.sim.state_hash(), hash);
    }
}

// DEPLOY: `un-deployed recycler menu -> 3` ARMS the deploy; the reticle picks the ground, `~`
// rotates a preview that a square cannot visibly turn, TAB cancels and SPACE orders it.

/// The rig with the recycler selected and the deploy armed, and the ground tile under the mark.
struct ArmedDeploy {
    MenuRig rig;
    Interaction player;
    Pick ground;
};

[[nodiscard]] ArmedDeploy arm_deploy() {
    ArmedDeploy out{make_menu_rig(), Interaction{UnitId{}}, Pick{}};
    out.player = Interaction{out.rig.tank};
    select_kind(out.rig, out.player, UnitKind::Recycler);
    out.player.handle(digit(3), out.rig.sim, out.rig.options);
    out.ground = pick(out.rig.sim, out.rig.options, out.rig.options.aim->x, out.rig.options.aim->y);
    return out;
}

TEST(DeployGesture, ThreeOnARecyclerArmsTheDeployAndOrdersNothingYet) {
    ArmedDeploy a = arm_deploy();
    EXPECT_TRUE(a.player.deploy_armed());
    EXPECT_TRUE(a.player.take_orders().empty()) << "3 arms; SPACE is what initiates";
    EXPECT_EQ(selected(a.player), (std::vector<UnitId>{a.rig.recycler}));
}

TEST(DeployGesture, SpaceOverTheGroundOrdersTheRecyclerToDeployThereAndDisarms) {
    ArmedDeploy a = arm_deploy();
    ASSERT_TRUE(a.ground.has_tile()) << "the rig's mark is on bare ground";
    ASSERT_TRUE(sim::deploy_site_ok(kSeed, a.rig.sim.units(), a.ground.tile_x, a.ground.tile_y));
    a.player.handle(key_frame(input::kKeySpace), a.rig.sim, a.rig.options);
    const std::vector<sim::Order> orders = a.player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.at(0).unit, a.rig.recycler);
    const auto* deploy = std::get_if<sim::orders::Deploy>(&orders.at(0).payload);
    ASSERT_NE(deploy, nullptr) << "SPACE is the Deploy order";
    EXPECT_EQ(deploy->tile_x, a.ground.tile_x) << "at the tile the reticle picks";
    EXPECT_EQ(deploy->tile_y, a.ground.tile_y);
    EXPECT_FALSE(a.player.deploy_armed()) << "initiated: the arming is spent";
    a.rig.sim.enqueue(orders.at(0));
    EXPECT_EQ(a.rig.sim.command_log().size(), 1U) << "and the sim accepts it";
}

TEST(DeployGesture, TabCancelsAndTheRecyclerStaysAVehicle) {
    ArmedDeploy a = arm_deploy();
    const std::uint64_t hash = a.rig.sim.state_hash();
    a.player.handle(key_frame(input::kKeyTab), a.rig.sim, a.rig.options);
    EXPECT_FALSE(a.player.deploy_armed());
    EXPECT_TRUE(a.player.take_orders().empty());
    EXPECT_TRUE(a.player.selection().empty()) << "TAB closes the menu, as it always does";
    a.player.handle(key_frame(input::kKeySpace), a.rig.sim, a.rig.options);
    for (const sim::Order& order : a.player.take_orders()) {
        EXPECT_EQ(std::get_if<sim::orders::Deploy>(&order.payload), nullptr)
            << "a cancelled deploy is not initiated by the next SPACE";
    }
    EXPECT_EQ(a.rig.sim.state_hash(), hash);
    EXPECT_EQ(a.rig.sim.find(a.rig.recycler)->kind, UnitKind::Recycler);
}

TEST(DeployGesture, TildeRotatesASquareIntoItselfAndKeepsTheArming) {
    ArmedDeploy a = arm_deploy();
    a.player.handle(key_frame(input::kKeyGrave), a.rig.sim, a.rig.options);
    EXPECT_TRUE(a.player.deploy_armed()) << "the deployment menu's own key";
    EXPECT_TRUE(a.player.take_orders().empty());
    a.player.handle(key_frame(input::kKeySpace), a.rig.sim, a.rig.options);
    const std::vector<sim::Order> orders = a.player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    const auto* deploy = std::get_if<sim::orders::Deploy>(&orders.at(0).payload);
    ASSERT_NE(deploy, nullptr);
    EXPECT_EQ(deploy->tile_x, a.ground.tile_x) << "a turned 2x2 is the same 2x2 on the same tiles";
    EXPECT_EQ(deploy->tile_y, a.ground.tile_y);
}

TEST(DeployGesture, TheTopMenuIsShownButNotReachableWhileArmed) {
    ArmedDeploy a = arm_deploy();
    const RowState row = a.rig.row;
    for (const std::int32_t scancode :
         {input::kKeyF1, input::kKeyF1 + 2, input::kKeyG, input::kKeyT}) {
        a.player.handle(key_frame(scancode), a.rig.sim, a.rig.options);
        EXPECT_TRUE(a.player.deploy_armed()) << "scancode " << scancode;
        EXPECT_EQ(selected(a.player), (std::vector<UnitId>{a.rig.recycler})) << scancode;
        EXPECT_FALSE(a.player.go_armed());
        EXPECT_FALSE(a.player.attack_armed());
        EXPECT_TRUE(a.player.take_orders().empty());
    }
    InputFrame shifted = fkey(5);
    shifted.keys.front().shift = true;
    a.player.handle(shifted, a.rig.sim, a.rig.options);
    EXPECT_EQ(a.rig.row.slots, row.slots) << "Shift+F moves nothing while a left menu is open";
}

TEST(DeployGesture, SpaceOffTheGroundDeploysNothingAndTheArmingSurvives) {
    ArmedDeploy a = arm_deploy();
    a.rig.options.camera.y += 500.0F; // well above every hill, looking up: no ground in range
    a.rig.options.camera.pitch = 0.3F;
    ASSERT_FALSE(
        pick(a.rig.sim, a.rig.options, a.rig.options.aim->x, a.rig.options.aim->y).has_tile());
    a.player.handle(key_frame(input::kKeySpace), a.rig.sim, a.rig.options);
    EXPECT_TRUE(a.player.take_orders().empty());
    EXPECT_TRUE(a.player.deploy_armed()) << "it waits for the reticle to come back to the ground";
}

TEST(DeployGesture, ASiteTheSimWouldRefuseIsRefusedHereAndTheArmingStays) {
    ArmedDeploy a = arm_deploy();
    ASSERT_TRUE(a.ground.has_tile());
    // The same board with a building already standing on the tile under the mark.
    std::vector<Spawn> spawns{
        {UnitKind::Tank, Side::Player, 40, 40},
        {UnitKind::Recycler, Side::Player, 44, 41},
        {UnitKind::DeployedRecycler, Side::Enemy, a.ground.tile_x, a.ground.tile_y}};
    const sim::Sim crowded = sim::Sim::create(sim::Scenario{kSeed, spawns});
    const Pick there = pick(crowded, a.rig.options, a.rig.options.aim->x, a.rig.options.aim->y);
    ASSERT_TRUE(there.has_tile());
    ASSERT_FALSE(sim::deploy_site_ok(kSeed, crowded.units(), there.tile_x, there.tile_y));
    a.player.handle(key_frame(input::kKeySpace), crowded, a.rig.options);
    EXPECT_TRUE(a.player.take_orders().empty());
    EXPECT_EQ(a.player.message(), "There is no room to deploy there.");
    EXPECT_TRUE(a.player.deploy_armed());
}

TEST(DeployGesture, TheArmingLapsesOnceTheSelectionIsNoLongerAnUndeployedRecycler) {
    ArmedDeploy a = arm_deploy();
    ASSERT_TRUE(a.ground.has_tile());
    a.player.handle(key_frame(input::kKeySpace), a.rig.sim, a.rig.options);
    for (const sim::Order& order : a.player.take_orders()) {
        a.rig.sim.enqueue(order);
    }
    a.player.handle(digit(3), a.rig.sim, a.rig.options); // armed again, on the way there
    ASSERT_TRUE(a.player.deploy_armed());
    for (int tick = 0; tick < 600 && a.rig.sim.find(a.rig.recycler)->kind == UnitKind::Recycler;
         ++tick) {
        a.rig.sim.step();
    }
    ASSERT_EQ(a.rig.sim.find(a.rig.recycler)->kind, UnitKind::DeployedRecycler);
    a.player.handle(InputFrame{}, a.rig.sim, a.rig.options);
    EXPECT_FALSE(a.player.deploy_armed()) << "a building has nothing left to deploy";
}

TEST(DeployGesture, OnlyTheDrivenRecyclerSelectedIsRefused) {
    MenuRig rig = make_menu_rig();
    Interaction player{rig.recycler}; // driving the recycler
    select_kind(rig, player, UnitKind::Recycler);
    ASSERT_EQ(selected(player), (std::vector<UnitId>{rig.recycler}));
    player.handle(digit(3), rig.sim, rig.options);
    EXPECT_FALSE(player.deploy_armed());
    EXPECT_EQ(player.message(), "You are driving that one; select another unit to give orders.");
}

TEST(MenuDigit, NothingSelectedMeansNoMenuAndNoOrder) {
    MenuRig rig = make_menu_rig();
    Interaction player{rig.tank};
    for (const int key : {1, 4}) {
        player.handle(digit(key), rig.sim, rig.options);
        EXPECT_TRUE(player.take_orders().empty());
        EXPECT_TRUE(player.message().empty());
    }
}

TEST(MenuDigit, AHoldNeverReachesADeadUnit) {
    // Three hostile tanks beside harvester A; harvester B is far away and untouched.
    sim::Sim sim = sim::Sim::create(sim::Scenario{kSeed,
                                                  {{UnitKind::Tank, Side::Player, 40, 40},
                                                   {UnitKind::Scavenger, Side::Player, 20, 20},
                                                   {UnitKind::Scavenger, Side::Player, 100, 100},
                                                   {UnitKind::Tank, Side::Enemy, 20, 21},
                                                   {UnitKind::Tank, Side::Enemy, 21, 20},
                                                   {UnitKind::Tank, Side::Enemy, 19, 20}}});
    UnitId doomed{};
    UnitId safe{};
    for (const sim::Unit& unit : sim.units()) {
        if (unit.kind == UnitKind::Scavenger) {
            (unit.x.floor_int() == 20 ? doomed : safe) = unit.id;
        }
    }
    SceneOptions options{};
    options.seed = kSeed;
    options.extent = kExtent;
    Interaction player{sim.units().front().id};
    // The two harvesters share one key, as a shift-built batch would: no batch exists yet.
    RowState row;
    row.update(sim, Side::Player, 0);
    for (RowSlot& slot : row.slots) {
        std::erase(slot.units, safe);
    }
    for (RowSlot& slot : row.slots) {
        if (!slot.units.empty() && slot.units.front() == doomed) {
            slot.units.push_back(safe);
        }
    }
    options.row = &row;
    const int key = 2; // F2: the tank is F1, the first harvester F2
    player.handle(fkey(key), sim, options);
    ASSERT_EQ(selected(player).size(), 2U);
    int ticks = 0;
    while (ticks < 6000 && sim.find(doomed)->alive) {
        sim.step();
        ++ticks;
    }
    ASSERT_FALSE(sim.find(doomed)->alive) << "the harvester never died, so nothing is proved";
    ASSERT_TRUE(sim.find(safe)->alive);
    player.handle(digit(4), sim, options);
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.at(0).unit, safe);
}

} // namespace
} // namespace protekt::game
