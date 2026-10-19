#include "protekt/sim/observer.hpp"
#include "protekt/sim/projectile.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

/// A shot takes time to arrive. The sim owns the bolt: damage lands when it gets there, it keeps
/// flying if its target dies, and it fizzles at range.
namespace protekt::sim {
namespace {

constexpr std::uint64_t kSeed = 42;

struct Place {
    Side side{};
    UnitKind kind{};
    int tile_x{};
    int tile_y{};
};

[[nodiscard]] Sim make(const std::vector<Place>& places) {
    Scenario scenario{};
    scenario.seed = kSeed;
    for (const Place& place : places) {
        Spawn spawn{};
        spawn.side = place.side;
        spawn.kind = place.kind;
        spawn.tile_x = place.tile_x;
        spawn.tile_y = place.tile_y;
        scenario.spawns.push_back(spawn);
    }
    return Sim::create(scenario);
}

constexpr UnitId kFirst{1};
constexpr UnitId kSecond{2};

/// One tank-gun step, returning the events it produced.
[[nodiscard]] std::vector<BattleEvent> step_events(Sim& sim) {
    const UnitSnapshot before = sim.units();
    const Tick tick = sim.tick();
    sim.step();
    return events_between(before, sim, tick);
}

/// Steps until the first bolt is in the air; returns how many steps that took. 0 if none came.
[[nodiscard]] int step_until_fired(Sim& sim, int limit = 200) {
    for (int step = 1; step <= limit; ++step) {
        sim.step();
        if (!sim.projectiles().empty()) {
            return step;
        }
    }
    return 0;
}

[[nodiscard]] std::int64_t speed_squared(const Projectile& p) {
    return static_cast<std::int64_t>(p.vx.raw) * p.vx.raw
           + static_cast<std::int64_t>(p.vy.raw) * p.vy.raw;
}

} // namespace

TEST(Projectile, TimeOfFlightDamageDoesNotLandOnTheFiringTick) {
    // A player tank twelve tiles from an enemy factory: the very edge of its range.
    Sim sim = make(
        {{Side::Player, UnitKind::Tank, 100, 100}, {Side::Enemy, UnitKind::Factory, 112, 100}});
    const std::uint16_t full = sim.find(kSecond)->hp;
    const int fired_at = step_until_fired(sim);
    ASSERT_GT(fired_at, 0) << "the gun never fired";
    EXPECT_EQ(sim.find(kSecond)->hp, full) << "the damage landed on the tick the gun fired";

    int arrived_at = 0;
    for (int flight = 1; flight <= 60 && arrived_at == 0; ++flight) {
        sim.step();
        if (sim.find(kSecond)->hp < full) {
            arrived_at = flight;
        }
    }
    ASSERT_GT(arrived_at, 0) << "the bolt never arrived";
    EXPECT_GT(arrived_at, 5) << "twelve tiles at kProjectileSpeed is not instant";
    EXPECT_LE(arrived_at, 30) << "a shot at the edge of range takes about a second";
}

TEST(Projectile, ArrivalAppliesExactlyItsDamageAndIsRemoved) {
    Sim sim = make(
        {{Side::Player, UnitKind::Tank, 100, 100}, {Side::Enemy, UnitKind::Factory, 106, 100}});
    const std::uint16_t full = sim.find(kSecond)->hp;
    ASSERT_GT(step_until_fired(sim), 0);
    ASSERT_EQ(sim.projectiles().size(), 1U);
    EXPECT_EQ(sim.projectiles().front().damage, kTankDamage);
    EXPECT_EQ(sim.projectiles().front().shooter, kFirst);

    for (int flight = 0; flight < 60 && sim.find(kSecond)->hp == full; ++flight) {
        sim.step();
    }
    EXPECT_EQ(sim.find(kSecond)->hp, full - kTankDamage);
    EXPECT_TRUE(sim.projectiles().empty()) << "a bolt that landed is gone";
    ASSERT_EQ(sim.impacts().size(), 1U);
    EXPECT_EQ(sim.impacts().front().shooter, kFirst);
    EXPECT_EQ(sim.impacts().front().victim, kSecond);
    EXPECT_EQ(sim.impacts().front().victim_hp, full - kTankDamage);
}

TEST(Projectile, TheFiredEventComesWithTheGunAndHitTakenWithTheBolt) {
    Sim sim = make(
        {{Side::Player, UnitKind::Tank, 100, 100}, {Side::Enemy, UnitKind::Factory, 106, 100}});
    int fired = 0;
    int hit = 0;
    int fired_tick = -1;
    int hit_tick = -1;
    for (int step = 0; step < 100; ++step) {
        const Tick tick = sim.tick();
        for (const BattleEvent& event : step_events(sim)) {
            if (event.kind == EventKind::Fired && fired++ == 0) {
                fired_tick = static_cast<int>(tick);
                EXPECT_EQ(event.actor, kFirst);
                EXPECT_EQ(event.subject, kSecond);
            }
            if (event.kind == EventKind::HitTaken && hit++ == 0) {
                hit_tick = static_cast<int>(tick);
                EXPECT_EQ(event.actor, kFirst);
                EXPECT_EQ(event.subject, kSecond);
                EXPECT_EQ(event.subject_hp, sim.find(kSecond)->hp);
            }
        }
    }
    ASSERT_GE(fired, 1);
    ASSERT_GE(hit, 1);
    EXPECT_LT(fired_tick, hit_tick) << "the hit comes after the shot";
}

TEST(Projectile, KeepsFlyingWhenItsTargetDiesBeforeItArrives) {
    // Thirteen tanks close to the enemy tank kill it (13 x 8 >= 100) a few ticks after the volley;
    // the watcher, twelve tiles away, fired at the same instant and its bolt is still in the air.
    std::vector<Place> places{{Side::Player, UnitKind::Tank, 100, 100},
                              {Side::Enemy, UnitKind::Tank, 112, 100}};
    for (int i = 0; i < 13; ++i) {
        places.push_back({Side::Player, UnitKind::Tank, 113 + i % 3, 103 + i / 3});
    }
    Sim sim = make(places);
    const UnitId watcher = kFirst;
    const UnitId target = kSecond;
    const auto watchers_bolt = [&sim, watcher]() -> const Projectile* {
        for (const Projectile& p : sim.projectiles()) {
            if (p.shooter == watcher) {
                return &p;
            }
        }
        return nullptr;
    };

    ASSERT_GT(step_until_fired(sim), 0);
    ASSERT_NE(watchers_bolt(), nullptr);
    const std::int64_t length = isqrt(speed_squared(*watchers_bolt()));
    const std::int64_t expiry_age = (kProjectileRange.raw + length - 1) / length;
    int target_died_at = 0;
    int bolt_gone_at = 0;
    std::uint32_t last_age = 0;
    for (int step = 1; step <= 120 && bolt_gone_at == 0; ++step) {
        const std::vector<BattleEvent> events = step_events(sim);
        for (const BattleEvent& event : events) {
            EXPECT_FALSE(event.kind == EventKind::HitTaken && event.actor == watcher)
                << "the watcher's bolt hurt something at step " << step;
        }
        if (target_died_at == 0 && !sim.find(target)->alive) {
            target_died_at = step;
        }
        if (watchers_bolt() == nullptr) {
            bolt_gone_at = step;
        } else {
            last_age = watchers_bolt()->age;
        }
    }
    ASSERT_GT(target_died_at, 0) << "the volley never killed the target";
    ASSERT_GT(bolt_gone_at, 0) << "the bolt never left the air";
    EXPECT_GT(bolt_gone_at, target_died_at + 1)
        << "the bolt vanished with its target instead of flying on";
    // It was removed at range and for no other reason: it flew every tick of its range.
    EXPECT_EQ(static_cast<std::int64_t>(last_age) + 1, expiry_age);
    EXPECT_EQ(sim.find(target)->hp, 0);
}

TEST(Projectile, ABoltFiredAtNothingFizzlesAtRangeWithNoHitTaken) {
    // The enemy tank stands still when the gun fires, then drives off north at right angles to the
    // line of fire: the bolt was aimed at where it WAS, and the bolt does not follow, so it flies
    // on into empty ground. (Driven off BEFORE the shot, the lead would have caught it.)
    Sim sim
        = make({{Side::Player, UnitKind::Tank, 100, 100}, {Side::Enemy, UnitKind::Tank, 108, 100}});
    ASSERT_GT(step_until_fired(sim), 0);
    sim.enqueue(Order{kSecond, orders::MoveTo{Fixed::from_int(108), Fixed::from_int(20)}});
    const std::uint16_t full = sim.find(kSecond)->hp;
    const std::int64_t length = isqrt(speed_squared(sim.projectiles().front()));
    ASSERT_GT(length, 0);
    const std::int64_t expiry_age = (kProjectileRange.raw + length - 1) / length;

    // The first bolt is the oldest, so it stays at the front until it goes.
    std::uint32_t last_age = sim.projectiles().front().age;
    bool gone = false;
    for (int step = 0; step < 200 && !gone; ++step) {
        const std::vector<BattleEvent> events = step_events(sim);
        // The enemy tank fired back before it was ordered away; only the first gun's bolt is in
        // question here.
        for (const BattleEvent& event : events) {
            EXPECT_FALSE(event.kind == EventKind::HitTaken && event.actor == kFirst);
        }
        for (const Impact& impact : sim.impacts()) {
            EXPECT_NE(impact.shooter, kFirst);
        }
        gone = sim.projectiles().empty() || sim.projectiles().front().age <= last_age;
        if (!gone) {
            last_age = sim.projectiles().front().age;
        }
    }
    ASSERT_TRUE(gone) << "the bolt never expired";
    EXPECT_EQ(static_cast<std::int64_t>(last_age) + 1, expiry_age)
        << "it did not fizzle at kProjectileRange";
    EXPECT_EQ(sim.find(kSecond)->hp, full);
}

TEST(Projectile, AMovingTankSendsItsOwnVelocityWithTheBolt) {
    // A tank driving "north" (towards smaller y) with the target to its east, level with where the
    // tank will be when it fires: the bolt goes east AND north.
    Sim moving = make(
        {{Side::Player, UnitKind::Tank, 100, 100}, {Side::Enemy, UnitKind::Factory, 108, 97}});
    moving.enqueue(Order{kFirst, orders::MoveTo{Fixed::from_int(100), Fixed::from_int(20)}});
    ASSERT_GT(step_until_fired(moving), 0);
    const Projectile& shot = moving.projectiles().front();
    const Unit& tank = *moving.find(kFirst);
    ASSERT_LT(tank.vy.raw, 0) << "the tank is not driving north";
    EXPECT_GT(shot.vx.raw, 0) << "fired east";
    EXPECT_LT(shot.vy.raw, 0) << "the bolt did not inherit the tank's northward drive";

    // Take the tank's velocity out: what is left is the aim, at kProjectileSpeed, towards the
    // target.
    const std::int64_t ax = static_cast<std::int64_t>(shot.vx.raw) - tank.vx.raw;
    const std::int64_t ay = static_cast<std::int64_t>(shot.vy.raw) - tank.vy.raw;
    const std::int64_t aim = isqrt(ax * ax + ay * ay);
    EXPECT_NEAR(static_cast<double>(aim), static_cast<double>(kProjectileSpeedRaw), 4.0);
    const std::int64_t tx = static_cast<std::int64_t>(moving.find(kSecond)->x.raw) - tank.x.raw;
    const std::int64_t ty = static_cast<std::int64_t>(moving.find(kSecond)->y.raw) - tank.y.raw;
    const double off_line
        = static_cast<double>(ax * ty - ay * tx) / static_cast<double>(isqrt(tx * tx + ty * ty));
    EXPECT_NEAR(off_line, 0.0, 8.0) << "the aim does not point at the target";
}

TEST(Projectile, AStationaryTankSendsOnlyTheFiredComponent) {
    Sim still = make(
        {{Side::Player, UnitKind::Tank, 100, 100}, {Side::Enemy, UnitKind::Factory, 108, 100}});
    ASSERT_GT(step_until_fired(still), 0);
    const Projectile& shot = still.projectiles().front();
    EXPECT_GT(shot.vx.raw, 0);
    EXPECT_EQ(shot.vy.raw, 0) << "nothing to inherit and nothing sideways in the aim";
    EXPECT_NEAR(static_cast<double>(shot.vx.raw), static_cast<double>(kProjectileSpeedRaw), 2.0);
}

TEST(Projectile, PassesThroughItsOwnSide) {
    // Our factory stands in the line of fire, four tiles out; the enemy factory is eight.
    Sim sim = make({{Side::Player, UnitKind::Tank, 100, 100},
                    {Side::Player, UnitKind::Factory, 104, 100},
                    {Side::Enemy, UnitKind::Factory, 108, 100}});
    const UnitId friendly{2};
    const UnitId enemy{3};
    const std::uint16_t friendly_hp = sim.find(friendly)->hp;
    const std::uint16_t enemy_hp = sim.find(enemy)->hp;
    for (int step = 0; step < 120; ++step) {
        sim.step();
        EXPECT_EQ(sim.find(friendly)->hp, friendly_hp) << "the bolt hurt its own side";
    }
    EXPECT_LT(sim.find(enemy)->hp, enemy_hp) << "the bolt did not get through to the enemy";
}

TEST(Projectile, TheSameBattleGivesTheSameBoltsAndTheSameHash) {
    const auto run = [] {
        std::vector<Place> places;
        for (int i = 0; i < 4; ++i) {
            places.push_back({Side::Player, UnitKind::Tank, 90 + i, 100 + i});
            places.push_back({Side::Enemy, UnitKind::Tank, 104 + i, 100 + i});
        }
        places.push_back({Side::Enemy, UnitKind::Turret, 110, 102});
        Sim sim = make(places);
        std::vector<std::vector<Projectile>> seen;
        bool bolts_flew = false;
        for (Tick tick = 0; tick < 600; ++tick) {
            if (tick == 10) {
                sim.enqueue(
                    Order{UnitId{1}, orders::MoveTo{Fixed::from_int(96), Fixed::from_int(90)}});
                sim.enqueue(Order{UnitId{3}, orders::Attack{UnitId{2}}});
            }
            sim.step();
            seen.push_back(sim.projectiles());
            bolts_flew = bolts_flew || !sim.projectiles().empty();
        }
        EXPECT_TRUE(bolts_flew) << "the scenario never put a bolt in the air";
        return std::make_pair(seen, sim.state_hash());
    };
    const auto a = run();
    const auto b = run();
    EXPECT_EQ(a.first, b.first);
    EXPECT_EQ(a.second, b.second);
}

TEST(Projectile, TheBoltsAreInTheStateHash) {
    // Two sims identical in every unit field but one with a bolt in the air must hash differently.
    Sim in_air = make(
        {{Side::Player, UnitKind::Tank, 100, 100}, {Side::Enemy, UnitKind::Factory, 112, 100}});
    Sim copy = in_air;
    ASSERT_GT(step_until_fired(in_air), 0);
    ASSERT_FALSE(in_air.projectiles().empty());
    const std::uint64_t with_bolt = in_air.state_hash();
    Sim other = copy;
    other.step();
    EXPECT_NE(with_bolt, other.state_hash());
    EXPECT_NE(in_air.state_hash(), copy.state_hash());
}

namespace {

/// A bare unit for aiming tests: no sim, so the test moves it itself, one tick at a time.
[[nodiscard]] Unit bare(UnitId id, Side side, int tile_x, int tile_y, std::int32_t vx_raw,
                        std::int32_t vy_raw) {
    Unit unit{};
    unit.id = id;
    unit.kind = UnitKind::Tank;
    unit.side = side;
    unit.x = Fixed::from_int(tile_x);
    unit.y = Fixed::from_int(tile_y);
    unit.vx = Fixed::from_raw(vx_raw);
    unit.vy = Fixed::from_raw(vy_raw);
    unit.hp = kTankHp;
    return unit;
}

constexpr std::int32_t kTenthTile = Fixed::kOne / 10; ///< a slow tank: 0.1 tiles per tick
constexpr UnitId kGun{1};
constexpr UnitId kMark{2};

/// Flies one bolt for up to 60 ticks while `mark` drives on at its velocity. `reverse_at` > 0
/// turns it round that many ticks into the flight. Returns the mark's hit points afterwards.
[[nodiscard]] std::uint16_t fly(Projectile bolt, Unit mark, int reverse_at = 0) {
    std::vector<Unit> units{bare(kGun, Side::Player, 0, 0, 0, 0), mark};
    std::vector<Projectile> bolts{bolt};
    std::vector<Impact> impacts;
    for (int tick = 1; tick <= 60 && !bolts.empty(); ++tick) {
        if (tick == reverse_at) {
            units[1].vx = Fixed::from_raw(-units[1].vx.raw);
            units[1].vy = Fixed::from_raw(-units[1].vy.raw);
        }
        units[1].x = Fixed::from_raw(units[1].x.raw + units[1].vx.raw);
        units[1].y = Fixed::from_raw(units[1].y.raw + units[1].vy.raw);
        advance_projectiles(bolts, units, impacts);
    }
    return units[1].hp;
}

} // namespace

TEST(Projectile, LeadHitsWhatAStraightShotMisses) {
    // Twelve tiles off, crossing the line of fire at 0.1 tiles per tick: it drifts over two tiles
    // while the bolt is in the air, against a hit radius of half a tile.
    const Unit gun = bare(kGun, Side::Player, 100, 100, 0, 0);
    const Unit mark = bare(kMark, Side::Enemy, 112, 100, 0, kTenthTile);

    Unit still_copy = mark; // what a gun that ignored the drift would have aimed at
    still_copy.vx = Fixed::zero();
    still_copy.vy = Fixed::zero();
    EXPECT_EQ(fly(launch(gun, still_copy, kTankDamage), mark), kTankHp)
        << "the control is wrong: an unled shot should miss this target";

    EXPECT_LT(fly(launch(gun, mark, kTankDamage), mark), kTankHp)
        << "the led shot did not damage a crossing target";
    EXPECT_EQ(fly(launch(gun, mark, kTankDamage), mark), kTankHp - kTankDamage);
}

TEST(Projectile, AStationaryTargetIsUnaffectedByLead) {
    const Unit gun = bare(kGun, Side::Player, 100, 100, 0, 0);
    const Unit mark = bare(kMark, Side::Enemy, 112, 105, 0, 0);
    const Projectile bolt = launch(gun, mark, kTankDamage);

    // The aim is the plain direction to the target at kProjectileSpeed: the lead term is zero.
    const std::int64_t dx = static_cast<std::int64_t>(mark.x.raw) - gun.x.raw;
    const std::int64_t dy = static_cast<std::int64_t>(mark.y.raw) - gun.y.raw;
    const std::int64_t dist = isqrt(dx * dx + dy * dy);
    EXPECT_EQ(bolt.vx.raw, dx * kProjectileSpeedRaw / dist);
    EXPECT_EQ(bolt.vy.raw, dy * kProjectileSpeedRaw / dist);
    EXPECT_EQ(bolt.aimed_at, kMark);
    EXPECT_EQ(fly(bolt, mark), kTankHp - kTankDamage);
}

TEST(Projectile, OwnMotionStillThrowsTheShot) {
    // A gun driving sideways (north, at 0.2 tiles per tick) at a target dead ahead to the east.
    const Unit gun = bare(kGun, Side::Player, 100, 100, 0, -Fixed::kOne / 5);
    const Unit mark = bare(kMark, Side::Enemy, 112, 100, 0, 0);
    const Projectile bolt = launch(gun, mark, kTankDamage);
    EXPECT_EQ(bolt.vy.raw, gun.vy.raw) << "the bolt lost the gun's own velocity";
    EXPECT_GT(bolt.vx.raw, 0);
    EXPECT_EQ(fly(bolt, mark), kTankHp) << "the shot corrected itself for the gun's motion";
}

TEST(Projectile, ATargetThatReversesIsMissed) {
    const Unit gun = bare(kGun, Side::Player, 100, 100, 0, 0);
    const Unit mark = bare(kMark, Side::Enemy, 112, 100, 0, kTenthTile);
    const Projectile bolt = launch(gun, mark, kTankDamage);
    ASSERT_EQ(fly(bolt, mark), kTankHp - kTankDamage) << "the control shot should land";
    EXPECT_EQ(fly(bolt, mark, 5), kTankHp) << "the bolt followed a target that turned round";
}

TEST(Projectile, LeadIsCappedAndAPointBlankTargetIsSane) {
    // Forty tiles off and driving at half a tile per tick: left uncapped the lead would be 37
    // tiles; capped at one second of flight it is 15.
    const Unit gun = bare(kGun, Side::Player, 100, 100, 0, 0);
    const Unit far = bare(kMark, Side::Enemy, 140, 100, 0, Fixed::kOne / 2);
    const Projectile bolt = launch(gun, far, kTankDamage);
    const double sideways = static_cast<double>(bolt.vy.raw) * 40.0;
    const double along = static_cast<double>(bolt.vx.raw) * 15.0;
    EXPECT_NEAR(sideways, along, along * 0.02) << "the lead is not capped at one second";

    // On top of the gun and moving: the flight time floors at one tick, so there is a direction.
    const Unit here = bare(kMark, Side::Enemy, 100, 100, kTenthTile, 0);
    const Projectile close = launch(gun, here, kTankDamage);
    EXPECT_NEAR(static_cast<double>(isqrt(speed_squared(close))),
                static_cast<double>(kProjectileSpeedRaw), 4.0);
}

namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::string without_comments(const fs::path& path) {
    std::ifstream in(path);
    std::string line;
    std::string out;
    while (std::getline(in, line)) {
        const std::size_t at = line.find("//");
        out += (at == std::string::npos ? line : line.substr(0, at)) + "\n";
    }
    return out;
}

} // namespace

TEST(Projectile, NothingUnderSimOrWorldGainedAFloat) {
    // Rules 13 and 14: the sim is Q16.16 integer, and neither it nor the world includes math.hpp.
    const std::regex floating(R"(\b(float|double)\b)");
    int scanned = 0;
    std::string offenders;
    for (const char* layer : {"sim", "world"}) {
        for (const fs::path& dir : {fs::path{PROTEKT_SOURCE_DIR} / "include/protekt" / layer,
                                    fs::path{PROTEKT_SOURCE_DIR} / "src" / layer}) {
            for (const auto& entry : fs::recursive_directory_iterator(dir)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                ++scanned;
                const std::string text = without_comments(entry.path());
                if (std::regex_search(text, floating)) {
                    offenders += entry.path().filename().string() + ": float or double\n";
                }
                if (text.find("math.hpp") != std::string::npos) {
                    offenders += entry.path().filename().string() + ": includes math.hpp\n";
                }
            }
        }
    }
    EXPECT_GT(scanned, 10);
    EXPECT_TRUE(offenders.empty()) << offenders;
    EXPECT_TRUE(fs::exists(fs::path{PROTEKT_SOURCE_DIR} / "src/sim/projectile.cpp"));
}

} // namespace protekt::sim
