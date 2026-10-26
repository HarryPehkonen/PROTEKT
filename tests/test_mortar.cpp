#include "protekt/sim/orders.hpp"
#include "protekt/sim/projectile.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

/// R3b-1: the mortar's lob and splash, and the rocket's heavy single-target bolt, as mechanics. No
/// roster fields either yet, so every shot here is made by hand with `launch` and flown with
/// `advance_projectiles`.
namespace protekt::sim {
namespace {

constexpr std::uint64_t kSeed = 42;

struct Level {
    int x{};
    int y{};
    bool found{};
};

/// The first tile of a row (and the row beside it) of `length` + 1 tiles within six height units of
/// each other, none a crater: ground on which a lob's arithmetic is not tangled with a hill.
[[nodiscard]] Level find_level_row(int length) {
    constexpr int kTolerance = 6;
    for (int y = 5; y < kMapTiles - 5; ++y) {
        for (int x = 5; x + length < kMapTiles - 5; ++x) {
            const int h0 = world::sample(kSeed, x, y).height;
            bool level = true;
            for (int i = 0; i <= length && level; ++i) {
                const world::Tile t = world::sample(kSeed, x + i, y);
                const world::Tile below = world::sample(kSeed, x + i, y + 1);
                level = t.terrain != world::Terrain::Crater && std::abs(t.height - h0) <= kTolerance
                        && std::abs(below.height - h0) <= kTolerance;
            }
            if (level) {
                return Level{x, y, true};
            }
        }
    }
    return Level{};
}

[[nodiscard]] Unit make_unit(std::uint32_t id, UnitKind kind, Side side, Fixed x, Fixed y) {
    Unit unit{};
    unit.id = UnitId{id};
    unit.kind = kind;
    unit.side = side;
    unit.x = x;
    unit.y = y;
    unit.hp = spec_of(kind).hp;
    return unit;
}

/// A unit `dx` / `dy` raw tiles from (`x`, `y`).
[[nodiscard]] Unit make_unit_at(std::uint32_t id, UnitKind kind, Side side, Level at, int dx_raw,
                                int dy_raw) {
    return make_unit(id, kind, side, Fixed::from_raw(at.x * Fixed::kOne + dx_raw),
                     Fixed::from_raw(at.y * Fixed::kOne + dy_raw));
}

constexpr int kTile = Fixed::kOne;

// --- the spec rows -----------------------------------------------------------------------------

TEST(Mortar, SpecRowIsPinnedToExactIntegers) {
    const UnitSpec& s = spec_of(UnitKind::Mortar);
    EXPECT_EQ(static_cast<int>(UnitKind::Mortar), 4);
    EXPECT_STREQ(s.name, "mortar");
    EXPECT_EQ(s.hp, 75);
    EXPECT_EQ(s.max_speed.raw, 6553);
    EXPECT_EQ(s.range.raw, 14 * kTile);
    EXPECT_EQ(s.damage, 16);
    EXPECT_EQ(s.cooldown, 45);
    EXPECT_EQ(s.bolt_speed.raw, 17476);
    EXPECT_EQ(s.arc.raw, 4 * kTile);
    EXPECT_EQ(s.max_travel.raw, 16 * kTile);
    EXPECT_EQ(s.splash_radius.raw, 98304); // 1.5 tiles
    EXPECT_EQ(s.gravity.raw, 147456);      // 2.25 height units per tick squared
    EXPECT_FALSE(s.buildable);
}

TEST(Mortar, OutranksATankAndIsOutrangedByATurret) {
    EXPECT_GT(spec_of(UnitKind::Mortar).range.raw, spec_of(UnitKind::Tank).range.raw);
    EXPECT_LT(spec_of(UnitKind::Mortar).range.raw, spec_of(UnitKind::Turret).range.raw);
}

TEST(Rocket, SpecRowIsPinnedToExactIntegers) {
    const UnitSpec& s = spec_of(UnitKind::Rocket);
    EXPECT_EQ(static_cast<int>(UnitKind::Rocket), 5);
    EXPECT_STREQ(s.name, "rocket");
    EXPECT_EQ(s.hp, 100);
    EXPECT_EQ(s.max_speed.raw, 13107);
    EXPECT_EQ(s.range.raw, 15 * kTile);
    EXPECT_EQ(s.damage, 24);
    EXPECT_EQ(s.cooldown, 75);
    EXPECT_EQ(s.bolt_speed.raw, 26214);
    EXPECT_EQ(s.arc.raw, 2 * kTile);
    EXPECT_EQ(s.max_travel.raw, 16 * kTile);
    EXPECT_EQ(s.splash_radius.raw, 0);
    EXPECT_EQ(s.gravity.raw, 0);
    EXPECT_FALSE(s.buildable);
}

TEST(Mortar, ExistingKindsKeepTheOldRangeAndNoSplashOrGravity) {
    for (const UnitKind kind :
         {UnitKind::Tank, UnitKind::Turret, UnitKind::Factory, UnitKind::HoverTank}) {
        const UnitSpec& s = spec_of(kind);
        EXPECT_EQ(s.max_travel.raw, 16 * kTile) << s.name;
        EXPECT_EQ(s.splash_radius.raw, 0) << s.name;
        EXPECT_EQ(s.gravity.raw, 0) << s.name;
    }
}

// Replaced (R3b2): was NeitherNewKindIsInEitherRoster -- R3b-1 kept them off the field; R3b2
// puts exactly one of each on every side.
TEST(Mortar, EachRosterCarriesExactlyOneOfEachNewKind) {
    for (const Side side : {Side::Player, Side::Enemy}) {
        int mortars = 0;
        int rockets = 0;
        for (const UnitKind kind : side_roster(default_data(), side)) {
            mortars += kind == UnitKind::Mortar ? 1 : 0;
            rockets += kind == UnitKind::Rocket ? 1 : 0;
        }
        EXPECT_EQ(mortars, 1);
        EXPECT_EQ(rockets, 1);
    }
    const Scenario s = skirmish(7);
    for (const Side side : {Side::Player, Side::Enemy}) {
        int mortars = 0;
        int rockets = 0;
        for (const Spawn& spawn : s.spawns) {
            mortars += spawn.side == side && spawn.kind == UnitKind::Mortar ? 1 : 0;
            rockets += spawn.side == side && spawn.kind == UnitKind::Rocket ? 1 : 0;
        }
        EXPECT_EQ(mortars, 1);
        EXPECT_EQ(rockets, 1);
    }
}

// --- gravity zero: an existing flight is what it was -------------------------------------------

TEST(Mortar, ATankBoltFliesTickForTickAsItAlwaysDid) {
    // Pinned from the code before gravity existed: the tank at (100, 100) shoots the tank at
    // (108, 100) on seed 42. The bolt dives along a straight line, hits on the 15th step.
    std::vector<Unit> units{
        make_unit(1, UnitKind::Tank, Side::Player, Fixed::from_int(100), Fixed::from_int(100)),
        make_unit(2, UnitKind::Tank, Side::Enemy, Fixed::from_int(108), Fixed::from_int(100))};
    std::vector<Projectile> bolts{launch(units[0], units[1], 8, kSeed)};
    std::vector<Impact> impacts;
    ASSERT_EQ(bolts[0].gravity.raw, 0);
    EXPECT_EQ(bolts[0].vx.raw, 34952);
    EXPECT_EQ(bolts[0].vz.raw, -1070405);
    EXPECT_EQ(bolts[0].z.raw, 16633036);
    constexpr std::array<std::int32_t, 14> expected{{15562631, 14492226, 13421821, 12351416,
                                                     11281011, 10210606, 9140201, 8069796, 6999391,
                                                     5928986, 4858581, 3788176, 2717771, 1647366}};
    for (const std::int32_t z : expected) {
        advance_projectiles(bolts, units, impacts, kSeed);
        ASSERT_EQ(bolts.size(), 1U);
        EXPECT_EQ(bolts[0].z.raw, z);
        EXPECT_EQ(bolts[0].vz.raw, -1070405) << "no gravity: vz never changes";
    }
    advance_projectiles(bolts, units, impacts, kSeed);
    EXPECT_TRUE(bolts.empty());
    EXPECT_EQ(units[1].hp, 92);
}

// --- the lob -----------------------------------------------------------------------------------

TEST(Mortar, ALobRisesAboveTheStraightLineAndComesDownAtTheAimPoint) {
    const Level at = find_level_row(12);
    ASSERT_TRUE(at.found);
    std::vector<Unit> units{make_unit_at(1, UnitKind::Mortar, Side::Player, at, 0, 0),
                            make_unit_at(2, UnitKind::Tank, Side::Enemy, at, 7 * kTile, 0)};
    const Fixed ground = ground_height(kSeed, units[0].x, units[0].y);
    std::vector<Projectile> bolts{launch(units[0], units[1], 16, kSeed)};
    std::vector<Impact> impacts;
    ASSERT_EQ(bolts.size(), 1U);
    const std::int32_t z_launch = bolts[0].z.raw;
    EXPECT_EQ(z_launch, ground.raw + kMuzzleHeight.raw) << "leaves the muzzle like any bolt";
    EXPECT_EQ(bolts[0].gravity.raw, 147456);
    EXPECT_EQ(bolts[0].splash_radius.raw, 98304);
    EXPECT_EQ(bolts[0].vx.raw, 17476) << "half the speed of a tank bolt";

    // The target's centre: ground + ride + half a body. The straight line from the muzzle to it.
    const std::int32_t z_aim = ground_height(kSeed, units[1].x, units[1].y).raw + kRideHeight.raw
                               + (kBodyHeight.raw / 2);
    std::int32_t peak = z_launch;
    std::int32_t z_before_end = 0;
    int ticks = 0;
    std::int32_t z_mid = 0;
    while (!bolts.empty() && ticks < 80) {
        z_before_end = bolts[0].z.raw;
        advance_projectiles(bolts, units, impacts, kSeed);
        ++ticks;
        if (!bolts.empty()) {
            peak = std::max(peak, bolts[0].z.raw);
            if (ticks == 13) {
                z_mid = bolts[0].z.raw;
            }
        }
    }
    // Mid-flight (13 of ~26 ticks) the bolt is far above the line between muzzle and aim point
    // (which on level ground is at the muzzle's own height): about 190 height units, 3 tiles.
    const std::int32_t line_mid = z_launch + (z_aim - z_launch) / 2;
    EXPECT_GT(z_mid - line_mid, 180 * Fixed::kOne);
    EXPECT_LT(z_mid - line_mid, 200 * Fixed::kOne);
    EXPECT_GT(peak - z_launch, 180 * Fixed::kOne);
    // It came down: the aim point is reached on step 26, the bolt hit the body and is gone.
    EXPECT_EQ(ticks, 26);
    EXPECT_LT(z_before_end - z_aim, 40 * Fixed::kOne) << "last seen a step above the aim point";
    EXPECT_GE(z_before_end, z_aim);
    EXPECT_EQ(units[1].hp, 100 - 16) << "and it landed on the tank it was aimed at";
}

// --- splash ------------------------------------------------------------------------------------

/// Fires a mortar at the first enemy (id 2) and flies it until it is gone.
void fly(std::vector<Unit>& units, std::vector<Impact>& impacts) {
    std::vector<Projectile> bolts{launch(units[0], units[1], 16, kSeed)};
    for (int i = 0; i < 80 && !bolts.empty(); ++i) {
        advance_projectiles(bolts, units, impacts, kSeed);
    }
    ASSERT_TRUE(bolts.empty());
}

TEST(Mortar, SplashHurtsEveryEnemyInTheRadiusAndNoOneBeyondIt) {
    const Level at = find_level_row(12);
    ASSERT_TRUE(at.found);
    std::vector<Unit> units{
        make_unit_at(1, UnitKind::Mortar, Side::Player, at, 0, 0),
        make_unit_at(2, UnitKind::Tank, Side::Enemy, at, 7 * kTile, 0),             // the aim
        make_unit_at(3, UnitKind::Tank, Side::Enemy, at, 7 * kTile, kTile),         // 1 tile
        make_unit_at(4, UnitKind::Tank, Side::Enemy, at, 7 * kTile + 2 * kTile, 0), // 2 tiles
    };
    std::vector<Impact> impacts;
    fly(units, impacts);
    EXPECT_EQ(units[1].hp, 84) << "the direct hit";
    EXPECT_EQ(units[2].hp, 84) << "one tile away: inside 1.5";
    EXPECT_EQ(units[3].hp, 100) << "two tiles away: outside it";
    ASSERT_EQ(impacts.size(), 2U);
    EXPECT_EQ(impacts[0].victim.value, 2U) << "the direct hit first, then the splash in id order";
    EXPECT_EQ(impacts[1].victim.value, 3U);
}

TEST(Mortar, SplashHurtsAFriendlyInTheRadiusToo) {
    const Level at = find_level_row(12);
    ASSERT_TRUE(at.found);
    std::vector<Unit> units{
        make_unit_at(1, UnitKind::Mortar, Side::Player, at, 0, 0),
        make_unit_at(2, UnitKind::Tank, Side::Enemy, at, 7 * kTile, 0),
        make_unit_at(3, UnitKind::Tank, Side::Player, at, 7 * kTile, kTile), // friendly, 1 tile
    };
    std::vector<Impact> impacts;
    fly(units, impacts);
    EXPECT_EQ(units[1].hp, 84);
    EXPECT_EQ(units[2].hp, 84) << "friendly fire is real";
    EXPECT_EQ(units[0].hp, 75) << "the mortar itself is seven tiles off";
}

TEST(Mortar, AShellThatMissesEveryoneStillBurstsOnTheGround) {
    const Level at = find_level_row(12);
    ASSERT_TRUE(at.found);
    // The aimed-at tank is dead, so the shell passes through where it stood and lands there.
    std::vector<Unit> units{
        make_unit_at(1, UnitKind::Mortar, Side::Player, at, 0, 0),
        make_unit_at(2, UnitKind::Tank, Side::Enemy, at, 7 * kTile, 0),
        make_unit_at(3, UnitKind::Tank, Side::Enemy, at, 7 * kTile, kTile),
    };
    std::vector<Projectile> bolts{launch(units[0], units[1], 16, kSeed)};
    units[1].alive = false;
    std::vector<Impact> impacts;
    for (int i = 0; i < 80 && !bolts.empty(); ++i) {
        advance_projectiles(bolts, units, impacts, kSeed);
    }
    EXPECT_EQ(units[2].hp, 84) << "splashed by a ground burst";
    ASSERT_EQ(impacts.size(), 1U);
}

TEST(Mortar, ABoltWithoutASplashRadiusNeverSplashes) {
    const Level at = find_level_row(12);
    ASSERT_TRUE(at.found);
    std::vector<Unit> units{
        make_unit_at(1, UnitKind::Rocket, Side::Player, at, 0, 0),
        make_unit_at(2, UnitKind::Tank, Side::Enemy, at, 8 * kTile, 0),
        make_unit_at(3, UnitKind::Tank, Side::Enemy, at, 8 * kTile, kTile / 2 + 1000),
    };
    std::vector<Projectile> bolts{launch(units[0], units[1], 24, kSeed)};
    ASSERT_EQ(bolts[0].gravity.raw, 0);
    ASSERT_EQ(bolts[0].vx.raw, 26214);
    std::vector<Impact> impacts;
    for (int i = 0; i < 80 && !bolts.empty(); ++i) {
        advance_projectiles(bolts, units, impacts, kSeed);
    }
    EXPECT_EQ(units[1].hp, 76) << "the rocket hits for 24";
    EXPECT_EQ(units[2].hp, 100) << "a unit beside the hit is untouched";
    ASSERT_EQ(impacts.size(), 1U);
}

TEST(Mortar, TwoSameSeedBoltsFlyIdentically) {
    const Level at = find_level_row(12);
    ASSERT_TRUE(at.found);
    auto run = [&] {
        std::vector<Unit> units{make_unit_at(1, UnitKind::Mortar, Side::Player, at, 0, 0),
                                make_unit_at(2, UnitKind::Tank, Side::Enemy, at, 9 * kTile, 0)};
        std::vector<Projectile> bolts{launch(units[0], units[1], 16, kSeed)};
        std::vector<Impact> impacts;
        std::vector<std::int32_t> zs;
        while (!bolts.empty()) {
            advance_projectiles(bolts, units, impacts, kSeed);
            zs.push_back(bolts.empty() ? -1 : bolts[0].z.raw);
        }
        return zs;
    };
    EXPECT_EQ(run(), run());
}

// --- M5: the ground-burst record --------------------------------------------------------------

TEST(Landing, ALobOntoEmptyGroundReportsATouchdownTileAndNothingElseDoes) {
    // The case the record exists for: a shell that lands on nothing makes no Impact, so without a
    // Landing the sim would say nothing happened.
    const Level at = find_level_row(16);
    ASSERT_TRUE(at.found);
    const int far_y = at.y < 100 ? at.y + 40 : at.y - 40;
    Sim sim = Sim::create(Scenario{
        kSeed,
        {{UnitKind::Tank, Side::Player, at.x, at.y}, {UnitKind::Tank, Side::Enemy, at.x, far_y}}});
    const UnitId tank = sim.units()[0].id;
    for (int tick = 0; tick < profile(UnitKind::Tank).cooldown_ticks; ++tick) {
        sim.step(); // a new unit starts with its gun reloading
    }
    for (int quiet = 0; quiet < 5; ++quiet) {
        sim.step();
        EXPECT_TRUE(sim.landings().empty()) << "ground nobody shot at reports nothing";
    }

    sim.enqueue(Order{tank, orders::FireLob{Fixed::from_int(1), Fixed::zero(), UnitId{},
                                            Fixed::from_raw(44800)}}); // ~0.68 climb a tile
    std::vector<Landing> seen;
    int landed_on = -1;
    for (int tick = 0; tick < 200 && landed_on < 0; ++tick) {
        sim.step();
        for (const Landing& landing : sim.landings()) {
            seen.push_back(landing);
            landed_on = tick;
        }
    }
    ASSERT_EQ(seen.size(), 1U) << "one shell, one Landing";
    EXPECT_EQ(seen[0].shooter, tank);
    EXPECT_FALSE(seen[0].hit) << "nothing was under it";
    EXPECT_TRUE(seen[0].lob);
    const Unit* gun = sim.find(tank);
    ASSERT_NE(gun, nullptr);
    EXPECT_EQ(seen[0].tile_y, gun->y.floor_int()) << "fired along +x";
    EXPECT_GE(seen[0].tile_x, gun->x.floor_int() + 2) << "a lob goes some way";
    EXPECT_LE(seen[0].tile_x, gun->x.floor_int() + 14) << "and no further than the mortar's range";
    EXPECT_TRUE(sim.impacts().empty()) << "no unit was hit";
    EXPECT_TRUE(sim.projectiles().empty());

    sim.step();
    EXPECT_TRUE(sim.landings().empty()) << "cleared at the start of the next step";
}

} // namespace
} // namespace protekt::sim
