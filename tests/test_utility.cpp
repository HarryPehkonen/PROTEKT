#include "protekt/ai/utility.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <variant>

#include "protekt/sim/projectile.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::ai {
namespace {

using sim::Side;
using sim::UnitId;
using sim::UnitKind;

[[nodiscard]] sim::UnitView unit(std::uint32_t id, UnitKind kind, Side side, int x, int y,
                                 std::uint16_t hp = 100) {
    sim::UnitView view{};
    view.id = UnitId{id};
    view.kind = kind;
    view.side = side;
    view.x = x;
    view.y = y;
    view.hp = hp;
    return view;
}

/// A side with one of each kind, well spread out, and an enemy to look at.
[[nodiscard]] sim::Observation field(const std::vector<sim::UnitView>& own,
                                     const std::vector<sim::UnitView>& enemy, sim::Tick tick = 0) {
    sim::Observation observation{};
    observation.tick = tick;
    observation.own = own;
    observation.enemy = enemy;
    return observation;
}

[[nodiscard]] int orders_for(const std::vector<sim::Order>& orders, UnitId unit) {
    return static_cast<int>(std::count_if(
        orders.begin(), orders.end(), [&](const sim::Order& order) { return order.unit == unit; }));
}

constexpr std::uint32_t kTank = 1;
constexpr std::uint32_t kFactory = 2;
constexpr std::uint32_t kTurret = 3;

/// Every value an order carries, as plain numbers: target, then the two points. Zero where the kind
/// has none, so two orders are the same order exactly when their contents and kinds match.
std::array<std::int64_t, 5> contents_of(const sim::Order& order) {
    std::array<std::int64_t, 5> out{};
    std::visit(
        [&out](const auto& p) {
            using P = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<P, sim::orders::MoveTo>
                          || std::is_same_v<P, sim::orders::GuardAt>) {
                out.at(1) = p.x.raw;
                out.at(2) = p.y.raw;
            } else if constexpr (std::is_same_v<P, sim::orders::Patrol>) {
                out.at(1) = p.x.raw;
                out.at(2) = p.y.raw;
                out.at(3) = p.x2.raw;
                out.at(4) = p.y2.raw;
            } else if constexpr (std::is_same_v<P, sim::orders::Attack>
                                 || std::is_same_v<P, sim::orders::FocusFire>) {
                out.at(0) = p.target.value;
            }
        },
        order.payload);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// What is worth shooting
// ---------------------------------------------------------------------------------------------

TEST(Utility, ATankBeatsAFactoryAtTheSameDistance) {
    EXPECT_GT(threat_of(UnitKind::Tank), threat_of(UnitKind::Factory));
    EXPECT_GT(threat_of(UnitKind::Turret), threat_of(UnitKind::Factory));
}

TEST(Utility, TheCloserOfTwoEqualsIsPreferred) {
    const sim::UnitView self = unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10);
    const sim::UnitView near = unit(50, UnitKind::Tank, Side::Player, 12, 10);
    const sim::UnitView far = unit(51, UnitKind::Tank, Side::Player, 18, 10);
    EXPECT_GT(score_engage(self, near), score_engage(self, far));
}

TEST(Utility, AThreatTwelveTilesAwayBeatsAFactoryAdjacentToYou) {
    // The claim the scoring formula exists for: distance alone must not decide it. A factory cannot
    // shoot back, and a tank at the edge of range can.
    const sim::UnitView self = unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10);
    const sim::UnitView tank_far = unit(50, UnitKind::Tank, Side::Player, 22, 10);
    const sim::UnitView factory_next_door = unit(51, UnitKind::Factory, Side::Player, 11, 10);
    EXPECT_GT(score_engage(self, tank_far), score_engage(self, factory_next_door));
}

TEST(Utility, DistanceIsSquaredAndWholeAndSymmetric) {
    const sim::UnitView a = unit(1, UnitKind::Tank, Side::Player, 3, 4);
    const sim::UnitView b = unit(2, UnitKind::Tank, Side::Enemy, 6, 8);
    EXPECT_EQ(distance_squared(a, b), 25); // 3-4-5
    EXPECT_EQ(distance_squared(b, a), 25);
    EXPECT_EQ(distance_squared(a, a), 0);
}

TEST(Utility, ChooseTargetIgnoresWhatIsOutOfRange) {
    const sim::Observation observation
        = field({}, {unit(50, UnitKind::Tank, Side::Player, 40, 10)});
    const sim::UnitView self = unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10);
    UtilityTuning tuning{};
    tuning.engage_range_tiles = 14;
    EXPECT_EQ(choose_target(self, observation, tuning), nullptr);
}

TEST(Utility, ChooseTargetTakesTheBestAndTiesGoToTheLowerId) {
    // Two identical tanks, exactly equidistant: whichever comes first in the observation wins, and
    // the observation is ordered by unit id. Nothing else may break the tie, or the same battle
    // would resolve differently on a machine that walked the list differently.
    const sim::Observation observation = field({}, {unit(50, UnitKind::Tank, Side::Player, 12, 10),
                                                    unit(51, UnitKind::Tank, Side::Player, 8, 10)});
    const sim::UnitView self = unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10);
    const sim::UnitView* chosen = choose_target(self, observation, UtilityTuning{});
    ASSERT_NE(chosen, nullptr);
    EXPECT_EQ(chosen->id, UnitId{50});
}

// ---------------------------------------------------------------------------------------------
// Posture: hold, engage, go home, or withdraw
// ---------------------------------------------------------------------------------------------

TEST(Utility, AHealthyTankWithAnEnemyInRangeEngagesIt) {
    const sim::Observation observation
        = field({}, {unit(50, UnitKind::Tank, Side::Player, 16, 10)});
    const Decision decision
        = decide(unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10), observation, UtilityTuning{});
    EXPECT_EQ(decision.intent, Intent::Engage);
    EXPECT_EQ(decision.target, UnitId{50});
    EXPECT_GT(decision.score, 0);
}

TEST(Utility, AHurtTankWithdrawsEvenWithAnEnemyRightInFrontOfIt) {
    // The gate: this is the test that fails if retreat is ever made to compete on the same scale as
    // engaging, because an enemy at two tiles scores very well.
    UtilityTuning tuning{};
    tuning.retreat_hp = 20;
    const sim::Observation observation
        = field({}, {unit(50, UnitKind::Tank, Side::Player, 12, 10)});
    const sim::UnitView hurt = unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10, 20);
    const Decision decision = decide(hurt, observation, tuning);
    EXPECT_EQ(decision.intent, Intent::Retreat);
    EXPECT_NE(decision.target, UnitId{50}) << "withdrawing is not attacking";
}

TEST(Utility, AHurtTankHeadsForItsOwnBaseWhenItWithdraws) {
    UtilityTuning tuning{};
    tuning.retreat_hp = 20;
    const sim::Observation observation
        = field({unit(kFactory, UnitKind::Factory, Side::Enemy, 60, 70)},
                {unit(50, UnitKind::Tank, Side::Player, 12, 10)});
    const sim::UnitView hurt = unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10, 5);
    const Decision decision = decide(hurt, observation, tuning);
    ASSERT_EQ(decision.intent, Intent::Retreat);
    EXPECT_EQ(decision.x, 60);
    EXPECT_EQ(decision.y, 70);
}

TEST(Utility, AHealthyTankIsNotWithdrawn) {
    UtilityTuning tuning{};
    tuning.retreat_hp = 20;
    const sim::Observation observation
        = field({}, {unit(50, UnitKind::Tank, Side::Player, 12, 10)});
    EXPECT_NE(
        decide(unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10, 21), observation, tuning).intent,
        Intent::Retreat);
}

TEST(Utility, ATankWithNothingToShootGoesBackToTheBaseArea) {
    UtilityTuning tuning{};
    tuning.station_radius_tiles = 8;
    const sim::UnitView self = unit(kTank, UnitKind::Tank, Side::Enemy, 40, 40);
    const sim::Observation observation
        = field({unit(kFactory, UnitKind::Factory, Side::Enemy, 10, 10)},
                {unit(50, UnitKind::Tank, Side::Player, 200, 200)});
    const Decision decision = decide(self, observation, tuning);
    ASSERT_EQ(decision.intent, Intent::Reposition);
    EXPECT_EQ(decision.x, 10);
    EXPECT_EQ(decision.y, 10);
}

TEST(Utility, ATankAlreadyHomeWithNothingToShootHolds) {
    UtilityTuning tuning{};
    tuning.station_radius_tiles = 8;
    const sim::Observation observation
        = field({unit(kFactory, UnitKind::Factory, Side::Enemy, 10, 10)},
                {unit(50, UnitKind::Tank, Side::Player, 200, 200)});
    const Decision decision
        = decide(unit(kTank, UnitKind::Tank, Side::Enemy, 12, 11), observation, tuning);
    EXPECT_EQ(decision.intent, Intent::Hold);
}

TEST(Utility, ATankWithNoBaseAndNothingToShootHoldsWhereItStands) {
    // Losing every factory must not make a tank wander: with nowhere to go, the honest decision is
    // to stay put, and it is the decision a test has to state or the behaviour gets invented by
    // accident.
    const sim::Observation observation
        = field({}, {unit(50, UnitKind::Tank, Side::Player, 200, 200)});
    EXPECT_EQ(decide(unit(kTank, UnitKind::Tank, Side::Enemy, 40, 40), observation, UtilityTuning{})
                  .intent,
              Intent::Hold);
}

TEST(Utility, ATurretHoldsItsPostAndNeverGoesHunting) {
    const sim::Observation observation
        = field({}, {unit(50, UnitKind::Tank, Side::Player, 12, 12)});
    const Decision decision = decide(unit(kTurret, UnitKind::Turret, Side::Enemy, 10, 10, 150),
                                     observation, UtilityTuning{});
    EXPECT_EQ(decision.intent, Intent::Hold);
    EXPECT_NE(decision.intent, Intent::Engage);
}

TEST(Utility, ATurretIsNotWithdrawnEvenWhenItIsNearlyDead) {
    // A turret that runs away is a turret that leaves the road open, and it cannot outrun a tank.
    const sim::Observation observation
        = field({}, {unit(50, UnitKind::Tank, Side::Player, 12, 12)});
    const Decision decision = decide(unit(kTurret, UnitKind::Turret, Side::Enemy, 10, 10, 3),
                                     observation, UtilityTuning{});
    EXPECT_NE(decision.intent, Intent::Retreat);
    EXPECT_NE(decision.intent, Intent::Reposition);
}

TEST(Utility, AFactoryHasNothingToDecide) {
    const sim::Observation observation
        = field({}, {unit(50, UnitKind::Tank, Side::Player, 12, 12)});
    const Decision decision = decide(unit(kFactory, UnitKind::Factory, Side::Enemy, 10, 10, 400),
                                     observation, UtilityTuning{});
    EXPECT_EQ(decision.intent, Intent::Hold);
}

TEST(Utility, HomeFactoryIsTheLowestIdLivingOne) {
    const sim::Observation observation = field({unit(9, UnitKind::Factory, Side::Enemy, 5, 5),
                                                unit(4, UnitKind::Factory, Side::Enemy, 7, 7)},
                                               {});
    const sim::UnitView* home = home_factory(observation);
    ASSERT_NE(home, nullptr);
    EXPECT_EQ(home->id, UnitId{4});
    EXPECT_EQ(home_factory(field({}, {})), nullptr);
}

// ---------------------------------------------------------------------------------------------
// Advance to contact: the posture that makes an opponent an opponent
// ---------------------------------------------------------------------------------------------

namespace {

/// Own base at home, an enemy base far away, and nothing within engagement range of anyone.
[[nodiscard]] sim::Observation two_bases(int home_x, int home_y, int enemy_x, int enemy_y) {
    return field({unit(kFactory, UnitKind::Factory, Side::Enemy, home_x, home_y),
                  unit(kTank, UnitKind::Tank, Side::Enemy, home_x, home_y)},
                 {unit(90, UnitKind::Factory, Side::Player, enemy_x, enemy_y)});
}

} // namespace

TEST(Utility, ATankWithNothingInRangeAdvancesTowardTheEnemyBase) {
    // The defect this posture exists for: without it, a utility commander never leaves its own
    // half, and two of them never fight -- measured, 0 shots in 2400 ticks on ten seeds.
    UtilityTuning tuning{};
    const sim::Observation observation = two_bases(10, 10, 110, 10);
    const Decision decision
        = decide(unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10), observation, tuning);
    ASSERT_EQ(decision.intent, Intent::Advance);
    EXPECT_GT(decision.x, 10) << "it moved toward the enemy, not away from it";
    EXPECT_LT(decision.x, 110) << "and it did not walk onto the enemy's doorstep";
}

TEST(Utility, TheAdvanceStagesOnTheMidpointOfTheTwoBases) {
    UtilityTuning tuning{};
    tuning.advance_percent = 50;
    const Decision decision = decide(unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10),
                                     two_bases(10, 10, 110, 10), tuning);
    ASSERT_EQ(decision.intent, Intent::Advance);
    EXPECT_EQ(decision.x, 60) << "halfway between the bases";
    EXPECT_EQ(decision.y, 10);
}

TEST(Utility, TheAdvanceStagesShortOfTheEnemyBaseAtAnyPercentage) {
    for (const int percent : {10, 25, 50, 75, 90}) {
        UtilityTuning tuning{};
        tuning.advance_percent = percent;
        const Decision decision = decide(unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10),
                                         two_bases(10, 10, 210, 10), tuning);
        ASSERT_EQ(decision.intent, Intent::Advance) << "at " << percent << "%";
        EXPECT_LE(decision.x, 10 + (percent * 200) / 100)
            << "the staging point must never overshoot the line at " << percent << "%";
    }
}

TEST(Utility, ATankAlreadyAtTheStagingPointHoldsThere) {
    UtilityTuning tuning{};
    tuning.advance_percent = 50;
    tuning.station_radius_tiles = 8;
    // Staging point is (60,10); stand two tiles from it.
    const Decision decision = decide(unit(kTank, UnitKind::Tank, Side::Enemy, 62, 10),
                                     two_bases(10, 10, 110, 10), tuning);
    EXPECT_EQ(decision.intent, Intent::Hold)
        << "arriving must not become re-deciding the same move forever";
}

TEST(Utility, AHurtTankStillWithdrawsRatherThanAdvancing) {
    UtilityTuning tuning{};
    tuning.retreat_hp = 20;
    const sim::Observation observation = two_bases(10, 10, 110, 10);
    const Decision decision
        = decide(unit(kTank, UnitKind::Tank, Side::Enemy, 60, 10, 5), observation, tuning);
    ASSERT_EQ(decision.intent, Intent::Retreat);
    EXPECT_EQ(decision.x, 10) << "away from the enemy, not toward it";
}

TEST(Utility, AnEnemyInRangeStillBeatsAdvancing) {
    UtilityTuning tuning{};
    EXPECT_EQ(tuning.engage_range_tiles, 14) << "this test's numbers assume the default range";
    sim::Observation observation = two_bases(10, 10, 110, 10);
    // Ten tiles away: inside the range. Thirty tiles would be out of it, and advancing past an
    // enemy nobody can shoot is the CORRECT behaviour -- which is what the first version of this
    // test got wrong, asserting Engage for an enemy that was out of range.
    observation.enemy.push_back(unit(91, UnitKind::Tank, Side::Player, 20, 10));
    const Decision decision
        = decide(unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10), observation, tuning);
    ASSERT_EQ(decision.intent, Intent::Engage);
    EXPECT_EQ(decision.target, UnitId{91});
}

TEST(Utility, WithNoEnemyBaseKnownATankStillGoesHomeInsteadOfWandering) {
    // Advance needs somewhere to go. Without an enemy base it must not invent a direction.
    UtilityTuning tuning{};
    const sim::Observation observation
        = field({unit(kFactory, UnitKind::Factory, Side::Enemy, 10, 10)},
                {unit(50, UnitKind::Tank, Side::Player, 200, 200)});
    const Decision decision
        = decide(unit(kTank, UnitKind::Tank, Side::Enemy, 40, 40), observation, tuning);
    EXPECT_EQ(decision.intent, Intent::Reposition);
    EXPECT_EQ(decision.x, 10);
}

TEST(Utility, TheEnemyBaseIsTheLowestIdLivingEnemyFactory) {
    const sim::Observation observation
        = field({}, {unit(9, UnitKind::Factory, Side::Player, 5, 5),
                     unit(4, UnitKind::Factory, Side::Player, 7, 7)});
    const sim::UnitView* base = enemy_factory(observation);
    ASSERT_NE(base, nullptr);
    EXPECT_EQ(base->id, UnitId{4});
    EXPECT_EQ(enemy_factory(field({}, {})), nullptr);
}

TEST(Utility, AdvancingIsDeterministic) {
    UtilityTuning tuning{};
    const sim::Observation observation = two_bases(10, 10, 110, 10);
    for (int i = 0; i < 20; ++i) {
        const Decision a
            = decide(unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10), observation, tuning);
        const Decision b
            = decide(unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10), observation, tuning);
        EXPECT_EQ(a.intent, b.intent);
        EXPECT_EQ(a.x, b.x);
        EXPECT_EQ(a.y, b.y);
    }
}

// ---------------------------------------------------------------------------------------------
// The commander: orders, seams, determinism
// ---------------------------------------------------------------------------------------------

TEST(Utility, ItThinksOnItsCycleAndNotInBetween) {
    UtilityTuning tuning{};
    tuning.cycle_ticks = 15;
    UtilityCommander commander{tuning};
    const sim::Observation observation = field({unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10)},
                                               {unit(50, UnitKind::Tank, Side::Player, 16, 10)});
    std::vector<sim::Order> orders;
    commander.think(field(observation.own, observation.enemy, 14), orders);
    EXPECT_TRUE(orders.empty()) << "tick 14 is not on a 15-tick cycle";
    commander.think(field(observation.own, observation.enemy, 15), orders);
    EXPECT_FALSE(orders.empty()) << "tick 15 is";
}

TEST(Utility, OneOrderPerUnitPerCycleAtMost) {
    UtilityTuning tuning{};
    tuning.cycle_ticks = 1;
    UtilityCommander commander{tuning};
    const sim::Observation observation
        = field({unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10),
                 unit(kFactory, UnitKind::Factory, Side::Enemy, 10, 12),
                 unit(kTurret, UnitKind::Turret, Side::Enemy, 11, 9)},
                {unit(50, UnitKind::Tank, Side::Player, 16, 10)});
    std::vector<sim::Order> orders;
    commander.think(observation, orders);
    EXPECT_EQ(orders_for(orders, UnitId{kTank}), 1);
    EXPECT_EQ(orders_for(orders, UnitId{kTurret}), 1);
    EXPECT_EQ(orders_for(orders, UnitId{kFactory}), 0) << "a factory cannot move or shoot";
}

TEST(Utility, ItNeverOrdersAnEnemyUnitOrItsOwnDead) {
    // The seam audit. A commander that reached into the other side's roster would be caught here,
    // and so would one that issued orders for units the observation no longer carries as alive.
    UtilityTuning tuning{};
    tuning.cycle_ticks = 1;
    UtilityCommander commander{tuning};
    const sim::Observation observation = field({unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10)},
                                               {unit(50, UnitKind::Tank, Side::Player, 16, 10),
                                                unit(51, UnitKind::Turret, Side::Player, 17, 10)});
    std::vector<sim::Order> orders;
    commander.think(observation, orders);
    ASSERT_FALSE(orders.empty());
    for (const sim::Order& order : orders) {
        EXPECT_EQ(order.unit, UnitId{kTank})
            << "only its own unit may be ordered, id " << order.unit.value;
        if (order.kind() == sim::OrderKind::Attack) {
            const UnitId target = std::get<protekt::sim::orders::Attack>(order.payload).target;
            EXPECT_NE(target, UnitId{kTank});
            EXPECT_TRUE(target == UnitId{50} || target == UnitId{51});
        }
    }
}

TEST(Utility, TheDecisionsItReportsAreTheOrdersItIssued) {
    UtilityTuning tuning{};
    tuning.cycle_ticks = 1;
    UtilityCommander commander{tuning};
    const sim::Observation observation = field({unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10)},
                                               {unit(50, UnitKind::Tank, Side::Player, 16, 10)});
    std::vector<sim::Order> orders;
    commander.think(observation, orders);
    const std::vector<Decision>& decisions = commander.decisions();
    ASSERT_EQ(decisions.size(), observation.own.size())
        << "every own unit gets a decision, even the ones that produce no order";
    const Decision& tank = decisions.front();
    EXPECT_EQ(tank.unit, UnitId{kTank});
    EXPECT_EQ(tank.intent, Intent::Engage);
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().kind(), sim::OrderKind::Attack);
    EXPECT_EQ(std::get<sim::orders::Attack>(orders.front().payload).target, UnitId{50});
}

TEST(Utility, HoldingMeansNoOrderForATankAndAGuardOrderForATurret) {
    // A tank holds by being given no order at all. A turret cannot: it has no stance of its own to
    // keep, so holding means being told to guard the tile it already stands on. Both halves are
    // checked, because "holding" that means different things per kind is where a commander quietly
    // stops working.
    UtilityTuning tuning{};
    tuning.cycle_ticks = 1;
    UtilityCommander commander{tuning};
    const sim::Observation observation
        = field({unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10),
                 unit(kTurret, UnitKind::Turret, Side::Enemy, 12, 9),
                 unit(kFactory, UnitKind::Factory, Side::Enemy, 10, 12)},
                {unit(50, UnitKind::Tank, Side::Player, 200, 200)});
    std::vector<sim::Order> orders;
    commander.think(observation, orders);

    EXPECT_EQ(orders_for(orders, UnitId{kTank}), 0)
        << "a tank with nothing to shoot at and its base under its feet is left alone";
    ASSERT_EQ(orders_for(orders, UnitId{kTurret}), 1);
    for (const sim::Order& order : orders) {
        if (order.unit == UnitId{kTurret}) {
            EXPECT_EQ(order.kind(), sim::OrderKind::GuardAt);
            const auto& guard = std::get<protekt::sim::orders::GuardAt>(order.payload);
            EXPECT_EQ(guard.x, sim::Fixed::from_int(12));
            EXPECT_EQ(guard.y, sim::Fixed::from_int(9));
        }
    }
    for (const sim::Order& order : orders) {
        EXPECT_NE(order.kind(), sim::OrderKind::Stop)
            << "Stop is a decision the player makes, not one the AI needs";
    }
}

TEST(Utility, ThinkingTwiceOnTheSameObservationDecidesTheSameThing) {
    UtilityTuning tuning{};
    tuning.cycle_ticks = 1;
    UtilityCommander commander{tuning};
    const sim::Observation observation
        = field({unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10),
                 unit(7, UnitKind::Tank, Side::Enemy, 40, 40),
                 unit(kTurret, UnitKind::Turret, Side::Enemy, 11, 9)},
                {unit(50, UnitKind::Tank, Side::Player, 16, 10),
                 unit(51, UnitKind::Factory, Side::Player, 18, 12)});
    std::vector<sim::Order> first;
    commander.think(observation, first);
    std::vector<sim::Order> second;
    commander.think(observation, second);
    ASSERT_EQ(first.size(), second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(first[i].unit, second[i].unit);
        EXPECT_EQ(first[i].kind(), second[i].kind());
        EXPECT_EQ(contents_of(first[i]), contents_of(second[i]));
    }
}

TEST(Utility, EveryUnitGetsADecisionEvenWithNoEnemiesAndNoBase) {
    UtilityTuning tuning{};
    tuning.cycle_ticks = 1;
    UtilityCommander commander{tuning};
    const sim::Observation observation
        = field({unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10),
                 unit(kTurret, UnitKind::Turret, Side::Enemy, 12, 10),
                 unit(kFactory, UnitKind::Factory, Side::Enemy, 14, 10)},
                {});
    std::vector<sim::Order> orders;
    commander.think(observation, orders);
    EXPECT_EQ(commander.decisions().size(), 3U);
    for (const Decision& decision : commander.decisions()) {
        EXPECT_EQ(decision.intent, Intent::Hold)
            << "with nothing to shoot and nowhere to go, holding is the only honest decision";
    }
}

TEST(Utility, AWithdrawalIsAnOrderTheSimulationCanActuallyCarryOut) {
    // The decisions are in whole tiles; the sim wants fixed point. This checks the conversion, in
    // the order the sim will read it, because a unit ordered to a tile it cannot express never
    // moves.
    UtilityTuning tuning{};
    tuning.cycle_ticks = 1;
    tuning.retreat_hp = 20;
    UtilityCommander commander{tuning};
    const sim::Observation observation
        = field({unit(kFactory, UnitKind::Factory, Side::Enemy, 30, 44),
                 unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10, 4)},
                {unit(50, UnitKind::Tank, Side::Player, 12, 10)});
    std::vector<sim::Order> orders;
    commander.think(observation, orders);
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().kind(), sim::OrderKind::MoveTo);
    EXPECT_EQ(orders.front().unit, UnitId{kTank});
    EXPECT_EQ(std::get<sim::orders::MoveTo>(orders.front().payload).x, sim::Fixed::from_int(30));
    EXPECT_EQ(std::get<sim::orders::MoveTo>(orders.front().payload).y, sim::Fixed::from_int(44));
}

// ---------------------------------------------------------------------------------------------
// No line: a gun that cannot see its target moves, it does not shoot the hill
// ---------------------------------------------------------------------------------------------

namespace {

/// The ground these tests stand on. Seed 42, row 30: a rise between (42,30) and (52,30) blocks
/// every line from one to the other. The probes in the comments were measured with `sim::can_see`
/// and are asserted again by each test, so a change to the ground fails loudly rather than
/// quietly turning a test into a control.
constexpr std::uint64_t kHillSeed = 42;

/// A unit as `sim::observe` would report it on that ground: tile and Q16.16 position agree.
[[nodiscard]] sim::UnitView at(std::uint32_t id, Side side, int x, int y) {
    sim::UnitView view = unit(id, UnitKind::Tank, side, x, y);
    view.raw_x = sim::Fixed::from_int(x).raw;
    view.raw_y = sim::Fixed::from_int(y).raw;
    return view;
}

[[nodiscard]] sim::Observation on_the_hill(const sim::UnitView& self, const sim::UnitView& enemy) {
    sim::Observation observation = field({self}, {enemy});
    observation.terrain = kHillSeed;
    return observation;
}

[[nodiscard]] bool sees(int from_x, int from_y, int at_x, int at_y) {
    return sim::can_see(kHillSeed, sim::Fixed::from_int(from_x), sim::Fixed::from_int(from_y),
                        sim::Fixed::from_int(at_x), sim::Fixed::from_int(at_y), UnitKind::Tank);
}

constexpr std::uint32_t kEnemyTank = 50;

} // namespace

TEST(UtilityLine, AUnitWithNoLineChoosesRepositionNotEngage) {
    // (42,30) -> (52,30): ten tiles, inside engage range, and the hill is in the way.
    ASSERT_FALSE(sees(42, 30, 52, 30));
    const Decision decision
        = decide(at(kTank, Side::Player, 42, 30),
                 on_the_hill(at(kTank, Side::Player, 42, 30), at(kEnemyTank, Side::Enemy, 52, 30)),
                 UtilityTuning{});
    EXPECT_NE(decision.intent, Intent::Engage) << "it would be shooting the hill";
    EXPECT_TRUE(decision.intent == Intent::Reposition || decision.intent == Intent::Advance)
        << "a unit that cannot see its target moves";
}

TEST(UtilityLine, AUnitWithALineStillEngagesAndFires) {
    // The control: without it, a bug that always repositions would pass the test above.
    ASSERT_TRUE(sees(100, 100, 110, 100));
    const sim::UnitView self = at(kTank, Side::Player, 100, 100);
    const Decision decision
        = decide(self, on_the_hill(self, at(kEnemyTank, Side::Enemy, 110, 100)), UtilityTuning{});
    EXPECT_EQ(decision.intent, Intent::Engage);
    EXPECT_EQ(decision.target, UnitId{kEnemyTank});

    // And the gun fires: the same two tanks in a real sim.
    sim::Sim world = sim::Sim::create(sim::Scenario{
        kHillSeed,
        {{UnitKind::Tank, Side::Player, 100, 100}, {UnitKind::Tank, Side::Enemy, 110, 100}}});
    std::size_t shots = 0;
    for (int tick = 0; tick < 120; ++tick) {
        world.step();
        shots += world.shots().size();
    }
    EXPECT_GT(shots, 0U);
}

TEST(UtilityLine, TheSidewaysStepIsTriedBeforeCharging) {
    // From (42,30) the row to (52,30) is blocked, but three tiles to one side there is a line:
    // (42,27) sees it, (42,33) does not. It steps there, along the side the terrain allows, rather
    // than walking at the hill.
    ASSERT_FALSE(sees(42, 30, 52, 30));
    ASSERT_TRUE(sees(42, 27, 52, 30));
    ASSERT_FALSE(sees(42, 33, 52, 30));
    const sim::UnitView self = at(kTank, Side::Player, 42, 30);
    const Decision decision
        = decide(self, on_the_hill(self, at(kEnemyTank, Side::Enemy, 52, 30)), UtilityTuning{});
    EXPECT_EQ(decision.intent, Intent::Reposition);
    EXPECT_EQ(decision.x, 42) << "sideways: the step is square to the line to the target";
    EXPECT_EQ(decision.y, 27);
}

TEST(UtilityLine, WhenNeitherSideHelpsItAdvancesOnTheTarget) {
    // (45,31) -> (55,31): no line from either side at 3, 6, 9 or 12 tiles. The only thing left is
    // the existing Advance, towards the target itself. No route is planned.
    ASSERT_FALSE(sees(45, 31, 55, 31));
    for (int step = 3; step <= 12; step += 3) {
        ASSERT_FALSE(sees(45, 31 - step, 55, 31));
        ASSERT_FALSE(sees(45, 31 + step, 55, 31));
    }
    const sim::UnitView self = at(kTank, Side::Player, 45, 31);
    const Decision decision
        = decide(self, on_the_hill(self, at(kEnemyTank, Side::Enemy, 55, 31)), UtilityTuning{});
    EXPECT_EQ(decision.intent, Intent::Advance);
    EXPECT_EQ(decision.x, 55);
    EXPECT_EQ(decision.y, 31);
}

TEST(UtilityLine, RepositioningKeepsItsTarget) {
    // Both ways of moving keep the target, so the unit fires the moment a line opens.
    const sim::UnitView self = at(kTank, Side::Player, 42, 30);
    const Decision sideways
        = decide(self, on_the_hill(self, at(kEnemyTank, Side::Enemy, 52, 30)), UtilityTuning{});
    ASSERT_EQ(sideways.intent, Intent::Reposition);
    EXPECT_EQ(sideways.target, UnitId{kEnemyTank});
    const sim::UnitView stuck = at(kTank, Side::Player, 45, 31);
    const Decision charging
        = decide(stuck, on_the_hill(stuck, at(kEnemyTank, Side::Enemy, 55, 31)), UtilityTuning{});
    ASSERT_EQ(charging.intent, Intent::Advance);
    EXPECT_EQ(charging.target, UnitId{kEnemyTank});

    // And once the unit is at the point it stepped to, it engages the same enemy.
    const sim::UnitView moved = at(kTank, Side::Player, sideways.x, sideways.y);
    const Decision then
        = decide(moved, on_the_hill(moved, at(kEnemyTank, Side::Enemy, 52, 30)), UtilityTuning{});
    EXPECT_EQ(then.intent, Intent::Engage);
    EXPECT_EQ(then.target, UnitId{kEnemyTank});
}

TEST(UtilityLine, NoLineMeansNoShot) {
    // A real sim, two tanks with a hill between them. Neither has a line, so neither fires: no
    // Shot, no bolt, in far more ticks than a fresh gun needs to reload.
    ASSERT_FALSE(sees(42, 30, 52, 30));
    ASSERT_FALSE(sees(52, 30, 42, 30));
    sim::Sim world = sim::Sim::create(sim::Scenario{
        kHillSeed,
        {{UnitKind::Tank, Side::Player, 42, 30}, {UnitKind::Tank, Side::Enemy, 52, 30}}});
    for (int tick = 0; tick < 300; ++tick) {
        world.step();
        EXPECT_TRUE(world.shots().empty()) << "fired into a hill at tick " << tick;
        EXPECT_TRUE(world.projectiles().empty()) << "a bolt is in the air at tick " << tick;
    }
}

TEST(UtilityLine, TheSameWorldGivesTheSameDecisions) {
    const auto history = [] {
        UtilityTuning tuning{};
        tuning.cycle_ticks = 1;
        UtilityCommander commander{tuning};
        sim::Sim world = sim::Sim::create(sim::Scenario{
            kHillSeed,
            {{UnitKind::Tank, Side::Player, 42, 30}, {UnitKind::Tank, Side::Enemy, 52, 30}}});
        std::vector<std::int64_t> trail;
        for (int tick = 0; tick < 200; ++tick) {
            std::vector<sim::Order> orders;
            commander.think(sim::observe(world, Side::Player), orders);
            for (const sim::Order& order : orders) {
                world.enqueue(order);
            }
            for (const Decision& d : commander.decisions()) {
                trail.push_back(static_cast<std::int64_t>(d.intent) * 1000000
                                + std::int64_t{d.x} * 1000 + d.y);
            }
            world.step();
        }
        trail.push_back(static_cast<std::int64_t>(world.state_hash()));
        return trail;
    };
    EXPECT_EQ(history(), history());
}

} // namespace protekt::ai
