// Standing controllers: a policy, a roster, a cadence -- and the orders they emit.
#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "protekt/ai/commander.hpp"
#include "protekt/cmd/controller.hpp"
#include "protekt/sim/fixed.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/world/world.hpp"

namespace {

using protekt::ai::Observation;
using protekt::ai::UnitView;
using protekt::cmd::Controller;
using protekt::cmd::ControllerSet;
using protekt::cmd::kDenyRadiusTiles;
using protekt::cmd::Policy;
using protekt::cmd::PolicyKind;
using protekt::sim::Fixed;
using protekt::sim::Order;
using protekt::sim::OrderKind;
using protekt::sim::Side;
using protekt::sim::UnitId;
using protekt::sim::UnitKind;

constexpr std::uint64_t kSeed = 42;

[[nodiscard]] UnitId id(std::uint32_t value) { return UnitId{value}; }
[[nodiscard]] Fixed tiles(int value) { return Fixed::from_int(value); }

[[nodiscard]] UnitView view(std::uint32_t value, Side side, int x, int y,
                            UnitKind kind = UnitKind::Turret) {
    return UnitView{id(value), kind, side, x, y, 100};
}

[[nodiscard]] Observation observation_of(std::vector<UnitView> own,
                                         std::vector<UnitView> enemy = {}) {
    Observation observation;
    observation.own = std::move(own);
    observation.enemy = std::move(enemy);
    return observation;
}

/// Adds a controller to a set and insists that nothing was taken away from it: the tests that
/// care about a partial takeover call `add` directly and check the returned ids.
void add_whole(ControllerSet& set, const Controller& controller) {
    EXPECT_TRUE(set.add(controller).empty()) << "no unit should have been taken away";
}

[[nodiscard]] std::vector<std::uint32_t> values_of(std::span<const UnitId> ids) {
    std::vector<std::uint32_t> values;
    values.reserve(ids.size());
    for (const UnitId id : ids) {
        values.push_back(id.value);
    }
    return values;
}

[[nodiscard]] std::vector<std::uint32_t> ordered_units(const std::vector<Order>& orders) {
    std::vector<std::uint32_t> values;
    values.reserve(orders.size());
    for (const Order& order : orders) {
        values.push_back(order.unit.value);
    }
    return values;
}

[[nodiscard]] Policy policy_of(PolicyKind kind, int x, int y, std::uint32_t cadence = 15) {
    Policy policy;
    policy.kind = kind;
    policy.x = tiles(x);
    policy.y = tiles(y);
    policy.cadence_ticks = cadence;
    return policy;
}

/// `Controller::create` returns an optional, and clang-tidy cannot see that an `ASSERT_TRUE` above
/// already guarded the dereference -- but it CAN see a check, so the unwrapping lives in one place.
/// A refusal becomes a failure of THIS test with a reason, not a crash of the suite.
[[nodiscard]] Controller must(std::optional<Controller> controller) {
    if (!controller.has_value()) {
        throw std::runtime_error("Controller::create refused a policy this test expects to accept");
    }
    return *controller;
}

TEST(Controller, CreateRefusesAZeroCadence) {
    EXPECT_FALSE(Controller::create(policy_of(PolicyKind::Hold, 10, 10, 0), {id(1)}).has_value())
        << "a controller that thinks every tick is a bug at 30 Hz";
}

TEST(Controller, CreateRefusesAnEmptyRoster) {
    EXPECT_FALSE(Controller::create(policy_of(PolicyKind::Hold, 10, 10), {}).has_value())
        << "a controller with nothing to command is a configuration mistake";
}

TEST(Controller, CreateKeepsTheRosterAscending) {
    Controller controller
        = must(Controller::create(policy_of(PolicyKind::Hold, 10, 10), {id(9), id(2), id(5)}));
    const std::vector<std::uint32_t> expected{2, 5, 9};
    EXPECT_EQ(values_of(controller.roster()), expected);
}

TEST(Controller, DueOnItsCadenceAndNeverOnTickZero) {
    Controller controller
        = must(Controller::create(policy_of(PolicyKind::Hold, 10, 10, 15), {id(1)}));
    EXPECT_FALSE(controller.due(0)) << "tick 0 is the deployment tick: the scenario placed things";
    EXPECT_FALSE(controller.due(1));
    EXPECT_FALSE(controller.due(14));
    EXPECT_TRUE(controller.due(15));
    EXPECT_FALSE(controller.due(16));
    EXPECT_TRUE(controller.due(30));
}

TEST(Controller, ThinkDoesNothingWhenNotDue) {
    Controller controller
        = must(Controller::create(policy_of(PolicyKind::Hold, 10, 10, 15), {id(1)}));
    std::vector<Order> orders;
    controller.think(observation_of({view(1, Side::Player, 10, 10)}), 7, orders);
    EXPECT_TRUE(orders.empty()) << "not due is not a licence to think quietly";
}

TEST(Controller, HoldGuardsThePointWithEveryLivingUnit) {
    Controller controller
        = must(Controller::create(policy_of(PolicyKind::Hold, 20, 30), {id(1), id(2), id(3)}));
    std::vector<Order> orders;
    controller.think(observation_of({view(1, Side::Player, 5, 5), view(2, Side::Player, 6, 6),
                                     view(3, Side::Player, 7, 7)}),
                     15, orders);
    ASSERT_EQ(orders.size(), 3U);
    EXPECT_EQ(ordered_units(orders), (std::vector<std::uint32_t>{1, 2, 3}));
    for (const Order& order : orders) {
        EXPECT_EQ(order.kind, OrderKind::GuardAt);
        EXPECT_EQ(order.x.raw, tiles(20).raw);
        EXPECT_EQ(order.y.raw, tiles(30).raw);
    }
}

TEST(Controller, DenySendsTheFarUnitsToThePointAndHoldsTheNearOnes) {
    Controller controller
        = must(Controller::create(policy_of(PolicyKind::Deny, 50, 50), {id(1), id(2)}));
    // Unit 1 is 20 tiles away -- outside the deny radius, so it has to come. Unit 2 is 2 tiles
    // away, already on the line, and holds the tile it is standing on.
    std::vector<Order> orders;
    controller.think(observation_of({view(1, Side::Player, 50, 70), view(2, Side::Player, 52, 50)}),
                     15, orders);
    ASSERT_EQ(orders.size(), 2U);
    EXPECT_EQ(orders[0].unit.value, 1U);
    EXPECT_EQ(orders[0].kind, OrderKind::MoveTo) << "a unit outside the radius must close the gap";
    EXPECT_EQ(orders[0].x.raw, tiles(50).raw);
    EXPECT_EQ(orders[0].y.raw, tiles(50).raw);
    EXPECT_EQ(orders[1].unit.value, 2U);
    EXPECT_EQ(orders[1].kind, OrderKind::GuardAt) << "a unit on the line holds where it stands";
    EXPECT_EQ(orders[1].x.raw, tiles(52).raw) << "and holds ITS OWN tile, not the policy point";
    EXPECT_EQ(orders[1].y.raw, tiles(50).raw);
}

TEST(Controller, DenyPutsTheBoundaryExactlyAtTheRadius) {
    // The radius is balance, so it lives in the contract and the boundary is asserted, not assumed.
    Controller controller
        = must(Controller::create(policy_of(PolicyKind::Deny, 50, 50), {id(1), id(2)}));
    std::vector<Order> orders;
    controller.think(observation_of({view(1, Side::Player, 50, 50 + kDenyRadiusTiles),
                                     view(2, Side::Player, 50, 50 + kDenyRadiusTiles + 1)}),
                     15, orders);
    ASSERT_EQ(orders.size(), 2U);
    EXPECT_EQ(orders[0].kind, OrderKind::GuardAt) << "exactly on the radius is on the line";
    EXPECT_EQ(orders[1].kind, OrderKind::MoveTo) << "one tile past it is not";
}

TEST(Controller, FocusAttacksTheEnemyNearestThePoint) {
    Controller controller
        = must(Controller::create(policy_of(PolicyKind::Focus, 40, 40), {id(1), id(2)}));
    std::vector<Order> orders;
    controller.think(observation_of({view(1, Side::Player, 10, 10), view(2, Side::Player, 11, 11)},
                                    {view(70, Side::Enemy, 60, 60), view(71, Side::Enemy, 41, 40)}),
                     15, orders);
    ASSERT_EQ(orders.size(), 2U);
    for (const Order& order : orders) {
        EXPECT_EQ(order.kind, OrderKind::Attack);
        EXPECT_EQ(order.target.value, 71U) << "the enemy nearest the point is the one to kill";
    }
}

TEST(Controller, FocusBreaksADistanceTieOnTheLowerId) {
    Controller controller = must(Controller::create(policy_of(PolicyKind::Focus, 40, 40), {id(1)}));
    std::vector<Order> orders;
    // Both three tiles from the point, on opposite sides: a tie, and a tie must not depend on the
    // order the observation happened to list them in.
    controller.think(observation_of({view(1, Side::Player, 10, 10)},
                                    {view(80, Side::Enemy, 43, 40), view(79, Side::Enemy, 37, 40)}),
                     15, orders);
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders[0].target.value, 79U)
        << "ties go to the lowest id, like the scripted opponent";
}

TEST(Controller, FocusWithNoEnemiesEmitsNothing) {
    Controller controller = must(Controller::create(policy_of(PolicyKind::Focus, 40, 40), {id(1)}));
    std::vector<Order> orders;
    controller.think(observation_of({view(1, Side::Player, 10, 10)}), 15, orders);
    EXPECT_TRUE(orders.empty()) << "nothing to shoot at is not a reason to issue a bogus target";
}

TEST(Controller, OrdersOnlyItsOwnLivingUnits) {
    Controller controller
        = must(Controller::create(policy_of(PolicyKind::Hold, 20, 20), {id(1), id(2), id(3)}));
    std::vector<Order> orders;
    // Unit 2 has died, unit 3 is not in the observation at all, and 99 is somebody else's.
    controller.think(observation_of({view(1, Side::Player, 1, 1), view(99, Side::Player, 2, 2)}),
                     15, orders);
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders[0].unit.value, 1U) << "a dead unit must not keep receiving orders";
}

TEST(ControllerSet, AddDropsUnitsAnotherControllerAlreadyOwns) {
    ControllerSet set;
    Controller first
        = must(Controller::create(policy_of(PolicyKind::Hold, 10, 10), {id(1), id(2)}));
    EXPECT_TRUE(set.add(first).empty());

    Controller second
        = must(Controller::create(policy_of(PolicyKind::Hold, 20, 20), {id(2), id(3)}));
    const std::vector<UnitId> refused = set.add(second);
    ASSERT_EQ(refused.size(), 1U);
    EXPECT_EQ(refused[0].value, 2U) << "the caller has to be told which units were not taken over";
    ASSERT_EQ(set.size(), 2U);
    EXPECT_EQ(set.controllers()[1].roster().size(), 1U) << "unit 2 belongs to the first controller";
    EXPECT_EQ(set.controllers()[1].roster()[0].value, 3U);
}

TEST(ControllerSet, OwnerOfReportsTheFirstController) {
    ControllerSet set;
    Controller first = must(Controller::create(policy_of(PolicyKind::Hold, 10, 10), {id(4)}));
    Controller second = must(Controller::create(policy_of(PolicyKind::Hold, 20, 20), {id(5)}));
    add_whole(set, first);
    add_whole(set, second);
    constexpr std::size_t kNoOwner = 9999;
    EXPECT_EQ(set.owner_of(id(4)).value_or(kNoOwner), 0U) << "the controller placed first owns it";
    EXPECT_EQ(set.owner_of(id(5)).value_or(kNoOwner), 1U);
    EXPECT_FALSE(set.owner_of(id(6)).has_value()) << "an unowned unit has no owner";
}

TEST(ControllerSet, ThinkDueEmitsInInsertionOrder) {
    ControllerSet set;
    Controller first = must(Controller::create(policy_of(PolicyKind::Hold, 10, 10), {id(2)}));
    Controller second = must(Controller::create(policy_of(PolicyKind::Hold, 20, 20), {id(1)}));
    add_whole(set, first);
    add_whole(set, second);
    std::vector<Order> orders;
    set.think_due(observation_of({view(1, Side::Player, 1, 1), view(2, Side::Player, 2, 2)}), 15,
                  orders);
    ASSERT_EQ(orders.size(), 2U);
    EXPECT_EQ(orders[0].unit.value, 2U) << "the controller placed first thinks first";
    EXPECT_EQ(orders[1].unit.value, 1U);
}

TEST(ControllerSet, DoesNothingBeforeTheCadence) {
    ControllerSet set;
    Controller first = must(Controller::create(policy_of(PolicyKind::Hold, 10, 10, 30), {id(1)}));
    add_whole(set, first);
    std::vector<Order> orders;
    set.think_due(observation_of({view(1, Side::Player, 1, 1)}), 29, orders);
    EXPECT_TRUE(orders.empty());
}

/// The claim the whole milestone rests on: a controller is not a special case in the simulation.
/// It issues orders through the same channel the player does, and the units actually move.
TEST(ControllerIntegration, ADenyControllerDrivesARealTankThroughTheSimulation) {
    auto sim = protekt::sim::Sim::create(protekt::sim::skirmish(kSeed));
    UnitId tank{};
    for (const protekt::sim::Unit& unit : sim.units()) {
        if (unit.side == Side::Player && unit.kind == UnitKind::Tank) {
            tank = unit.id;
            break;
        }
    }
    ASSERT_TRUE(tank.valid());

    const protekt::sim::Unit& start = *std::find_if(sim.units().begin(), sim.units().end(),
                                                    [tank](const auto& u) { return u.id == tank; });
    const int tx = start.x.floor_int();
    const int ty = start.y.floor_int();
    const std::int32_t start_raw_x = start.x.raw;
    const std::int32_t start_raw_y = start.y.raw;

    // Drive along a heading with a clear run: with seed 42 this spawn faces craters, and the sim
    // is right to refuse them (measured -- see tests/test_player.cpp).
    const auto clear_run = [tx, ty](float heading) {
        for (int d = 1; d <= 10; ++d) {
            const auto tile = protekt::world::sample(
                kSeed, tx + static_cast<int>(std::sin(heading) * static_cast<float>(d)),
                ty + static_cast<int>(std::cos(heading) * static_cast<float>(d)));
            if (tile.terrain == protekt::world::Terrain::Crater) {
                return false;
            }
        }
        return true;
    };
    float heading = 0.0F;
    bool found = false;
    for (int step = 0; step < 32 && !found; ++step) {
        heading = static_cast<float>(step) * 0.19635F; // 32 steps around the circle
        found = clear_run(heading);
    }
    ASSERT_TRUE(found) << "no clear heading out of the spawn";

    Policy deny = policy_of(PolicyKind::Deny, 0, 0);
    deny.x = Fixed::from_int(tx + static_cast<int>(std::sin(heading) * 8.0F));
    deny.y = Fixed::from_int(ty + static_cast<int>(std::cos(heading) * 8.0F));
    Controller controller = must(Controller::create(deny, {tank}));

    for (protekt::sim::Tick tick = 1; tick <= 150; ++tick) {
        std::vector<Order> orders;
        controller.think(protekt::ai::observe(sim, Side::Player), tick, orders);
        for (const Order& order : orders) {
            sim.enqueue(order);
        }
        sim.step();
    }

    const protekt::sim::Unit& end = *std::find_if(sim.units().begin(), sim.units().end(),
                                                  [tank](const auto& u) { return u.id == tank; });

    // What a Deny policy actually promises: the unit gets ONTO the line and holds there. Not "it
    // moved a long way" -- a unit that starts near the line should barely move, and this test
    // originally demanded four tiles of travel, which is a claim about the metric rather than about
    // the policy. Distances here are squared Euclidean, the metric the contract names.
    const auto distance_tiles = [](std::int32_t x_raw, std::int32_t y_raw, int px, int py) {
        const double dx = (static_cast<double>(x_raw) / 65536.0) - static_cast<double>(px);
        const double dy = (static_cast<double>(y_raw) / 65536.0) - static_cast<double>(py);
        return std::sqrt((dx * dx) + (dy * dy));
    };
    const int point_x = deny.x.floor_int();
    const int point_y = deny.y.floor_int();
    const double started = distance_tiles(start_raw_x, start_raw_y, point_x, point_y);
    const double finished = distance_tiles(end.x.raw, end.y.raw, point_x, point_y);
    EXPECT_LT(finished, started) << "the controller never closed the gap: " << started << " -> "
                                 << finished;
    EXPECT_LE(finished, static_cast<double>(kDenyRadiusTiles) + 0.5)
        << "it stopped " << finished << " tiles from the point, short of the " << kDenyRadiusTiles
        << "-tile line it was told to hold";
}

} // namespace
