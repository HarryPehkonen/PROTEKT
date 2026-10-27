#include "protekt/sim/pellets.hpp"
#include "protekt/sim/projectile.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/world/world.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

/// The shotgun: a fixed fan of pellets about the aim, bounded by a cone that is data, with no RNG
/// and no trigonometry in it. Every number is read from the spec or the function under test --
/// there is no second table to drift from.
namespace protekt::sim {
namespace {

constexpr int kTile = Fixed::kOne;

[[nodiscard]] Fixed2D aim_east() noexcept { return Fixed2D{Fixed::from_int(1), Fixed::zero()}; }

/// A cone whose half-angle has a tangent of a quarter: about 14 degrees.
const Fixed kQuarter = Fixed::from_raw(kTile / 4);

TEST(Shotgun, TheSameInputsProduceTheSamePelletsEveryTime) {
    const PelletFan a = pellet_fan(aim_east(), 5, kQuarter);
    const PelletFan b = pellet_fan(aim_east(), 5, kQuarter);
    ASSERT_EQ(a.count, b.count);
    for (int i = 0; i < a.count; ++i) {
        const std::size_t at = static_cast<std::size_t>(i);
        EXPECT_EQ(a.dir.at(at).x.raw, b.dir.at(at).x.raw) << "pellet " << i;
        EXPECT_EQ(a.dir.at(at).y.raw, b.dir.at(at).y.raw) << "pellet " << i;
    }
}

TEST(Shotgun, OnePelletIsTheAimDirectionItself) {
    const PelletFan fan = pellet_fan(aim_east(), 1, kQuarter);
    ASSERT_EQ(fan.count, 1);
    EXPECT_EQ(fan.dir.at(0).x.raw, kTile) << "the aim, normalised";
    EXPECT_EQ(fan.dir.at(0).y.raw, 0);
}

TEST(Shotgun, FivePelletsAreSymmetricAboutTheAim) {
    const PelletFan fan = pellet_fan(aim_east(), 5, kQuarter);
    ASSERT_EQ(fan.count, 5);
    EXPECT_EQ(fan.dir.at(0).x.raw, fan.dir.at(4).x.raw) << "mirror images share their x";
    EXPECT_EQ(fan.dir.at(0).y.raw, -fan.dir.at(4).y.raw) << "and negate their y";
    EXPECT_EQ(fan.dir.at(1).x.raw, fan.dir.at(3).x.raw);
    EXPECT_EQ(fan.dir.at(1).y.raw, -fan.dir.at(3).y.raw);
    EXPECT_EQ(fan.dir.at(2).y.raw, 0) << "the middle pellet is on the aim axis";
    EXPECT_LT(fan.dir.at(0).y.raw, fan.dir.at(1).y.raw) << "the fan opens outwards";
}

TEST(Shotgun, EveryPelletIsAboutUnitLength) {
    for (const int count : {1, 2, 3, 5, 8, kMaxPellets}) {
        const PelletFan fan = pellet_fan(aim_east(), count, kQuarter);
        ASSERT_EQ(fan.count, count);
        for (int i = 0; i < fan.count; ++i) {
            const Fixed2D d = fan.dir.at(static_cast<std::size_t>(i));
            const std::int64_t dx = d.x.raw;
            const std::int64_t dy = d.y.raw;
            const std::int64_t len = isqrt(dx * dx + dy * dy);
            EXPECT_GE(len, kTile - kTile / 50) << "count " << count << " pellet " << i;
            EXPECT_LE(len, kTile + kTile / 50) << "count " << count << " pellet " << i;
        }
    }
}

TEST(Shotgun, TheCountIsClampedToTheArray) {
    EXPECT_EQ(pellet_fan(aim_east(), kMaxPellets + 5, kQuarter).count, kMaxPellets);
    EXPECT_EQ(pellet_fan(aim_east(), 0, kQuarter).count, 1);
    EXPECT_EQ(pellet_fan(aim_east(), -3, kQuarter).count, 1);
    EXPECT_EQ(pellet_fan(aim_east(), 1, kQuarter).count, 1);
}

// --- the cone is a bound, not a hope ------------------------------------------------------------

TEST(Shotgun, NoPelletLeavesTheConeForAnyAimOrCount) {
    // Cross and dot of a pellet against the aim give its tangent off the axis; the cone says that
    // may not exceed the half-tangent. Two raw units of slack: the normalising division truncates.
    const std::array<Fixed2D, 4> aims = {{{Fixed::from_int(1), Fixed::zero()},
                                          {Fixed::zero(), Fixed::from_int(-3)},
                                          {Fixed::from_raw(12345), Fixed::from_raw(-54321)},
                                          {Fixed::from_int(-7), Fixed::from_int(7)}}};
    for (const Fixed2D aim : aims) {
        for (int count = 1; count <= kMaxPellets; ++count) {
            const PelletFan fan = pellet_fan(aim, count, kQuarter);
            for (int i = 0; i < fan.count; ++i) {
                const Fixed2D d = fan.dir.at(static_cast<std::size_t>(i));
                const std::int64_t cross = static_cast<std::int64_t>(aim.x.raw) * d.y.raw
                                           - static_cast<std::int64_t>(aim.y.raw) * d.x.raw;
                const std::int64_t dot = static_cast<std::int64_t>(aim.x.raw) * d.x.raw
                                         + static_cast<std::int64_t>(aim.y.raw) * d.y.raw;
                ASSERT_GT(dot, 0) << "a pellet may never fly backwards";
                const std::int64_t slack = 2 * (std::llabs(aim.x.raw) + std::llabs(aim.y.raw));
                EXPECT_LE(std::llabs(cross) * Fixed::kOne, kQuarter.raw * dot + slack * Fixed::kOne)
                    << "count " << count << " pellet " << i;
            }
        }
    }
}

TEST(Shotgun, TheOutermostPelletsSitOnTheEdgeOfTheCone) {
    const PelletFan fan = pellet_fan(aim_east(), 5, kQuarter);
    // tan = y / x of the outer pellet, to within the normalising division's truncation.
    const std::int64_t tangent
        = static_cast<std::int64_t>(fan.dir.at(4).y.raw) * kTile / fan.dir.at(4).x.raw;
    EXPECT_LE(std::llabs(tangent - kQuarter.raw), 4);
}

TEST(Shotgun, AWiderCountDoesNotWidenTheCone) {
    const PelletFan few = pellet_fan(aim_east(), 3, kQuarter);
    const PelletFan many = pellet_fan(aim_east(), kMaxPellets, kQuarter);
    EXPECT_LE(std::abs(few.dir.at(2).y.raw - many.dir.at(kMaxPellets - 1).y.raw), 4);
}

// --- the weapon ---------------------------------------------------------------------------------

constexpr std::uint64_t kSeed = 42;

TEST(Shotgun, TheSpecRowIsAFlatShortRangedFan) {
    const UnitSpec& s = spec_of(UnitKind::Shotgun);
    EXPECT_EQ(static_cast<int>(UnitKind::Shotgun), 10) << "appended: no earlier index moves";
    EXPECT_EQ(s.gravity.raw, 0) << "flat, or a hill cannot eat its pellets";
    EXPECT_GT(s.pellets, 1);
    EXPECT_LE(s.pellets, kMaxPellets);
    EXPECT_GT(s.cone_half_tan.raw, 0);
    EXPECT_GT(s.max_travel.raw, s.range.raw) << "a stray pellet outlives the aim, as a bolt does";
    EXPECT_LT(s.max_travel.raw, spec_of(UnitKind::Tank).max_travel.raw) << "short of reach";
}

struct Level {
    int x{};
    int y{};
    bool found{};
};

/// A row of `length` + 1 tiles level to within twelve height units, none a crater, with the rows
/// beside it level too: ground on which a flat pellet's arithmetic is not tangled with a hill.
[[nodiscard]] Level find_level_row(int length) {
    for (int y = 5; y < kMapTiles - 6; ++y) {
        for (int x = 5; x + length < kMapTiles - 5; ++x) {
            const int h0 = world::sample(kSeed, x, y).height;
            bool level = true;
            for (int i = 0; i <= length && level; ++i) {
                for (int row = -1; row <= 1; ++row) {
                    const world::Tile t = world::sample(kSeed, x + i, y + row);
                    level = level && t.terrain != world::Terrain::Crater
                            && std::abs(t.height - h0) <= 12;
                }
            }
            if (level) {
                return Level{x, y, true};
            }
        }
    }
    return Level{};
}

[[nodiscard]] Unit make_unit(std::uint32_t id, UnitKind kind, Side side, Level at, int dx_raw,
                             int dy_raw) {
    Unit unit{};
    unit.id = UnitId{id};
    unit.kind = kind;
    unit.side = side;
    unit.x = Fixed::from_raw(at.x * kTile + dx_raw);
    unit.y = Fixed::from_raw(at.y * kTile + dy_raw);
    unit.hp = spec_of(kind).hp;
    return unit;
}

struct Volley {
    std::vector<Unit> units;
    std::vector<Impact> impacts;
    std::vector<Landing> landings;
    std::size_t pellets{};
};

/// The shotgun at units[0] fires level along +x; the pellets then fly until none is left.
[[nodiscard]] Volley fire_east(std::vector<Unit> units) {
    Volley v{};
    v.units = std::move(units);
    std::vector<Projectile> bolts;
    const Projectile aimed
        = launch_towards(v.units[0], Fixed::from_int(1), Fixed::zero(), Fixed::zero(), UnitId{},
                         spec_of(UnitKind::Shotgun).damage, kSeed);
    fan_out(aimed, UnitKind::Shotgun, bolts);
    v.pellets = bolts.size();
    for (int tick = 0; tick < 400 && !bolts.empty(); ++tick) {
        advance_projectiles(bolts, v.units, v.impacts, v.landings, kSeed);
    }
    return v;
}

[[nodiscard]] std::uint16_t lost(const Volley& v, std::size_t index) {
    return static_cast<std::uint16_t>(spec_of(v.units[index].kind).hp - v.units[index].hp);
}

TEST(Shotgun, AShotIsAsManyPelletsAsTheSpecSays) {
    const Level at = find_level_row(12);
    ASSERT_TRUE(at.found);
    const Volley v = fire_east({make_unit(1, UnitKind::Shotgun, Side::Player, at, 0, 0)});
    EXPECT_EQ(v.pellets, static_cast<std::size_t>(spec_of(UnitKind::Shotgun).pellets));
}

TEST(Shotgun, ATargetInsideTheConeAtCloseRangeTakesDamageAndFarOutOfReachDoesNot) {
    const Level at = find_level_row(12);
    ASSERT_TRUE(at.found);
    const Volley close = fire_east({make_unit(1, UnitKind::Shotgun, Side::Player, at, 0, 0),
                                    make_unit(2, UnitKind::Tank, Side::Enemy, at, 2 * kTile, 0)});
    const Volley medium = fire_east({make_unit(1, UnitKind::Shotgun, Side::Player, at, 0, 0),
                                     make_unit(2, UnitKind::Tank, Side::Enemy, at, 6 * kTile, 0)});
    const Volley far = fire_east({make_unit(1, UnitKind::Shotgun, Side::Player, at, 0, 0),
                                  make_unit(2, UnitKind::Tank, Side::Enemy, at, 11 * kTile, 0)});
    EXPECT_GT(lost(close, 1), 0);
    EXPECT_GT(lost(medium, 1), 0);
    EXPECT_GT(lost(close, 1), lost(medium, 1)) << "vicinity: more pellets reach the near target";
    EXPECT_EQ(lost(far, 1), 0) << "beyond max_travel nothing arrives";
    EXPECT_TRUE(far.impacts.empty());
}

TEST(Shotgun, ATargetOffTheAxisButInsideTheConeIsHitAndOneOutsideIsNot) {
    const Level at = find_level_row(12);
    ASSERT_TRUE(at.found);
    // Four tiles out, 0.7 tiles north: a tangent of 0.175, inside the 0.25 cone.
    const Volley inside
        = fire_east({make_unit(1, UnitKind::Shotgun, Side::Player, at, 0, 0),
                     make_unit(2, UnitKind::Tank, Side::Enemy, at, 4 * kTile, kTile * 7 / 10)});
    // The same distance, 2.5 tiles north: a tangent of 0.625, far outside it.
    const Volley outside
        = fire_east({make_unit(1, UnitKind::Shotgun, Side::Player, at, 0, 0),
                     make_unit(2, UnitKind::Tank, Side::Enemy, at, 4 * kTile, kTile * 5 / 2)});
    EXPECT_GT(lost(inside, 1), 0) << "you need not aim exactly at the target";
    EXPECT_EQ(lost(outside, 1), 0);
    EXPECT_TRUE(outside.impacts.empty());
}

/// A tile pair `reach` tiles apart on one row, level at both ends, with a rise between that a
/// muzzle-height shot cannot clear. Found by scanning, not asserted.
[[nodiscard]] Level find_hill_between(int reach) {
    for (int y = 5; y < kMapTiles - 5; ++y) {
        for (int x = 5; x + reach < kMapTiles - 5; ++x) {
            const std::int64_t base
                = ground_height(kSeed, Fixed::from_int(x), Fixed::from_int(y)).raw;
            const std::int64_t end
                = ground_height(kSeed, Fixed::from_int(x + reach), Fixed::from_int(y)).raw;
            if (std::llabs(end - base) > static_cast<std::int64_t>(6) * kTile) {
                continue;
            }
            for (int i = 1; i < reach * 4; ++i) {
                const int px_raw = x * kTile + i * kTile / 4;
                const Fixed px = Fixed::from_raw(px_raw);
                if (ground_height(kSeed, px, Fixed::from_int(y)).raw
                    > base + kMuzzleHeight.raw + static_cast<std::int64_t>(4) * kTile) {
                    return Level{x, y, true};
                }
            }
        }
    }
    return Level{};
}

TEST(Shotgun, AHillBetweenShooterAndTargetEatsEveryPellet) {
    const Level at = find_hill_between(6);
    ASSERT_TRUE(at.found) << "seed 42 offers no hill to hide behind in six tiles";
    const Volley v = fire_east({make_unit(1, UnitKind::Shotgun, Side::Player, at, 0, 0),
                                make_unit(2, UnitKind::Tank, Side::Enemy, at, 6 * kTile, 0)});
    EXPECT_EQ(lost(v, 1), 0);
    EXPECT_TRUE(v.impacts.empty()) << "pellets stopped by ground report no impact";
}

TEST(Shotgun, ALargeFriendlyInThePathTakesThePelletsAndShieldsTheEnemyBehindIt) {
    const Level at = find_level_row(12);
    ASSERT_TRUE(at.found);
    const Volley open = fire_east({make_unit(1, UnitKind::Shotgun, Side::Player, at, 0, 0),
                                   make_unit(2, UnitKind::Tank, Side::Enemy, at, 5 * kTile, 0)});
    const Volley shielded
        = fire_east({make_unit(1, UnitKind::Shotgun, Side::Player, at, 0, 0),
                     make_unit(2, UnitKind::Tank, Side::Enemy, at, 5 * kTile, 0),
                     make_unit(3, UnitKind::Factory, Side::Player, at, 3 * kTile, 0)});
    ASSERT_GT(lost(open, 1), 0) << "the control: unshielded, the enemy is hit";
    EXPECT_GT(lost(shielded, 2), 0) << "friendly fire: the shot hurts its own side";
    EXPECT_LT(lost(shielded, 1), lost(open, 1)) << "and the hull shields what is behind it";
}

// --- friendly fire in flight, for every weapon ---------------------------------------------------

TEST(Shotgun, AnOrdinaryBoltNowHitsAFriendlyInItsWay) {
    const Level at = find_level_row(12);
    ASSERT_TRUE(at.found);
    std::vector<Unit> units{make_unit(1, UnitKind::Tank, Side::Player, at, 0, 0),
                            make_unit(2, UnitKind::Factory, Side::Player, at, 3 * kTile, 0)};
    std::vector<Projectile> bolts{launch_towards(units[0], Fixed::from_int(1), Fixed::zero(),
                                                 Fixed::zero(), UnitId{}, 8, kSeed)};
    std::vector<Impact> impacts;
    for (int tick = 0; tick < 100 && !bolts.empty(); ++tick) {
        advance_projectiles(bolts, units, impacts, kSeed);
    }
    EXPECT_LT(units[1].hp, spec_of(UnitKind::Factory).hp) << "the rule is the rule";
}

TEST(Shotgun, ABoltNeverHitsADeadUnitOrItsOwnShooter) {
    const Level at = find_level_row(12);
    ASSERT_TRUE(at.found);
    std::vector<Unit> units{make_unit(1, UnitKind::Tank, Side::Player, at, 0, 0),
                            make_unit(2, UnitKind::Factory, Side::Player, at, 3 * kTile, 0)};
    units[1].alive = false;
    std::vector<Projectile> bolts{launch_towards(units[0], Fixed::from_int(1), Fixed::zero(),
                                                 Fixed::zero(), UnitId{}, 8, kSeed)};
    std::vector<Impact> impacts;
    for (int tick = 0; tick < 100 && !bolts.empty(); ++tick) {
        advance_projectiles(bolts, units, impacts, kSeed);
    }
    EXPECT_TRUE(impacts.empty());
    EXPECT_EQ(units[0].hp, spec_of(UnitKind::Tank).hp);
}

// --- determinism ---------------------------------------------------------------------------------

[[nodiscard]] Sim shotgun_duel() {
    Scenario scenario{};
    scenario.seed = kSeed;
    const auto add = [&](Side side, UnitKind kind, int tile_x) {
        Spawn spawn{};
        spawn.side = side;
        spawn.kind = kind;
        spawn.tile_x = tile_x;
        spawn.tile_y = 100;
        scenario.spawns.push_back(spawn);
    };
    add(Side::Player, UnitKind::Shotgun, 100);
    add(Side::Enemy, UnitKind::Factory, 103);
    return Sim::create(scenario);
}

TEST(Shotgun, TheSameShotFromTheSameStateHasTheSameImpactsAndHash) {
    Sim a = shotgun_duel();
    Sim b = shotgun_duel();
    EXPECT_EQ(a.state_hash(), b.state_hash());
    bool any_impact = false;
    for (int tick = 0; tick < 120; ++tick) {
        a.step();
        b.step();
        ASSERT_EQ(a.state_hash(), b.state_hash()) << "tick " << tick;
        ASSERT_EQ(a.impacts().size(), b.impacts().size());
        any_impact = any_impact || !a.impacts().empty();
    }
    EXPECT_TRUE(any_impact) << "the duel must actually fire pellets";
}

TEST(Shotgun, ASimWithNoShotgunHashesAsItDidBeforeTheKindExisted) {
    // The kind adds no per-unit or per-bolt hashed state: a pellet is an ordinary flat bolt. Two
    // shotgun-free sims differing only in an unused spec cannot diverge, so pin the property that
    // matters: a tank-only battle's hash is a pure function of the battle.
    Scenario scenario{};
    scenario.seed = kSeed;
    Spawn tank{};
    tank.side = Side::Player;
    tank.kind = UnitKind::Tank;
    tank.tile_x = 100;
    tank.tile_y = 100;
    scenario.spawns.push_back(tank);
    tank.side = Side::Enemy;
    tank.tile_x = 106;
    scenario.spawns.push_back(tank);
    Sim a = Sim::create(scenario);
    Sim b = Sim::create(scenario);
    for (int tick = 0; tick < 200; ++tick) {
        a.step();
        b.step();
    }
    EXPECT_EQ(a.state_hash(), b.state_hash());
}

} // namespace
} // namespace protekt::sim
