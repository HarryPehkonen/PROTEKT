#include "protekt/game/interaction.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "protekt/game/pick.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::game {
namespace {

using input::Click;
using input::InputFrame;
using input::KeyEvent;
using input::MouseButton;
using sim::Order;
using sim::OrderKind;
using sim::Scenario;
using sim::Side;
using sim::Spawn;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr render::Extent kExtent{1280, 720};

struct Rig {
    sim::Sim sim;
    SceneOptions options;
    sim::UnitId tank_a{}; ///< driven: at tile (40,40)
    sim::UnitId tank_b{}; ///< at tile (42,40)
    sim::UnitId turret{}; ///< at tile (40,42)
    sim::UnitId enemy{};  ///< at tile (40,45)
};

/// A known layout, so a click test knows which pixel belongs to which unit.
///
/// NONE of these may share the camera's x: the camera sits at tile (40,30) looking down +Z, so a
/// unit at x=40 is directly behind whatever is in front of it on that line, and a click on it lands
/// on the near unit instead. The first version of this rig put the turret and the enemy at x=40
/// behind the driven tank, and five tests failed for exactly that reason -- the picker was right.
[[nodiscard]] Rig make_rig() {
    Rig rig{};
    rig.sim = sim::Sim::create(Scenario{kSeed,
                                        {{UnitKind::Tank, Side::Player, 40, 40},
                                         {UnitKind::Tank, Side::Player, 44, 41},
                                         {UnitKind::Turret, Side::Player, 36, 41},
                                         {UnitKind::Tank, Side::Enemy, 38, 44}}});
    for (const sim::Unit& unit : rig.sim.units()) {
        if (unit.kind == UnitKind::Tank && unit.side == Side::Player && unit.x.floor_int() == 40) {
            rig.tank_a = unit.id;
        } else if (unit.kind == UnitKind::Tank && unit.side == Side::Player) {
            rig.tank_b = unit.id;
        } else if (unit.kind == UnitKind::Turret) {
            rig.turret = unit.id;
        } else {
            rig.enemy = unit.id;
        }
    }
    // Place the camera from tank A's tile, so the world position is derived the same way the mesh
    // and the picker derive it.
    const float ground = height_metres(kSeed, 40, 40);
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    rig.options.camera = Camera{80.0F, ground + 3.0F, 60.0F, 0.0F, -0.12F};
    return rig;
}

[[nodiscard]] Click at(const Rig& rig, sim::UnitId unit, MouseButton button = MouseButton::Left,
                       bool shift = false) {
    const sim::Unit* u = rig.sim.find(unit);
    const float x
        = static_cast<float>(u->x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(u->y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = height_metres(kSeed, u->x.floor_int(), u->y.floor_int());
    const Projected p = project(rig.options.camera, kExtent, x, ground + 0.9F, z);
    return Click{static_cast<int>(p.x), static_cast<int>(p.y), button, true, shift};
}

/// A pixel that is definitely ground: low on the screen, well below every unit.
[[nodiscard]] Click ground_click(bool right = false, bool shift = false) {
    return Click{640, 700, right ? MouseButton::Right : MouseButton::Left, true, shift};
}

[[nodiscard]] InputFrame with_click(const Click& click) {
    InputFrame frame{};
    frame.clicks.push_back(click);
    return frame;
}

[[nodiscard]] InputFrame with_key(std::int32_t scancode, bool shift = false) {
    InputFrame frame{};
    frame.keys.push_back(KeyEvent{scancode, true, shift});
    return frame;
}

[[nodiscard]] std::vector<sim::UnitId> selected(const Interaction& interaction) {
    std::vector<sim::UnitId> ids;
    for (const sim::UnitId id : interaction.selection().units()) {
        ids.push_back(id);
    }
    return ids;
}

} // namespace

// ---------------------------------------------------------------- selection

TEST(Interaction, LeftClickOnAnOwnUnitSelectsExactlyIt) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_click(at(rig, rig.tank_a)), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_a}));

    player.handle(with_click(at(rig, rig.tank_b)), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}))
        << "a plain click replaces the selection; it does not add to it";
    EXPECT_TRUE(player.message().empty());
}

TEST(Interaction, ShiftClickAddsAndRemoves) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_click(at(rig, rig.tank_b)), rig.sim, rig.options);
    player.handle(with_click(at(rig, rig.turret, MouseButton::Left, true)), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b, rig.turret}))
        << "the selection is kept in ascending id order, not click order";

    player.handle(with_click(at(rig, rig.tank_b, MouseButton::Left, true)), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.turret}))
        << "shift-click toggles out";
}

TEST(Interaction, LeftClickOnTheGroundClearsTheSelection) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_click(at(rig, rig.tank_b)), rig.sim, rig.options);
    ASSERT_FALSE(player.selection().empty());
    player.handle(with_click(ground_click()), rig.sim, rig.options);
    EXPECT_TRUE(player.selection().empty()) << "the player asked for nothing; give him nothing";
}

TEST(Interaction, LeftClickOnAHostileFiresAtItAndKeepsTheSelection) {
    // The assertion is about the ORDER, and deliberately not about the message: a test that checks
    // "something was refused" keeps passing when the click stops being a refusal.
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_click(at(rig, rig.tank_b)), rig.sim, rig.options);
    player.handle(with_click(at(rig, rig.enemy)), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}))
        << "clicking the enemy must not throw away the player's own selection";

    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1u) << "the trigger orders one unit and nobody else";
    EXPECT_EQ(orders.at(0).kind, sim::OrderKind::FocusFire)
        << "it must not be an Attack: that would set a hunting stance and fight the keyboard";
    EXPECT_EQ(orders.at(0).unit, rig.tank_a)
        << "the unit the player is sitting in is the one firing";
    EXPECT_EQ(orders.at(0).target, rig.enemy);
    EXPECT_FALSE(player.message().empty()) << "the player has to be told what happened";
}

TEST(Interaction, TabCyclesThroughOwnUnitsInAscendingOrderAndWraps) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    std::vector<sim::UnitId> seen;
    for (int i = 0; i < 4; ++i) {
        player.handle(with_key(input::kKeyTab), rig.sim, rig.options);
        ASSERT_EQ(selected(player).size(), 1U);
        seen.push_back(selected(player).front());
    }
    EXPECT_EQ(seen.at(0), rig.tank_a) << "cycling starts at the lowest own id";
    EXPECT_EQ(seen.at(1), rig.tank_b);
    EXPECT_EQ(seen.at(2), rig.turret);
    EXPECT_EQ(seen.at(3), rig.tank_a) << "and wraps back to the beginning";
}

TEST(Interaction, EscapeClearsTheSelection) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_click(at(rig, rig.tank_b)), rig.sim, rig.options);
    ASSERT_FALSE(player.selection().empty());
    player.handle(with_key(input::kKeyEscape), rig.sim, rig.options);
    EXPECT_TRUE(player.selection().empty());
}

// ---------------------------------------------------------------- orders

TEST(Interaction, RightClickOnTheGroundMovesTheSelectionThere) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_click(at(rig, rig.tank_b)), rig.sim, rig.options);
    player.handle(with_click(ground_click(true)), rig.sim, rig.options);

    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().kind, OrderKind::MoveTo);
    EXPECT_EQ(orders.front().unit, rig.tank_b);
    EXPECT_GT(orders.front().y.raw, 0) << "the order carries the tile the click landed on";
}

TEST(Interaction, TheDrivenTankIsNeverOrderedByAClick) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_click(at(rig, rig.tank_b)), rig.sim, rig.options);
    player.handle(with_click(at(rig, rig.tank_a, MouseButton::Left, true)), rig.sim, rig.options);
    ASSERT_EQ(player.selection().size(), 2U);

    player.handle(with_click(ground_click(true)), rig.sim, rig.options);
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U) << "only the unit the player is not sitting in may be ordered";
    EXPECT_EQ(orders.front().unit, rig.tank_b);
}

TEST(Interaction, RightClickWithOnlyTheDrivenTankSelectedIsRefused) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_click(at(rig, rig.tank_a)), rig.sim, rig.options);
    player.handle(with_click(ground_click(true)), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_FALSE(player.message().empty())
        << "the player must be told that he is driving that one, not left guessing";
}

TEST(Interaction, RightClickOnAHostileOrdersAnAttack) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_click(at(rig, rig.tank_b)), rig.sim, rig.options);
    player.handle(with_click(at(rig, rig.enemy, MouseButton::Right)), rig.sim, rig.options);

    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().kind, OrderKind::Attack);
    EXPECT_EQ(orders.front().unit, rig.tank_b);
    EXPECT_EQ(orders.front().target, rig.enemy);
}

TEST(Interaction, RightClickWithNothingSelectedIsRefused) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_click(ground_click(true)), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_FALSE(player.message().empty());
}

TEST(Interaction, OrdersAreDrainedOnce) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_click(at(rig, rig.tank_b)), rig.sim, rig.options);
    player.handle(with_click(ground_click(true)), rig.sim, rig.options);
    EXPECT_EQ(player.take_orders().size(), 1U);
    EXPECT_TRUE(player.take_orders().empty()) << "the same click must not be applied twice";
}

TEST(Interaction, ADeadUnitLeavesTheSelection) {
    // Its own scenario: the turret must actually DIE, so it is surrounded by three enemy tanks. The
    // shared rig cannot promise that, and a test whose precondition might not happen proves
    // nothing.
    sim::Sim sim = sim::Sim::create(Scenario{kSeed,
                                             {{UnitKind::Turret, Side::Player, 40, 42},
                                              {UnitKind::Tank, Side::Enemy, 40, 43},
                                              {UnitKind::Tank, Side::Enemy, 41, 42},
                                              {UnitKind::Tank, Side::Enemy, 39, 42}}});
    sim::UnitId turret{};
    for (const sim::Unit& unit : sim.units()) {
        if (unit.kind == UnitKind::Turret) {
            turret = unit.id;
        }
    }
    ASSERT_TRUE(turret.valid());
    const float ground = height_metres(kSeed, 40, 42);
    SceneOptions options{};
    options.seed = kSeed;
    options.extent = kExtent;
    options.camera = Camera{80.0F, ground + 6.0F, 70.0F, 0.0F, -0.3F};

    Interaction player{sim::UnitId{}};
    const sim::Unit* t = sim.find(turret);
    const Projected p
        = project(options.camera, kExtent, static_cast<float>(t->x.raw) / 65536.0F * kMetresPerTile,
                  ground + 0.9F, static_cast<float>(t->y.raw) / 65536.0F * kMetresPerTile);
    InputFrame frame{};
    frame.clicks.push_back(
        Click{static_cast<int>(p.x), static_cast<int>(p.y), MouseButton::Left, true, false});
    player.handle(frame, sim, options);
    ASSERT_EQ(selected(player), (std::vector<sim::UnitId>{turret}));

    std::uint32_t ticks = 0;
    while (ticks < 6000U && sim.find(turret)->alive) {
        sim.step();
        ++ticks;
    }
    ASSERT_FALSE(sim.find(turret)->alive) << "the turret never died, so nothing is proved";
    player.handle(InputFrame{}, sim, options);
    EXPECT_TRUE(player.selection().empty()) << "a dead unit cannot stay selected";
}

TEST(Interaction, TheSameInputProducesTheSameOrders) {
    const auto play = [] {
        Rig rig = make_rig();
        Interaction player{rig.tank_a};
        player.handle(with_click(at(rig, rig.tank_b)), rig.sim, rig.options);
        player.handle(with_click(at(rig, rig.turret, MouseButton::Left, true)), rig.sim,
                      rig.options);
        player.handle(with_click(ground_click(true)), rig.sim, rig.options);
        return player.take_orders();
    };
    const std::vector<Order> first = play();
    const std::vector<Order> second = play();
    ASSERT_EQ(first.size(), second.size());
    ASSERT_EQ(first.size(), 2U) << "both selected units get the order, in ascending id order";
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(first.at(i).unit, second.at(i).unit);
        EXPECT_EQ(first.at(i).kind, second.at(i).kind);
        EXPECT_EQ(first.at(i).x.raw, second.at(i).x.raw);
        EXPECT_EQ(first.at(i).y.raw, second.at(i).y.raw);
    }
}

} // namespace protekt::game
