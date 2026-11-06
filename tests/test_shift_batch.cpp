// THE SHIFT-BATCH (Harri, round 10, 2026-11-05):
//   "The batch build sequence should only start after I let go of the shift key. If I hit tab
//    before letting go of shift, the whole build is cancelled completely -- nothing will be built
//    unless I redo. If I'm in the middle of a batch build, I can select the recycler or whatever
//    is doing the build and hit shift-tab to cancel the REST of the build. What was already built
//    will remain."
// Three rules that interact, tested together. The key's STATE is what is under test, so every
// frame here is spelt the way the window spells it: Shift is a key event of its own, going down
// and going up, and a press carries the modifier flag it was made under.
#include <gtest/gtest.h>

#include <cstdint>
#include <variant>
#include <vector>

#include "protekt/game/interaction.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/input/bindings.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"

namespace protekt::game {
namespace {

using input::InputFrame;
using input::KeyEvent;
using sim::Order;
using sim::Scenario;
using sim::Side;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr Extent kExtent{1280, 720};

[[nodiscard]] KeyEvent down(std::int32_t scancode, bool shift) {
    return KeyEvent{scancode, true, shift};
}
[[nodiscard]] KeyEvent up(std::int32_t scancode, bool shift) {
    return KeyEvent{scancode, false, shift};
}

[[nodiscard]] InputFrame frame_of(std::vector<KeyEvent> keys) {
    InputFrame frame{};
    frame.keys = std::move(keys);
    return frame;
}

/// A producer to build from and a tank to drive, nothing else; the producer is brought up with the
/// top menu's own key, as the player does. `funded` adds haulers and wreckage so a queue can pay.
struct Rig {
    sim::Sim sim;
    SceneOptions options;
    sim::UnitId producer;
    Interaction player;
};

[[nodiscard]] Rig make_rig(UnitKind producer_kind, bool funded) {
    Scenario scenario{
        kSeed, {{UnitKind::Tank, Side::Player, 40, 40}, {producer_kind, Side::Player, 46, 41}}};
    if (funded) {
        scenario.spawns.push_back({UnitKind::Scavenger, Side::Player, 48, 47});
        scenario.spawns.push_back({UnitKind::Scavenger, Side::Player, 48, 48});
        scenario.spawns.push_back({UnitKind::Scavenger, Side::Player, 48, 49});
        scenario.objectives = {};
        scenario.store_capacity = 400;
        const sim::Fixed half = sim::Fixed::from_raw(sim::Fixed::kOne / 2);
        for (int i = 0; i < 90; ++i) {
            scenario.scrap.push_back(
                sim::Scrap{sim::fixed_add(sim::Fixed::from_int(42 + (i % 10)), half),
                           sim::fixed_add(sim::Fixed::from_int(36 + (i / 10)), half)});
        }
    }
    sim::Sim sim = sim::Sim::create(scenario);
    const sim::UnitId tank = sim.units().at(0).id;
    const sim::UnitId producer = sim.units().at(1).id;
    Rig rig{std::move(sim), SceneOptions{}, producer, Interaction{tank}};
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    // `1` is the deployed recycler, `2` the factory.
    rig.player.handle(
        frame_of({down(input::kKey1 + (producer_kind == UnitKind::Factory ? 1 : 0), false)}),
        rig.sim, rig.options);
    return rig;
}

/// One frame of the app: the keys are handled, the orders go to the sim, the sim steps. Returns the
/// orders that frame made.
std::vector<Order> frame(Rig& rig, std::vector<KeyEvent> keys) {
    rig.player.handle(frame_of(std::move(keys)), rig.sim, rig.options);
    std::vector<Order> orders = rig.player.take_orders();
    for (const Order& order : orders) {
        rig.sim.enqueue(order);
    }
    rig.sim.step();
    return orders;
}

[[nodiscard]] std::size_t builds_in(const std::vector<Order>& orders) {
    std::size_t builds = 0;
    for (const Order& order : orders) {
        builds += order.kind() == sim::OrderKind::Build ? 1 : 0;
    }
    return builds;
}

[[nodiscard]] std::size_t cancels_in(const std::vector<Order>& orders) {
    std::size_t cancels = 0;
    for (const Order& order : orders) {
        cancels += order.kind() == sim::OrderKind::CancelBuild ? 1 : 0;
    }
    return cancels;
}

[[nodiscard]] std::size_t queued(const Rig& rig) { return rig.sim.find(rig.producer)->queue.count; }

[[nodiscard]] int player_tanks(const sim::Sim& sim) {
    int tanks = 0;
    for (const sim::Unit& unit : sim.units()) {
        tanks += unit.alive && unit.side == Side::Player && unit.kind == UnitKind::Tank ? 1 : 0;
    }
    return tanks;
}

constexpr std::int32_t kRow1 = input::kKey1;     ///< the recycler's first row: a harvester
constexpr std::int32_t kRow3 = input::kKey1 + 2; ///< the factory's third row: the tank

} // namespace

// ---- RULE 1: the batch begins when shift goes up
// -------------------------------------------------

TEST(ShiftBatch, PressesWhileShiftIsHeldOrderNothingAndLettingGoOrdersThemAll) {
    Rig rig = make_rig(UnitKind::DeployedRecycler, false);
    EXPECT_TRUE(frame(rig, {down(input::kKeyLShift, true)}).empty());
    EXPECT_TRUE(frame(rig, {down(kRow1, true)}).empty()) << "held: not ordered yet";
    EXPECT_TRUE(frame(rig, {down(kRow1, true)}).empty());
    EXPECT_TRUE(frame(rig, {down(kRow1, true)}).empty());
    EXPECT_EQ(queued(rig), 0U) << "the producer has not been told anything while shift is down";

    const std::vector<Order> orders = frame(rig, {up(input::kKeyLShift, false)});
    ASSERT_EQ(builds_in(orders), 3U) << "letting go starts the batch, all of it";
    const auto* first = std::get_if<sim::orders::Build>(&orders.at(0).payload);
    ASSERT_NE(first, nullptr);
    EXPECT_NE(first->batch, 0U);
    for (const Order& order : orders) {
        EXPECT_EQ(std::get<sim::orders::Build>(order.payload).batch, first->batch)
            << "one gesture, one token";
    }
    EXPECT_EQ(queued(rig), 3U);
}

TEST(ShiftBatch, TheRightShiftKeyLetsGoToo) {
    Rig rig = make_rig(UnitKind::DeployedRecycler, false);
    EXPECT_TRUE(frame(rig, {down(input::kKeyRShift, true), down(kRow1, true)}).empty());
    EXPECT_EQ(builds_in(frame(rig, {up(input::kKeyRShift, false)})), 1U);
}

TEST(ShiftBatch, AKeyLeavingWhileShiftIsStillDownDoesNotStartTheBatch) {
    // The release that matters is SHIFT's. Another key going up -- the digit just pressed, or a
    // driving key -- must not be read as "shift is up".
    Rig rig = make_rig(UnitKind::DeployedRecycler, false);
    EXPECT_TRUE(frame(rig, {down(input::kKeyLShift, true), down(kRow1, true)}).empty());
    EXPECT_TRUE(frame(rig, {up(kRow1, true)}).empty()) << "the digit key coming up";
    EXPECT_TRUE(frame(rig, {up(input::kKeyW, true)}).empty()) << "an unrelated key coming up";
    EXPECT_TRUE(frame(rig, {up(input::kKeyRShift, true)}).empty())
        << "the OTHER shift key coming up while the left is still down is not let-go either";
    EXPECT_EQ(queued(rig), 0U);
    EXPECT_EQ(builds_in(frame(rig, {up(input::kKeyLShift, false)})), 1U);
}

TEST(ShiftBatch, ShiftReleasedAndPressedAgainBeginsAFreshHold) {
    // Down, press, up: ordered. Then down and a press again: held once more, not leaked.
    Rig rig = make_rig(UnitKind::DeployedRecycler, false);
    frame(rig, {down(input::kKeyLShift, true), down(kRow1, true)});
    ASSERT_EQ(builds_in(frame(rig, {up(input::kKeyLShift, false)})), 1U);
    ASSERT_TRUE(frame(rig, {down(input::kKeyLShift, true)}).empty());
    EXPECT_TRUE(frame(rig, {down(kRow1, true)}).empty()) << "refused as busy, or held; never sent";
    EXPECT_EQ(queued(rig), 1U);
}

TEST(ShiftBatch, AKeyPressedWithoutShiftMeansShiftIsAlreadyUp) {
    // A release the window never saw (focus lost with shift down) must not hold the batch for ever:
    // the next press made with no shift modifier proves shift is up, and the batch begins.
    Rig rig = make_rig(UnitKind::DeployedRecycler, false);
    frame(rig, {down(input::kKeyLShift, true), down(kRow1, true), down(kRow1, true)});
    ASSERT_EQ(queued(rig), 0U);
    const std::vector<Order> orders = frame(rig, {down(input::kKeyW, false)});
    EXPECT_EQ(builds_in(orders), 2U);
}

TEST(ShiftBatch, AShiftedPressWithNoShiftKeyEventIsStillHeldUntilARelease) {
    // The tests and the replays of old drove the digit with the flag alone: the flag says shift is
    // down, so the press is held, and a pressed key without the flag is what lets it go.
    Rig rig = make_rig(UnitKind::DeployedRecycler, false);
    EXPECT_TRUE(frame(rig, {down(kRow1, true)}).empty());
    EXPECT_EQ(builds_in(frame(rig, {down(input::kKeyW, false)})), 1U);
}

TEST(ShiftBatch, ABarePressStillOrdersAtOnce) {
    Rig rig = make_rig(UnitKind::DeployedRecycler, false);
    EXPECT_EQ(builds_in(frame(rig, {down(kRow1, false)})), 1U) << "no shift, no waiting";
}

TEST(ShiftBatch, HeldPressesCountAgainstTheQueueCapAndTheKindRule) {
    Rig rig = make_rig(UnitKind::DeployedRecycler, false);
    std::vector<KeyEvent> keys{down(input::kKeyLShift, true)};
    for (std::size_t n = 0; n < sim::kBuildQueueDepth + 1; ++n) {
        keys.push_back(down(kRow1, true));
    }
    keys.push_back(down(kRow1 + 1, true)); // another kind into the same run
    EXPECT_TRUE(frame(rig, std::move(keys)).empty());
    EXPECT_FALSE(rig.player.message().empty());
    EXPECT_EQ(builds_in(frame(rig, {up(input::kKeyLShift, false)})), sim::kBuildQueueDepth)
        << "ten held, the eleventh and the other kind refused when pressed";
}

// ---- RULE 2: TAB before letting go cancels it all -----------------------------------------------

TEST(ShiftBatch, TabBeforeLettingGoCancelsTheWholeBatchAndNothingIsBuilt) {
    Rig rig = make_rig(UnitKind::DeployedRecycler, false);
    frame(rig,
          {down(input::kKeyLShift, true), down(kRow1, true), down(kRow1, true), down(kRow1, true)});
    const std::vector<Order> at_tab = frame(rig, {down(input::kKeyTab, true)});
    EXPECT_EQ(builds_in(at_tab), 0U);
    EXPECT_EQ(cancels_in(at_tab), 0U) << "nothing had been sent, so there is nothing to cancel";
    const std::vector<Order> at_release = frame(rig, {up(input::kKeyLShift, false)});
    EXPECT_TRUE(at_release.empty()) << "the release finds nothing held";
    for (int tick = 0; tick < 5; ++tick) {
        rig.sim.step();
    }
    EXPECT_EQ(queued(rig), 0U);
    EXPECT_TRUE(rig.sim.command_log().empty()) << "the sim never heard of this batch";
}

TEST(ShiftBatch, TabInTheSameFrameAsThePressesCancelsThemToo) {
    Rig rig = make_rig(UnitKind::DeployedRecycler, false);
    const std::vector<Order> orders
        = frame(rig, {down(input::kKeyLShift, true), down(kRow1, true), down(kRow1, true),
                      down(input::kKeyTab, true), up(input::kKeyLShift, false)});
    EXPECT_TRUE(orders.empty());
    EXPECT_EQ(queued(rig), 0U);
}

TEST(ShiftBatch, TabAfterLettingGoCancelsNothing) {
    // Order matters within one frame: [shift up, Tab] lets go FIRST, so the batch has begun and a
    // bare Tab is only the selection clear it always was.
    Rig rig = make_rig(UnitKind::DeployedRecycler, false);
    const std::vector<Order> orders
        = frame(rig, {down(input::kKeyLShift, true), down(kRow1, true), down(kRow1, true),
                      up(input::kKeyLShift, false), down(input::kKeyTab, false)});
    EXPECT_EQ(builds_in(orders), 2U);
    EXPECT_EQ(cancels_in(orders), 0U);
    EXPECT_EQ(queued(rig), 2U);
    EXPECT_TRUE(rig.player.selection().empty()) << "and Tab cleared the selection as ever";
}

TEST(ShiftBatch, AfterACancelTheBatchCanBeRedone) {
    Rig rig = make_rig(UnitKind::DeployedRecycler, false);
    frame(rig, {down(input::kKeyLShift, true), down(kRow1, true), down(input::kKeyTab, true),
                up(input::kKeyLShift, false)});
    ASSERT_EQ(queued(rig), 0U);
    frame(rig, {down(input::kKey1, false)}); // bring the recycler up again
    const std::vector<Order> orders = frame(rig, {down(input::kKeyLShift, true), down(kRow1, true),
                                                  down(kRow1, true), up(input::kKeyLShift, false)});
    EXPECT_EQ(builds_in(orders), 2U) << "nothing of the cancelled batch is left to leak in";
}

// ---- RULE 3: SHIFT-TAB on the builder cancels the rest ------------------------------------------

TEST(ShiftBatch, ShiftTabOnTheBuilderMidBatchCancelsTheRestAndWhatIsBuiltStays) {
    Rig rig = make_rig(UnitKind::Factory, true);
    // Let the haulers bank enough for the first tank, then queue four.
    for (int tick = 0;
         tick < 20000 && rig.sim.ore(Side::Player) < sim::spec_of(UnitKind::Tank).cost; ++tick) {
        rig.sim.step();
    }
    ASSERT_GE(rig.sim.ore(Side::Player), sim::spec_of(UnitKind::Tank).cost);
    const int before = player_tanks(rig.sim);
    frame(rig, {down(input::kKeyLShift, true), down(kRow3, true), down(kRow3, true),
                down(kRow3, true), down(kRow3, true), up(input::kKeyLShift, false)});
    ASSERT_EQ(queued(rig), 4U);
    // Run until the first tank has arrived: three are left.
    for (int tick = 0; tick < 20000 && queued(rig) == 4U; ++tick) {
        rig.sim.step();
    }
    ASSERT_EQ(queued(rig), 3U);
    ASSERT_EQ(player_tanks(rig.sim), before + 1);
    ASSERT_EQ(rig.player.selection().units().size(), 1U) << "the factory is still selected";

    const std::vector<Order> orders = frame(rig, {down(input::kKeyTab, true)});
    ASSERT_EQ(cancels_in(orders), 1U);
    EXPECT_EQ(orders.at(0).unit, rig.producer);
    EXPECT_EQ(queued(rig), 0U) << "the rest is cancelled";
    for (int tick = 0; tick < 2000; ++tick) {
        rig.sim.step();
    }
    EXPECT_EQ(player_tanks(rig.sim), before + 1) << "what was built stays, and no more come";
}

TEST(ShiftBatch, PlainTabMidBatchDoesNotCancelTheRest) {
    // The modifier is what distinguishes the two: Tab alone only clears the selection.
    Rig rig = make_rig(UnitKind::DeployedRecycler, false);
    frame(rig, {down(input::kKeyLShift, true), down(kRow1, true), down(kRow1, true),
                up(input::kKeyLShift, false)});
    ASSERT_EQ(queued(rig), 2U);
    const std::vector<Order> orders = frame(rig, {down(input::kKeyTab, false)});
    EXPECT_EQ(cancels_in(orders), 0U);
    EXPECT_EQ(queued(rig), 2U);
}

TEST(ShiftBatch, ShiftTabWithNothingQueuedOrdersNothing) {
    Rig rig = make_rig(UnitKind::DeployedRecycler, false);
    EXPECT_TRUE(frame(rig, {down(input::kKeyTab, true)}).empty());
}

TEST(ShiftBatch, ShiftTabCancelsTheSelectedBuilderAndNotAnotherOne) {
    Scenario scenario{kSeed,
                      {{UnitKind::Tank, Side::Player, 40, 40},
                       {UnitKind::DeployedRecycler, Side::Player, 46, 41},
                       {UnitKind::Factory, Side::Player, 50, 45}}};
    sim::Sim sim = sim::Sim::create(scenario);
    const sim::UnitId recycler = sim.units().at(1).id;
    const sim::UnitId factory = sim.units().at(2).id;
    Rig rig{std::move(sim), SceneOptions{}, recycler, Interaction{sim::UnitId{1}}};
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    // Both start a batch of their own.
    frame(rig, {down(input::kKey1, false)}); // the recycler
    frame(rig, {down(kRow1, false)});
    frame(rig, {down(input::kKeyEscape, false)});
    frame(rig, {down(input::kKey1 + 1, false)}); // the factory
    frame(rig, {down(kRow3, false)});
    ASSERT_EQ(rig.sim.find(recycler)->queue.count, 1U);
    ASSERT_EQ(rig.sim.find(factory)->queue.count, 1U);
    // The factory is selected: it alone loses its queue.
    const std::vector<Order> orders = frame(rig, {down(input::kKeyTab, true)});
    ASSERT_EQ(cancels_in(orders), 1U);
    EXPECT_EQ(orders.at(0).unit, factory);
    EXPECT_EQ(rig.sim.find(factory)->queue.count, 0U);
    EXPECT_EQ(rig.sim.find(recycler)->queue.count, 1U);
}

// ---- THE SIM HALF: CancelBuild
// --------------------------------------------------------------------

TEST(CancelBuild, EmptiesTheQueueAndGivesBackWhatTheCurrentEntryPaid) {
    Rig rig = make_rig(UnitKind::Factory, true);
    const std::uint16_t cost = sim::spec_of(UnitKind::Tank).cost;
    for (int tick = 0; tick < 20000 && rig.sim.ore(Side::Player) < cost; ++tick) {
        rig.sim.step();
    }
    ASSERT_GE(rig.sim.ore(Side::Player), cost);
    rig.sim.enqueue(Order{rig.producer, sim::orders::Build{UnitKind::Tank, 7}});
    rig.sim.enqueue(Order{rig.producer, sim::orders::Build{UnitKind::Tank, 7}});
    rig.sim.step();
    ASSERT_EQ(queued(rig), 2U);
    ASSERT_GT(rig.sim.find(rig.producer)->queue.left, 0) << "the front entry has paid and started";
    const std::uint16_t paid_up = rig.sim.ore(Side::Player);
    const int tanks = player_tanks(rig.sim);

    rig.sim.enqueue(Order{rig.producer, sim::orders::CancelBuild{}});
    rig.sim.step();
    EXPECT_EQ(queued(rig), 0U);
    EXPECT_EQ(rig.sim.find(rig.producer)->queue.left, 0U);
    EXPECT_GE(rig.sim.ore(Side::Player), paid_up + cost)
        << "the cost the cancelled entry had paid comes back (the haulers may add more)";
    for (int tick = 0; tick < 1500; ++tick) {
        rig.sim.step();
    }
    EXPECT_EQ(player_tanks(rig.sim), tanks) << "a cancelled build never arrives";
}

TEST(CancelBuild, AnUnpaidFrontEntryRefundsNothing) {
    Rig rig = make_rig(UnitKind::Factory, false); // the bank is empty: the entry waits for scrap
    rig.sim.enqueue(Order{rig.producer, sim::orders::Build{UnitKind::Tank, 0}});
    rig.sim.step();
    ASSERT_EQ(queued(rig), 1U);
    ASSERT_EQ(rig.sim.find(rig.producer)->queue.left, 0U);
    rig.sim.enqueue(Order{rig.producer, sim::orders::CancelBuild{}});
    rig.sim.step();
    EXPECT_EQ(queued(rig), 0U);
    EXPECT_EQ(rig.sim.ore(Side::Player), 0U) << "nothing was paid, so nothing is returned";
}

TEST(CancelBuild, TheProducerTakesANewBatchAfterwards) {
    Rig rig = make_rig(UnitKind::Factory, false);
    rig.sim.enqueue(Order{rig.producer, sim::orders::Build{UnitKind::Tank, 3}});
    rig.sim.step();
    rig.sim.enqueue(Order{rig.producer, sim::orders::CancelBuild{}});
    rig.sim.step();
    rig.sim.enqueue(Order{rig.producer, sim::orders::Build{UnitKind::Scout, 4}});
    rig.sim.step();
    ASSERT_EQ(queued(rig), 1U);
    EXPECT_EQ(rig.sim.find(rig.producer)->queue.entries.at(0).kind, UnitKind::Scout);
}

TEST(CancelBuild, OnlyAProducerTakesIt) {
    Rig rig = make_rig(UnitKind::Factory, false);
    const sim::UnitId tank = rig.sim.units().at(0).id;
    const std::size_t logged = rig.sim.command_log().size();
    rig.sim.enqueue(Order{tank, sim::orders::CancelBuild{}});
    EXPECT_EQ(rig.sim.command_log().size(), logged) << "a tank has no queue to cancel: not logged";
    rig.sim.enqueue(Order{rig.producer, sim::orders::CancelBuild{}});
    EXPECT_EQ(rig.sim.command_log().size(), logged + 1);
}

} // namespace protekt::game
