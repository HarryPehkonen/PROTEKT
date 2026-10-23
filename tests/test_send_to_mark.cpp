// N3: G arms a go, the next digit 0-9 sends the selection to that nav mark. While armed the digits
// are MARK numbers, not N2's selection keys; the order is the same MoveTo a right click makes.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "protekt/game/interaction.hpp"
#include "protekt/game/nav_marks.hpp"
#include "protekt/game/pick.hpp"
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

/// The digit `n` (0-9) as the window reports it: HID 30 is "1" ... 38 is "9", 39 is "0".
[[nodiscard]] InputFrame digit(int n) {
    return key_frame(n == 0 ? input::kKey0 : input::kKey1 + n - 1);
}

struct Rig {
    sim::Sim sim;
    SceneOptions options;
    NavMarks marks;
    std::vector<UnitId> own;
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
    player.handle(digit(2), rig.sim, rig.options);
    ASSERT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(1)}));
}

/// Mark 0 at (50,50), mark 1 at (52,50), mark 2 at (54,52): numbered by the lowest free slot.
void drop_marks(Rig& rig) {
    rig.marks.press(50, 50);
    rig.marks.press(52, 50);
    rig.marks.press(54, 52);
}

[[nodiscard]] InputFrame right_click() {
    InputFrame frame{};
    frame.clicks.push_back(input::Click{640, 360, input::MouseButton::Right, true, false});
    return frame;
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
    expect_move_to(orders.at(0), rig.own.at(1), 54, 52);
    EXPECT_FALSE(player.go_armed()) << "used up";
}

TEST(SendToMark, ZeroIsAMarkNumberToo) {
    Rig rig = make_rig();
    drop_marks(rig);
    Interaction player{rig.own.at(0)};
    select_second(rig, player);
    player.handle(key_frame(input::kKeyG), rig.sim, rig.options);
    player.handle(digit(0), rig.sim, rig.options);
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    expect_move_to(orders.at(0), rig.own.at(1), 50, 50);
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
    player.handle(digit(3), rig.sim, rig.options); // N2 would select own[2]; mark 3 does not exist
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(1)}));
    // And the very next digit, with the arming used up, is N2's selection key again.
    player.handle(digit(3), rig.sim, rig.options);
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
    player.handle(digit(1), rig.sim, rig.options); // an ordinary roster key again
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(1 - 1)}));
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(SendToMark, AnyOtherKeyDisarmsAndStillDoesItsOwnJob) {
    Rig rig = make_rig();
    drop_marks(rig);
    Interaction player{rig.own.at(0)};
    select_second(rig, player);
    player.handle(key_frame(input::kKeyG), rig.sim, rig.options);
    player.handle(key_frame(input::kKeyQ), rig.sim, rig.options);
    EXPECT_FALSE(player.go_armed());
    EXPECT_EQ(player.weapon(), Weapon::Rocket);
    player.handle(digit(0), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(SendToMark, ARightClickOrderDisarmsTheGo) {
    Rig rig = make_rig();
    drop_marks(rig);
    Interaction player{rig.own.at(0)};
    select_second(rig, player);
    player.handle(key_frame(input::kKeyG), rig.sim, rig.options);
    player.handle(right_click(), rig.sim, rig.options);
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

TEST(SendToMark, TheDrivenUnitIsSkippedAndTheRestStillGoToTheMark) {
    Rig rig = make_rig();
    drop_marks(rig);
    Interaction player{rig.own.at(0)};
    player.handle(digit(1), rig.sim, rig.options); // the driven tank
    aim_at_unit(rig, rig.own.at(1));
    player.handle(key_frame(input::kKeySpace), rig.sim, rig.options); // adds own[1]
    ASSERT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(0), rig.own.at(1)}));
    player.handle(key_frame(input::kKeyG), rig.sim, rig.options);
    player.handle(digit(1), rig.sim, rig.options);
    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    expect_move_to(orders.at(0), rig.own.at(1), 52, 50);
    EXPECT_EQ(player.message(),
              "Sent the rest to the mark; you are driving that one, so it stays put.");
}

TEST(SendToMark, OnlyTheDrivenUnitSelectedIsRefused) {
    Rig rig = make_rig();
    drop_marks(rig);
    Interaction player{rig.own.at(0)};
    player.handle(digit(1), rig.sim, rig.options);
    player.handle(key_frame(input::kKeyG), rig.sim, rig.options);
    player.handle(digit(0), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_EQ(player.message(), "You are driving that one; select another unit to give orders.");
}

TEST(SendToMark, TwoSameSeedRunsProduceByteIdenticalOrders) {
    const auto run = [] {
        Rig rig = make_rig();
        drop_marks(rig);
        Interaction player{rig.own.at(0)};
        std::vector<sim::Order> all;
        for (const InputFrame& frame : {digit(2), key_frame(input::kKeyG), digit(1)}) {
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
    player.handle(digit(1), rig.sim, rig.options); // the driven tank
    aim_at_unit(rig, rig.own.at(1));
    player.handle(key_frame(input::kKeySpace), rig.sim, rig.options); // adds the hover tank
    rig.options.aim = Aim{640.0F, 360.0F};
    ASSERT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(0), rig.own.at(1)}));
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
    ASSERT_EQ(orders.size(), 1U) << "the driven tank is not ordered";
    expect_attack(orders.at(0), rig.own.at(1), kFarTank);
    EXPECT_EQ(player.message(), "attacking the target in row 3.");
    EXPECT_FALSE(player.attack_armed());
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(0), rig.own.at(1)}))
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
    player.handle(digit(1), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_EQ(selected(player), (std::vector<UnitId>{rig.own.at(0)}))
        << "the mode is over: 1 is the roster's key again";
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

TEST(AttackList, ARightClickOrderCancelsIt) {
    Rig rig = make_listing_rig();
    Interaction player{rig.own.at(0)};
    select_second(rig, player);
    player.handle(key_frame(input::kKeyT), rig.sim, rig.options);
    ASSERT_TRUE(player.attack_armed());
    player.handle(right_click(), rig.sim, rig.options);
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
        player.handle(digit(1), rig.sim, rig.options); // the driven tank, alone
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

} // namespace
} // namespace protekt::game
