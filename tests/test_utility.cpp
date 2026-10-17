#include "protekt/ai/utility.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>

namespace protekt::ai {
namespace {

using sim::Side;
using sim::UnitId;
using sim::UnitKind;

[[nodiscard]] UnitView unit(std::uint32_t id, UnitKind kind, Side side, int x, int y,
                            std::uint16_t hp = 100) {
    UnitView view{};
    view.id = UnitId{id};
    view.kind = kind;
    view.side = side;
    view.x = x;
    view.y = y;
    view.hp = hp;
    return view;
}

/// A side with one of each kind, well spread out, and an enemy to look at.
[[nodiscard]] Observation field(const std::vector<UnitView>& own,
                                const std::vector<UnitView>& enemy, sim::Tick tick = 0) {
    Observation observation{};
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

} // namespace

// ---------------------------------------------------------------------------------------------
// What is worth shooting
// ---------------------------------------------------------------------------------------------

TEST(Utility, ATankBeatsAFactoryAtTheSameDistance) {
    EXPECT_GT(threat_of(UnitKind::Tank), threat_of(UnitKind::Factory));
    EXPECT_GT(threat_of(UnitKind::Turret), threat_of(UnitKind::Factory));
}

TEST(Utility, TheCloserOfTwoEqualsIsPreferred) {
    const UnitView self = unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10);
    const UnitView near = unit(50, UnitKind::Tank, Side::Player, 12, 10);
    const UnitView far = unit(51, UnitKind::Tank, Side::Player, 18, 10);
    EXPECT_GT(score_engage(self, near), score_engage(self, far));
}

TEST(Utility, AThreatTwelveTilesAwayBeatsAFactoryAdjacentToYou) {
    // The claim the scoring formula exists for: distance alone must not decide it. A factory cannot
    // shoot back, and a tank at the edge of range can.
    const UnitView self = unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10);
    const UnitView tank_far = unit(50, UnitKind::Tank, Side::Player, 22, 10);
    const UnitView factory_next_door = unit(51, UnitKind::Factory, Side::Player, 11, 10);
    EXPECT_GT(score_engage(self, tank_far), score_engage(self, factory_next_door));
}

TEST(Utility, DistanceIsSquaredAndWholeAndSymmetric) {
    const UnitView a = unit(1, UnitKind::Tank, Side::Player, 3, 4);
    const UnitView b = unit(2, UnitKind::Tank, Side::Enemy, 6, 8);
    EXPECT_EQ(distance_squared(a, b), 25); // 3-4-5
    EXPECT_EQ(distance_squared(b, a), 25);
    EXPECT_EQ(distance_squared(a, a), 0);
}

TEST(Utility, ChooseTargetIgnoresWhatIsOutOfRange) {
    const Observation observation = field({}, {unit(50, UnitKind::Tank, Side::Player, 40, 10)});
    const UnitView self = unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10);
    UtilityTuning tuning{};
    tuning.engage_range_tiles = 14;
    EXPECT_EQ(choose_target(self, observation, tuning), nullptr);
}

TEST(Utility, ChooseTargetTakesTheBestAndTiesGoToTheLowerId) {
    // Two identical tanks, exactly equidistant: whichever comes first in the observation wins, and
    // the observation is ordered by unit id. Nothing else may break the tie, or the same battle
    // would resolve differently on a machine that walked the list differently.
    const Observation observation = field({}, {unit(50, UnitKind::Tank, Side::Player, 12, 10),
                                               unit(51, UnitKind::Tank, Side::Player, 8, 10)});
    const UnitView self = unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10);
    const UnitView* chosen = choose_target(self, observation, UtilityTuning{});
    ASSERT_NE(chosen, nullptr);
    EXPECT_EQ(chosen->id, UnitId{50});
}

// ---------------------------------------------------------------------------------------------
// Posture: hold, engage, go home, or withdraw
// ---------------------------------------------------------------------------------------------

TEST(Utility, AHealthyTankWithAnEnemyInRangeEngagesIt) {
    const Observation observation = field({}, {unit(50, UnitKind::Tank, Side::Player, 16, 10)});
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
    const Observation observation = field({}, {unit(50, UnitKind::Tank, Side::Player, 12, 10)});
    const UnitView hurt = unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10, 20);
    const Decision decision = decide(hurt, observation, tuning);
    EXPECT_EQ(decision.intent, Intent::Retreat);
    EXPECT_NE(decision.target, UnitId{50}) << "withdrawing is not attacking";
}

TEST(Utility, AHurtTankHeadsForItsOwnBaseWhenItWithdraws) {
    UtilityTuning tuning{};
    tuning.retreat_hp = 20;
    const Observation observation = field({unit(kFactory, UnitKind::Factory, Side::Enemy, 60, 70)},
                                          {unit(50, UnitKind::Tank, Side::Player, 12, 10)});
    const UnitView hurt = unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10, 5);
    const Decision decision = decide(hurt, observation, tuning);
    ASSERT_EQ(decision.intent, Intent::Retreat);
    EXPECT_EQ(decision.x, 60);
    EXPECT_EQ(decision.y, 70);
}

TEST(Utility, AHealthyTankIsNotWithdrawn) {
    UtilityTuning tuning{};
    tuning.retreat_hp = 20;
    const Observation observation = field({}, {unit(50, UnitKind::Tank, Side::Player, 12, 10)});
    EXPECT_NE(
        decide(unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10, 21), observation, tuning).intent,
        Intent::Retreat);
}

TEST(Utility, ATankWithNothingToShootGoesBackToTheBaseArea) {
    UtilityTuning tuning{};
    tuning.station_radius_tiles = 8;
    const UnitView self = unit(kTank, UnitKind::Tank, Side::Enemy, 40, 40);
    const Observation observation = field({unit(kFactory, UnitKind::Factory, Side::Enemy, 10, 10)},
                                          {unit(50, UnitKind::Tank, Side::Player, 200, 200)});
    const Decision decision = decide(self, observation, tuning);
    ASSERT_EQ(decision.intent, Intent::Reposition);
    EXPECT_EQ(decision.x, 10);
    EXPECT_EQ(decision.y, 10);
}

TEST(Utility, ATankAlreadyHomeWithNothingToShootHolds) {
    UtilityTuning tuning{};
    tuning.station_radius_tiles = 8;
    const Observation observation = field({unit(kFactory, UnitKind::Factory, Side::Enemy, 10, 10)},
                                          {unit(50, UnitKind::Tank, Side::Player, 200, 200)});
    const Decision decision
        = decide(unit(kTank, UnitKind::Tank, Side::Enemy, 12, 11), observation, tuning);
    EXPECT_EQ(decision.intent, Intent::Hold);
}

TEST(Utility, ATankWithNoBaseAndNothingToShootHoldsWhereItStands) {
    // Losing every factory must not make a tank wander: with nowhere to go, the honest decision is
    // to stay put, and it is the decision a test has to state or the behaviour gets invented by
    // accident.
    const Observation observation = field({}, {unit(50, UnitKind::Tank, Side::Player, 200, 200)});
    EXPECT_EQ(decide(unit(kTank, UnitKind::Tank, Side::Enemy, 40, 40), observation, UtilityTuning{})
                  .intent,
              Intent::Hold);
}

TEST(Utility, ATurretHoldsItsPostAndNeverGoesHunting) {
    const Observation observation = field({}, {unit(50, UnitKind::Tank, Side::Player, 12, 12)});
    const Decision decision = decide(unit(kTurret, UnitKind::Turret, Side::Enemy, 10, 10, 150),
                                     observation, UtilityTuning{});
    EXPECT_EQ(decision.intent, Intent::Hold);
    EXPECT_NE(decision.intent, Intent::Engage);
}

TEST(Utility, ATurretIsNotWithdrawnEvenWhenItIsNearlyDead) {
    // A turret that runs away is a turret that leaves the road open, and it cannot outrun a tank.
    const Observation observation = field({}, {unit(50, UnitKind::Tank, Side::Player, 12, 12)});
    const Decision decision = decide(unit(kTurret, UnitKind::Turret, Side::Enemy, 10, 10, 3),
                                     observation, UtilityTuning{});
    EXPECT_NE(decision.intent, Intent::Retreat);
    EXPECT_NE(decision.intent, Intent::Reposition);
}

TEST(Utility, AFactoryHasNothingToDecide) {
    const Observation observation = field({}, {unit(50, UnitKind::Tank, Side::Player, 12, 12)});
    const Decision decision = decide(unit(kFactory, UnitKind::Factory, Side::Enemy, 10, 10, 400),
                                     observation, UtilityTuning{});
    EXPECT_EQ(decision.intent, Intent::Hold);
}

TEST(Utility, HomeFactoryIsTheLowestIdLivingOne) {
    const Observation observation = field({unit(9, UnitKind::Factory, Side::Enemy, 5, 5),
                                           unit(4, UnitKind::Factory, Side::Enemy, 7, 7)},
                                          {});
    const UnitView* home = home_factory(observation);
    ASSERT_NE(home, nullptr);
    EXPECT_EQ(home->id, UnitId{4});
    EXPECT_EQ(home_factory(field({}, {})), nullptr);
}

// ---------------------------------------------------------------------------------------------
// Advance to contact: the posture that makes an opponent an opponent
// ---------------------------------------------------------------------------------------------

namespace {

/// Own base at home, an enemy base far away, and nothing within engagement range of anyone.
[[nodiscard]] Observation two_bases(int home_x, int home_y, int enemy_x, int enemy_y) {
    return field({unit(kFactory, UnitKind::Factory, Side::Enemy, home_x, home_y),
                  unit(kTank, UnitKind::Tank, Side::Enemy, home_x, home_y)},
                 {unit(90, UnitKind::Factory, Side::Player, enemy_x, enemy_y)});
}

} // namespace

TEST(Utility, ATankWithNothingInRangeAdvancesTowardTheEnemyBase) {
    // The defect this posture exists for: without it, a utility commander never leaves its own
    // half, and two of them never fight -- measured, 0 shots in 2400 ticks on ten seeds.
    UtilityTuning tuning{};
    const Observation observation = two_bases(10, 10, 110, 10);
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
    const Observation observation = two_bases(10, 10, 110, 10);
    const Decision decision
        = decide(unit(kTank, UnitKind::Tank, Side::Enemy, 60, 10, 5), observation, tuning);
    ASSERT_EQ(decision.intent, Intent::Retreat);
    EXPECT_EQ(decision.x, 10) << "away from the enemy, not toward it";
}

TEST(Utility, AnEnemyInRangeStillBeatsAdvancing) {
    UtilityTuning tuning{};
    EXPECT_EQ(tuning.engage_range_tiles, 14) << "this test's numbers assume the default range";
    Observation observation = two_bases(10, 10, 110, 10);
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
    const Observation observation = field({unit(kFactory, UnitKind::Factory, Side::Enemy, 10, 10)},
                                          {unit(50, UnitKind::Tank, Side::Player, 200, 200)});
    const Decision decision
        = decide(unit(kTank, UnitKind::Tank, Side::Enemy, 40, 40), observation, tuning);
    EXPECT_EQ(decision.intent, Intent::Reposition);
    EXPECT_EQ(decision.x, 10);
}

TEST(Utility, TheEnemyBaseIsTheLowestIdLivingEnemyFactory) {
    const Observation observation = field({}, {unit(9, UnitKind::Factory, Side::Player, 5, 5),
                                               unit(4, UnitKind::Factory, Side::Player, 7, 7)});
    const UnitView* base = enemy_factory(observation);
    ASSERT_NE(base, nullptr);
    EXPECT_EQ(base->id, UnitId{4});
    EXPECT_EQ(enemy_factory(field({}, {})), nullptr);
}

TEST(Utility, AdvancingIsDeterministic) {
    UtilityTuning tuning{};
    const Observation observation = two_bases(10, 10, 110, 10);
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
    const Observation observation = field({unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10)},
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
    const Observation observation = field({unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10),
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
    const Observation observation = field({unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10)},
                                          {unit(50, UnitKind::Tank, Side::Player, 16, 10),
                                           unit(51, UnitKind::Turret, Side::Player, 17, 10)});
    std::vector<sim::Order> orders;
    commander.think(observation, orders);
    ASSERT_FALSE(orders.empty());
    for (const sim::Order& order : orders) {
        EXPECT_EQ(order.unit, UnitId{kTank})
            << "only its own unit may be ordered, id " << order.unit.value;
        if (order.kind == sim::OrderKind::Attack) {
            EXPECT_NE(order.target, UnitId{kTank});
            EXPECT_TRUE(order.target == UnitId{50} || order.target == UnitId{51});
        }
    }
}

TEST(Utility, TheDecisionsItReportsAreTheOrdersItIssued) {
    UtilityTuning tuning{};
    tuning.cycle_ticks = 1;
    UtilityCommander commander{tuning};
    const Observation observation = field({unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10)},
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
    EXPECT_EQ(orders.front().kind, sim::OrderKind::Attack);
    EXPECT_EQ(orders.front().target, UnitId{50});
}

TEST(Utility, HoldingMeansNoOrderForATankAndAGuardOrderForATurret) {
    // The name of this test used to promise "no order at all", which was not true of a turret: a
    // turret holds by being told to guard the tile it stands on, since it has no stance of its own
    // to keep. The delegated arm noticed the contradiction and reported it instead of bending the
    // code; this is the version that says what it means, and checks both halves.
    UtilityTuning tuning{};
    tuning.cycle_ticks = 1;
    UtilityCommander commander{tuning};
    const Observation observation = field({unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10),
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
            EXPECT_EQ(order.kind, sim::OrderKind::GuardAt);
            EXPECT_EQ(order.x, sim::Fixed::from_int(12));
            EXPECT_EQ(order.y, sim::Fixed::from_int(9));
        }
    }
    for (const sim::Order& order : orders) {
        EXPECT_NE(order.kind, sim::OrderKind::Stop)
            << "Stop is a decision the player makes, not one the AI needs";
    }
}

TEST(Utility, ThinkingTwiceOnTheSameObservationDecidesTheSameThing) {
    UtilityTuning tuning{};
    tuning.cycle_ticks = 1;
    UtilityCommander commander{tuning};
    const Observation observation = field({unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10),
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
        EXPECT_EQ(first[i].kind, second[i].kind);
        EXPECT_EQ(first[i].target, second[i].target);
        EXPECT_EQ(first[i].x, second[i].x);
        EXPECT_EQ(first[i].y, second[i].y);
    }
}

TEST(Utility, EveryUnitGetsADecisionEvenWithNoEnemiesAndNoBase) {
    UtilityTuning tuning{};
    tuning.cycle_ticks = 1;
    UtilityCommander commander{tuning};
    const Observation observation = field({unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10),
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
    const Observation observation = field({unit(kFactory, UnitKind::Factory, Side::Enemy, 30, 44),
                                           unit(kTank, UnitKind::Tank, Side::Enemy, 10, 10, 4)},
                                          {unit(50, UnitKind::Tank, Side::Player, 12, 10)});
    std::vector<sim::Order> orders;
    commander.think(observation, orders);
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().kind, sim::OrderKind::MoveTo);
    EXPECT_EQ(orders.front().unit, UnitId{kTank});
    EXPECT_EQ(orders.front().x, sim::Fixed::from_int(30));
    EXPECT_EQ(orders.front().y, sim::Fixed::from_int(44));
}

} // namespace protekt::ai
