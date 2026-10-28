// THE PLAYER'S OWN GUN. The sim's step 4 auto-engages for every armed unit that has no focus
// target: it reads the unit's KIND and picks the nearest hostile in range, without knowing that a
// human is sitting in one of those units. That is the reported bug -- walk a driven hull up to an
// enemy building and the hull shoots it on its own, whatever weapon the player has up.
//
// The remedy is one fact the sim learns as an ORDER, `TakeControl`, so a replay's order stream is
// the whole input: the sim remembers the one unit the player is driving and step 4 skips exactly
// that unit. Nothing else about the AI moves -- which is what these cases are here to hold.
//
// The enemies here are RECYCLERS (unarmed, range zero) on purpose: they never fire, so every SHOT
// in the record is a player gun's and "who fired" is unambiguous. The one case that needs a gun on
// the other side spawns an enemy TANK for it.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::sim {
namespace {

constexpr std::uint64_t kSeed = 42;
/// Sixty ticks of window: a tank's cooldown is 15, so an auto-engaging tank gets four shots in.
/// "Fires within sixty ticks" is the owner's own report, and it has room to be true.
constexpr int kWindow = 60;

/// Every unit that fired at least once during `ticks` steps, read off the sim's per-step SHOT
/// record -- not off the projectile list, which answers a different question (a bolt still in the
/// air is a shot already made and it outlives the step that made it).
[[nodiscard]] std::vector<UnitId> shooters_over(Sim& sim, int ticks) {
    std::vector<UnitId> shooters;
    for (int tick = 0; tick < ticks; ++tick) {
        sim.step();
        for (const Sim::Shot& shot : sim.shots()) {
            if (std::find(shooters.begin(), shooters.end(), shot.shooter) == shooters.end()) {
                shooters.push_back(shot.shooter);
            }
        }
    }
    return shooters;
}

[[nodiscard]] bool fired(const std::vector<UnitId>& shooters, UnitId id) {
    return std::find(shooters.begin(), shooters.end(), id) != shooters.end();
}

/// The scenario built from `Scenario::spawns` alone: nothing is in range but what the case puts
/// there, so "the tank fired" cannot be borrowed from some other unit's gun.
[[nodiscard]] Sim build(std::vector<Spawn> spawns) {
    Scenario scenario{};
    scenario.seed = kSeed;
    scenario.spawns = std::move(spawns);
    return Sim::create(scenario);
}

/// The `index`th unit of a side, in the id order `Sim::create` places them in.
[[nodiscard]] UnitId nth(const Sim& sim, Side side, int index) {
    int seen = 0;
    for (const Unit& unit : sim.units()) {
        if (unit.side != side) {
            continue;
        }
        if (seen == index) {
            return unit.id;
        }
        ++seen;
    }
    return UnitId{};
}

void take_control(Sim& sim, UnitId unit) {
    sim.enqueue(Order{unit, orders::TakeControl{}});
    sim.step();
}

/// A duel: one player tank, one enemy recycler three tiles due east. Nothing else is on the map.
struct Duel {
    Sim sim;
    UnitId tank{};
    UnitId enemy{};
};

[[nodiscard]] Duel duel() {
    Duel out{};
    out.sim = build(
        {{UnitKind::Tank, Side::Player, 100, 100}, {UnitKind::Recycler, Side::Enemy, 103, 100}});
    out.tank = nth(out.sim, Side::Player, 0);
    out.enemy = nth(out.sim, Side::Enemy, 0);
    return out;
}

/// A squad: two player tanks ten tiles apart, and two enemy recyclers -- the near one (three to
/// six tiles off) and the far one, out of the FIRST tank's reach but inside the second's. So which
/// tank may shoot what is a real question, and a case can say "the rest of the army kept firing".
struct Squad {
    Sim sim;
    UnitId tank{};       ///< player tank at (100,100)
    UnitId spare{};      ///< player tank at (100,110)
    UnitId near_enemy{}; ///< enemy recycler at (103,105): 5.8 tiles from either tank
    UnitId far_enemy{};  ///< enemy recycler at (96,115): in the spare's reach, not the tank's
};

[[nodiscard]] Squad squad() {
    Squad out{};
    out.sim = build({{UnitKind::Tank, Side::Player, 100, 100},
                     {UnitKind::Tank, Side::Player, 100, 110},
                     {UnitKind::Recycler, Side::Enemy, 103, 105},
                     {UnitKind::Recycler, Side::Enemy, 96, 115}});
    out.tank = nth(out.sim, Side::Player, 0);
    out.spare = nth(out.sim, Side::Player, 1);
    out.near_enemy = nth(out.sim, Side::Enemy, 0);
    out.far_enemy = nth(out.sim, Side::Enemy, 1);
    return out;
}

TEST(Driven, APlayerTankWithAHostileInRangeFiresOnItsOwn) {
    // THE REPORTED BUG, as a fact about the sim: a hull nobody has taken fires at the nearest
    // hostile in range within sixty ticks with no order and no trigger. This is what a battle looks
    // like today -- and it must keep looking like it for every unit the player is NOT in.
    Duel d = duel();
    EXPECT_TRUE(fired(shooters_over(d.sim, kWindow), d.tank))
        << "the premise of the bug report: the hull fires itself";
}

TEST(Driven, ATakenTankDoesNotFireUntilThePlayerPullsTheTrigger) {
    Duel d = duel();
    take_control(d.sim, d.tank);
    EXPECT_FALSE(fired(shooters_over(d.sim, kWindow), d.tank))
        << "the player's own hull fired itself -- the bug the owner reported";
}

TEST(Driven, AnUndrivenTankInTheSameScenarioStillFires) {
    // The fix is ONE unit wide. Take control of the first tank and the second one keeps firing at
    // the enemy it was already engaging, exactly as it did before the order existed.
    Squad s = squad();
    take_control(s.sim, s.tank);
    const std::vector<UnitId> shooters = shooters_over(s.sim, kWindow);
    EXPECT_TRUE(fired(shooters, s.spare)) << "an undriven tank must keep firing";
    EXPECT_FALSE(fired(shooters, s.tank)) << "the driven tank fired itself";
}

TEST(Driven, TakingADifferentUnitReleasesTheOldOne) {
    // The helm is one slot, not a growing set: taking a second unit releases the first, which goes
    // back to firing itself. The player is in exactly one tank at a time.
    Squad s = squad();
    take_control(s.sim, s.tank);
    EXPECT_FALSE(fired(shooters_over(s.sim, kWindow), s.tank)) << "the taken tank fired";

    take_control(s.sim, s.spare);
    const std::vector<UnitId> shooters = shooters_over(s.sim, kWindow);
    EXPECT_FALSE(fired(shooters, s.spare)) << "the newly taken tank fired itself";
    EXPECT_TRUE(fired(shooters, s.tank)) << "the released tank did not go back to firing";
}

TEST(Driven, ATakeControlNamingADeadUnitSilencesNobody) {
    // An order naming a unit that has died is not an order the sim may carry out (the sim
    // re-validates at apply time), and above all it must not move the helm: the unit the player is
    // really in stays silent, and the rest of the army keeps firing. A dead name treated as a new
    // helm would hand the first tank back its gun.
    Squad s = squad();
    take_control(s.sim, s.tank);
    EXPECT_FALSE(fired(shooters_over(s.sim, profile(UnitKind::Tank).cooldown_ticks * 20), s.tank))
        << "the premise: the driven tank held fire while the spare killed the near recycler";
    ASSERT_FALSE(s.sim.find(s.near_enemy)->alive) << "the spare tank never killed the near enemy";

    take_control(s.sim, s.near_enemy); // dead: dropped, and the helm stays where it was
    const std::vector<UnitId> shooters = shooters_over(s.sim, kWindow);
    EXPECT_FALSE(fired(shooters, s.tank)) << "a dead name moved the helm off the player's tank";
    EXPECT_TRUE(fired(shooters, s.spare)) << "the rest of the army was silenced by a dead name";
}

TEST(Driven, TheEnemyKeepsFiringAtATakenTank) {
    // Nothing about the AI changes: the skip is the player's own hull, not a truce. An enemy tank
    // in range still shoots the unit the player is sitting in.
    Sim sim = build(
        {{UnitKind::Tank, Side::Player, 100, 100}, {UnitKind::Tank, Side::Enemy, 110, 100}});
    const UnitId mine = nth(sim, Side::Player, 0);
    const UnitId enemy = nth(sim, Side::Enemy, 0);
    take_control(sim, mine);
    const std::vector<UnitId> shooters = shooters_over(sim, kWindow);
    EXPECT_FALSE(fired(shooters, mine)) << "the player's hull fired itself";
    EXPECT_TRUE(fired(shooters, enemy)) << "the enemy's gun must be untouched by the order";
}

} // namespace
} // namespace protekt::sim
