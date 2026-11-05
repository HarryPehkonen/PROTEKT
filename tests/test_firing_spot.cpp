#include "protekt/sim/projectile.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>

/// An Attack order that cannot land its shot must go where it can. Harri: "I sometimes get units
/// to attack the enemy, but they just sit there, doing nothing. I assume it's because they are
/// behind a hill, and can't actually hit the target."
///
/// Two mechanisms make exactly that symptom, and the first test tells them apart on a real case:
///   (a) NO LINE: the unit stops at its weapon range (that is the Attack stop rule), the gun will
///       not fire into a hill (`has_line`), so it stands in range, loaded, and silent;
///   (b) NO PATH: the unit cannot reach a place with a line at all (the concave-corner trap).
/// Targets are FACTORIES (unarmed), so nothing shoots back and every hp change is the tank's own.
namespace protekt::sim {
namespace {

struct Case {
    std::uint64_t seed;
    int tank_x;
    int tank_y;
    int factory_x;
    int factory_y;
};

/// Found by scanning seeds 1-30 for a tank in weapon range of a factory with no line to it: every
/// one stood still for 600 ticks with an Attack order and fired nothing.
constexpr std::array<Case, 8> kCases{{
    {4, 158, 193, 167, 195},
    {8, 172, 95, 181, 97},
    {9, 172, 158, 181, 160},
    {10, 60, 193, 69, 195},
    {13, 60, 158, 69, 160},
    {20, 172, 158, 181, 160},
    {21, 123, 151, 132, 153},
    {24, 60, 158, 69, 160},
}};

struct Duel {
    Sim sim;
    UnitId tank{};
    UnitId factory{};
};

[[nodiscard]] Duel duel(const Case& c, UnitKind kind = UnitKind::Tank) {
    Scenario scenario{};
    scenario.seed = c.seed;
    Spawn tank{};
    tank.side = Side::Player;
    tank.kind = kind;
    tank.tile_x = c.tank_x;
    tank.tile_y = c.tank_y;
    Spawn factory{};
    factory.side = Side::Enemy;
    factory.kind = UnitKind::Factory;
    factory.tile_x = c.factory_x;
    factory.tile_y = c.factory_y;
    scenario.spawns.push_back(tank);
    scenario.spawns.push_back(factory);
    Duel out{};
    out.sim = Sim::create(scenario);
    for (const Unit& unit : out.sim.units()) {
        (unit.side == Side::Player ? out.tank : out.factory) = unit.id;
    }
    return out;
}

[[nodiscard]] std::int64_t gap2(const Unit& a, const Unit& b) {
    const std::int64_t dx = static_cast<std::int64_t>(a.x.raw) - b.x.raw;
    const std::int64_t dy = static_cast<std::int64_t>(a.y.raw) - b.y.raw;
    return (dx * dx) + (dy * dy);
}

/// Shots fired over `ticks` ticks.
[[nodiscard]] int shots_in(Sim& sim, int ticks) {
    int shots = 0;
    for (int tick = 0; tick < ticks; ++tick) {
        sim.step();
        shots += static_cast<int>(sim.shots().size());
    }
    return shots;
}

/// The test's own search, by brute force and independent of `firing_spot`: the tile nearest the
/// tank, within 6 tiles, that can be stood on, is in range of the factory and has a line -- and at
/// least `apart` tiles from it (a MoveTo within the 1-tile arrival radius goes nowhere).
[[nodiscard]] std::optional<std::array<int, 2>> brute_spot(const Duel& d, const Case& c,
                                                           int apart = 0) {
    const Unit& tank = *d.sim.find(d.tank);
    const Unit& factory = *d.sim.find(d.factory);
    const std::int64_t range = profile(tank.kind).weapon_range.raw;
    std::optional<std::array<int, 2>> best{};
    std::int64_t best_d2 = 0;
    for (int ty = c.tank_y - 6; ty <= c.tank_y + 6; ++ty) {
        for (int tx = c.tank_x - 6; tx <= c.tank_x + 6; ++tx) {
            if (!tile_passable(c.seed, tx, ty)) {
                continue;
            }
            Unit probe = tank;
            probe.x = Fixed::from_raw((tx << Fixed::kShift) + (Fixed::kOne / 2));
            probe.y = Fixed::from_raw((ty << Fixed::kShift) + (Fixed::kOne / 2));
            const std::int64_t d2 = gap2(probe, tank);
            const std::int64_t least = static_cast<std::int64_t>(apart) * Fixed::kOne;
            if (d2 < least * least || gap2(probe, factory) > range * range
                || !has_line(probe, factory, c.seed) || (best && d2 >= best_d2)) {
                continue;
            }
            best = std::array<int, 2>{tx, ty};
            best_d2 = d2;
        }
    }
    return best;
}

TEST(FiringSpot, AnAttackerWithNoLineIsStoppedByNoLineNotByNoPath) {
    // Seed 4, a Tank at (158,193), the enemy factory at (167,195).
    const Case c = kCases.at(0);
    Duel d = duel(c);
    const Unit& tank = *d.sim.find(d.tank);
    const Unit& factory = *d.sim.find(d.factory);
    const std::int64_t range = profile(UnitKind::Tank).weapon_range.raw;

    // (a) The facts of "no line": it is already in weapon range, and the gun has no line.
    ASSERT_LE(gap2(tank, factory), range * range) << "the target is out of range: not this bug";
    ASSERT_FALSE(has_line(tank, factory, c.seed)) << "the tank has a line: not this bug";

    // (b) Excluded: there IS a place with a line, and the tank gets there when told to (a plain
    // MoveTo, which is all the player's Go Here is), and from there it has a line.
    const auto spot = brute_spot(d, c, 2);
    if (!spot) {
        FAIL() << "no tile 2 to 6 away has a line";
    }
    Duel walker = duel(c);
    const auto middle
        = [](int tile) { return Fixed::from_raw((tile << Fixed::kShift) + (Fixed::kOne / 2)); };
    walker.sim.enqueue(
        Order{walker.tank, orders::MoveTo{middle(spot->at(0)), middle(spot->at(1))}});
    // Walking, not attacking: it fires on its own the moment it has a line, which is the proof.
    EXPECT_GT(shots_in(walker.sim, 600), 0)
        << "the tank could not reach a place with a line: that would be the navigation trap";
    EXPECT_TRUE(has_line(*walker.sim.find(walker.tank), *walker.sim.find(walker.factory), c.seed));

    // The symptom, and the rule: given the Attack order it must fire.
    d.sim.enqueue(Order{d.tank, orders::Attack{d.factory}});
    const int shots = shots_in(d.sim, 600);
    EXPECT_GT(shots, 0) << "in range, loaded, no line, and it sat there: " << shots << " shots";
    EXPECT_LT(d.sim.find(d.factory)->hp, profile(UnitKind::Factory).hp);
}

TEST(FiringSpot, EveryHillBoundCaseFindsItsWayToAShot) {
    for (const Case& c : kCases) {
        Duel d = duel(c);
        ASSERT_FALSE(has_line(*d.sim.find(d.tank), *d.sim.find(d.factory), c.seed))
            << "seed " << c.seed << " has a line from the start";
        d.sim.enqueue(Order{d.tank, orders::Attack{d.factory}});
        const int shots = shots_in(d.sim, 600);
        EXPECT_GT(shots, 0) << "seed " << c.seed << " sat there";
    }
}

TEST(FiringSpot, ItGoesToTheNearestTileThatHasALine) {
    const Case c = kCases.at(0);
    Duel d = duel(c);
    const auto expected = brute_spot(d, c);
    if (!expected) {
        FAIL() << "the brute force found no tile";
    }
    const auto spot
        = firing_spot(c.seed, d.sim.units(), *d.sim.find(d.tank), *d.sim.find(d.factory));
    if (!spot) {
        FAIL() << "firing_spot found no tile";
    }
    EXPECT_EQ(spot->x.raw >> Fixed::kShift, expected->at(0));
    EXPECT_EQ(spot->y.raw >> Fixed::kShift, expected->at(1));
}

TEST(FiringSpot, TheSameStateGivesTheSameSpotAndTheSameBattle) {
    const Case c = kCases.at(2);
    Duel a = duel(c);
    Duel b = duel(c);
    const auto first
        = firing_spot(c.seed, a.sim.units(), *a.sim.find(a.tank), *a.sim.find(a.factory));
    const auto second
        = firing_spot(c.seed, b.sim.units(), *b.sim.find(b.tank), *b.sim.find(b.factory));
    if (!first || !second) {
        FAIL() << "firing_spot found no tile";
    }
    EXPECT_EQ(first->x.raw, second->x.raw);
    EXPECT_EQ(first->y.raw, second->y.raw);
    a.sim.enqueue(Order{a.tank, orders::Attack{a.factory}});
    b.sim.enqueue(Order{b.tank, orders::Attack{b.factory}});
    EXPECT_EQ(shots_in(a.sim, 300), shots_in(b.sim, 300));
    EXPECT_EQ(a.sim.state_hash(), b.sim.state_hash());
}

TEST(FiringSpot, AUnitThatAlreadyHasALineDoesNotMove) {
    // Same tank, the factory pulled to a tile it can see: nothing here has changed for it.
    Scenario scenario{};
    scenario.seed = 42;
    Spawn tank{};
    tank.side = Side::Player;
    tank.kind = UnitKind::Tank;
    tank.tile_x = 100;
    tank.tile_y = 100;
    Spawn factory{};
    factory.side = Side::Enemy;
    factory.kind = UnitKind::Factory;
    factory.tile_x = 105;
    factory.tile_y = 100;
    scenario.spawns.push_back(tank);
    scenario.spawns.push_back(factory);
    Sim sim = Sim::create(scenario);
    const UnitId tank_id = sim.units().at(0).id;
    const UnitId factory_id = sim.units().at(1).id;
    ASSERT_TRUE(has_line(*sim.find(tank_id), *sim.find(factory_id), 42));
    const Fixed x = sim.find(tank_id)->x;
    const Fixed y = sim.find(tank_id)->y;
    sim.enqueue(Order{tank_id, orders::Attack{factory_id}});
    EXPECT_GT(shots_in(sim, 100), 0);
    EXPECT_EQ(sim.find(tank_id)->x.raw, x.raw);
    EXPECT_EQ(sim.find(tank_id)->y.raw, y.raw);
}

TEST(FiringSpot, ABuildingCannotMoveSoItHasNoSpot) {
    const Case c = kCases.at(0);
    Duel d = duel(c, UnitKind::DeployedTurret);
    EXPECT_FALSE(firing_spot(c.seed, d.sim.units(), *d.sim.find(d.tank), *d.sim.find(d.factory))
                     .has_value());
}

TEST(FiringSpot, AnUndeployedTurretStaysWhereItIsAndFiresNothing) {
    // A turret's Attack "shoots the named target in place": with no line it has nothing to do, and
    // the new rule does not make it walk.
    const Case c = kCases.at(0);
    Duel d = duel(c, UnitKind::Turret);
    const Fixed x = d.sim.find(d.tank)->x;
    const Fixed y = d.sim.find(d.tank)->y;
    d.sim.enqueue(Order{d.tank, orders::Attack{d.factory}});
    EXPECT_EQ(shots_in(d.sim, 300), 0);
    EXPECT_EQ(d.sim.find(d.tank)->x.raw, x.raw);
    EXPECT_EQ(d.sim.find(d.tank)->y.raw, y.raw);
}

TEST(FiringSpot, ALobbingGunNeedsNoLineAndDoesNotRelocateForOne) {
    const Case c = kCases.at(0);
    Duel d = duel(c, UnitKind::MortarBike);
    ASSERT_TRUE(has_line(*d.sim.find(d.tank), *d.sim.find(d.factory), c.seed));
}

} // namespace
} // namespace protekt::sim
