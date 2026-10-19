#include "protekt/sim/observer.hpp"
#include "protekt/sim/projectile.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
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

// A bolt now has a height, and the world has hills: a level shot hits the ground of a hill, and a
// shot at a unit on a different level misses it. The tests below that fly a bolt in a straight line
// over "open ground" therefore stand where the ground really is level, found by looking for it.
struct TileAt {
    int x{};
    int y{};
};

[[nodiscard]] int tile_height(int x, int y) { return world::sample(kSeed, x, y).height; }

[[nodiscard]] bool is_crater(int x, int y) {
    return world::sample(kSeed, x, y).terrain == world::Terrain::Crater;
}

/// The first tile of a row of `length` + 1 tiles whose heights stay within `tolerance` units of the
/// first one, none of them a crater: ground a level shot can be fired along and measured against.
[[nodiscard]] std::optional<TileAt> find_level_row(int length, int tolerance) {
    for (int y = 5; y < kMapTiles - 5; ++y) {
        for (int x = 5; x + length < kMapTiles - 5; ++x) {
            const int h0 = tile_height(x, y);
            bool level = true;
            for (int i = 0; i <= length && level; ++i) {
                level = !is_crater(x + i, y) && std::abs(tile_height(x + i, y) - h0) <= tolerance;
            }
            if (level) {
                return TileAt{x, y};
            }
        }
    }
    return std::nullopt;
}

/// The centre of a cross whose four arms, `arm` tiles each, stay within `tolerance` units of the
/// centre's height, none of it a crater: ground a level shot can be fired along in any of the four
/// directions and hit what stands there.
[[nodiscard]] std::optional<TileAt> find_level_cross(int arm, int tolerance) {
    for (int y = arm + 1; y < kMapTiles - arm - 1; ++y) {
        for (int x = arm + 1; x < kMapTiles - arm - 1; ++x) {
            const int h0 = tile_height(x, y);
            bool level = true;
            for (int i = -arm; i <= arm && level; ++i) {
                level = !is_crater(x + i, y) && !is_crater(x, y + i)
                        && std::abs(tile_height(x + i, y) - h0) <= tolerance
                        && std::abs(tile_height(x, y + i) - h0) <= tolerance;
            }
            if (level) {
                return TileAt{x, y};
            }
        }
    }
    return std::nullopt;
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
    const std::optional<TileAt> found = find_level_row(18, 4);
    ASSERT_TRUE(found.has_value());
    const TileAt row = found.value_or(TileAt{});
    std::vector<Place> places{{Side::Player, UnitKind::Tank, row.x, row.y},
                              {Side::Enemy, UnitKind::Tank, row.x + 12, row.y}};
    for (int i = 0; i < 13; ++i) {
        places.push_back({Side::Player, UnitKind::Tank, row.x + 13 + i % 3, row.y + 3 + i / 3});
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
    const std::optional<TileAt> found = find_level_row(18, 4); // level: nothing else may stop it
    ASSERT_TRUE(found.has_value());
    const TileAt row = found.value_or(TileAt{});
    Sim sim = make({{Side::Player, UnitKind::Tank, row.x, row.y},
                    {Side::Enemy, UnitKind::Tank, row.x + 8, row.y}});
    ASSERT_GT(step_until_fired(sim), 0);
    sim.enqueue(Order{kSecond, orders::MoveTo{Fixed::from_int(row.x + 8), Fixed::from_int(20)}});
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
        advance_projectiles(bolts, units, impacts, kSeed);
    }
    return units[1].hp;
}

} // namespace

TEST(Projectile, AnEstimateHitsWhatAStraightShotMisses) {
    // Twelve tiles off, crossing the line of fire at 0.1 tiles per tick: it drifts over two tiles
    // while the bolt is in the air, against a hit radius of half a tile. (Was LeadHitsWhat...:
    // launch() no longer predicts, so the lead is handed to the gun as the estimate it would have
    // learned, a little over the 2.25 tiles of drift.)
    Unit gun = bare(kGun, Side::Player, 100, 100, 0, 0);
    const Unit mark = bare(kMark, Side::Enemy, 112, 100, 0, kTenthTile);

    EXPECT_EQ(fly(launch(gun, mark, kTankDamage, kSeed), mark), kTankHp)
        << "the control is wrong: a gun with no estimate should miss this target";

    gun.aim_target = kMark;
    gun.aim_y = Fixed::from_raw(Fixed::kOne * 9 / 4);
    EXPECT_LT(fly(launch(gun, mark, kTankDamage, kSeed), mark), kTankHp)
        << "the shot with the estimate did not damage a crossing target";
    EXPECT_EQ(fly(launch(gun, mark, kTankDamage, kSeed), mark), kTankHp - kTankDamage);
}

TEST(Projectile, AStationaryTargetIsUnaffectedByLead) {
    const Unit gun = bare(kGun, Side::Player, 100, 100, 0, 0);
    const Unit mark = bare(kMark, Side::Enemy, 112, 105, 0, 0);
    const Projectile bolt = launch(gun, mark, kTankDamage, kSeed);

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
    const Projectile bolt = launch(gun, mark, kTankDamage, kSeed);
    EXPECT_EQ(bolt.vy.raw, gun.vy.raw) << "the bolt lost the gun's own velocity";
    EXPECT_GT(bolt.vx.raw, 0);
    EXPECT_EQ(fly(bolt, mark), kTankHp) << "the shot corrected itself for the gun's motion";
}

TEST(Projectile, ATargetThatReversesIsMissed) {
    // The estimate is the lead the target had; a target that turns round is missed. (Same test as
    // before, with the estimate standing in for the old computed lead.)
    Unit gun = bare(kGun, Side::Player, 100, 100, 0, 0);
    const Unit mark = bare(kMark, Side::Enemy, 112, 100, 0, kTenthTile);
    gun.aim_target = kMark;
    gun.aim_y = Fixed::from_raw(Fixed::kOne * 9 / 4);
    const Projectile bolt = launch(gun, mark, kTankDamage, kSeed);
    ASSERT_EQ(fly(bolt, mark), kTankHp - kTankDamage) << "the control shot should land";
    EXPECT_EQ(fly(bolt, mark, 5), kTankHp) << "the bolt followed a target that turned round";
}

TEST(Projectile, TheAimPointIsTheTargetPlusTheEstimate) {
    // Was LeadIsCappedAndAPointBlankTargetIsSane. The cap of the old lead is gone with the lead;
    // the cap of the estimate is pinned in test_ai_aim.cpp. What is left to pin here is the
    // geometry: the bolt points at target + estimate, and a point-blank target still has a heading.
    Unit gun = bare(kGun, Side::Player, 100, 100, 0, 0);
    const Unit mark = bare(kMark, Side::Enemy, 112, 100, 0, Fixed::kOne / 2);
    gun.aim_target = kMark;
    gun.aim_y = Fixed::from_int(4);
    const Projectile bolt = launch(gun, mark, kTankDamage, kSeed);
    // The aim point is (112, 104) from (100, 100): twelve across, four up. The mark's own
    // velocity, half a tile per tick, played no part.
    EXPECT_NEAR(static_cast<double>(bolt.vy.raw) * 12.0, static_cast<double>(bolt.vx.raw) * 4.0,
                static_cast<double>(bolt.vx.raw) * 0.05);

    // On top of the gun with no estimate: no direction to aim in, so due east, at full speed.
    const Unit here = bare(kMark, Side::Enemy, 100, 100, kTenthTile, 0);
    const Projectile close
        = launch(bare(kGun, Side::Player, 100, 100, 0, 0), here, kTankDamage, kSeed);
    EXPECT_EQ(close.vx.raw, kProjectileSpeedRaw);
    EXPECT_EQ(close.vy.raw, 0);
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

namespace {

/// A lone tank, copied out of a sim so a test can place and move it freely.
[[nodiscard]] Unit lone_tank() {
    const Sim sim = make({{Side::Player, UnitKind::Tank, 100, 100}});
    return *sim.find(kFirst);
}

/// Flies `bolt` until it is gone or `limit` ticks pass; returns the ticks flown.
int fly_until_gone(Projectile bolt, std::vector<Unit>& units, std::vector<Impact>& impacts,
                   int limit = 200) {
    std::vector<Projectile> air{bolt};
    int ticks = 0;
    while (!air.empty() && ticks < limit) {
        advance_projectiles(air, units, impacts, kSeed);
        ++ticks;
    }
    return ticks;
}

} // namespace

TEST(Projectile, ABoltFiredAtNothingFliesTheWayItWasAimed) {
    // THE SIGN TEST. The map plane is x east, y south (smaller y is north, as the tank test above
    // drives "north" with vy < 0). Each direction must come out with its own signs and no others.
    const Unit tank = lone_tank();
    struct Case {
        int dx;
        int dy;
        int sign_x;
        int sign_y;
    };
    for (const Case c : {Case{1, 0, 1, 0}, Case{-1, 0, -1, 0}, Case{0, -1, 0, -1}, Case{0, 1, 0, 1},
                         Case{1, 1, 1, 1}, Case{-3, 2, -1, 1}}) {
        const Projectile bolt = launch_towards(tank, Fixed::from_int(c.dx), Fixed::from_int(c.dy),
                                               Fixed::zero(), UnitId{}, kTankDamage, kSeed);
        const auto sign = [](std::int32_t v) { return (v > 0) - (v < 0); };
        EXPECT_EQ(sign(bolt.vx.raw), c.sign_x) << c.dx << "," << c.dy;
        EXPECT_EQ(sign(bolt.vy.raw), c.sign_y) << c.dx << "," << c.dy;
        EXPECT_NEAR(static_cast<double>(isqrt(speed_squared(bolt))),
                    static_cast<double>(kProjectileSpeedRaw), 2.0)
            << "the direction is normalised, whatever its length: " << c.dx << "," << c.dy;
        // Same heading: the cross product of the request and the velocity vanishes.
        const double cross
            = static_cast<double>(c.dx) * bolt.vy.raw - static_cast<double>(c.dy) * bolt.vx.raw;
        EXPECT_NEAR(cross, 0.0, 4.0 * 3.0) << c.dx << "," << c.dy;
        EXPECT_EQ(bolt.x, tank.x);
        EXPECT_EQ(bolt.y, tank.y);
    }
}

TEST(Projectile, ABoltFiredAtNothingNamesNoTarget) {
    const Unit tank = lone_tank();
    const Projectile bolt = launch_towards(tank, Fixed::from_int(1), Fixed::zero(), Fixed::zero(),
                                           UnitId{}, kTankDamage, kSeed);
    EXPECT_FALSE(bolt.aimed_at.valid());
    EXPECT_EQ(bolt.shooter, tank.id);
    EXPECT_EQ(bolt.side, tank.side);
    EXPECT_EQ(bolt.damage, kTankDamage);
    EXPECT_EQ(bolt.age, 0U);
}

TEST(Projectile, ABoltFiredAtNothingKeepsTheShootersOwnVelocity) {
    // The twin of AMovingTankSendsItsOwnVelocityWithTheBolt, for the path with no target.
    Unit tank = lone_tank();
    tank.vx = Fixed::from_raw(300);
    tank.vy = Fixed::from_raw(-200);
    const Projectile bolt = launch_towards(tank, Fixed::from_int(1), Fixed::zero(), Fixed::zero(),
                                           UnitId{}, kTankDamage, kSeed);
    EXPECT_EQ(bolt.vx.raw, kProjectileSpeedRaw + 300) << "aim plus the tank's own drive";
    EXPECT_EQ(bolt.vy.raw, -200) << "nothing sideways in the aim, all of it the tank's";
}

TEST(Projectile, ABoltFiredAtNothingStillHitsWhatIsInItsPath) {
    const std::optional<TileAt> found = find_level_cross(8, 20);
    ASSERT_TRUE(found.has_value());
    const TileAt cross = found.value_or(TileAt{});
    const int cx = cross.x;
    const int cy = cross.y;
    for (const auto& [fx, fy, ex, ey] :
         {std::array<int, 4>{1, 0, cx + 8, cy}, std::array<int, 4>{0, 1, cx, cy + 8},
          std::array<int, 4>{-1, 0, cx - 8, cy}, std::array<int, 4>{0, -1, cx, cy - 8}}) {
        Sim world = make(
            {{Side::Player, UnitKind::Tank, cx, cy}, {Side::Enemy, UnitKind::Factory, ex, ey}});
        std::vector<Unit> units = world.units();
        const std::uint16_t full = units.at(1).hp;
        std::vector<Impact> impacts;
        const Projectile bolt
            = launch_towards(units.at(0), Fixed::from_int(fx), Fixed::from_int(fy), Fixed::zero(),
                             UnitId{}, kTankDamage, kSeed);
        fly_until_gone(bolt, units, impacts);
        EXPECT_EQ(units.at(1).hp, full - kTankDamage) << fx << "," << fy;
        ASSERT_EQ(impacts.size(), 1U) << fx << "," << fy;
        EXPECT_EQ(impacts.front().victim, kSecond);
    }
}

TEST(Projectile, ABoltFiredAtNothingPassesThroughItsOwnSide) {
    Sim world = make(
        {{Side::Player, UnitKind::Tank, 100, 100}, {Side::Player, UnitKind::Factory, 106, 100}});
    std::vector<Unit> units = world.units();
    const std::uint16_t full = units.at(1).hp;
    std::vector<Impact> impacts;
    const Projectile bolt = launch_towards(units.at(0), Fixed::from_int(1), Fixed::zero(),
                                           Fixed::zero(), UnitId{}, kTankDamage, kSeed);
    fly_until_gone(bolt, units, impacts);
    EXPECT_EQ(units.at(1).hp, full)
        << "no aimed_at, so nothing excuses it from friendly pass-through";
    EXPECT_TRUE(impacts.empty());
}

TEST(Projectile, ABoltFiredAtNothingFizzlesAtRange) {
    Sim world = make({{Side::Player, UnitKind::Tank, 100, 100},
                      {Side::Enemy, UnitKind::Factory, 100, 130}}); // well off the line east
    std::vector<Unit> units = world.units();
    std::vector<Impact> impacts;
    const Projectile bolt = launch_towards(units.at(0), Fixed::from_int(1), Fixed::zero(),
                                           Fixed::zero(), UnitId{}, kTankDamage, kSeed);
    const std::int64_t length = isqrt(speed_squared(bolt));
    const std::int64_t expiry_age = (kProjectileRange.raw + length - 1) / length;
    const int flown = fly_until_gone(bolt, units, impacts);
    EXPECT_LT(flown, 200) << "it never expired";
    EXPECT_EQ(flown, expiry_age) << "it did not fizzle at kProjectileRange";
    EXPECT_TRUE(impacts.empty()) << "and nothing took a hit";
}

// ---------------------------------------------------------------------------------------------
// Origin + path = target: the player's shot flies along the aim and a name is not a path
// ---------------------------------------------------------------------------------------------

namespace {

/// A patch of ground where every tile below spawns exactly where it is asked to: the tank, and
/// units fourteen tiles east (out of the tank's twelve-tile reach, inside a bolt's sixteen).
constexpr int kOx = 20;
constexpr int kOy = 60;

/// A new tank starts reloading; step it until the gun is ready, so the next order's shot is made.
void reload(Sim& sim) {
    for (int tick = 0; tick < profile(UnitKind::Tank).cooldown_ticks; ++tick) {
        sim.step();
    }
}

void run_ticks(Sim& sim, int ticks) {
    for (int tick = 0; tick < ticks; ++tick) {
        sim.step();
    }
}

} // namespace

TEST(Projectile, ABoltAimedAtATargetStillFliesAlongTheDirection) {
    // A name changes WHO may be hit, never WHERE the bolt goes. The named unit sits in very
    // different places -- ahead, beside, far off, and BEHIND the direction asked for -- and the
    // velocity's heading must be the direction's, every time. Through the sim's own order path, the
    // way a click takes it; the named units are friendly so that nothing auto-engages them.
    const Fixed dir_x = Fixed::from_int(1);
    const Fixed dir_y = Fixed::zero();
    struct Where {
        int dx;
        int dy;
    };
    for (const Where w : {Where{4, 0}, Where{0, 4}, Where{-4, 0}, Where{-3, -3}, Where{4, 3}}) {
        Sim world = make({{Side::Player, UnitKind::Tank, kOx, kOy},
                          {Side::Player, UnitKind::Factory, kOx + w.dx, kOy + w.dy}});
        reload(world);
        world.enqueue(Order{kFirst, orders::FireTowards{dir_x, dir_y, kSecond}});
        world.step();
        ASSERT_EQ(world.projectiles().size(), 1U) << w.dx << "," << w.dy;
        const Projectile& bolt = world.projectiles().front();
        const Projectile plain = launch_towards(*world.find(kFirst), dir_x, dir_y, Fixed::zero(),
                                                UnitId{}, kTankDamage, kSeed);
        EXPECT_EQ(bolt.vx, plain.vx) << w.dx << "," << w.dy;
        EXPECT_EQ(bolt.vy.raw, 0) << "due east, whatever the unit's bearing: " << w.dx << ","
                                  << w.dy;
        EXPECT_EQ(bolt.aimed_at, kSecond) << "the name is carried as intent";
    }
}

TEST(Projectile, ABoltAimedAtATargetOffsetFromThePathMissesIt) {
    // Out of the tank's own 12-tile reach (so nothing auto-engages it) but inside the bolt's 16:
    // the only thing that could hurt it is the ordered shot.
    const auto damage_taken = [](int tile_y) {
        Sim world = make({{Side::Player, UnitKind::Tank, kOx, kOy},
                          {Side::Enemy, UnitKind::Factory, kOx + 14, tile_y}});
        const std::uint16_t full = world.find(kSecond)->hp;
        reload(world);
        world.enqueue(
            Order{kFirst, orders::FireTowards{Fixed::from_int(1), Fixed::zero(), kSecond}});
        run_ticks(world, 90);
        return static_cast<int>(full) - static_cast<int>(world.find(kSecond)->hp);
    };
    // One tile off the line is twice kProjectileHitRadius.
    EXPECT_EQ(damage_taken(kOy + 1), 0) << "the shot named it, but it flew past";
    EXPECT_EQ(damage_taken(kOy - 1), 0);
    EXPECT_EQ(damage_taken(kOy), static_cast<int>(kTankDamage)) << "control: ON the line";
}

TEST(Projectile, ThePlayersShotDoesNotLeadAMovingTarget) {
    // A hostile tank crosses toward the aim line, walking south. `launch` (the AI's gun) would aim
    // at where it will be; the player's shot goes where the crosshair pointed, and that is all.
    Sim world = make({{Side::Player, UnitKind::Tank, kOx, kOy},
                      {Side::Enemy, UnitKind::Tank, kOx + 14, kOy - 14}});
    const UnitId hostile = kSecond;
    const std::uint16_t full = world.find(hostile)->hp;
    const Fixed start_y = world.find(hostile)->y;
    world.enqueue(
        Order{hostile, orders::MoveTo{Fixed::from_int(kOx + 14), Fixed::from_int(kOy + 8)}});
    reload(world); // the hostile is under way while the gun readies, so it HAS a velocity to lead
    ASSERT_GT(world.find(hostile)->vy.raw, 0) << "premise: it is moving";
    world.enqueue(Order{kFirst, orders::FireTowards{Fixed::from_int(1), Fixed::zero(), hostile}});
    world.step();
    ASSERT_EQ(world.projectiles().size(), 1U);
    EXPECT_EQ(world.projectiles().front().vy.raw, 0)
        << "the bolt left along the aim, not at a prediction";
    EXPECT_GT(world.projectiles().front().vx.raw, 0);

    int ticks = 0;
    Fixed hostile_y_at_the_bolts_end = start_y;
    while (!world.projectiles().empty() && ticks < 200) {
        for (const Projectile& bolt : world.projectiles()) {
            EXPECT_EQ(bolt.y, world.find(kFirst)->y) << "never bends toward the target";
        }
        hostile_y_at_the_bolts_end = world.find(hostile)->y;
        world.step();
        ++ticks;
    }
    EXPECT_GT(hostile_y_at_the_bolts_end.raw, start_y.raw)
        << "premise: it really was walking south";
    EXPECT_LT(hostile_y_at_the_bolts_end.raw, Fixed::from_int(kOy - 1).raw)
        << "premise: and had not reached the line while the bolt was in the air";
    EXPECT_EQ(world.find(hostile)->hp, full) << "a lead would have found it; the aim did not";
}

TEST(Projectile, ABoltAimedAtAFriendlyHitsIt) {
    const auto friendly_hp = [](UnitId aimed_at) {
        Sim world = make({{Side::Player, UnitKind::Tank, kOx, kOy},
                          {Side::Player, UnitKind::Factory, kOx + 4, kOy}});
        reload(world);
        world.enqueue(
            Order{kFirst, orders::FireTowards{Fixed::from_int(1), Fixed::zero(), aimed_at}});
        run_ticks(world, 90);
        return static_cast<int>(world.find(kSecond)->hp);
    };
    const int full = profile(UnitKind::Factory).hp;
    EXPECT_EQ(friendly_hp(kSecond), full - kTankDamage) << "aimed at it, aimed along it: it is hit";
    EXPECT_EQ(friendly_hp(UnitId{}), full) << "the same shot at nothing passes through";
}

TEST(Projectile, ATriggerShotNeverMayHitItsOwnShooter) {
    // A unit naming ITSELF must not shoot itself dead on the first tick, as the order validity
    // check already guarantees for FocusFire.
    Sim world = make({{Side::Player, UnitKind::Tank, kOx, kOy}});
    reload(world);
    world.enqueue(Order{kFirst, orders::FireTowards{Fixed::from_int(1), Fixed::zero(), kFirst}});
    run_ticks(world, 30);
    EXPECT_EQ(world.find(kFirst)->hp, profile(UnitKind::Tank).hp);
}

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

// ---------------------------------------------------------------------------------------------
// A bolt has a height: pitch matters, a bolt does not follow the ground, and it ends on meeting it
// ---------------------------------------------------------------------------------------------

namespace {

constexpr int kRowLength = 10;
constexpr int kTargetDistance = 8; ///< tiles from the gun to the target on the level row

/// The bolt's own account of a shot at a tank `kTargetDistance` tiles away along a level row.
struct Shot {
    std::uint16_t target_hp{};
    std::size_t impacts{};
    int ticks_flown{};
    Projectile last{}; ///< the bolt as it was on the last tick it existed
};

/// The player's shot along (1, 0, `dir_z`): the pitch is the whole of the difference between the
/// cases below. The target is hostile and stands still on the same level row.
[[nodiscard]] Shot shoot_along_the_row(Fixed dir_z) {
    const std::optional<TileAt> row = find_level_row(kRowLength, 4);
    EXPECT_TRUE(row.has_value()) << "no level row of ground on seed " << kSeed;
    const TileAt at = row.value_or(TileAt{});
    std::vector<Unit> units{bare(kFirst, Side::Player, at.x, at.y, 0, 0),
                            bare(kSecond, Side::Enemy, at.x + kTargetDistance, at.y, 0, 0)};
    std::vector<Projectile> air{launch_towards(units.at(0), Fixed::from_int(1), Fixed::zero(),
                                               dir_z, UnitId{}, kTankDamage, kSeed)};
    std::vector<Impact> impacts;
    Shot shot{};
    while (!air.empty() && shot.ticks_flown < 200) {
        shot.last = air.front();
        advance_projectiles(air, units, impacts, kSeed);
        ++shot.ticks_flown;
    }
    shot.target_hp = units.at(1).hp;
    shot.impacts = impacts.size();
    return shot;
}

constexpr Fixed kTenthSlope = Fixed::from_raw(Fixed::kOne / 10); ///< a tenth up per unit along

} // namespace

TEST(BoltHeight, ABoltCarriesItsOwnHeightAndSpeedAtTheMuzzle) {
    const std::optional<TileAt> found = find_level_row(kRowLength, 4);
    ASSERT_TRUE(found.has_value());
    const TileAt row = found.value_or(TileAt{});
    const Unit gun = bare(kFirst, Side::Player, row.x, row.y, 0, 0);
    const Fixed ground = ground_height(kSeed, gun.x, gun.y);
    const Projectile level = launch_towards(gun, Fixed::from_int(1), Fixed::zero(), Fixed::zero(),
                                            UnitId{}, kTankDamage, kSeed);
    EXPECT_EQ(level.z.raw, ground.raw + kMuzzleHeight.raw) << "it leaves the gun at muzzle height";
    EXPECT_EQ(level.vz.raw, 0);
    // A tenth up per unit along: a bolt that goes up, and whose total speed is still the speed.
    const Projectile up = launch_towards(gun, Fixed::from_int(1), Fixed::zero(), kTenthSlope,
                                         UnitId{}, kTankDamage, kSeed);
    EXPECT_GT(up.vz.raw, 0);
    EXPECT_LT(up.vx.raw, level.vx.raw) << "the horizontal share shrinks as it climbs";
    const std::int64_t vz_tiles = up.vz.raw / kHeightUnitsPerTile;
    const std::int64_t speed2 = static_cast<std::int64_t>(up.vx.raw) * up.vx.raw
                                + static_cast<std::int64_t>(up.vy.raw) * up.vy.raw
                                + vz_tiles * vz_tiles;
    EXPECT_NEAR(static_cast<double>(isqrt(speed2)), static_cast<double>(kProjectileSpeedRaw), 8.0)
        << "pitch must not change how fast a bolt flies";
    const Projectile down
        = launch_towards(gun, Fixed::from_int(1), Fixed::zero(), Fixed::from_raw(-kTenthSlope.raw),
                         UnitId{}, kTankDamage, kSeed);
    EXPECT_EQ(down.vz.raw, -up.vz.raw) << "down is exactly up, mirrored";
}

TEST(BoltHeight, TheGroundTheSimUsesIsTheTilesHeightAtAWholeTile) {
    for (const TileAt at : {TileAt{20, 60}, TileAt{100, 100}, TileAt{3, 250}}) {
        EXPECT_EQ(ground_height(kSeed, Fixed::from_int(at.x), Fixed::from_int(at.y)).raw,
                  tile_height(at.x, at.y) * Fixed::kOne)
            << at.x << "," << at.y;
    }
    // Halfway along an edge it is halfway between the two corners.
    const Fixed mid = Fixed::from_raw(Fixed::kOne * 100 + Fixed::kOne / 2);
    const std::int64_t twice
        = static_cast<std::int64_t>(ground_height(kSeed, mid, Fixed::from_int(100)).raw) * 2;
    EXPECT_EQ(twice, static_cast<std::int64_t>(tile_height(100, 100) + tile_height(101, 100))
                         * Fixed::kOne);
}

TEST(BoltHeight, AimingLevelHitsTheSameTarget) {
    // The control for the two tests below: the same gun, the same row, the same target, level.
    const Shot shot = shoot_along_the_row(Fixed::zero());
    EXPECT_EQ(shot.target_hp, kTankHp - kTankDamage);
    EXPECT_EQ(shot.impacts, 1U);
}

TEST(BoltHeight, AimingUpPassesOverATargetsHead) {
    const Shot shot = shoot_along_the_row(kTenthSlope);
    EXPECT_EQ(shot.target_hp, kTankHp) << "a shot aimed over a tank's head hit it";
    EXPECT_EQ(shot.impacts, 0U);
    // It went on up and flew its whole range: nothing stopped it, in the air or on the ground.
    EXPECT_GT(shot.last.z.raw,
              ground_height(kSeed, shot.last.x, shot.last.y).raw + body_top(UnitKind::Tank).raw)
        << "the bolt is not above the tank at all";
    EXPECT_GE(shot.ticks_flown, 25);
}

TEST(BoltHeight, AimingDownMeetsTheGroundAndStops) {
    const Shot shot = shoot_along_the_row(Fixed::from_raw(-kTenthSlope.raw));
    EXPECT_EQ(shot.target_hp, kTankHp) << "a bolt that meets the ground hurt something";
    EXPECT_EQ(shot.impacts, 0U);
    // It is gone where it met the ground: a tenth down per unit along, from a muzzle 36.8 units up
    // on ground that is level to a few units, is about 5.75 tiles on (a little less where the row
    // rises) -- short of the target eight tiles away, and nowhere near the sixteen it would fly if
    // it were let go.
    const std::optional<TileAt> found = find_level_row(kRowLength, 4);
    ASSERT_TRUE(found.has_value());
    const TileAt row = found.value_or(TileAt{});
    const std::int64_t flown = static_cast<std::int64_t>(shot.last.x.raw)
                               - static_cast<std::int64_t>(row.x) * Fixed::kOne;
    EXPECT_GT(flown, 4 * Fixed::kOne) << "it stopped in mid air";
    EXPECT_LT(flown, 7 * Fixed::kOne) << "it flew on past the ground";
    EXPECT_LT(shot.ticks_flown, 25);
}

namespace {

struct Duel {
    TileAt gun;
    TileAt target;
};

/// Is a straight line from the gun's muzzle to the middle of the target's body clear of the
/// ground? A gun cannot shoot through a hill, so a target behind one proves nothing about aim.
[[nodiscard]] bool line_is_clear(const Duel& duel) {
    const std::int64_t z0
        = ground_height(kSeed, Fixed::from_int(duel.gun.x), Fixed::from_int(duel.gun.y)).raw
          + kMuzzleHeight.raw;
    const std::int64_t z1
        = ground_height(kSeed, Fixed::from_int(duel.target.x), Fixed::from_int(duel.target.y)).raw
          + (body_bottom(UnitKind::Tank).raw + body_top(UnitKind::Tank).raw) / 2;
    constexpr int kSteps = 80;
    for (int i = 1; i < kSteps; ++i) {
        const Fixed x = Fixed::from_raw(duel.gun.x * Fixed::kOne
                                        + (duel.target.x - duel.gun.x) * Fixed::kOne * i / kSteps);
        const Fixed y = Fixed::from_raw(duel.gun.y * Fixed::kOne
                                        + (duel.target.y - duel.gun.y) * Fixed::kOne * i / kSteps);
        const std::int64_t line = z0 + (z1 - z0) * i / kSteps;
        if (ground_height(kSeed, x, y).raw + 4 * Fixed::kOne > line) {
            return false;
        }
    }
    return true;
}

/// A gun on flat ground and a target on a different level, 6 to 10 tiles off with a clear line of
/// fire. `want` says whether the target is on a ridge (above, at least 60 units) or at the foot of
/// a crater (below, at least 60 units, with a crater tile beside it).
enum class Ground : std::uint8_t { Ridge, Crater };

[[nodiscard]] std::optional<Duel> find_duel(Ground want) {
    for (int gy = 12; gy < kMapTiles - 12; gy += 2) {
        for (int gx = 12; gx < kMapTiles - 12; gx += 2) {
            if (world::sample(kSeed, gx, gy).terrain != world::Terrain::Flat) {
                continue;
            }
            for (int dy = -9; dy <= 9; ++dy) {
                for (int dx = -9; dx <= 9; ++dx) {
                    const int d2 = dx * dx + dy * dy;
                    if (d2 < 36 || d2 > 100) {
                        continue;
                    }
                    const int tx = gx + dx;
                    const int ty = gy + dy;
                    const world::Tile tile = world::sample(kSeed, tx, ty);
                    const int rise = tile.height - tile_height(gx, gy);
                    bool fits = false;
                    if (want == Ground::Ridge) {
                        fits = tile.terrain == world::Terrain::Ridge && rise >= 60;
                    } else {
                        fits = tile.terrain != world::Terrain::Crater && rise <= -60
                               && (is_crater(tx + 1, ty) || is_crater(tx - 1, ty)
                                   || is_crater(tx, ty + 1) || is_crater(tx, ty - 1));
                    }
                    if (fits && line_is_clear(Duel{{gx, gy}, {tx, ty}})) {
                        return Duel{{gx, gy}, {tx, ty}};
                    }
                }
            }
        }
    }
    return std::nullopt;
}

/// The target's hit points after `ticks` of a battle between the gun and it, and after the same
/// aim fired LEVEL from the same gun at the same target. Only the first is the sim's own shot.
struct DuelResult {
    std::uint16_t sim_hp{};
    std::uint16_t level_hp{};
};

[[nodiscard]] DuelResult fight(const Duel& duel) {
    Sim world = make({{Side::Player, UnitKind::Tank, duel.gun.x, duel.gun.y},
                      {Side::Enemy, UnitKind::Tank, duel.target.x, duel.target.y}});
    DuelResult result{};
    // The spawns must have stayed where they were put, or this is a different duel.
    EXPECT_EQ(world.find(kFirst)->x, Fixed::from_int(duel.gun.x));
    EXPECT_EQ(world.find(kSecond)->x, Fixed::from_int(duel.target.x));
    EXPECT_EQ(world.find(kSecond)->y, Fixed::from_int(duel.target.y));
    for (int tick = 0; tick < 90; ++tick) {
        world.step();
    }
    result.sim_hp = world.find(kSecond)->hp;

    std::vector<Unit> units{world.units()};
    units.at(1).hp = kTankHp;
    std::vector<Projectile> air{launch_towards(
        units.at(0), Fixed::from_int(duel.target.x - duel.gun.x),
        Fixed::from_int(duel.target.y - duel.gun.y), Fixed::zero(), UnitId{}, kTankDamage, kSeed)};
    std::vector<Impact> impacts;
    for (int tick = 0; tick < 200 && !air.empty(); ++tick) {
        advance_projectiles(air, units, impacts, kSeed);
    }
    result.level_hp = units.at(1).hp;
    return result;
}

} // namespace

TEST(BoltHeight, AGunIsNotFooledByUnevenGround) {
    // THE GUARD AGAINST A STANDOFF. A gun on level ground must hit a target standing at the foot
    // of a crater and one on a ridge, because it aims at where the target REALLY is; and the
    // control is that the very same shot fired LEVEL hits neither -- so this is a test of ground.
    for (const Ground want : {Ground::Crater, Ground::Ridge}) {
        const char* name = want == Ground::Crater ? "crater" : "ridge";
        const std::optional<Duel> duel = find_duel(want);
        ASSERT_TRUE(duel.has_value()) << "no " << name << " duel on seed " << kSeed;
        const DuelResult result = fight(duel.value_or(Duel{}));
        EXPECT_LT(result.sim_hp, kTankHp) << "the sim's gun never hit a target on a " << name;
        EXPECT_EQ(result.level_hp, kTankHp)
            << "a level shot hit a target on a " << name << ": the test is not testing ground";
    }
}

TEST(BoltHeight, ThePitchAndTheHeightAreHashedAndReplayed) {
    // A bolt's height is state: two sims that differ only in the pitch of one shot must not hash
    // alike once the bolt is in the air.
    const std::optional<TileAt> found = find_level_row(kRowLength, 4);
    ASSERT_TRUE(found.has_value());
    const TileAt row = found.value_or(TileAt{});
    const auto hash_after = [&](Fixed dir_z) {
        Sim world = make({{Side::Player, UnitKind::Tank, row.x, row.y}});
        reload(world);
        world.enqueue(
            Order{kFirst, orders::FireTowards{Fixed::from_int(1), Fixed::zero(), UnitId{}, dir_z}});
        run_ticks(world, 3);
        EXPECT_EQ(world.projectiles().size(), 1U);
        return world.state_hash();
    };
    EXPECT_EQ(hash_after(Fixed::zero()), hash_after(Fixed::zero()));
    EXPECT_NE(hash_after(Fixed::zero()), hash_after(kTenthSlope));
    EXPECT_NE(hash_after(kTenthSlope), hash_after(Fixed::from_raw(-kTenthSlope.raw)));
}

} // namespace protekt::sim
