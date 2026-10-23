#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "protekt/sim/orders.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/scrap.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"

namespace {

using protekt::sim::Fixed;
using protekt::sim::Order;
using protekt::sim::Scenario;
using protekt::sim::Scrap;
using protekt::sim::Side;
using protekt::sim::Sim;
using protekt::sim::Spawn;
using protekt::sim::Stance;
using protekt::sim::Unit;
using protekt::sim::UnitId;
using protekt::sim::UnitKind;

constexpr std::uint64_t kSeed = 20261023;
/// A skirmish that ENDS with wreckage on the ground and nothing left to fight: the player's three
/// tanks against one of the enemy's, on the open ground east of the factory. The scavenger (unit 2,
/// tile (58, 40)) walks to the wreck and hauls it home to the factory at (52, 45).
Scenario fetch_battle() {
    Scenario s;
    s.seed = kSeed;
    s.spawns = {Spawn{UnitKind::Factory, Side::Player, 52, 45},
                Spawn{UnitKind::Scavenger, Side::Player, 58, 40},
                Spawn{UnitKind::Tank, Side::Player, 57, 41},
                Spawn{UnitKind::Tank, Side::Player, 58, 41},
                Spawn{UnitKind::Tank, Side::Player, 59, 41},
                Spawn{UnitKind::Tank, Side::Enemy, 57, 43}};
    return s;
}

/// The same fight, with the hauler parked well back: eight tiles of open ground between it and the
/// wreckage, so that reaching a piece is work it visibly does rather than a step it is already
/// inside of.
Scenario walk_battle() {
    Scenario s = fetch_battle();
    s.spawns.at(1).tile_x = 49;
    s.spawns.at(1).tile_y = 44;
    return s;
}

/// A battle the player can LOSE a gun in and still win: a turret forward at (58, 45) against three
/// enemy tanks, its two tanks back at (50, 45)/(51, 45), and the hauler held out of the fight at
/// (48, 47) until it is over. The enemy's wrecks land near the front, the hauler banks them, and a
/// side that has lost a gun and can afford a tank gets one built.
Scenario build_battle() {
    Scenario s;
    s.seed = kSeed;
    s.spawns = {Spawn{UnitKind::Factory, Side::Player, 52, 45},
                Spawn{UnitKind::Scavenger, Side::Player, 48, 47},
                Spawn{UnitKind::Turret, Side::Player, 58, 45},
                Spawn{UnitKind::Tank, Side::Player, 50, 45},
                Spawn{UnitKind::Tank, Side::Player, 51, 45},
                Spawn{UnitKind::Tank, Side::Player, 52, 46},
                Spawn{UnitKind::Tank, Side::Enemy, 60, 45},
                Spawn{UnitKind::Tank, Side::Enemy, 60, 46},
                Spawn{UnitKind::Tank, Side::Enemy, 61, 46}};
    return s;
}

/// How many living ARMED units a side has: the count the build rule looks at.
int armed_alive(const Sim& sim, Side side) {
    int n = 0;
    for (const Unit& unit : sim.units()) {
        if (unit.alive && unit.side == side && protekt::sim::spec_of(unit.kind).range.raw > 0) {
            ++n;
        }
    }
    return n;
}

/// Holds the hauler out of the fight for `ticks`, then lets it go: the rig has to lose a gun and
/// win the fight before anything is collected, and a fetch that starts mid-battle is a coin toss.
void hold_the_hauler(Sim& sim, int ticks) {
    sim.enqueue(Order{sim.units().at(1).id,
                      protekt::sim::orders::GuardAt{Fixed::from_int(48), Fixed::from_int(47)}});
    for (int i = 0; i < ticks; ++i) {
        sim.step();
    }
    sim.enqueue(Order{sim.units().at(1).id, protekt::sim::orders::Stop{}});
}

constexpr std::int64_t kFetchRadiusRaw2
    = (static_cast<std::int64_t>(protekt::sim::kFetchRadiusTiles) << 16)
      * (static_cast<std::int64_t>(protekt::sim::kFetchRadiusTiles) << 16);

std::size_t living_sides(const Sim& sim, Side side) {
    std::size_t n = 0;
    for (const Unit& unit : sim.units()) {
        if (unit.alive && unit.side == side) {
            ++n;
        }
    }
    return n;
}

/// Steps until `done()` or the tick limit, and reports whether it became true.
template <typename Predicate> bool step_until(Sim& sim, int limit, Predicate done) {
    for (int i = 0; i < limit; ++i) {
        sim.step();
        if (done(sim)) {
            return true;
        }
    }
    return false;
}

std::int64_t distance_to_nearest_piece(const Sim& sim) {
    const Unit& scav = sim.units().at(1);
    std::int64_t best = -1;
    for (const Scrap& piece : sim.scraps()) {
        const std::int64_t dx = static_cast<std::int64_t>(piece.x.raw) - scav.x.raw;
        const std::int64_t dy = static_cast<std::int64_t>(piece.y.raw) - scav.y.raw;
        const std::int64_t d = dx * dx + dy * dy;
        if (best < 0 || d < best) {
            best = d;
        }
    }
    return best;
}

} // namespace

TEST(Scavenger, TheGroundIsWorkUntilThereIsWreckageOnIt) {
    // Before anything dies there is nothing to fetch: the hauler stands exactly where it was put,
    // and the sim is not driving it (Idle, not Fetch).
    Sim sim = Sim::create(fetch_battle());
    const Unit& before = sim.units().at(1);
    const auto x0 = before.x.raw;
    const auto y0 = before.y.raw;
    ASSERT_TRUE(sim.scraps().empty());
    ASSERT_EQ(before.stance, Stance::Idle);
    sim.step();
    sim.step();
    sim.step();
    const Unit& after = sim.units().at(1);
    EXPECT_EQ(after.x.raw, x0);
    EXPECT_EQ(after.y.raw, y0);
    EXPECT_EQ(after.stance, Stance::Idle) << "no wreckage: there is nothing to be sent to";
}

TEST(Scavenger, AnIdleHaulerIsSentToTheWreckageAndReachesIt) {
    // The sim drives a hauler with no orders of its own: it is sent to the wreckage, it closes on
    // the piece, and it takes it.
    Sim sim = Sim::create(walk_battle());
    ASSERT_TRUE(step_until(sim, 600, [](const Sim& s) { return !s.scraps().empty(); }))
        << "the wreck never appeared";
    sim.step(); // the wreck appears at the END of the step it died in: the fetch runs next tick
    EXPECT_EQ(sim.units().at(1).stance, Stance::Fetch) << "the sim drives a hauler with no orders";
    const std::size_t pieces = sim.scraps().size();
    ASSERT_GT(pieces, 0U) << "the wreck is on the ground";
    const std::int64_t start = distance_to_nearest_piece(sim);
    ASSERT_GE(start, 0) << "there is wreckage to fetch";
    ASSERT_GT(start, kFetchRadiusRaw2) << "the work is real: it starts outside the pick-up radius";
    std::int64_t closest = start;
    ASSERT_TRUE(step_until(sim, 900, [&closest, pieces](const Sim& s) {
        closest = std::min(closest, distance_to_nearest_piece(s));
        // A piece inside its own yard's radius is taken and handed over in the SAME tick, so "it
        // took one" is either a load aboard or one piece fewer on the ground.
        return s.units().at(1).cargo > 0 || s.scraps().size() < pieces;
    })) << "it never reached a piece";
    EXPECT_LE(closest, kFetchRadiusRaw2) << "it got close enough to take it";
    EXPECT_LT(closest, start) << "and it closed on it, rather than drifting away";
    EXPECT_LT(sim.scraps().size(), pieces) << "the piece it took left the ground";
}

TEST(Scavenger, TheLoadIsBankedAtItsOwnFactory) {
    Sim sim = Sim::create(fetch_battle());
    const std::size_t pieces_at_start = sim.scraps().size();
    EXPECT_EQ(pieces_at_start, 0);
    ASSERT_TRUE(step_until(sim, 600, [](const Sim& s) { return !s.scraps().empty(); }));
    ASSERT_TRUE(step_until(sim, 2000, [](const Sim& s) { return s.ore(Side::Player) > 0; }))
        << "a full fetch is one pick-up and one delivery";
    const Unit& scav = sim.units().at(1);
    EXPECT_EQ(sim.ore(Side::Player), 1) << "one piece, one load, one delivery";
    EXPECT_EQ(scav.cargo, 0) << "the load changed hands: it is not still aboard";
    EXPECT_EQ(scav.stance, Stance::Fetch) << "three pieces are still on the ground";
    // The delivery happened inside the yard: near its OWN factory, not just anywhere.
    const Unit& home = sim.units().at(0);
    const std::int64_t radius = static_cast<std::int64_t>(protekt::sim::kDeliverRadiusTiles) << 16;
    const std::int64_t dx = static_cast<std::int64_t>(scav.x.raw) - home.x.raw;
    const std::int64_t dy = static_cast<std::int64_t>(scav.y.raw) - home.y.raw;
    EXPECT_LE(dx * dx + dy * dy, radius * radius);
    // And the piece it handed over left the ground.
    EXPECT_EQ(sim.scraps().size(), 3U);
}

TEST(Scavenger, EveryLoadOnTheGroundIsEventuallyBanked) {
    // The whole loop, end to end: one wreck's four pieces, carried home one at a time, until the
    // ground is bare and the ledger reads four.
    Sim sim = Sim::create(fetch_battle());
    ASSERT_TRUE(step_until(sim, 600, [](const Sim& s) { return !s.scraps().empty(); }))
        << "the wreck never appeared";
    ASSERT_TRUE(step_until(sim, 4000, [](const Sim& s) {
        return s.ore(Side::Player) >= protekt::sim::kScrapPerWreck;
    })) << "the hauler never banked the wreck";
    EXPECT_EQ(sim.ore(Side::Player), protekt::sim::kScrapPerWreck) << "one wreck, one load a piece";
    EXPECT_TRUE(sim.scraps().empty()) << "everything on the ground was carried off";
    EXPECT_EQ(sim.units().at(1).stance, Stance::Idle) << "with nothing left to fetch, it stands";
    EXPECT_EQ(living_sides(sim, Side::Player), 5U) << "the factory, the hauler and three tanks";
}

TEST(Scavenger, AnOrderOutranksTheFetch) {
    // The sim drives a hauler only while it has no orders: told to go somewhere, it goes there --
    // and picks up what is under its tracks on the way, because that is a matter of where it is.
    Sim sim = Sim::create(fetch_battle());
    ASSERT_TRUE(step_until(sim, 600, [](const Sim& s) { return !s.scraps().empty(); }));
    const std::size_t pieces = sim.scraps().size();
    const Scrap target = sim.scraps().at(0);
    const UnitId scav = sim.units().at(1).id;
    sim.enqueue(Order{scav, protekt::sim::orders::MoveTo{target.x, target.y}});
    sim.step();
    EXPECT_EQ(sim.units().at(1).stance, Stance::MoveTo)
        << "an order replaces the fetch, every time";
    ASSERT_TRUE(step_until(sim, 600, [](const Sim& s) { return s.units().at(1).cargo > 0; }))
        << "it drove over the piece and did not take it";
    EXPECT_EQ(sim.units().at(1).stance, Stance::MoveTo) << "still obeying the order, not fetching";
    EXPECT_EQ(sim.scraps().size(), pieces - 1) << "the piece it took left the ground";
}

TEST(Scavenger, WithNoBaseToBankItTheLoadStaysAboard) {
    // Hauling with no factory of its own: the load is kept, nothing is banked, and the hauler does
    // not wander -- there is nowhere to take it.
    Scenario s = fetch_battle();
    s.spawns.erase(s.spawns.begin()); // no factory
    Sim sim = Sim::create(s);
    ASSERT_TRUE(step_until(sim, 2000, [](const Sim& s) { return s.units().at(0).cargo > 0; }))
        << "the hauler never picked anything up";
    EXPECT_EQ(sim.ore(Side::Player), 0) << "nowhere to bank it: the ledger stays empty";
    for (int i = 0; i < 120; ++i) {
        sim.step();
    }
    EXPECT_EQ(sim.units().at(0).cargo, 1) << "it keeps what it picked up";
    EXPECT_EQ(sim.units().at(0).stance, Stance::Idle) << "and waits where it stands";
}

TEST(Scavenger, TwoRunsOfTheSameBattleBankTheSameOre) {
    Sim a = Sim::create(fetch_battle());
    Sim b = Sim::create(fetch_battle());
    for (int i = 0; i < 3000; ++i) {
        a.step();
        b.step();
    }
    EXPECT_EQ(a.ore(Side::Player), b.ore(Side::Player));
    EXPECT_EQ(a.ore(Side::Enemy), b.ore(Side::Enemy));
    EXPECT_EQ(a.scraps().size(), b.scraps().size());
    EXPECT_EQ(a.state_hash(), b.state_hash());
    EXPECT_GT(a.ore(Side::Player), 0) << "and there was something to be deterministic about";
}

TEST(Scrap, WreckageNeverRestsOnGroundAVehicleCannotStandOn) {
    // The scatter is a pure hash, so it can drop a piece in a crater -- a tile no vehicle may
    // enter. A piece there can never be reached and the hauler would drive at the crater rim
    // forever; the battle's litter therefore SETTLES on ground a hauler can stand on. The anchor
    // below is a real case: the raw scatter for this very wreck puts a piece on tile (55, 41),
    // which is impassable.
    Sim sim = Sim::create(fetch_battle());
    ASSERT_TRUE(step_until(sim, 600, [](const Sim& s) { return !s.scraps().empty(); }));
    // The enemy's hull, exactly where it fell.
    const Unit* dead = nullptr;
    for (const Unit& unit : sim.units()) {
        if (!unit.alive && unit.side == Side::Enemy) {
            dead = &unit;
        }
    }
    ASSERT_NE(dead, nullptr) << "the enemy tank died, so there is a wreck";
    std::vector<Scrap> raw;
    protekt::sim::scatter(raw, kSeed, dead->id, dead->x, dead->y);
    ASSERT_EQ(raw.size(), static_cast<std::size_t>(protekt::sim::kScrapPerWreck));
    int off_ground = 0;
    for (const Scrap& piece : raw) {
        const int tx = piece.x.raw >> 16;
        const int ty = piece.y.raw >> 16;
        if (!protekt::sim::tile_passable(kSeed, tx, ty)) {
            ++off_ground;
        }
    }
    ASSERT_GT(off_ground, 0) << "the anchor is stale: the raw scatter no longer lands off-ground, "
                                "so this test would pass with the settling removed";
    // The battle's own litter, though, is never on a tile no vehicle may enter.
    for (const Scrap& piece : sim.scraps()) {
        const int tx = piece.x.raw >> 16;
        const int ty = piece.y.raw >> 16;
        EXPECT_TRUE(protekt::sim::tile_passable(kSeed, tx, ty))
            << "a piece settled on impassable ground at (" << tx << ", " << ty << ")";
    }
}

TEST(Build, AFactoryWithOreAndALostGunBuildsATank) {
    // The whole economy, end to end: the player loses a gun, wins the fight, hauls the wrecks home
    // and the factory builds what it lost. Exactly one build, on the production beat, for
    // `kTankOreCost` loads, at the factory.
    Sim sim = Sim::create(build_battle());
    const std::size_t spawned = sim.units().size();
    const int armed_at_start = armed_alive(sim, Side::Player);
    hold_the_hauler(sim, 900);
    EXPECT_EQ(sim.units().size(), spawned) << "nothing is built before anything is afforded";
    ASSERT_LT(armed_alive(sim, Side::Player), armed_at_start) << "the rig lost a gun to the enemy";

    std::uint32_t built_tick = 0;
    int ore_before = 0;
    int ore_after = 0;
    int built_id = 0;
    for (int t = 0; t < 6000; ++t) {
        const int before = sim.ore(Side::Player);
        sim.step();
        if (sim.units().size() > spawned) {
            built_tick = sim.tick();
            ore_before = before;
            ore_after = sim.ore(Side::Player);
            built_id = static_cast<int>(sim.units().back().id.value);
            break;
        }
    }
    ASSERT_NE(built_tick, 0U) << "the factory never built anything";
    const Unit& built = sim.units().back();
    EXPECT_EQ(built.kind, UnitKind::Tank) << "a tank is what a factory makes";
    EXPECT_EQ(built.hp, protekt::sim::kTankHp) << "it comes off the line at full strength";
    EXPECT_GT(built_id, static_cast<int>(spawned)) << "a new unit, not a spawned one";
    EXPECT_EQ((built_tick - 1) % protekt::sim::kBuildIntervalTicks, 0U)
        << "built on the production beat, not whenever the ore arrived";
    EXPECT_EQ(ore_before - ore_after, static_cast<int>(protekt::sim::kTankOreCost))
        << "exactly the price of a tank left the ledger";
    // It appears at the factory: its own tile or the nearest free one within the build radius.
    const Unit& home = sim.units().at(0);
    const int dx = std::abs((built.x.raw >> 16) - (home.x.raw >> 16));
    const int dy = std::abs((built.y.raw >> 16) - (home.y.raw >> 16));
    EXPECT_LE(dx, protekt::sim::kBuildSpawnRadiusTiles);
    EXPECT_LE(dy, protekt::sim::kBuildSpawnRadiusTiles);
    // And the side is back up to strength: it replaced a loss, it did not rearm.
    EXPECT_EQ(armed_alive(sim, Side::Player), armed_at_start);
}

TEST(Build, NoLossIsNoBuild) {
    // The other half of the rule: a side that has lost nothing does not spend, however rich it is.
    Sim sim = Sim::create(fetch_battle());
    for (int t = 0; t < 3000; ++t) {
        sim.step();
    }
    EXPECT_GT(sim.ore(Side::Player), 0) << "the hauler banked the wreck";
    EXPECT_EQ(sim.units().size(), 6U) << "and nothing was built: no gun was lost";
    EXPECT_EQ(sim.ore(Side::Player), static_cast<int>(protekt::sim::kScrapPerWreck))
        << "the ledger is still what was delivered";
}

TEST(Ledger, RepairHappensAndTheLedgerPaysForNothing) {
    // The ruling: repairs and replenishment are always available, whatever the scrap says, and ore
    // is only ever spent on new items. This rig is a fight the player wins while being shot: a hull
    // takes a hit, mends 1 hp on the repair beat, takes the next. Every mend happens while the
    // ledger is still empty, and the ledger never goes down.
    Sim sim = Sim::create(fetch_battle());
    std::vector<std::uint16_t> last_hp(sim.units().size(), 0);
    for (std::size_t i = 0; i < sim.units().size(); ++i) {
        last_hp.at(i) = sim.units().at(i).hp;
    }
    bool mended = false;
    std::uint16_t ore_high = 0;
    for (int t = 0; t < 3000; ++t) {
        const std::uint16_t ore_before = sim.ore(Side::Player);
        sim.step();
        for (std::size_t i = 0; i < sim.units().size() && i < last_hp.size(); ++i) {
            const Unit& unit = sim.units().at(i);
            if (unit.alive && unit.hp > last_hp.at(i)) {
                mended = true;
                // The ruling in one line: a mend costs nothing. Ore can have ARRIVED this tick
                // (the hauler is still working), but the repair never takes any.
                EXPECT_GE(sim.ore(Side::Player), ore_before) << "a mended hull drew on the ledger";
            }
            last_hp.at(i) = unit.hp;
        }
        EXPECT_GE(sim.ore(Side::Player), ore_before) << "ore is never spent on repair or ammo";
        ore_high = std::max(ore_high, sim.ore(Side::Player));
    }
    EXPECT_TRUE(mended) << "no unit was ever mended, so the ruling was never at risk";
    EXPECT_GE(ore_high, static_cast<std::uint16_t>(protekt::sim::kScrapPerWreck))
        << "the hauler banked the wreck, so there was ore to be tempted with";
}

TEST(Scavenger, TheHaulerIsNeverLeftDrivingAtAPieceItCannotReach) {
    // The end of the loop for a wreck whose scatter dropped a piece in a crater: every piece is
    // still fetched and banked, and the hauler ends up standing rather than steering at a rim it
    // can never cross. Without the settle step this rig leaves one piece on the ground forever.
    Sim sim = Sim::create(walk_battle());
    ASSERT_TRUE(step_until(sim, 4000, [](const Sim& s) {
        return s.scraps().empty() && s.ore(Side::Player) >= protekt::sim::kScrapPerWreck;
    })) << "a piece was left where no hauler could reach it";
    EXPECT_EQ(sim.ore(Side::Player), protekt::sim::kScrapPerWreck);
    EXPECT_EQ(sim.units().at(1).stance, Stance::Idle) << "nothing left to fetch: it stands";
}
