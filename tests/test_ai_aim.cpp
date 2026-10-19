#include "protekt/sim/projectile.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace protekt::sim {
namespace {

constexpr UnitId kGun{1};
constexpr UnitId kMark{2};
constexpr UnitId kOther{3};

constexpr std::int32_t kTenthTile = Fixed::kOne / 10; ///< 0.1 tiles per tick, crossing the line

[[nodiscard]] Unit bare(UnitId id, Side side, int tile_x, int tile_y, std::int32_t vy_raw) {
    Unit unit{};
    unit.id = id;
    unit.kind = UnitKind::Tank;
    unit.side = side;
    unit.x = Fixed::from_int(tile_x);
    unit.y = Fixed::from_int(tile_y);
    unit.vy = Fixed::from_raw(vy_raw);
    unit.hp = kTankHp;
    return unit;
}

[[nodiscard]] std::int64_t length(Fixed x, Fixed y) {
    return isqrt(static_cast<std::int64_t>(x.raw) * x.raw
                 + static_cast<std::int64_t>(y.raw) * y.raw);
}

/// A range with no sim: the gun at (100,100), a mark twelve tiles east that drives north at
/// `mark_vy`, and a spare enemy far away. The test moves the units itself, one tick at a time.
struct Range {
    std::vector<Unit> units;
    explicit Range(std::int32_t mark_vy)
        : units{bare(kGun, Side::Player, 100, 100, 0), bare(kMark, Side::Enemy, 112, 100, mark_vy),
                bare(kOther, Side::Enemy, 100, 140, 0)} {}
    Unit& gun() { return units[0]; }
    Unit& mark() { return units[1]; }
    void put_mark_at_start() {
        mark().x = Fixed::from_int(112);
        mark().y = Fixed::from_int(100);
    }
};

struct Result {
    bool hit{};
    std::int64_t miss{}; ///< closest the bolt came to the mark, raw tiles, sampled each tick
};

/// One shot the way the sim fires it: retarget, launch, then fly it while the mark drives on at
/// its velocity. The gun's estimate is learned inside advance_projectiles, as in a battle.
/// `teleport_to_y`, when set, jumps the mark there the moment the bolt leaves the gun.
[[nodiscard]] Result shoot(Range& range, int teleport_to_y = -1) {
    retarget_aim(range.gun(), kMark);
    std::vector<Projectile> bolts{launch(range.gun(), range.mark(), kTankDamage)};
    if (teleport_to_y >= 0) {
        range.mark().y = Fixed::from_int(teleport_to_y);
    }
    std::vector<Impact> impacts;
    const std::uint16_t hp = range.mark().hp;
    Result result{false, INT64_MAX};
    for (int tick = 1; tick <= 60 && !bolts.empty(); ++tick) {
        range.mark().x = Fixed::from_raw(range.mark().x.raw + range.mark().vx.raw);
        range.mark().y = Fixed::from_raw(range.mark().y.raw + range.mark().vy.raw);
        const Projectile before = bolts.front();
        advance_projectiles(bolts, range.units, impacts);
        const std::int64_t dx
            = static_cast<std::int64_t>(before.x.raw) + before.vx.raw - range.mark().x.raw;
        const std::int64_t dy
            = static_cast<std::int64_t>(before.y.raw) + before.vy.raw - range.mark().y.raw;
        result.miss = std::min(result.miss, isqrt(dx * dx + dy * dy));
    }
    result.hit = range.mark().hp < hp;
    if (result.hit) {
        range.mark().hp = hp; // so the next shot's hit is a drop again
    }
    return result;
}

} // namespace

TEST(AiAim, AFreshGunMissesACrossingTarget) {
    Range crossing(kTenthTile);
    EXPECT_EQ(crossing.gun().aim_x.raw, 0);
    EXPECT_EQ(crossing.gun().aim_y.raw, 0);
    EXPECT_FALSE(shoot(crossing).hit) << "a fresh gun hit a target crossing its line of fire";

    // The control: the same gun, the same range, a target that holds still, is hit first time.
    Range still(0);
    EXPECT_TRUE(shoot(still).hit) << "a fresh gun missed a stationary target";
}

TEST(AiAim, TheEstimateConvergesOnASteadyTarget) {
    Range range(kTenthTile);
    std::vector<Result> shots;
    for (int shot = 0; shot < 4; ++shot) {
        range.put_mark_at_start();
        shots.push_back(shoot(range));
    }
    // One correction is enough against a steady course; after that the gun hits, and a hit costs
    // nothing, so the estimate (and the miss, which is within the hit radius) holds still.
    EXPECT_FALSE(shots[0].hit);
    EXPECT_GT(shots[0].miss, shots[1].miss);
    EXPECT_TRUE(shots[1].hit) << "the first correction did not land the second shot";
    EXPECT_TRUE(shots[2].hit) << "still missing on the third shot";
    EXPECT_TRUE(shots[3].hit);
    // The lead it learned is the drift over the flight: a little over two tiles, ahead (north).
    EXPECT_GT(range.gun().aim_y.raw, 2 * Fixed::kOne);
    EXPECT_LT(range.gun().aim_y.raw, 3 * Fixed::kOne);
}

TEST(AiAim, ChangingSpeedMakesItMissAgain) {
    Range range(kTenthTile);
    for (int shot = 0; shot < 4; ++shot) {
        range.put_mark_at_start();
        (void)shoot(range);
    }
    range.put_mark_at_start();
    ASSERT_TRUE(shoot(range).hit) << "the control is wrong: the gun should have converged";

    // The evasion: the target changes speed. The estimate is a flight-time stale, so it misses.
    range.mark().vy = Fixed::from_raw(2 * kTenthTile);
    range.put_mark_at_start();
    EXPECT_FALSE(shoot(range).hit) << "the gun hit a target that changed speed";

    range.mark().vy = Fixed::from_raw(-kTenthTile); // and a reversal is missed too
    range.put_mark_at_start();
    // (the correction from the faster run was applied meanwhile, so it is now a different miss)
    EXPECT_FALSE(shoot(range).hit) << "the gun hit a target that reversed";
}

TEST(AiAim, TheEstimateIsClamped) {
    Range range(0);
    bool reached_cap = false;
    for (int shot = 0; shot < 6; ++shot) {
        range.put_mark_at_start();
        // The mark jumps eight tiles north the moment the bolt leaves: a miss bigger than the cap,
        // every time, always the same way.
        (void)shoot(range, 108);
        EXPECT_LE(length(range.gun().aim_x, range.gun().aim_y), kMaxAimLeadRaw);
        reached_cap
            = reached_cap || length(range.gun().aim_x, range.gun().aim_y) >= kMaxAimLeadRaw - 2;
    }
    EXPECT_TRUE(reached_cap) << "the estimate never grew: the test is not winding it up";
}

TEST(AiAim, SwitchingTargetsZerosTheEstimate) {
    Range range(kTenthTile);
    (void)shoot(range);
    ASSERT_NE(range.gun().aim_y.raw, 0) << "nothing was learned, so there is nothing to zero";
    ASSERT_EQ(range.gun().aim_target, kMark);

    retarget_aim(range.gun(), kMark); // the same target: the belief stays
    EXPECT_NE(range.gun().aim_y.raw, 0);

    retarget_aim(range.gun(), kOther);
    EXPECT_EQ(range.gun().aim_x.raw, 0);
    EXPECT_EQ(range.gun().aim_y.raw, 0);
    EXPECT_EQ(range.gun().aim_target, kOther);

    // And a target that goes away leaves nothing behind either.
    range.gun().aim_y = Fixed::from_int(2);
    retarget_aim(range.gun(), UnitId{});
    EXPECT_EQ(range.gun().aim_y.raw, 0);
    EXPECT_FALSE(range.gun().aim_target.valid());
}

TEST(AiAim, ALessonAboutAnotherTargetIsDiscarded) {
    // A bolt in the air when the gun changed its mind teaches the new target nothing.
    Range range(kTenthTile);
    retarget_aim(range.gun(), kMark);
    std::vector<Projectile> bolts{launch(range.gun(), range.mark(), kTankDamage)};
    retarget_aim(range.gun(), kOther);
    std::vector<Impact> impacts;
    for (int tick = 1; tick <= 60 && !bolts.empty(); ++tick) {
        range.mark().y = Fixed::from_raw(range.mark().y.raw + range.mark().vy.raw);
        advance_projectiles(bolts, range.units, impacts);
    }
    EXPECT_EQ(range.gun().aim_x.raw, 0);
    EXPECT_EQ(range.gun().aim_y.raw, 0);
}

TEST(AiAim, AHitCostsNoCorrection) {
    Range range(0);
    ASSERT_TRUE(shoot(range).hit);
    EXPECT_EQ(range.gun().aim_x.raw, 0);
    EXPECT_EQ(range.gun().aim_y.raw, 0);

    // Converged: the estimate holds the same lead through the hit that follows.
    Range moving(kTenthTile);
    for (int shot = 0; shot < 4; ++shot) {
        moving.put_mark_at_start();
        (void)shoot(moving);
    }
    const Fixed learned = moving.gun().aim_y;
    moving.put_mark_at_start();
    ASSERT_TRUE(shoot(moving).hit);
    EXPECT_EQ(moving.gun().aim_y.raw, learned.raw);
}

TEST(AiAim, ThePlayersManualShotNeitherLeadsNorLearns) {
    Range range(kTenthTile);
    Projectile bolt
        = launch_towards(range.gun(), Fixed::from_int(1), Fixed::zero(), kMark, kTankDamage);
    EXPECT_EQ(bolt.vy.raw, 0) << "the manual shot was led";
    range.gun().aim_target = kMark;
    std::vector<Projectile> bolts{bolt};
    std::vector<Impact> impacts;
    for (int tick = 1; tick <= 60 && !bolts.empty(); ++tick) {
        range.mark().y = Fixed::from_raw(range.mark().y.raw + range.mark().vy.raw);
        advance_projectiles(bolts, range.units, impacts);
    }
    EXPECT_EQ(range.gun().aim_x.raw, 0);
    EXPECT_EQ(range.gun().aim_y.raw, 0) << "a manual shot taught the AI gun";
}

namespace {

/// A player tank ordered to attack an enemy tank that is driving north, across its line of fire.
[[nodiscard]] Sim crossing_battle() {
    Scenario scenario{};
    scenario.seed = 7;
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Player, 100, 100});
    scenario.spawns.push_back(Spawn{UnitKind::Tank, Side::Enemy, 108, 100});
    Sim sim = Sim::create(scenario);
    sim.enqueue(Order{kGun, orders::Attack{kMark}});
    sim.enqueue(Order{kMark, orders::MoveTo{Fixed::from_int(108), Fixed::from_int(108)}});
    return sim;
}

} // namespace

TEST(AiAim, TheSimLearnsThroughItsOwnGuns) {
    Sim sim = crossing_battle();
    bool learned = false;
    bool first_shot_landed = false;
    bool fired = false;
    for (int tick = 0; tick < 300 && !learned; ++tick) {
        sim.step();
        for (const Impact& impact : sim.impacts()) {
            if (impact.shooter == kGun && !fired) {
                first_shot_landed = true;
            }
        }
        fired = fired || !sim.shots().empty();
        learned = sim.find(kGun)->aim_y.raw != 0 || sim.find(kGun)->aim_x.raw != 0;
    }
    EXPECT_TRUE(learned) << "a sim-fired gun never updated its estimate";
    EXPECT_FALSE(first_shot_landed);
    EXPECT_EQ(sim.find(kGun)->aim_target, kMark);
}

TEST(AiAim, TheEstimateHistoryIsDeterministicAndHashed) {
    // Two runs of the same battle give the same estimate at every tick and the same hash. What is
    // relied on is both: the estimate compared directly, and state_hash(), which hashes the
    // estimate and the bolts' closest-approach fields (the golden and replay suites then pin the
    // whole stream against recorded values). There is no RNG to seed: the estimate is a pure
    // function of the battle so far.
    Sim first = crossing_battle();
    Sim second = crossing_battle();
    bool ever_learned = false;
    for (int tick = 0; tick < 200; ++tick) {
        first.step();
        second.step();
        ASSERT_EQ(first.find(kGun)->aim_x.raw, second.find(kGun)->aim_x.raw) << tick;
        ASSERT_EQ(first.find(kGun)->aim_y.raw, second.find(kGun)->aim_y.raw) << tick;
        ASSERT_EQ(first.state_hash(), second.state_hash()) << tick;
        ever_learned
            = ever_learned || first.find(kGun)->aim_x.raw != 0 || first.find(kGun)->aim_y.raw != 0;
    }
    EXPECT_TRUE(ever_learned) << "the battle never moved the estimate: nothing was compared";
}

} // namespace protekt::sim
