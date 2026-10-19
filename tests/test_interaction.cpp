#include "protekt/game/interaction.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <variant>
#include <vector>

#include "protekt/game/pick.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/sounds.hpp"
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
    sim::UnitId enemy{};  ///< at tile (38,44)
    sim::UnitId enemy2{}; ///< at tile (46,44): a second target, clear of every other unit's line
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
                                         {UnitKind::Tank, Side::Enemy, 38, 44},
                                         {UnitKind::Tank, Side::Enemy, 46, 44}}});
    for (const sim::Unit& unit : rig.sim.units()) {
        if (unit.kind == UnitKind::Tank && unit.side == Side::Player && unit.x.floor_int() == 40) {
            rig.tank_a = unit.id;
        } else if (unit.kind == UnitKind::Tank && unit.side == Side::Player) {
            rig.tank_b = unit.id;
        } else if (unit.kind == UnitKind::Turret) {
            rig.turret = unit.id;
        } else if (!rig.enemy.valid()) {
            rig.enemy = unit.id;
        } else {
            rig.enemy2 = unit.id;
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

/// A click with the MARK on `unit`.
///
/// Under the aim model a click does not aim: the mark aims, and it is the middle of the screen with
/// the mouse's height. So this moves the mark onto the unit and records a click as well; the claims
/// the tests below make are unchanged (this click selects that unit), and the aim rule itself has
/// its own tests in tests/test_aim.cpp.
[[nodiscard]] Click at(Rig& rig, sim::UnitId unit, MouseButton button = MouseButton::Left,
                       bool shift = false) {
    const sim::Unit* u = rig.sim.find(unit);
    const float x
        = static_cast<float>(u->x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float z
        = static_cast<float>(u->y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    const float ground = height_metres(kSeed, u->x.floor_int(), u->y.floor_int());
    const Projected p = project(rig.options.camera, kExtent, x, ground + 0.9F, z);
    rig.options.aim = Aim{p.x, p.y};
    return Click{static_cast<int>(p.x), static_cast<int>(p.y), button, true, shift};
}

/// The mark on a pixel that is definitely ground: low on the screen, well below every unit.
[[nodiscard]] Click ground_click(Rig& rig, bool right = false, bool shift = false) {
    rig.options.aim = Aim{640.0F, 700.0F};
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

/// Select with the keyboard, which is the only way to select now: the mark on `unit`, then Space.
void choose(Rig& rig, Interaction& player, sim::UnitId unit, bool shift = false) {
    static_cast<void>(at(rig, unit));
    player.handle(with_key(input::kKeySpace, shift), rig.sim, rig.options);
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

TEST(Interaction, SpacePlainReplacesAndShiftSpaceAddsAndRemoves) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_a);
    choose(rig, player, rig.tank_b);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}));
    choose(rig, player, rig.turret, true);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b, rig.turret}));
    choose(rig, player, rig.tank_b, true);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.turret}));
}

// ---------------------------------------------------------------- the trigger

TEST(Trigger, ALeftClickOnAnOwnUnitFiresAtItAndDoesNotChangeTheSelection) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.turret);
    const std::vector<sim::UnitId> before = selected(player);

    player.handle(with_click(at(rig, rig.tank_b)), rig.sim, rig.options);
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.at(0).kind(), OrderKind::FocusFire);
    EXPECT_EQ(orders.at(0).unit, rig.tank_a);
    EXPECT_EQ(std::get<sim::orders::FocusFire>(orders.at(0).payload).target, rig.tank_b);
    EXPECT_EQ(selected(player), before) << "a click is a trigger, not a selection";
}

TEST(Trigger, ALeftClickOnAnUnselectedOwnUnitDoesNotSelectIt) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_click(at(rig, rig.tank_b)), rig.sim, rig.options);
    EXPECT_TRUE(player.selection().empty());
    player.handle(with_click(at(rig, rig.turret, MouseButton::Left, true)), rig.sim, rig.options);
    EXPECT_TRUE(player.selection().empty()) << "Shift does not turn the trigger into a selector";
}

TEST(Trigger, AClickOnGroundOrSkyIssuesNoOrderAndLeavesTheSelection) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    const std::vector<sim::UnitId> before = selected(player);

    player.handle(with_click(ground_click(rig)), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty()) << "ground";
    EXPECT_EQ(selected(player), before);

    for (sim::Tick t = 0; t < kManualShotIntervalTicks; ++t) {
        rig.sim.step();
    }
    rig.options.aim = Aim{640.0F, 5.0F}; // the top of the frame: sky
    player.handle(with_click(Click{640, 5, MouseButton::Left, true, false}), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty()) << "sky";
    EXPECT_EQ(selected(player), before);
}

TEST(Trigger, AClickOnGroundOrSkyIsAShotThePlayerCanHear) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    EXPECT_FALSE(player.take_dry_shot()) << "no click, no shot";
    player.handle(with_click(ground_click(rig)), rig.sim, rig.options);
    EXPECT_TRUE(player.take_dry_shot()) << "no order must not mean no sound";
    EXPECT_FALSE(player.take_dry_shot()) << "drained once";

    const audio::Voice voice = dry_shot_voice();
    EXPECT_EQ(voice.kind, audio::SoundKind::YourShot);
    EXPECT_FLOAT_EQ(voice.gain, 1.0F);
    EXPECT_FLOAT_EQ(voice.pan, 0.0F);
}

TEST(Trigger, AShotAtAUnitIsLeftToTheSimToMakeASoundFor) {
    // The SIM's Fired event is the voice for a shot at a unit; a second one from the game layer
    // would be the same shot heard twice.
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_click(at(rig, rig.enemy)), rig.sim, rig.options);
    EXPECT_FALSE(player.take_dry_shot());
}

TEST(Trigger, TwoClicksInsideOneIntervalAreOneShotAndALaterClickIsAnother) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    InputFrame two{};
    two.clicks.push_back(ground_click(rig));
    two.clicks.push_back(ground_click(rig));
    player.handle(two, rig.sim, rig.options);
    EXPECT_TRUE(player.take_dry_shot());
    EXPECT_FALSE(player.take_dry_shot()) << "the second click in the same frame is swallowed";

    for (sim::Tick t = 0; t + 1 < kManualShotIntervalTicks; ++t) {
        rig.sim.step();
    }
    player.handle(with_click(ground_click(rig)), rig.sim, rig.options);
    EXPECT_FALSE(player.take_dry_shot()) << "one tick short of the interval";

    rig.sim.step();
    player.handle(with_click(ground_click(rig)), rig.sim, rig.options);
    EXPECT_TRUE(player.take_dry_shot()) << "a full interval later the gun is ready";
}

TEST(Trigger, TheTimerLimitsTheButtonForOrdersToo) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    InputFrame two{};
    two.clicks.push_back(at(rig, rig.enemy));
    two.clicks.push_back(at(rig, rig.enemy));
    player.handle(two, rig.sim, rig.options);
    EXPECT_EQ(player.take_orders().size(), 1U);
}

TEST(Trigger, TheTimerDoesNotTouchOtherInput) {
    // A right click or a key right after a shot is not a shot.
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    player.handle(with_click(ground_click(rig)), rig.sim, rig.options);
    static_cast<void>(player.take_dry_shot());
    player.handle(with_click(ground_click(rig, true)), rig.sim, rig.options);
    EXPECT_EQ(player.take_orders().size(), 1U);
}

TEST(Interaction, LeftClickOnAHostileFiresAtItAndKeepsTheSelection) {
    // The assertion is about the ORDER, and deliberately not about the message: a test that checks
    // "something was refused" keeps passing when the click stops being a refusal.
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    player.handle(with_click(at(rig, rig.enemy)), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}))
        << "clicking the enemy must not throw away the player's own selection";

    const std::vector<sim::Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1u) << "the trigger orders one unit and nobody else";
    EXPECT_EQ(orders.at(0).kind(), sim::OrderKind::FocusFire)
        << "it must not be an Attack: that would set a hunting stance and fight the keyboard";
    EXPECT_EQ(orders.at(0).unit, rig.tank_a)
        << "the unit the player is sitting in is the one firing";
    EXPECT_EQ(std::get<sim::orders::FocusFire>(orders.at(0).payload).target, rig.enemy);
    EXPECT_FALSE(player.message().empty()) << "the player has to be told what happened";
}

TEST(Interaction, TabClearsTheSelection) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    choose(rig, player, rig.turret, true);
    ASSERT_EQ(player.selection().size(), 2U);
    player.handle(with_key(input::kKeyTab), rig.sim, rig.options);
    EXPECT_TRUE(player.selection().empty());
}

// ---------------------------------------------------------------- space and T

TEST(Interaction, SpaceSelectsTheUnitUnderTheCrosshairAndShiftSpaceToggles) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    static_cast<void>(at(rig, rig.tank_b));
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}));

    static_cast<void>(at(rig, rig.turret));
    player.handle(with_key(input::kKeySpace, true), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b, rig.turret}));

    player.handle(with_key(input::kKeySpace, true), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}))
        << "shift-space on a selected unit toggles it out";

    static_cast<void>(at(rig, rig.turret));
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.turret}))
        << "plain space replaces the selection, as a plain click does";
    EXPECT_TRUE(player.message().empty());
}

TEST(Interaction, SpaceOverGroundHostileOrNothingLeavesTheSelectionAlone) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    const std::vector<sim::UnitId> before = selected(player);
    ASSERT_EQ(before.size(), 1U);

    static_cast<void>(ground_click(rig));
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
    EXPECT_EQ(selected(player), before) << "ground";
    EXPECT_TRUE(player.message().empty()) << "ignored, not refused";

    static_cast<void>(at(rig, rig.enemy));
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
    EXPECT_EQ(selected(player), before) << "hostile";
    EXPECT_TRUE(player.message().empty());

    rig.options.aim.reset();
    player.handle(with_key(input::kKeySpace), rig.sim, rig.options);
    EXPECT_EQ(selected(player), before) << "no aim point";
    EXPECT_TRUE(player.take_orders().empty());
}

TEST(Interaction, TOrdersAnAttackFromEverySelectedUnitAndKeepsTheSelection) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.turret);
    choose(rig, player, rig.tank_b, true);
    choose(rig, player, rig.tank_a, true);
    const std::vector<sim::UnitId> before = selected(player);
    ASSERT_EQ(before.size(), 3U);

    static_cast<void>(at(rig, rig.enemy));
    player.handle(with_key(input::kKeyT), rig.sim, rig.options);

    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 2U) << "the driven tank is not ordered";
    EXPECT_LT(orders.at(0).unit, orders.at(1).unit) << "ascending id order";
    for (const Order& order : orders) {
        EXPECT_EQ(order.kind(), OrderKind::Attack);
        EXPECT_NE(order.unit, rig.tank_a);
        EXPECT_EQ(std::get<sim::orders::Attack>(order.payload).target, rig.enemy);
    }
    EXPECT_EQ(selected(player), before);
}

TEST(Interaction, TWithNothingSelectedIsRefusedAndSaysWhy) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    static_cast<void>(at(rig, rig.enemy));
    player.handle(with_key(input::kKeyT), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_EQ(player.message(), "Nothing is selected to give an order to.");
}

TEST(Interaction, TOffAHostileIsRefused) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);

    static_cast<void>(ground_click(rig));
    player.handle(with_key(input::kKeyT), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty()) << "ground";
    EXPECT_FALSE(player.message().empty());

    static_cast<void>(at(rig, rig.turret));
    player.handle(with_key(input::kKeyT), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty()) << "own unit";
    EXPECT_FALSE(player.message().empty());
    EXPECT_EQ(selected(player), (std::vector<sim::UnitId>{rig.tank_b}));
}

TEST(Interaction, TWithOnlyTheDrivenTankSelectedIsRefused) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_a);
    static_cast<void>(at(rig, rig.enemy));
    player.handle(with_key(input::kKeyT), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_EQ(player.message(), "You are driving that one; select another unit to give orders.");
}

TEST(Interaction, TRetargetsWithoutReselecting) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    const std::vector<sim::UnitId> before = selected(player);

    static_cast<void>(at(rig, rig.enemy));
    player.handle(with_key(input::kKeyT), rig.sim, rig.options);
    const std::vector<Order> first = player.take_orders();
    ASSERT_EQ(first.size(), 1U);
    EXPECT_EQ(std::get<sim::orders::Attack>(first.front().payload).target, rig.enemy);

    static_cast<void>(at(rig, rig.enemy2));
    player.handle(with_key(input::kKeyT), rig.sim, rig.options);
    const std::vector<Order> second = player.take_orders();
    ASSERT_EQ(second.size(), 1U);
    EXPECT_EQ(second.front().unit, rig.tank_b);
    EXPECT_EQ(std::get<sim::orders::Attack>(second.front().payload).target, rig.enemy2);
    EXPECT_EQ(selected(player), before);
}

TEST(Interaction, EscapeClearsTheSelection) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    ASSERT_FALSE(player.selection().empty());
    player.handle(with_key(input::kKeyEscape), rig.sim, rig.options);
    EXPECT_TRUE(player.selection().empty());
}

// ---------------------------------------------------------------- orders

TEST(Interaction, RightClickOnTheGroundMovesTheSelectionThere) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    player.handle(with_click(ground_click(rig, true)), rig.sim, rig.options);

    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().kind(), OrderKind::MoveTo);
    EXPECT_EQ(orders.front().unit, rig.tank_b);
    EXPECT_GT(std::get<sim::orders::MoveTo>(orders.front().payload).y.raw, 0)
        << "the order carries the tile the click landed on";
}

TEST(Interaction, TheDrivenTankIsNeverOrderedByAClick) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    choose(rig, player, rig.tank_a, true);
    ASSERT_EQ(player.selection().size(), 2U);

    player.handle(with_click(ground_click(rig, true)), rig.sim, rig.options);
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U) << "only the unit the player is not sitting in may be ordered";
    EXPECT_EQ(orders.front().unit, rig.tank_b);
}

TEST(Interaction, RightClickWithOnlyTheDrivenTankSelectedIsRefused) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_a);
    player.handle(with_click(ground_click(rig, true)), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_FALSE(player.message().empty())
        << "the player must be told that he is driving that one, not left guessing";
}

TEST(Interaction, RightClickOnAHostileOrdersAnAttack) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    player.handle(with_click(at(rig, rig.enemy, MouseButton::Right)), rig.sim, rig.options);

    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().kind(), OrderKind::Attack);
    EXPECT_EQ(orders.front().unit, rig.tank_b);
    EXPECT_EQ(std::get<sim::orders::Attack>(orders.front().payload).target, rig.enemy);
}

TEST(Interaction, RightClickWithNothingSelectedIsRefused) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    player.handle(with_click(ground_click(rig, true)), rig.sim, rig.options);
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_FALSE(player.message().empty());
}

TEST(Interaction, OrdersAreDrainedOnce) {
    Rig rig = make_rig();
    Interaction player{rig.tank_a};
    choose(rig, player, rig.tank_b);
    player.handle(with_click(ground_click(rig, true)), rig.sim, rig.options);
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
    options.aim = Aim{p.x, p.y}; // the MARK selects, so the mark goes on the turret
    const InputFrame frame = with_key(input::kKeySpace);
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
        choose(rig, player, rig.tank_b);
        choose(rig, player, rig.turret, true);
        player.handle(with_click(ground_click(rig, true)), rig.sim, rig.options);
        return player.take_orders();
    };
    const std::vector<Order> first = play();
    const std::vector<Order> second = play();
    ASSERT_EQ(first.size(), second.size());
    ASSERT_EQ(first.size(), 2U) << "both selected units get the order, in ascending id order";
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(first.at(i).unit, second.at(i).unit);
        EXPECT_EQ(first.at(i).kind(), second.at(i).kind());
        EXPECT_EQ(std::get<sim::orders::MoveTo>(first.at(i).payload).x.raw,
                  std::get<sim::orders::MoveTo>(second.at(i).payload).x.raw);
        EXPECT_EQ(std::get<sim::orders::MoveTo>(first.at(i).payload).y.raw,
                  std::get<sim::orders::MoveTo>(second.at(i).payload).y.raw);
    }
}

TEST(Interaction, AClickAimsWithTheMarkAndNotItsOwnPixel) {
    // The mark is the gun. A click's own pixel is only where the pointer happened to be: it decides
    // nothing, or the mark could say one thing while the click did another.
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    static_cast<void>(at(rig, rig.enemy)); // puts the MARK on the enemy
    ASSERT_TRUE(rig.options.aim.has_value());
    const Click elsewhere{5, 700, MouseButton::Left, true, false}; // no unit is behind that pixel
    player.handle(with_click(elsewhere), rig.sim, rig.options);
    const std::vector<Order> orders = player.take_orders();
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(std::get<sim::orders::FocusFire>(orders.at(0).payload).target, rig.enemy);
}

TEST(Interaction, AClickWithNoMarkDoesNothing) {
    // No pointer has ever been seen, so there is no mark and nothing a click could mean. This is
    // what keeps a headless or scripted frame from selecting anything by accident.
    Rig rig = make_rig();
    Interaction player(rig.tank_a);
    ASSERT_FALSE(rig.options.aim.has_value());
    const Click click{640, 360, MouseButton::Left, true, false};
    player.handle(with_click(click), rig.sim, rig.options);
    EXPECT_TRUE(player.selection().empty());
    EXPECT_TRUE(player.take_orders().empty());
    EXPECT_FALSE(player.take_dry_shot()) << "and it makes no sound either";
}

} // namespace protekt::game
