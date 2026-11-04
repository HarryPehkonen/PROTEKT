#include "protekt/sim/orders.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <vector>

/// THE MORTAR BIKE'S DOCTRINE (Harri, 2026-11-03): it goes to a spot, fires, and moves to a
/// different spot at once -- "I fire one off, move, fire again, move". It belongs to the VEHICLE:
/// its range is the weapon's and these tests never ask the weapon for the behaviour.
namespace protekt::sim {
namespace {

struct Field {
    std::uint64_t seed{};
    int x{};
    int y{};
};

/// A seed and a corner with 40 x 24 tiles of clear ground, so no relocation is a question of
/// terrain.
[[nodiscard]] Field open_field() {
    for (std::uint64_t seed = 1; seed < 400; ++seed) {
        for (int y = 20; y < 200; y += 5) {
            for (int x = 20; x < 200; x += 5) {
                bool clear = true;
                for (int dy = 0; dy < 24 && clear; ++dy) {
                    for (int dx = 0; dx < 40 && clear; ++dx) {
                        clear = tile_passable(seed, x + dx, y + dy);
                    }
                }
                if (clear) {
                    return Field{seed, x, y};
                }
            }
        }
    }
    return Field{};
}

[[nodiscard]] std::int64_t tiles_squared(const Unit& a, const Unit& b) {
    const std::int64_t dx = (static_cast<std::int64_t>(a.x.raw) - b.x.raw) >> Fixed::kShift;
    const std::int64_t dy = (static_cast<std::int64_t>(a.y.raw) - b.y.raw) >> Fixed::kShift;
    return dx * dx + dy * dy;
}

[[nodiscard]] std::int64_t raw_distance_squared(const Unit& a, const Unit& b) {
    const std::int64_t dx = static_cast<std::int64_t>(a.x.raw) - b.x.raw;
    const std::int64_t dy = static_cast<std::int64_t>(a.y.raw) - b.y.raw;
    return dx * dx + dy * dy;
}

[[nodiscard]] UnitId first_of(const Sim& sim, UnitKind kind, Side side) {
    for (const Unit& unit : sim.units()) {
        if (unit.kind == kind && unit.side == side) {
            return unit.id;
        }
    }
    return UnitId{};
}

struct Volley {
    Tick tick{};
    Fixed x{};
    Fixed y{};
};

/// A bike ordered to attack a factory 12 tiles off, run for `ticks`: where it stood at each shot.
[[nodiscard]] std::vector<Volley> shots_from(Sim& sim, UnitId bike, UnitId target, int ticks) {
    sim.enqueue(Order{bike, orders::Attack{target}});
    std::vector<Volley> volleys;
    volleys.reserve(32);
    for (int t = 0; t < ticks; ++t) {
        sim.step();
        for (const Sim::Shot& shot : sim.shots()) {
            if (shot.shooter == bike) {
                const Unit* unit = sim.find(bike);
                volleys.push_back(Volley{sim.tick(), unit->x, unit->y});
            }
        }
    }
    return volleys;
}

[[nodiscard]] Scenario one_bike_one_factory(const Field& f) {
    Scenario s;
    s.seed = f.seed;
    s.spawns = {{UnitKind::MortarBike, Side::Player, f.x, f.y + 10},
                {UnitKind::Factory, Side::Enemy, f.x + 12, f.y + 10}};
    return s;
}

/// What a bike against one turret (range 16, the bike's 14) costs it, standing or scooting.
struct Duel {
    long ticks_inside{}; ///< ticks it was alive and inside the turret's range
    long hull_lost{};
    int died{};
};

/// The bike ordered at a turret 32 tiles off. `stands` is the old doctrine -- walk to range, then
/// hold there and fire -- which a bike on a guard post still is, since only an engaging bike
/// scoots.
[[nodiscard]] Duel duel(const Field& f, int offset, bool stands) {
    Scenario s;
    s.seed = f.seed;
    s.spawns = {{UnitKind::MortarBike, Side::Player, f.x, f.y + 10 + offset},
                {UnitKind::Turret, Side::Enemy, f.x + 32, f.y + 10}};
    Sim sim = Sim::create(s);
    const UnitId bike{1};
    const UnitId turret{2};
    sim.enqueue(Order{bike, orders::Attack{turret}});
    const std::int64_t reach = spec_of(UnitKind::MortarBike).range().raw;
    const std::int64_t danger = spec_of(UnitKind::Turret).range().raw;
    Duel result{};
    bool holding = false;
    for (int t = 0; t < 1200; ++t) {
        sim.step();
        const Unit& b = *sim.find(bike);
        const Unit& gun = *sim.find(turret);
        if (stands && !holding && raw_distance_squared(b, gun) <= reach * reach) {
            sim.enqueue(Order{bike, orders::GuardAt{b.x, b.y}});
            holding = true;
        }
        if (b.alive && gun.alive && raw_distance_squared(b, gun) <= danger * danger) {
            ++result.ticks_inside;
        }
        if (!b.alive || !gun.alive) {
            break;
        }
    }
    result.hull_lost = spec_of(UnitKind::MortarBike).hp - sim.find(bike)->hp;
    result.died = sim.find(bike)->alive ? 0 : 1;
    return result;
}

} // namespace

TEST(MortarBikeScoot, AfterFiringTheBikeMovesToADifferentSpotBeforeItFiresAgain) {
    const Field f = open_field();
    ASSERT_NE(f.seed, 0U);
    Sim sim = Sim::create(one_bike_one_factory(f));
    const UnitId bike = first_of(sim, UnitKind::MortarBike, Side::Player);
    const UnitId target = first_of(sim, UnitKind::Factory, Side::Enemy);
    sim.enqueue(Order{bike, orders::Attack{target}});

    bool fired = false;
    Unit at_shot{};
    double farthest = 0.0;
    for (int t = 0; t < 300 && !fired; ++t) {
        sim.step();
        for (const Sim::Shot& shot : sim.shots()) {
            fired = fired || shot.shooter == bike;
        }
    }
    ASSERT_TRUE(fired) << "it never fired";
    at_shot = *sim.find(bike);
    // The reload is 45 ticks; the bike must be somewhere else well before it is over.
    const std::uint16_t reload = spec_of(UnitKind::MortarBike).cooldown;
    for (std::uint16_t t = 0; t + 2 < reload; ++t) {
        sim.step();
        const std::int64_t d2 = raw_distance_squared(at_shot, *sim.find(bike));
        const double d = static_cast<double>(d2 >> Fixed::kShift) / Fixed::kOne;
        farthest = d > farthest ? d : farthest;
    }
    EXPECT_GT(farthest, 9.0) << "(tiles squared) it stood where it fired";
}

TEST(MortarBikeScoot, EveryShotComesFromADifferentSpotAndStillReachesTheTarget) {
    const Field f = open_field();
    ASSERT_NE(f.seed, 0U);
    Sim sim = Sim::create(one_bike_one_factory(f));
    const UnitId bike = first_of(sim, UnitKind::MortarBike, Side::Player);
    const UnitId target = first_of(sim, UnitKind::Factory, Side::Enemy);
    const std::vector<Volley> volleys = shots_from(sim, bike, target, 600);
    ASSERT_GE(volleys.size(), 5U);
    const std::int64_t range = spec_of(UnitKind::MortarBike).range().raw;
    for (std::size_t i = 0; i < volleys.size(); ++i) {
        Unit from = *sim.find(bike);
        from.x = volleys[i].x;
        from.y = volleys[i].y;
        EXPECT_LE(raw_distance_squared(from, *sim.find(target)), range * range)
            << "shot " << i << " was fired from outside its range";
        if (i > 0) {
            Unit before = from;
            before.x = volleys[i - 1].x;
            before.y = volleys[i - 1].y;
            EXPECT_GT(tiles_squared(from, before), 9) << "shot " << i << " from the same spot";
        }
    }
    EXPECT_LT(sim.find(target)->hp, spec_of(UnitKind::Factory).hp)
        << "and the shells did land on it";
}

TEST(MortarBikeScoot, TheSpotIsTheSameForTheSameSeedAndIdAndVariesWithTheId) {
    const Field f = open_field();
    ASSERT_NE(f.seed, 0U);
    Sim sim = Sim::create(one_bike_one_factory(f));
    const Unit bike = *sim.find(first_of(sim, UnitKind::MortarBike, Side::Player));
    const Unit target = *sim.find(first_of(sim, UnitKind::Factory, Side::Enemy));
    const std::optional<ScootSpot> first = scoot_spot(f.seed, sim.units(), bike, target, 100);
    const std::optional<ScootSpot> second = scoot_spot(f.seed, sim.units(), bike, target, 100);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    const ScootSpot once = first.value_or(ScootSpot{});
    const ScootSpot twice = second.value_or(ScootSpot{});
    EXPECT_EQ(once.x.raw, twice.x.raw);
    EXPECT_EQ(once.y.raw, twice.y.raw);

    // The spot keeps the target in range: relocating out of it would end the engagement.
    const std::int64_t range = spec_of(UnitKind::MortarBike).range().raw;
    Unit there = bike;
    there.x = once.x;
    there.y = once.y;
    EXPECT_LE(raw_distance_squared(there, target), range * range);

    // Other ids and other shots choose other spots: it is not one fixed place.
    int differing = 0;
    for (std::uint32_t id = 2; id < 12; ++id) {
        Unit other = bike;
        other.id = UnitId{id};
        const std::optional<ScootSpot> found = scoot_spot(f.seed, sim.units(), other, target, 100);
        ASSERT_TRUE(found.has_value());
        const ScootSpot spot = found.value_or(ScootSpot{});
        differing += (spot.x.raw != once.x.raw || spot.y.raw != once.y.raw) ? 1 : 0;
    }
    EXPECT_GE(differing, 5);
    int later = 0;
    for (Tick tick = 101; tick < 121; ++tick) {
        const std::optional<ScootSpot> found = scoot_spot(f.seed, sim.units(), bike, target, tick);
        ASSERT_TRUE(found.has_value());
        const ScootSpot spot = found.value_or(ScootSpot{});
        later += (spot.x.raw != once.x.raw || spot.y.raw != once.y.raw) ? 1 : 0;
    }
    EXPECT_GE(later, 5);
}

TEST(MortarBikeScoot, TwoBikesAtOnceBothKeepFiringTheShoveSortsOutTheirSpots) {
    const Field f = open_field();
    ASSERT_NE(f.seed, 0U);
    Scenario s;
    s.seed = f.seed;
    s.spawns = {{UnitKind::MortarBike, Side::Player, f.x, f.y + 10},
                {UnitKind::MortarBike, Side::Player, f.x, f.y + 11},
                {UnitKind::Factory, Side::Enemy, f.x + 12, f.y + 10}};
    Sim sim = Sim::create(s);
    const UnitId a{1};
    const UnitId b{2};
    const UnitId target = first_of(sim, UnitKind::Factory, Side::Enemy);
    sim.enqueue(Order{a, orders::Attack{target}});
    sim.enqueue(Order{b, orders::Attack{target}});
    int fired_a = 0;
    int fired_b = 0;
    for (int t = 0; t < 900; ++t) {
        sim.step();
        for (const Sim::Shot& shot : sim.shots()) {
            fired_a += shot.shooter == a ? 1 : 0;
            fired_b += shot.shooter == b ? 1 : 0;
        }
    }
    EXPECT_GE(fired_a, 8) << "bike 1 was trapped";
    EXPECT_GE(fired_b, 8) << "bike 2 was trapped";
}

TEST(MortarBikeScoot, ADrivenOrderedMoveIsNotRelocatedAway) {
    // The doctrine is for a bike that is ENGAGING. One told to go somewhere goes there and stays.
    const Field f = open_field();
    ASSERT_NE(f.seed, 0U);
    Sim sim = Sim::create(one_bike_one_factory(f));
    const UnitId bike = first_of(sim, UnitKind::MortarBike, Side::Player);
    sim.enqueue(Order{bike, orders::MoveTo{Fixed::from_int(f.x + 2), Fixed::from_int(f.y + 10)}});
    for (int t = 0; t < 200; ++t) {
        sim.step();
    }
    const Unit settled = *sim.find(bike);
    for (int t = 0; t < 200; ++t) {
        sim.step();
    }
    EXPECT_EQ(sim.find(bike)->x.raw, settled.x.raw);
    EXPECT_EQ(sim.find(bike)->y.raw, settled.y.raw);
}

TEST(MortarBikeScoot, UnderATurretItLosesLessHullForEveryTickInsideItsRangeThanOneThatStands) {
    // A bike must be inside 14 tiles to fire and a turret reaches 16, so the TIME inside range is
    // not what the doctrine saves: the standing bike is dead in about a hundred ticks and the
    // scooting one lives to fight on, so its total is longer. What changes is what each tick
    // inside costs, because a turret's bolt is aimed at where a mover was. Not a guarantee: some
    // scooting bikes still die, and the test asserts only that it is better.
    const Field f = open_field();
    ASSERT_NE(f.seed, 0U);
    Duel standing{};
    Duel scooting{};
    for (int offset = -6; offset <= 6; offset += 3) {
        const Duel a = duel(f, offset, true);
        const Duel b = duel(f, offset, false);
        standing.ticks_inside += a.ticks_inside;
        standing.hull_lost += a.hull_lost;
        standing.died += a.died;
        scooting.ticks_inside += b.ticks_inside;
        scooting.hull_lost += b.hull_lost;
        scooting.died += b.died;
    }
    ASSERT_GT(standing.ticks_inside, 0);
    ASSERT_GT(scooting.ticks_inside, 0);
    // hull lost per tick inside, compared without division: a/b < c/d <=> a*d < c*b
    EXPECT_LT(scooting.hull_lost * standing.ticks_inside,
              standing.hull_lost * scooting.ticks_inside)
        << "standing lost " << standing.hull_lost << " in " << standing.ticks_inside
        << " ticks; scooting lost " << scooting.hull_lost << " in " << scooting.ticks_inside;
    EXPECT_LE(scooting.died, standing.died);
}

} // namespace protekt::sim
