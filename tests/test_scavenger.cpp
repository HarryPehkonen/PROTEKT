#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "protekt/sim/orders.hpp"
#include "protekt/sim/outcome.hpp"
#include "protekt/sim/replay.hpp"
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
using protekt::sim::WarOutcome;

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
// The player's tanks stand off each other's lines of fire and the factory's: a bolt hits a friendly
// in its way (friendly fire), so the old file along row 45 shot itself and the factory.
Scenario build_battle() {
    Scenario s;
    s.seed = kSeed;
    s.spawns = {Spawn{UnitKind::Factory, Side::Player, 52, 45},
                Spawn{UnitKind::Scavenger, Side::Player, 48, 47},
                Spawn{UnitKind::Turret, Side::Player, 58, 45},
                Spawn{UnitKind::Tank, Side::Player, 50, 46},
                Spawn{UnitKind::Tank, Side::Player, 51, 47},
                Spawn{UnitKind::Tank, Side::Player, 52, 49},
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

TEST(Build, NothingIsBuiltUntilItIsOrdered) {
    // Pins the REMOVAL of the sim's automatic production beat. The player loses a gun, wins the
    // fight and hauls the wrecks home, so the bank fills past the removed beat's price while the
    // side sits short of its starting guns -- exactly the state the old beat replaced a tank in.
    // Many production beats go by and NOTHING is built; units come from a queued Build order alone
    // (the queue's own tests pin that). The bank is not asserted frozen (the haulers keep
    // delivering), only the units produced.
    Sim sim = Sim::create(build_battle());
    const std::size_t spawned = sim.units().size();
    const int armed_at_start = armed_alive(sim, Side::Player);
    hold_the_hauler(sim, 900);
    ASSERT_LT(armed_alive(sim, Side::Player), armed_at_start) << "the rig lost a gun to the enemy";

    int richest = sim.ore(Side::Player);
    for (int t = 0; t < 6000; ++t) {
        sim.step();
        richest = std::max(richest, static_cast<int>(sim.ore(Side::Player)));
        ASSERT_EQ(sim.units().size(), spawned)
            << "a unit appeared at tick " << sim.tick() << " with no Build order queued";
    }
    // The removed beat's price was 4 loads; the bank passed it, so the beat would have fired.
    constexpr int kRemovedBeatPrice = 4;
    EXPECT_GE(richest, kRemovedBeatPrice) << "the bank never held what the beat charged";
    EXPECT_LT(armed_alive(sim, Side::Player), armed_at_start) << "the lost gun is never replaced";
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

/// OBJ-a -- THE WAR IS WON AND LOST ON RECYCLERS. C8 shipped an attrition verdict (no gun and no
/// factory able to build one); Harri retired it: destroy the enemy's recycler and you WIN, whatever
/// they have left; lose yours and you LOSE, instantly and completely. `war_outcome` is the one
/// place the rule lives, and it is derived, never stored and never hashed.
///
/// The rig: each side has a recycler and a gun, the player's gun two tiles from the enemy's
/// recycler and the enemy's gun far across the map, so the recycler can die with every other
/// enemy unit alive and armed.
Scenario a_recycler_war() {
    Scenario s;
    s.seed = 20261023;
    s.spawns = {Spawn{UnitKind::Tank, Side::Player, 60, 50},
                Spawn{UnitKind::Recycler, Side::Player, 55, 50},
                Spawn{UnitKind::Tank, Side::Enemy, 120, 120},
                Spawn{UnitKind::Recycler, Side::Enemy, 62, 50}};
    return s;
}

UnitId first_of(const Sim& sim, Side side, UnitKind kind) {
    for (const Unit& unit : sim.units()) {
        if (unit.side == side && unit.kind == kind) {
            return unit.id;
        }
    }
    return UnitId{};
}

TEST(Outcome, KillingTheEnemyRecyclerWinsAtOnceWhateverTheEnemyHasLeft) {
    Sim sim = Sim::create(a_recycler_war());
    EXPECT_EQ(sim.war_outcome(), WarOutcome::Undecided) << "both recyclers alive";
    const UnitId target = first_of(sim, Side::Enemy, UnitKind::Recycler);
    sim.enqueue(
        Order{first_of(sim, Side::Player, UnitKind::Tank), protekt::sim::orders::Attack{target}});
    ASSERT_TRUE(step_until(sim, 3000, [&](const Sim& s) { return !s.find(target)->alive; }))
        << "the rig must kill the enemy recycler";
    EXPECT_EQ(armed_alive(sim, Side::Enemy), 1)
        << "the enemy's gun is alive: this is not attrition";
    EXPECT_EQ(sim.war_outcome(), WarOutcome::PlayerWon);
}

TEST(Outcome, LosingYourRecyclerLosesAtOnceWithYourGunsStillFiring) {
    Scenario s = a_recycler_war();
    // Mirror the fight: the enemy's gun is at the player's recycler, the player's gun is far away.
    s.spawns = {Spawn{UnitKind::Tank, Side::Player, 120, 120},
                Spawn{UnitKind::Recycler, Side::Player, 62, 50},
                Spawn{UnitKind::Tank, Side::Enemy, 60, 50},
                Spawn{UnitKind::Recycler, Side::Enemy, 130, 130}};
    Sim sim = Sim::create(s);
    const UnitId mine = first_of(sim, Side::Player, UnitKind::Recycler);
    sim.enqueue(
        Order{first_of(sim, Side::Enemy, UnitKind::Tank), protekt::sim::orders::Attack{mine}});
    ASSERT_TRUE(step_until(sim, 3000, [&](const Sim& w) { return !w.find(mine)->alive; }));
    EXPECT_EQ(armed_alive(sim, Side::Player), 1) << "the player still has a gun";
    EXPECT_EQ(sim.war_outcome(), WarOutcome::PlayerLost);
}

TEST(Outcome, ASideWithNoGunAndNoFactoryHasNotLostWhileItsRecyclerLives) {
    // The direct retirement of the attrition rule: the player has only a recycler, the enemy only
    // a recycler and a gun. Under C8 the player read as lost; now nothing is decided.
    Scenario s;
    s.seed = 1;
    s.spawns = {Spawn{UnitKind::Recycler, Side::Player, 60, 50},
                Spawn{UnitKind::Recycler, Side::Enemy, 120, 120},
                Spawn{UnitKind::Tank, Side::Enemy, 125, 120}};
    const Sim sim = Sim::create(s);
    ASSERT_EQ(armed_alive(sim, Side::Player), 0);
    EXPECT_EQ(sim.war_outcome(), WarOutcome::Undecided);
}

TEST(Outcome, ARigThatNeverGaveAnyoneARecyclerIsUndecidedForEver) {
    // The guard C8 taught: a side that never HAD a recycler has not lost one. Without it every rig
    // whose scenario spawns none reads as a defeat on tick zero -- and as a victory, for the other.
    Scenario s;
    s.seed = 20261023;
    s.spawns
        = {Spawn{UnitKind::Tank, Side::Player, 60, 50}, Spawn{UnitKind::Tank, Side::Enemy, 62, 50},
           Spawn{UnitKind::Tank, Side::Enemy, 62, 51}, Spawn{UnitKind::Tank, Side::Enemy, 61, 51}};
    Sim sim = Sim::create(s);
    EXPECT_EQ(sim.war_outcome(), WarOutcome::Undecided);
    for (int i = 0; i < 3000; ++i) {
        sim.step();
    }
    ASSERT_EQ(armed_alive(sim, Side::Player), 0) << "the player's gun must have died";
    EXPECT_EQ(sim.war_outcome(), WarOutcome::Undecided)
        << "annihilated, yet no recycler was ever lost";
}

TEST(Outcome, AOneSidedRecyclerRigOnlyDecidesForTheSideThatHadOne) {
    // Only the player was given a recycler: its enemy never had one, so cannot be "won against".
    Scenario s;
    s.seed = 1;
    s.spawns = {Spawn{UnitKind::Recycler, Side::Player, 60, 50},
                Spawn{UnitKind::Tank, Side::Enemy, 120, 120}};
    EXPECT_EQ(Sim::create(s).war_outcome(), WarOutcome::Undecided);
}

TEST(Outcome, TheOutcomeIsDerivedNotHashedAndAskingChangesNothing) {
    Sim asked = Sim::create(a_recycler_war());
    Sim silent = Sim::create(a_recycler_war());
    for (int i = 0; i < 400; ++i) {
        const WarOutcome first = asked.war_outcome();
        EXPECT_EQ(asked.war_outcome(), first) << "two reads disagree";
        asked.step();
        silent.step();
    }
    EXPECT_EQ(asked.state_hash(), silent.state_hash());
}

TEST(Outcome, AnEmptyBattleIsUndecided) {
    EXPECT_EQ(Sim::create(Scenario{}).war_outcome(), WarOutcome::Undecided);
}

// -------------------------------------------------------------- OBJ: win/lose as a LIST
//
// Harri's call: destroying the enemy recycler/matriarch wins INSTANTLY and TOTALLY whatever they
// have left, and losing yours loses instantly and completely. But win and lose are SCRIPTED
// conditions (an escort mission fails if the APC dies and wins when it arrives), so the rule is a
// list in the scenario's own data rather than a predicate compiled into the sim.
//
// A unit in the sim engages hostiles in range on its own (the commander owns goals and the helm,
// not the trigger), so these rigs place each side's units clear of what they must NOT touch: the
// one death a test asks for is the only one in reach. The orders below are what makes the death
// happen on the tick the test wants, not what makes it possible at all.

namespace {

namespace sim = protekt::sim;

using sim::Objective;
using sim::Scenario;
using sim::Side;
using sim::Sim;
using sim::Spawn;
using sim::UnitId;
using sim::UnitKind;
using sim::WarOutcome;

/// The player's gun two tiles from the enemy's recycler, the enemy's gun at the far edge.
[[nodiscard]] Scenario a_war_the_player_can_win(std::vector<Objective> objectives) {
    Scenario scenario;
    scenario.seed = 20261023;
    scenario.spawns = {Spawn{UnitKind::Tank, Side::Player, 60, 50},
                       Spawn{UnitKind::Recycler, Side::Player, 55, 50},
                       Spawn{UnitKind::Tank, Side::Enemy, 120, 120},
                       Spawn{UnitKind::Recycler, Side::Enemy, 62, 50}};
    scenario.objectives = std::move(objectives);
    return scenario;
}

/// Its mirror: the enemy's gun two tiles from the player's recycler, the player's gun far away.
[[nodiscard]] Scenario a_war_the_player_loses(std::vector<Objective> objectives) {
    Scenario scenario;
    scenario.seed = 20261023;
    scenario.spawns = {Spawn{UnitKind::Tank, Side::Player, 120, 120},
                       Spawn{UnitKind::Recycler, Side::Player, 62, 50},
                       Spawn{UnitKind::Tank, Side::Enemy, 60, 50},
                       Spawn{UnitKind::Recycler, Side::Enemy, 170, 170}};
    scenario.objectives = std::move(objectives);
    return scenario;
}

/// The player's hauler is the near one and the recycler stands clear of it, so the enemy's gun can
/// kill the hauler without touching the base the loss condition is not about.
[[nodiscard]] Scenario a_war_where_the_hauler_is_the_near_one(std::vector<Objective> objectives) {
    Scenario scenario;
    scenario.seed = 20261023;
    scenario.spawns = {Spawn{UnitKind::Scavenger, Side::Player, 62, 50},
                       Spawn{UnitKind::Recycler, Side::Player, 45, 50},
                       Spawn{UnitKind::Tank, Side::Enemy, 60, 50},
                       Spawn{UnitKind::Recycler, Side::Enemy, 170, 170}};
    scenario.objectives = std::move(objectives);
    return scenario;
}

/// The enemy's GUN is the near one, and its recycler stands at the far edge: what dies here is a
/// gun, and no condition about recyclers can fire by accident.
[[nodiscard]] Scenario a_war_where_the_enemy_gun_is_what_dies(std::vector<Objective> objectives) {
    Scenario scenario;
    scenario.seed = 20261023;
    scenario.spawns = {Spawn{UnitKind::Tank, Side::Player, 60, 50},
                       Spawn{UnitKind::Recycler, Side::Player, 55, 50},
                       Spawn{UnitKind::Tank, Side::Enemy, 62, 50},
                       Spawn{UnitKind::Recycler, Side::Enemy, 130, 130}};
    scenario.objectives = std::move(objectives);
    return scenario;
}

[[nodiscard]] UnitId the_only(const Sim& sim, Side side, UnitKind kind) {
    for (const sim::Unit& unit : sim.units()) {
        if (unit.side == side && unit.kind == kind) {
            return unit.id;
        }
    }
    return UnitId{};
}

/// One order, aimed at a target two tiles away, and then as long as it takes. The sim fires nothing
/// on its own, so a unit dies here only because this order said so.
[[nodiscard]] bool kill_with(Sim& sim, Side shooter, UnitId target, int limit) {
    sim.enqueue(sim::Order{the_only(sim, shooter, UnitKind::Tank), sim::orders::Attack{target}});
    for (int tick = 0; tick < limit; ++tick) {
        sim.step();
        if (!sim.find(target)->alive) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST(Objective, TheBuiltInCompositionCarriesTheRecyclerRuleAsItsOwnData) {
    // The rule is data now, and the data the game starts with is the rule Harri ruled: your own
    // recycler first, so a pair that dies together reads as a loss.
    const std::vector<Objective> built_in = sim::built_in_objectives();
    ASSERT_EQ(built_in.size(), 2U);
    EXPECT_EQ(built_in.at(0).verdict, WarOutcome::PlayerLost) << "the loss is checked first";
    EXPECT_EQ(built_in.at(0).side, Side::Player);
    EXPECT_EQ(built_in.at(0).kind, UnitKind::Recycler);
    EXPECT_EQ(built_in.at(1).verdict, WarOutcome::PlayerWon);
    EXPECT_EQ(built_in.at(1).side, Side::Enemy);
    EXPECT_EQ(built_in.at(1).kind, UnitKind::Recycler);
    EXPECT_EQ(sim::default_data().objectives, built_in);
    EXPECT_EQ(sim::skirmish(7ULL).objectives, built_in);
    // And a composition that says nothing about conditions plays the built-in ones, so a game
    // started from a scenario file that lists only units is still a war that can be won.
    EXPECT_EQ(sim::ScenarioData{}.objectives, built_in);
}

TEST(Objective, TheBuiltInListStillDecidesTheRecyclerWarBothWays) {
    Sim won = Sim::create(a_war_the_player_can_win(sim::built_in_objectives()));
    EXPECT_EQ(won.war_outcome(), WarOutcome::Undecided) << "both recyclers alive";
    ASSERT_TRUE(kill_with(won, Side::Player, the_only(won, Side::Enemy, UnitKind::Recycler), 3000));
    EXPECT_EQ(won.war_outcome(), WarOutcome::PlayerWon);

    Sim lost = Sim::create(a_war_the_player_loses(sim::built_in_objectives()));
    ASSERT_TRUE(
        kill_with(lost, Side::Enemy, the_only(lost, Side::Player, UnitKind::Recycler), 3000))
        << "the enemy gun must kill the player's recycler";
    EXPECT_EQ(lost.war_outcome(), WarOutcome::PlayerLost);
}

TEST(Objective, ALostHaulerCanBeWhatLosesTheWar) {
    // An escort in the pieces that exist: lose the hauler your economy runs on and you have lost,
    // whatever your recycler is doing.
    const std::vector<Objective> escort{
        Objective{WarOutcome::PlayerLost, Side::Player, UnitKind::Scavenger},
        Objective{WarOutcome::PlayerWon, Side::Enemy, UnitKind::Recycler}};
    Sim sim = Sim::create(a_war_where_the_hauler_is_the_near_one(escort));
    ASSERT_EQ(sim.war_outcome(), WarOutcome::Undecided);
    ASSERT_TRUE(
        kill_with(sim, Side::Enemy, the_only(sim, Side::Player, UnitKind::Scavenger), 4000));
    EXPECT_TRUE(sim.find(the_only(sim, Side::Player, UnitKind::Recycler))->alive)
        << "the recycler is alive: this is not the built-in rule";
    EXPECT_EQ(sim.war_outcome(), WarOutcome::PlayerLost);
}

TEST(Objective, TheFirstConditionThatFiresHandsDownTheVerdict) {
    // The list is ordered and the order IS the meaning: the same death, with two conditions that
    // both watch it, hands down whichever verdict stands first.
    const Objective win_first{WarOutcome::PlayerWon, Side::Enemy, UnitKind::Tank};
    const Objective lose_first{WarOutcome::PlayerLost, Side::Enemy, UnitKind::Tank};

    Sim won = Sim::create(a_war_where_the_enemy_gun_is_what_dies({win_first, lose_first}));
    ASSERT_TRUE(kill_with(won, Side::Player, the_only(won, Side::Enemy, UnitKind::Tank), 4000));
    EXPECT_EQ(won.war_outcome(), WarOutcome::PlayerWon) << "the first of the two spoke";

    Sim lost = Sim::create(a_war_where_the_enemy_gun_is_what_dies({lose_first, win_first}));
    ASSERT_TRUE(kill_with(lost, Side::Player, the_only(lost, Side::Enemy, UnitKind::Tank), 4000));
    EXPECT_EQ(lost.war_outcome(), WarOutcome::PlayerLost)
        << "the same death, and the list's order turned it round";
}

TEST(Objective, AMissionWithNoConditionsIsASandboxNotACrash) {
    Sim sim = Sim::create(a_war_the_player_can_win({}));
    ASSERT_TRUE(kill_with(sim, Side::Player, the_only(sim, Side::Enemy, UnitKind::Recycler), 3000));
    EXPECT_EQ(sim.war_outcome(), WarOutcome::Undecided)
        << "a scenario that asks for no verdict gets none";
}

// ---------------------------------------------------------------- C0: the map starts with scrap
//
// His words for instant action: *"each side gets a non-deployed recycler, some scrap, some
// vehicles."* The units are there and the litter is there, but the map started CLEAN -- scrap only
// ever fell out of a wreck -- so the hauler had nothing to fetch and the ledger could only rise
// once a fight had already happened. A skirmish now begins with a field of it near each base.

namespace {

namespace sim = protekt::sim;

using sim::Scenario;
using sim::Side;
using sim::Sim;
using sim::Spawn;
using sim::UnitId;
using sim::UnitKind;

/// The tile a piece of ground scrap stands on: its centre is at `int + 1/2`, in Q16.16 raw units.
[[nodiscard]] bool at_a_tile_centre(const sim::Scrap& piece) {
    constexpr std::int32_t kHalf = sim::Fixed::kOne / 2;
    return piece.x.raw % sim::Fixed::kOne == kHalf && piece.y.raw % sim::Fixed::kOne == kHalf;
}

[[nodiscard]] int tile_of(std::int32_t raw) { return raw / sim::Fixed::kOne; }

/// How many pieces stand within `tiles` of a point, as whole tiles.
[[nodiscard]] int pieces_near(const Scenario& scenario, int tile_x, int tile_y, int tiles) {
    int count = 0;
    for (const sim::Scrap& piece : scenario.scrap) {
        const int dx = tile_of(piece.x.raw) - tile_x;
        const int dy = tile_of(piece.y.raw) - tile_y;
        if (std::max(std::abs(dx), std::abs(dy)) <= tiles) {
            ++count;
        }
    }
    return count;
}

/// Where the last unit of `side` stands in the roster: the base's own corner, near enough for a
/// "is the field near this side" count.
void last_spawn_tile(const Scenario& scenario, Side side, int* tile_x, int* tile_y) {
    for (const Spawn& spawn : scenario.spawns) {
        if (spawn.side == side) {
            *tile_x = spawn.tile_x;
            *tile_y = spawn.tile_y;
        }
    }
}

} // namespace

TEST(ScrapField, ASkirmishStartsWithScrapNearBothBases) {
    const Scenario scenario = sim::skirmish(42);
    ASSERT_FALSE(scenario.scrap.empty()) << "the map still starts clean";
    int player_x = 0;
    int player_y = 0;
    int enemy_x = 0;
    int enemy_y = 0;
    last_spawn_tile(scenario, Side::Player, &player_x, &player_y);
    last_spawn_tile(scenario, Side::Enemy, &enemy_x, &enemy_y);
    EXPECT_GE(pieces_near(scenario, player_x, player_y, 24), sim::kInitialScrapPerSide)
        << "the player's side of the map got less than a field";
    EXPECT_GE(pieces_near(scenario, enemy_x, enemy_y, 24), sim::kInitialScrapPerSide)
        << "the enemy's side of the map got less than a field";
    for (const sim::Scrap& piece : scenario.scrap) {
        EXPECT_TRUE(at_a_tile_centre(piece)) << "a piece must stand at a tile's centre";
        EXPECT_TRUE(sim::tile_passable(scenario.seed, tile_of(piece.x.raw), tile_of(piece.y.raw)))
            << "a field on impassable ground is a field nobody can fetch";
    }
}

TEST(ScrapField, AFieldIsAPureFunctionOfItsSeedAndItsCentre) {
    std::vector<sim::Scrap> once;
    std::vector<sim::Scrap> twice;
    sim::seed_field(once, 42ULL, sim::kInitialScrapPerSide, 40, 40);
    sim::seed_field(twice, 42ULL, sim::kInitialScrapPerSide, 40, 40);
    ASSERT_EQ(once.size(), twice.size());
    for (std::size_t i = 0; i < once.size(); ++i) {
        EXPECT_EQ(once.at(i).x.raw, twice.at(i).x.raw) << i;
        EXPECT_EQ(once.at(i).y.raw, twice.at(i).y.raw) << i;
    }
    std::vector<sim::Scrap> elsewhere;
    sim::seed_field(elsewhere, 42ULL, sim::kInitialScrapPerSide, 120, 120);
    ASSERT_EQ(elsewhere.size(), once.size());
    bool moved = false;
    for (std::size_t i = 0; i < once.size() && !moved; ++i) {
        moved = once.at(i).x.raw != elsewhere.at(i).x.raw
                || once.at(i).y.raw != elsewhere.at(i).y.raw;
    }
    EXPECT_TRUE(moved) << "the field ignored its centre";
}

TEST(ScrapField, NoStartingPieceOpensInsideABuilding) {
    // A vehicle is no problem -- it drives off and the piece is there when it goes. A BUILDING
    // never moves, so a piece inside one is lost for the rest of the war, and the rings start
    // outside the base cluster to keep that from happening; this checks the ground the sim actually
    // ended up with, after settling.
    const Sim sim = Sim::create(sim::skirmish(42));
    ASSERT_FALSE(sim.scraps().empty());
    for (const sim::Scrap& piece : sim.scraps()) {
        const int tile_x = tile_of(piece.x.raw);
        const int tile_y = tile_of(piece.y.raw);
        EXPECT_TRUE(sim::tile_passable(42ULL, tile_x, tile_y))
            << "a piece on a crater rim is a wall";
        EXPECT_FALSE(sim::building_blocks(sim.units(), tile_x, tile_y))
            << "a piece inside a building can never be fetched";
    }
}

TEST(ScrapField, AStartingPieceIsFetchableAndTheLedgerRises) {
    // A hauler beside a starting piece: it takes it and banks a load, which is the whole point of
    // the field -- the economy can move before anyone has died.
    Scenario scenario;
    scenario.seed = 20261023;
    // The hauler banks at its FACTORY, not at the recycler (`home_factory`), so a rig without one
    // can carry a load for ever without the ledger ever moving.
    scenario.spawns = {Spawn{UnitKind::Scavenger, Side::Player, 40, 40},
                       Spawn{UnitKind::Factory, Side::Player, 37, 40},
                       Spawn{UnitKind::Recycler, Side::Player, 34, 40}};
    scenario.objectives = {};
    const sim::Fixed centre
        = sim::fixed_add(sim::Fixed::from_int(40), sim::Fixed::from_raw(sim::Fixed::kOne / 2));
    scenario.scrap = {sim::Scrap{centre, centre}};
    Sim sim = Sim::create(scenario);
    ASSERT_EQ(sim.scraps().size(), 1U);
    EXPECT_EQ(sim.ore(Side::Player), 0);
    for (int tick = 0; tick < 1200 && sim.ore(Side::Player) == 0; ++tick) {
        sim.step();
    }
    EXPECT_EQ(sim.ore(Side::Player), 1) << "the hauler could not reach the field's own piece";
    EXPECT_TRUE(sim.scraps().empty()) << "the piece was taken, not duplicated";
}

TEST(ScrapField, ACompositionThatAsksForNoScrapStillGetsNone) {
    // The built-in composition carries a field; a composition that says nothing about scrap (or a
    // rig assembled by hand) starts on clean ground, so every existing rig is unchanged.
    EXPECT_GT(sim::ScenarioData{}.scrap_per_side, 0) << "the built-in amount is the dial";
    sim::ScenarioData bare;
    bare.scrap_per_side = 0;
    EXPECT_TRUE(sim::skirmish(42, bare).scrap.empty());
    EXPECT_TRUE(Scenario{}.scrap.empty()) << "a hand-built battle starts clean";
}

// ------------------------------------------------------------------------------- the build queue

namespace {

namespace sim = protekt::sim;

/// A player's factory at (52, 45) with three haulers and `pieces` loads of wreckage laid out in a
/// block west of it, and nothing else: no enemy, so nothing shoots, and no armed unit, so the
/// factory's own tank-replacement beat (C4) has nothing to replace. The ledger therefore rises by
/// the haulers' deliveries alone, which is the only way a test can put scrap in the bank.
Scenario queue_battle(int pieces) {
    Scenario s;
    s.seed = kSeed;
    s.objectives = {};
    s.spawns = {Spawn{UnitKind::Factory, Side::Player, 52, 45},
                Spawn{UnitKind::Scavenger, Side::Player, 48, 47},
                Spawn{UnitKind::Scavenger, Side::Player, 48, 48},
                Spawn{UnitKind::Scavenger, Side::Player, 48, 49}};
    const sim::Fixed half = sim::Fixed::from_raw(sim::Fixed::kOne / 2);
    for (int i = 0; i < pieces; ++i) {
        s.scrap.push_back(Scrap{sim::fixed_add(sim::Fixed::from_int(42 + (i % 10)), half),
                                sim::fixed_add(sim::Fixed::from_int(36 + (i / 10)), half)});
    }
    return s;
}

/// The player's factory in `sim`.
UnitId factory_of(const Sim& sim) {
    for (const Unit& unit : sim.units()) {
        if (unit.kind == UnitKind::Factory && unit.side == Side::Player) {
            return unit.id;
        }
    }
    return UnitId{};
}

/// A build order for `producer`; batch 0 is a lone build.
Order build(UnitId producer, UnitKind kind, std::uint32_t batch = 0) {
    return Order{producer, sim::orders::Build{kind, batch}};
}

/// Every unit with an id past `first`, in the order the sim made them: what the queue built.
std::vector<const Unit*> built_since(const Sim& sim, UnitId first) {
    std::vector<const Unit*> out;
    for (const Unit& unit : sim.units()) {
        if (unit.id.value > first.value) {
            out.push_back(&unit);
        }
    }
    return out;
}

UnitId last_id(const Sim& sim) { return sim.units().back().id; }

constexpr int kQueueLimit = 20000; ///< ticks a queue test may spend waiting for the haulers

} // namespace

TEST(BuildQueue, AnEntryPaysItsKindsCostWhenItStartsAndTakesItsKindsTicks) {
    Sim sim = Sim::create(queue_battle(30));
    const UnitId factory = factory_of(sim);
    const UnitId first = last_id(sim);
    const auto& spec = sim::spec_of(UnitKind::Scavenger);
    sim.enqueue(build(factory, UnitKind::Scavenger));

    // The entry waits for its scrap and pays NOTHING until it can pay it all: the bank only rises.
    std::uint16_t before = 0;
    ASSERT_TRUE(step_until(sim, kQueueLimit, [&before, factory](const Sim& s) {
        const bool started = s.find(factory)->queue.left > 0;
        if (!started) {
            before = s.ore(Side::Player);
        }
        return started;
    })) << "an affordable entry never started";
    EXPECT_EQ(before, spec.cost - 1)
        << "it must start on the very tick the bank reached its cost, not before and not later";
    EXPECT_EQ(sim.ore(Side::Player), 0)
        << "it pays its cost when it STARTS: the whole " << spec.cost << " leaves the one bank";

    // And it takes exactly its kind's ticks: nothing is built before, a hauler is built on the
    // tick the countdown ends.
    const sim::Tick started_at = sim.tick();
    ASSERT_TRUE(built_since(sim, first).empty()) << "nothing arrives on the tick it starts";
    ASSERT_TRUE(step_until(sim, 2 * spec.build_ticks,
                           [first](const Sim& s) { return !built_since(s, first).empty(); }));
    EXPECT_EQ(sim.tick() - started_at, spec.build_ticks) << "ten seconds for a scavenger, no less";
    const auto made = built_since(sim, first);
    ASSERT_EQ(made.size(), 1U);
    EXPECT_EQ(made.front()->kind, UnitKind::Scavenger);
    EXPECT_EQ(made.front()->side, Side::Player);
    EXPECT_EQ(sim.find(factory)->queue.count, 0U) << "the item leaves the queue when it is done";
}

TEST(BuildQueue, AQueueOfThreeDrainsInOrderOneAtATime) {
    Sim sim = Sim::create(queue_battle(90));
    const UnitId factory = factory_of(sim);
    const UnitId first = last_id(sim);
    sim.enqueue(build(factory, UnitKind::Scavenger));
    sim.enqueue(build(factory, UnitKind::Tank));
    sim.enqueue(build(factory, UnitKind::Scavenger));

    std::vector<UnitKind> kinds;
    std::vector<sim::Tick> arrived;
    for (int tick = 0; tick < 3 * kQueueLimit && kinds.size() < 3; ++tick) {
        sim.step();
        const auto made = built_since(sim, first);
        if (made.size() > kinds.size()) {
            ASSERT_EQ(made.size(), kinds.size() + 1) << "two units arrived on one tick";
            kinds.push_back(made.back()->kind);
            arrived.push_back(sim.tick());
        }
    }
    ASSERT_EQ(kinds.size(), 3U) << "the queue never finished";
    EXPECT_EQ(kinds.at(0), UnitKind::Scavenger) << "first in, first out";
    EXPECT_EQ(kinds.at(1), UnitKind::Tank);
    EXPECT_EQ(kinds.at(2), UnitKind::Scavenger);
    EXPECT_GE(arrived.at(1) - arrived.at(0), sim::spec_of(UnitKind::Tank).build_ticks)
        << "the tank's countdown starts only once the hauler before it has arrived";
    EXPECT_GE(arrived.at(2) - arrived.at(1), sim::spec_of(UnitKind::Scavenger).build_ticks)
        << "one entry at a time: the second hauler's countdown waits for the tank";
}

TEST(BuildQueue, AnEntryWithNoScrapPausesAndResumesTheTickTheScrapReturns) {
    // THE RULING: a queue that runs short of scrap PAUSES, it does not cancel. The bank starts at
    // nothing and climbs one load at a time; the entry must sit untouched all the way up and begin
    // on the very step the twentieth load lands.
    Sim sim = Sim::create(queue_battle(30));
    const UnitId factory = factory_of(sim);
    const auto cost = sim::spec_of(UnitKind::Scavenger).cost;
    sim.enqueue(build(factory, UnitKind::Scavenger));

    std::uint16_t held_at = 0;
    std::uint16_t before_start = 0;
    bool started = false;
    for (int tick = 0; tick < kQueueLimit && !started; ++tick) {
        before_start = sim.ore(Side::Player);
        sim.step();
        const sim::Unit& producer = *sim.find(factory);
        if (producer.queue.left > 0) {
            started = true;
            break;
        }
        ASSERT_EQ(producer.queue.count, 1U) << "a short queue must not cancel its entry";
        ASSERT_EQ(producer.queue.left, 0U) << "a short queue must not count down";
        held_at = std::max(held_at, sim.ore(Side::Player));
    }
    ASSERT_TRUE(started);
    EXPECT_GT(held_at, 0) << "the entry watched real scrap arrive and still held: nothing is "
                             "spent in part";
    EXPECT_LT(held_at, cost);
    EXPECT_EQ(before_start, cost - 1) << "it resumed on the tick the scrap was there";
    EXPECT_EQ(sim.ore(Side::Player), 0);
}

TEST(BuildQueue, TheEleventhEntryIsRefusedAtTheCapOfTen) {
    Sim sim = Sim::create(queue_battle(0));
    const UnitId factory = factory_of(sim);
    for (std::uint32_t press = 0; press < 11; ++press) {
        sim.enqueue(build(factory, UnitKind::Scavenger, 7));
    }
    sim.step();
    EXPECT_EQ(sim.find(factory)->queue.count, sim::kBuildQueueDepth)
        << "ten shift-presses queue ten, and the eleventh is not an eleventh entry";
    EXPECT_EQ(sim::kBuildQueueDepth, 10U);

    const std::size_t logged = sim.command_log().size();
    sim.enqueue(build(factory, UnitKind::Scavenger, 7));
    EXPECT_EQ(sim.command_log().size(), logged)
        << "a press at the cap is refused at the door, so a replay never sees it";
}

TEST(BuildQueue, AnOrderThatNamesNothingBuildableOrNoProducerIsRefused) {
    Sim sim = Sim::create(queue_battle(0));
    const UnitId factory = factory_of(sim);
    const UnitId hauler = sim.units().at(1).id;
    sim.enqueue(build(factory, UnitKind::Turret)); // no cost, no time: not buildable
    sim.enqueue(build(factory, UnitKind::Factory));
    sim.enqueue(build(hauler, UnitKind::Scavenger)); // a hauler is not a producer
    sim.enqueue(build(factory, static_cast<UnitKind>(sim::kUnitKindCount)));   // not a kind at all
    sim.enqueue(build(factory, UnitKind::Scavenger, sim::kMaxBatchToken + 1)); // not a token
    EXPECT_TRUE(sim.command_log().empty())
        << "a refused order must not reach the log, or a replay would carry it";
    sim.enqueue(build(factory, UnitKind::Scavenger, sim::kMaxBatchToken));
    EXPECT_EQ(sim.command_log().size(), 1U) << "the largest token the order can carry is fine";
}

TEST(BuildQueue, ALoneBuildAndABatchMakeTheSameKindsUnderDifferentBatchIds) {
    // Three haulers: two shift-queued under one token, one lone. They are the same kind; what
    // differs is the batch each arrives in, which is what a function key will group by.
    Sim sim = Sim::create(queue_battle(90));
    const UnitId factory = factory_of(sim);
    const UnitId first = last_id(sim);
    sim.enqueue(build(factory, UnitKind::Scavenger, 9));
    sim.enqueue(build(factory, UnitKind::Scavenger, 9));
    sim.enqueue(build(factory, UnitKind::Scavenger));
    ASSERT_TRUE(step_until(sim, 3 * kQueueLimit,
                           [first](const Sim& s) { return built_since(s, first).size() == 3; }));
    const auto made = built_since(sim, first);
    for (const Unit* unit : made) {
        EXPECT_EQ(unit->kind, UnitKind::Scavenger);
    }
    EXPECT_EQ(made.at(0)->batch, 9U) << "a shift-queued run carries the order's shared token";
    EXPECT_EQ(made.at(1)->batch, 9U) << "...on every member, or the row cannot group them";
    EXPECT_NE(made.at(2)->batch, 0U) << "a lone build is a batch of one: it has an id of its own";
    EXPECT_NE(made.at(2)->batch, 9U) << "...which is not the batch's";
    EXPECT_EQ(sim.units().at(1).batch, 0U) << "a unit the scenario placed belongs to no batch";
}

TEST(BuildQueue, TwoLoneBuildsGetTwoDifferentBatchesAndNeitherIsAShiftToken) {
    Sim sim = Sim::create(queue_battle(0));
    const UnitId factory = factory_of(sim);
    sim.enqueue(build(factory, UnitKind::Scavenger));
    sim.enqueue(build(factory, UnitKind::Scavenger));
    sim.step();
    const sim::BuildQueue& queue = sim.find(factory)->queue;
    ASSERT_EQ(queue.count, 2U);
    EXPECT_NE(queue.entries.at(0).batch, queue.entries.at(1).batch)
        << "each lone press is a batch of its own";
    EXPECT_GT(queue.entries.at(0).batch, sim::kMaxBatchToken)
        << "the sim's own ids live above every token an order can carry, so they cannot collide";
    EXPECT_GT(queue.entries.at(1).batch, sim::kMaxBatchToken);
}

TEST(BuildQueue, AUnitIsBuiltOnGroundBesideADeployedRecyclerNeverInsideItsFootprint) {
    Scenario scenario = queue_battle(30);
    scenario.spawns.push_back(Spawn{UnitKind::DeployedRecycler, Side::Player, 60, 45});
    Sim sim = Sim::create(scenario);
    UnitId producer{};
    for (const Unit& unit : sim.units()) {
        if (unit.kind == UnitKind::DeployedRecycler) {
            producer = unit.id;
        }
    }
    ASSERT_TRUE(producer.valid());
    const UnitId first = last_id(sim);
    sim.enqueue(build(producer, UnitKind::Scavenger));
    ASSERT_TRUE(step_until(sim, 2 * kQueueLimit, [first](const Sim& s) {
        return !built_since(s, first).empty();
    })) << "a deployed recycler is a producer too";
    const Unit& made = *built_since(sim, first).front();
    EXPECT_FALSE(sim::building_blocks(sim.units(), made.x.floor_int(), made.y.floor_int()))
        << "a unit is never spawned inside a building's footprint";
    EXPECT_LE(std::abs(made.x.floor_int() - 60), sim::kBuildSpawnRadiusTiles);
    EXPECT_LE(std::abs(made.y.floor_int() - 45), sim::kBuildSpawnRadiusTiles);
}

TEST(BuildQueue, AQueueThatNeverStartsHashesAsAnEmptyOneAndAQueuedEntryMovesTheHash) {
    Sim idle = Sim::create(queue_battle(5));
    Sim queued = Sim::create(queue_battle(5));
    const std::uint64_t at_start = idle.state_hash();
    EXPECT_EQ(queued.state_hash(), at_start);
    queued.enqueue(build(factory_of(queued), UnitKind::Scavenger, 3));
    queued.step();
    idle.step();
    EXPECT_NE(queued.state_hash(), idle.state_hash())
        << "a queued entry changes what the next tick does, so it is state";
}

TEST(BuildQueue, AReplayThatQueuesBuildsReproducesTickForTick) {
    Sim live = Sim::create(queue_battle(40));
    const UnitId factory = factory_of(live);
    live.enqueue(build(factory, UnitKind::Scavenger, 4));
    live.enqueue(build(factory, UnitKind::Scavenger, 4));
    std::vector<std::uint64_t> hashes;
    for (int tick = 0; tick < 3000; ++tick) {
        if (tick == 600) {
            live.enqueue(build(factory, UnitKind::Scavenger));
        }
        live.step();
        hashes.push_back(live.state_hash());
    }
    ASSERT_GT(live.units().size(), 4U) << "the live battle must have BUILT something to reproduce";

    sim::Replay replay;
    replay.seed = live.seed();
    replay.commands = live.command_log();
    const std::optional<sim::Replay> text = sim::parse(sim::to_text(replay), nullptr);
    ASSERT_TRUE(text.has_value()) << "a queued build must survive the replay text";
    ASSERT_EQ(text->commands.size(), 3U);

    Sim again = Sim::create(queue_battle(40));
    std::size_t next = 0;
    for (int tick = 0; tick < 3000; ++tick) {
        while (next < text->commands.size() && text->commands.at(next).tick == again.tick()) {
            again.enqueue(text->commands.at(next).order);
            ++next;
        }
        again.step();
        ASSERT_EQ(again.state_hash(), hashes.at(static_cast<std::size_t>(tick)))
            << "diverged at tick " << tick;
    }
}
