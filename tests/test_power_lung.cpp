// SLICE N3 (SIM): THE POWER LUNG -- built in, destructible, and killing it DISABLES the structure.
//
// His words (2026-11-13): "When you build a Gun Spire, or ANYTHING on the alien side that needs a
// Lung, the Lungs are built into the structure. ... if I am on the human side, and I want to get
// past a Gun Spire, I direct my assets to attack the Lung. That disables the Gun Spire, no matter
// what is connected to it. They are easy to repair. They do not show up in the "3 Attack" list, so
// you have to actually POINT at a lung to direct anything to attack it specifically."
//
// The five rules, each pinned below:
//   1. a Lung is BUILT IN: completing a structure completes its Lung, no extra cost;
//   2. it is DESTRUCTIBLE and losing it DISABLES -- never destroys -- the structure;
//   3. "no matter what is connected to it": a neighbour's Lung does not keep a structure running;
//   4. EASY TO REPAIR: a few scrap and a few seconds, against the structure's price and build time;
//   5. NOT in the `3 Attack` list, but a PICK finds it.
//
// The numbers are LITERALS, never read back from the table under test. The Spire's gun is the Gun
// Tower's (36 damage, 40-tick reload, 18 tiles), PROVISIONAL; the Lung's 30 hit points and its
// restore (5 scrap, 90 ticks) are PROVISIONAL too: he has given no figure for either.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "protekt/game/attack_aim.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/targets.hpp"
#include "protekt/sim/orders.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"

namespace protekt {
namespace {

using sim::Side;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr int kW = 56;
constexpr int kH = 32;

// The literals the slice stands on.
constexpr int kSpireCost = 75;
constexpr int kSpireBuildTicks = 1500;
constexpr int kLungHitPoints = 30;
constexpr int kRestoreCost = 5;
constexpr int kRestoreTicks = 90;

struct Block {
    int x{};
    int y{};
};

/// A block of ground wholly passable, found by scanning from the top-left; fixed for the seed.
[[nodiscard]] Block open_block() {
    for (int by = 20; by < 200; by += 2) {
        for (int bx = 20; bx < 200; bx += 2) {
            bool clear = true;
            for (int dy = 0; dy < kH && clear; ++dy) {
                for (int dx = 0; dx < kW && clear; ++dx) {
                    clear = sim::tile_passable(kSeed, bx + dx, by + dy);
                }
            }
            if (clear) {
                return Block{bx, by};
            }
        }
    }
    return Block{-1, -1};
}

struct Spot {
    Side side;
    UnitKind kind;
    int dx;
    int dy;
};

[[nodiscard]] sim::Sim make(const Block& b, const std::vector<Spot>& spots,
                            std::uint16_t ore = 100) {
    sim::Scenario scenario{};
    scenario.seed = kSeed;
    for (const Spot& s : spots) {
        sim::Spawn spawn{};
        spawn.side = s.side;
        spawn.kind = s.kind;
        spawn.tile_x = b.x + s.dx;
        spawn.tile_y = b.y + s.dy;
        scenario.spawns.push_back(spawn);
    }
    scenario.store_capacity = 400;
    scenario.start_ore = ore;
    return sim::Sim::create(scenario);
}

[[nodiscard]] const sim::Unit* first_of(const sim::Sim& s, UnitKind kind, Side side) {
    for (const sim::Unit& u : s.units()) {
        if (u.kind == kind && u.side == side) {
            return &u;
        }
    }
    return nullptr;
}

[[nodiscard]] std::size_t count_of(const sim::Sim& s, UnitKind kind) {
    std::size_t n = 0;
    for (const sim::Unit& u : s.units()) {
        n += u.kind == kind ? 1U : 0U;
    }
    return n;
}

/// The Lung built into `host`, living or dead; null if none.
[[nodiscard]] const sim::Unit* lung_for(const sim::Sim& s, sim::UnitId host) {
    for (const sim::Unit& u : s.units()) {
        if (u.kind == UnitKind::PowerLung && u.host == host) {
            return &u;
        }
    }
    return nullptr;
}

[[nodiscard]] sim::Fixed tile(int t) { return sim::Fixed::from_int(t); }

/// Step `ticks`, counting the shots `shooter` fired.
[[nodiscard]] int shots_in(sim::Sim& s, sim::UnitId shooter, int ticks) {
    int n = 0;
    for (int t = 0; t < ticks; ++t) {
        s.step();
        for (const sim::Sim::Shot& shot : s.shots()) {
            n += shot.shooter == shooter ? 1 : 0;
        }
    }
    return n;
}

// ------------------------------------------------------------------------------ the six kinds

constexpr std::array<UnitKind, 6> kNeedy{UnitKind::Kiln,  UnitKind::GunSpire,   UnitKind::Antenna,
                                         UnitKind::Dower, UnitKind::Stronghold, UnitKind::Jammer};

TEST(PowerLung, TheSixAlienBuildingsNeedOneAndNothingElseDoes) {
    for (const UnitKind kind : kNeedy) {
        EXPECT_TRUE(sim::needs_lung(kind)) << sim::spec_of(kind).name;
    }
    for (int k = 0; k < sim::kUnitKindCount; ++k) {
        const auto kind = static_cast<UnitKind>(k);
        const bool one_of_six = std::find(kNeedy.begin(), kNeedy.end(), kind) != kNeedy.end();
        EXPECT_EQ(sim::needs_lung(kind), one_of_six) << sim::spec_of(kind).name;
    }
}

TEST(PowerLung, ACompletedStructureOfEveryNeedyKindComesWithOneLungAndNoCostLine) {
    const Block b = open_block();
    ASSERT_GE(b.x, 0);
    for (const UnitKind kind : kNeedy) {
        const sim::Sim s = make(b, {{Side::Player, kind, 4, 4}});
        const sim::Unit* host = first_of(s, kind, Side::Player);
        ASSERT_NE(host, nullptr) << sim::spec_of(kind).name;
        EXPECT_EQ(count_of(s, UnitKind::PowerLung), 1U) << sim::spec_of(kind).name;
        const sim::Unit* lung = lung_for(s, host->id);
        ASSERT_NE(lung, nullptr) << sim::spec_of(kind).name << " has no Lung";
        EXPECT_TRUE(lung->alive);
        EXPECT_EQ(lung->hp, kLungHitPoints);
        EXPECT_EQ(lung->side, host->side);
        // Built INTO it: inside the rectangle the structure stands on, on its east edge.
        const sim::Footprint f = sim::footprint_around(*host);
        EXPECT_TRUE(f.covers(lung->x.floor_int(), lung->y.floor_int())) << sim::spec_of(kind).name;
        EXPECT_EQ(lung->x.floor_int(), f.x + f.width - 1) << "the east edge";
        // ...and it cost nothing: the bank is as the scenario left it.
        EXPECT_EQ(s.ore(Side::Player), 100U) << sim::spec_of(kind).name;
        EXPECT_FALSE(sim::structure_disabled(s.units(), *host));
        EXPECT_EQ(sim::spec_of(UnitKind::PowerLung).cost, 0) << "no cost line of its own";
    }
}

TEST(PowerLung, AStructureThatNeedsNoLungGetsNone) {
    const Block b = open_block();
    const sim::Sim s = make(
        b, {{Side::Player, UnitKind::GunTower, 4, 4}, {Side::Player, UnitKind::Factory, 20, 4}});
    EXPECT_EQ(count_of(s, UnitKind::PowerLung), 0U);
}

TEST(PowerLung, ARaisedStructureIsCompletedWithItsLungOnTheTickItStandsAndNotBefore) {
    const Block b = open_block();
    ASSERT_GE(b.x, 0);
    for (const UnitKind kind : kNeedy) {
        sim::Sim s = make(b, {{Side::Player, UnitKind::Constructor, 2, 2}});
        const sim::UnitId builder = s.units().front().id;
        s.enqueue(sim::Order{builder, sim::orders::Place{kind, b.x + 12, b.y + 12, false}});
        s.step();
        const sim::Unit* site = first_of(s, kind, Side::Player);
        ASSERT_NE(site, nullptr) << sim::spec_of(kind).name;
        ASSERT_TRUE(sim::under_construction(*site));
        EXPECT_EQ(count_of(s, UnitKind::PowerLung), 0U) << "a SITE has no Lung yet";
        const sim::UnitId id = site->id;
        int guard = 0;
        while (sim::under_construction(*s.find(id)) && guard++ < 3000) {
            EXPECT_EQ(count_of(s, UnitKind::PowerLung), 0U);
            s.step();
        }
        ASSERT_FALSE(sim::under_construction(*s.find(id))) << "it never finished";
        const sim::Unit* lung = lung_for(s, id);
        ASSERT_NE(lung, nullptr) << sim::spec_of(kind).name << " finished with no Lung";
        EXPECT_TRUE(lung->alive);
        EXPECT_EQ(count_of(s, UnitKind::PowerLung), 1U);
        // The structure's price was its price: the Lung added nothing to the bill.
        EXPECT_EQ(static_cast<int>(s.ore(Side::Player)),
                  100 - static_cast<int>(sim::spec_of(kind).cost))
            << sim::spec_of(kind).name;
    }
}

// ---------------------------------------------------------------- the Gun Spire's gun, and its
// Lung

TEST(PowerLung, AGunSpireWithItsLungShootsAnEnemyInRange) {
    // The first assertion watched RED, before any Lung code: the Spire was unarmed (its spec row
    // carried no weapon), so this found no shot.
    const Block b = open_block();
    ASSERT_GE(b.x, 0);
    sim::Sim s = make(
        b, {{Side::Player, UnitKind::GunSpire, 4, 12}, {Side::Enemy, UnitKind::Factory, 16, 8}});
    const sim::UnitId spire = first_of(s, UnitKind::GunSpire, Side::Player)->id;
    const std::uint16_t before = first_of(s, UnitKind::Factory, Side::Enemy)->hp;
    EXPECT_GT(shots_in(s, spire, 200), 0) << "the Spire never fired";
    EXPECT_LT(first_of(s, UnitKind::Factory, Side::Enemy)->hp, before) << "and its bolt hit";
}

/// The raid, set up: a Player Spire, an unarmed enemy Factory in the Spire's range as bait, two
/// enemy tanks west of the Spire that are ordered to attack its Lung (which stands on the EAST
/// edge, so their bolts must go in through the Spire's own walls), and a Constructor held far off
/// to restore it later.
struct Raid {
    sim::Sim sim;
    sim::UnitId spire;
    sim::UnitId bait;
    sim::UnitId lung;
    sim::UnitId tank_a;
    sim::UnitId tank_b;
    sim::UnitId builder;
    Block block;
};

[[nodiscard]] Raid make_raid(const std::vector<Spot>& extra = {}) {
    const Block b = open_block();
    std::vector<Spot> spots{{Side::Player, UnitKind::GunSpire, 20, 12},
                            {Side::Enemy, UnitKind::Factory, 34, 8},
                            {Side::Enemy, UnitKind::Tank, 12, 14},
                            {Side::Enemy, UnitKind::Tank, 12, 16},
                            {Side::Player, UnitKind::Constructor, 22, 30}};
    spots.insert(spots.end(), extra.begin(), extra.end());
    Raid r{make(b, spots), {}, {}, {}, {}, {}, {}, b};
    r.spire = first_of(r.sim, UnitKind::GunSpire, Side::Player)->id;
    r.bait = first_of(r.sim, UnitKind::Factory, Side::Enemy)->id;
    r.lung = lung_for(r.sim, r.spire)->id;
    std::vector<const sim::Unit*> tanks;
    for (const sim::Unit& u : r.sim.units()) {
        if (u.kind == UnitKind::Tank && u.side == Side::Enemy) {
            tanks.push_back(&u);
        }
    }
    r.tank_a = tanks.at(0)->id;
    r.tank_b = tanks.at(1)->id;
    r.builder = first_of(r.sim, UnitKind::Constructor, Side::Player)->id;
    return r;
}

/// Order the raid's tanks onto the Lung and step until it is dead (or `limit` ticks pass).
/// Returns the ticks it took, or -1.
[[nodiscard]] int raid_the_lung(Raid& r, int limit = 400) {
    r.sim.enqueue(sim::Order{r.tank_a, sim::orders::Attack{r.lung}});
    r.sim.enqueue(sim::Order{r.tank_b, sim::orders::Attack{r.lung}});
    for (int t = 1; t <= limit; ++t) {
        r.sim.step();
        if (!r.sim.find(r.lung)->alive) {
            return t;
        }
    }
    return -1;
}

/// The tanks go home: out of range for good, so the Spire's fate is the Lung's alone.
void raiders_leave(Raid& r) {
    r.sim.enqueue(
        sim::Order{r.tank_a, sim::orders::MoveTo{tile(r.block.x + 1), tile(r.block.y + 14)}});
    r.sim.enqueue(
        sim::Order{r.tank_b, sim::orders::MoveTo{tile(r.block.x + 1), tile(r.block.y + 16)}});
}

TEST(PowerLung, ALungIsKilledByABoltAimedAtItThroughItsOwnStructuresWalls) {
    Raid r = make_raid();
    const int took = raid_the_lung(r);
    ASSERT_GT(took, 0) << "the raid never killed the Lung";
    EXPECT_EQ(static_cast<int>(r.sim.tick()), took);
    EXPECT_FALSE(r.sim.find(r.lung)->alive);
    EXPECT_EQ(r.sim.find(r.lung)->hp, 0);
}

TEST(PowerLung, TheRaidsBaitIsInTheSpiresReachSoItsSilenceBelowMeansSomething) {
    // The control for the test below: the same world with no raid. The Spire (Lung alive) shoots
    // the bait, which loses hit points. Without this, "a disabled Spire fired nothing" could be a
    // bait standing out of range.
    Raid r = make_raid();
    r.sim.enqueue(
        sim::Order{r.tank_a, sim::orders::MoveTo{tile(r.block.x + 1), tile(r.block.y + 14)}});
    r.sim.enqueue(
        sim::Order{r.tank_b, sim::orders::MoveTo{tile(r.block.x + 1), tile(r.block.y + 16)}});
    const std::uint16_t before = r.sim.find(r.bait)->hp;
    EXPECT_GT(shots_in(r.sim, r.spire, 400), 0);
    EXPECT_LT(r.sim.find(r.bait)->hp, before);
}

TEST(PowerLung, KillingTheLungDisablesTheSpireAndItStopsShootingButStillStands) {
    Raid r = make_raid();
    ASSERT_GT(raid_the_lung(r), 0);
    raiders_leave(r);
    r.sim.step(); // the order lands; the raiders are still in range for a few ticks
    const sim::Unit* spire = r.sim.find(r.spire);
    ASSERT_NE(spire, nullptr);
    EXPECT_TRUE(sim::structure_disabled(r.sim.units(), *spire));
    // Let the raiders get well clear: whatever they do to the Spire on the way out is done.
    for (int t = 0; t < 200; ++t) {
        r.sim.step();
    }
    const std::uint16_t spire_hp = r.sim.find(r.spire)->hp;
    const std::uint16_t bait_hp = r.sim.find(r.bait)->hp;
    ASSERT_GT(bait_hp, 0);
    // The bait stands in its reach and is an enemy; with its Lung the Spire would shoot it.
    EXPECT_EQ(shots_in(r.sim, r.spire, 600), 0) << "a disabled Spire fired";
    EXPECT_EQ(r.sim.find(r.bait)->hp, bait_hp) << "and nothing hurt the bait";
    // DISABLED, NOT DESTROYED: it is standing, whole as the raid left it, and still its side's.
    EXPECT_TRUE(r.sim.find(r.spire)->alive);
    EXPECT_EQ(r.sim.find(r.spire)->hp, spire_hp);
    EXPECT_GT(r.sim.find(r.spire)->hp, 0);
    EXPECT_EQ(r.sim.find(r.spire)->side, Side::Player);
    EXPECT_FALSE(sim::under_construction(*r.sim.find(r.spire)));
}

TEST(PowerLung, ADeadLungIsNotAWreckAndItsStructureIsNotADeadStructure) {
    Raid r = make_raid();
    ASSERT_GT(raid_the_lung(r), 0);
    raiders_leave(r);
    for (int t = 0; t < 40; ++t) {
        r.sim.step();
    }
    const sim::Unit& spire = *r.sim.find(r.spire);
    // The Lung leaves no wreck to scavenge: it is a component, not a vehicle.
    EXPECT_TRUE(r.sim.scraps().empty());
    // The structure's footprint still stands: it is still a building for everything that asks.
    const sim::Footprint f = sim::footprint_around(spire);
    EXPECT_TRUE(
        f.all_tiles([&](int tx, int ty) { return sim::building_blocks(r.sim.units(), tx, ty); }));
    // It is not wiped out: its side has not lost one.
    EXPECT_FALSE(r.sim.kind_wiped_out(Side::Player, UnitKind::GunSpire));
    // And it is the same structure, with the same id and the Lung's host link intact.
    EXPECT_EQ(lung_for(r.sim, r.spire)->id, r.lung);
}

TEST(PowerLung, ALungIsHurtOnlyByABoltAimedAtItNeverInPassing) {
    // The tank is ordered at the SPIRE, which is what a bolt aimed at the Spire from the east hits
    // FIRST -- the Lung stands inside the east edge, in the lane. It must come through untouched.
    const Block b = open_block();
    sim::Sim s = make(
        b, {{Side::Player, UnitKind::GunSpire, 20, 12}, {Side::Enemy, UnitKind::Tank, 30, 14}});
    const sim::UnitId spire = first_of(s, UnitKind::GunSpire, Side::Player)->id;
    const sim::UnitId tank = first_of(s, UnitKind::Tank, Side::Enemy)->id;
    s.enqueue(sim::Order{tank, sim::orders::Attack{spire}});
    for (int t = 0; t < 120; ++t) {
        s.step();
    }
    EXPECT_LT(s.find(spire)->hp, sim::spec_of(UnitKind::GunSpire).hp) << "the Spire was hit";
    EXPECT_TRUE(lung_for(s, spire)->alive);
    EXPECT_EQ(lung_for(s, spire)->hp, kLungHitPoints) << "and its Lung was not, in passing";
}

TEST(PowerLung, ASpireThatFiresEastIsNotHurtByItsOwnLungOnTheEdge) {
    // The Lung is in the lane of every shot the Spire makes to the east. Neither the bolt nor the
    // gun's friend-in-the-lane check may count it: a Spire must be able to shoot that way.
    const Block b = open_block();
    sim::Sim s = make(
        b, {{Side::Player, UnitKind::GunSpire, 4, 12}, {Side::Enemy, UnitKind::Factory, 16, 8}});
    const sim::UnitId spire = first_of(s, UnitKind::GunSpire, Side::Player)->id;
    EXPECT_GT(shots_in(s, spire, 200), 1);
    EXPECT_EQ(lung_for(s, spire)->hp, kLungHitPoints);
}

TEST(PowerLung, AGunDoesNotChooseALungForItselfItIsShotOnlyWhenNamed) {
    // Auto-engagement is "the nearest hostile in range". A Lung is never the nearest hostile: a
    // tank beside a hostile Spire's Lung shoots the Spire's other things, and leaves the Lung be.
    const Block b = open_block();
    sim::Sim s = make(
        b, {{Side::Enemy, UnitKind::GunSpire, 20, 12}, {Side::Player, UnitKind::Tank, 32, 13}});
    const sim::UnitId spire = first_of(s, UnitKind::GunSpire, Side::Enemy)->id;
    for (int t = 0; t < 120; ++t) {
        s.step();
    }
    EXPECT_TRUE(lung_for(s, spire)->alive) << "a gun chose the Lung without being told to";
}

// ------------------------------------------------------------------------- "no matter what"

TEST(PowerLung, NothingConnectedToAStructureKeepsItRunningOnceItsOwnLungIsDead) {
    // A second Spire touches the first (their footprints share an edge) and keeps its own Lung: the
    // first is disabled all the same, and the second is not. And a Kiln beside them, powered by its
    // own Lung, changes nothing for either.
    Raid r = make_raid(
        {{Side::Player, UnitKind::GunSpire, 24, 12}, {Side::Player, UnitKind::Kiln, 16, 4}});
    const sim::Unit* first = r.sim.find(r.spire);
    const sim::Unit* second = nullptr;
    for (const sim::Unit& u : r.sim.units()) {
        if (u.kind == UnitKind::GunSpire && u.id != r.spire) {
            second = &u;
        }
    }
    ASSERT_NE(second, nullptr);
    // They touch: the first's east edge is the second's west edge.
    EXPECT_EQ(sim::footprint_around(*first).x + sim::footprint_around(*first).width,
              sim::footprint_around(*second).x);
    ASSERT_GT(raid_the_lung(r), 0);
    EXPECT_TRUE(sim::structure_disabled(r.sim.units(), *r.sim.find(r.spire)));
    EXPECT_FALSE(sim::structure_disabled(r.sim.units(), *r.sim.find(second->id)))
        << "a neighbour's death spread";
    EXPECT_TRUE(lung_for(r.sim, second->id)->alive);
}

// --------------------------------------------------------------------------------- the Dower

TEST(PowerLung, ADowerWithADeadLungMendsNobodyUntilItIsRestored) {
    // The Dower is the Service Bay's twin: a unit standing on its platform is mended. Disabled, it
    // mends nobody. The patient is an Assault Tank, hurt, on the platform; the raiders leave once
    // the Lung is dead.
    const Block b = open_block();
    sim::Scenario scenario{};
    scenario.seed = kSeed;
    scenario.store_capacity = 400;
    scenario.start_ore = 100;
    const auto add = [&](Side side, UnitKind kind, int dx, int dy, std::uint16_t hp = 0) {
        sim::Spawn s{};
        s.side = side;
        s.kind = kind;
        s.tile_x = b.x + dx;
        s.tile_y = b.y + dy;
        s.hp = hp;
        scenario.spawns.push_back(s);
    };
    add(Side::Player, UnitKind::Dower, 20, 12);
    add(Side::Player, UnitKind::AssaultTank, 16, 15, 60); // beside the platform: it drives on
    add(Side::Enemy, UnitKind::Tank, 10, 14);
    add(Side::Enemy, UnitKind::Tank, 10, 16);
    add(Side::Player, UnitKind::Constructor, 22, 30);
    sim::Sim s = sim::Sim::create(scenario);
    const sim::UnitId dower = first_of(s, UnitKind::Dower, Side::Player)->id;
    const sim::UnitId patient = first_of(s, UnitKind::AssaultTank, Side::Player)->id;
    const sim::UnitId lung = lung_for(s, dower)->id;
    std::vector<sim::UnitId> tanks;
    for (const sim::Unit& u : s.units()) {
        if (u.kind == UnitKind::Tank) {
            tanks.push_back(u.id);
        }
    }
    // The patient drives onto the platform (a unit cannot be SPAWNED on one) while the raid begins.
    s.enqueue(sim::Order{patient, sim::orders::MoveTo{tile(b.x + 24), tile(b.y + 16)}});
    for (const sim::UnitId t : tanks) {
        s.enqueue(sim::Order{t, sim::orders::Attack{lung}});
    }
    int killed = -1;
    for (int t = 1; t <= 400 && killed < 0; ++t) {
        s.step();
        killed = s.find(lung)->alive ? -1 : t;
    }
    ASSERT_GT(killed, 0) << "the Dower's Lung never died";
    for (const sim::UnitId t : tanks) {
        s.enqueue(sim::Order{t, sim::orders::MoveTo{tile(b.x + 1), tile(b.y + 15)}});
    }
    for (int t = 0; t < 200; ++t) {
        s.step(); // the raiders get clear
    }
    ASSERT_TRUE(s.find(patient)->alive);
    ASSERT_TRUE(s.find(dower)->alive);
    ASSERT_TRUE(sim::footprint_around(*s.find(dower))
                    .covers(s.find(patient)->x.floor_int(), s.find(patient)->y.floor_int()))
        << "the patient is not on the platform";
    const std::uint16_t hurt = s.find(patient)->hp;
    ASSERT_LT(hurt, sim::spec_of(UnitKind::AssaultTank).hp);
    for (int t = 0; t < 600; ++t) {
        s.step();
    }
    EXPECT_EQ(s.find(patient)->hp, hurt) << "a disabled Dower mended somebody";
    EXPECT_TRUE(s.find(dower)->alive);

    // Restore: the Constructor drives to the platform's edge; once the Lung is back, it mends.
    const sim::Footprint f = sim::footprint_around(*s.find(dower));
    s.enqueue(sim::Order{first_of(s, UnitKind::Constructor, Side::Player)->id,
                         sim::orders::MoveTo{tile(f.x + f.width + 1), tile(f.y + 2)}});
    for (int t = 0; t < 600 && !s.find(lung)->alive; ++t) {
        s.step();
    }
    ASSERT_TRUE(s.find(lung)->alive) << "the Lung was never restored";
    EXPECT_FALSE(sim::structure_disabled(s.units(), *s.find(dower)));
    for (int t = 0; t < 300; ++t) {
        s.step();
    }
    EXPECT_GT(s.find(patient)->hp, hurt) << "the restored Dower still mends nobody";
}

// ------------------------------------------------------------------------------- the repair

TEST(PowerLung, ARestoredLungRestoresTheFunctionAndCostsAFewScrapAndAFewSeconds) {
    Raid r = make_raid();
    ASSERT_GT(raid_the_lung(r), 0);
    raiders_leave(r);
    for (int t = 0; t < 200; ++t) {
        r.sim.step();
    }
    ASSERT_EQ(shots_in(r.sim, r.spire, 100), 0) << "disabled";
    const std::uint16_t bait_before = r.sim.find(r.bait)->hp;

    // No Constructor near it: nothing happens, however long it waits.
    for (int t = 0; t < 300; ++t) {
        r.sim.step();
    }
    EXPECT_FALSE(r.sim.find(r.lung)->alive) << "a Lung restored itself";
    const std::uint16_t bank = r.sim.ore(Side::Player);

    // The Constructor goes to the Spire's east edge.
    const sim::Footprint f = sim::footprint_around(*r.sim.find(r.spire));
    r.sim.enqueue(
        sim::Order{r.builder, sim::orders::MoveTo{tile(f.x + f.width + 1), tile(f.y + 2)}});
    int arrived = -1;
    for (int t = 1; t <= 600 && arrived < 0; ++t) {
        r.sim.step();
        if (r.sim.find(r.lung)->restore_left != 0 || r.sim.find(r.lung)->alive) {
            arrived = t;
        }
    }
    ASSERT_GT(arrived, 0) << "the restore never began";
    // PAID FOR when it began: exactly the Lung's few scrap, taken once.
    EXPECT_EQ(static_cast<int>(bank) - static_cast<int>(r.sim.ore(Side::Player)), kRestoreCost);
    int taken = 0;
    while (!r.sim.find(r.lung)->alive && taken < 400) {
        r.sim.step();
        ++taken;
    }
    EXPECT_LE(taken, kRestoreTicks) << "restoring took longer than its pinned duration";
    EXPECT_TRUE(r.sim.find(r.lung)->alive);
    EXPECT_EQ(r.sim.find(r.lung)->hp, kLungHitPoints) << "restored whole";
    EXPECT_EQ(static_cast<int>(bank) - static_cast<int>(r.sim.ore(Side::Player)), kRestoreCost)
        << "and charged once, not again";
    EXPECT_FALSE(sim::structure_disabled(r.sim.units(), *r.sim.find(r.spire)));
    // THE FUNCTION IS BACK: the Spire shoots the bait again.
    EXPECT_GT(shots_in(r.sim, r.spire, 200), 0) << "restored, and still silent";
    EXPECT_LT(r.sim.find(r.bait)->hp, bait_before);
}

TEST(PowerLung, RestoringALungIsCheapAgainstRebuildingTheStructure) {
    // Literal ceilings: a tenth of the structure's scrap and of its build time, for the cheapest
    // of the six. (5 of 50 scrap and 90 of 1000 ticks for the Dower; 5 of 75 and 90 of 1500 for the
    // Spire.) His rule 4: "easy to repair" -- the counter-play is a raid, not a demolition.
    for (const UnitKind kind : kNeedy) {
        const sim::UnitSpec& row = sim::spec_of(kind);
        EXPECT_LE(sim::kLungRestoreCost * 10, row.cost) << row.name;
        EXPECT_LE(sim::kLungRestoreTicks * 10, row.build_ticks) << row.name;
        EXPECT_GT(sim::kLungRestoreCost, 0) << "cheap, not free: the raid has to cost a little";
    }
    EXPECT_EQ(sim::kLungRestoreCost, kRestoreCost);
    EXPECT_EQ(sim::kLungRestoreTicks, kRestoreTicks);
    EXPECT_EQ(sim::kLungHp, kLungHitPoints);
    EXPECT_EQ(sim::spec_of(UnitKind::GunSpire).cost, kSpireCost);
    EXPECT_EQ(sim::spec_of(UnitKind::GunSpire).build_ticks, kSpireBuildTicks);
    // And a Lung dies to a handful of tank bolts, the structure to many more: a raid is cheap too.
    EXPECT_LT(kLungHitPoints * 3, static_cast<int>(sim::spec_of(UnitKind::GunSpire).hp));
}

TEST(PowerLung, ARestoreShortOfScrapWaitsAndNeverCancels) {
    // Built with an empty bank: the Constructor stands by the dead Lung and nothing is restored
    // until there is scrap, and nothing is charged for the wait.
    const Block b = open_block();
    sim::Sim s = make(b,
                      {{Side::Player, UnitKind::GunSpire, 20, 12},
                       {Side::Enemy, UnitKind::Tank, 12, 14},
                       {Side::Player, UnitKind::Constructor, 25, 13}},
                      0);
    const sim::UnitId spire = first_of(s, UnitKind::GunSpire, Side::Player)->id;
    const sim::UnitId lung = lung_for(s, spire)->id;
    const sim::UnitId tank = first_of(s, UnitKind::Tank, Side::Enemy)->id;
    s.enqueue(sim::Order{tank, sim::orders::Attack{lung}});
    for (int t = 0; t < 400 && s.find(lung)->alive; ++t) {
        s.step();
    }
    ASSERT_FALSE(s.find(lung)->alive);
    for (int t = 0; t < 300; ++t) {
        s.step();
    }
    EXPECT_FALSE(s.find(lung)->alive) << "restored with no scrap";
    EXPECT_EQ(s.find(lung)->restore_left, 0) << "started without paying";
    EXPECT_EQ(s.ore(Side::Player), 0U);
}

TEST(PowerLung, ADestroyedStructureTakesItsLungWithItAndItIsNeverRestored) {
    const Block b = open_block();
    sim::Sim s = make(b, {{Side::Player, UnitKind::Jammer, 20, 12},
                          {Side::Enemy, UnitKind::Tank, 12, 14},
                          {Side::Enemy, UnitKind::Tank, 12, 16},
                          {Side::Enemy, UnitKind::Tank, 12, 18},
                          {Side::Player, UnitKind::Constructor, 25, 13}});
    const sim::UnitId jammer = first_of(s, UnitKind::Jammer, Side::Player)->id;
    const sim::UnitId lung = lung_for(s, jammer)->id;
    for (const sim::Unit& u : s.units()) {
        if (u.kind == UnitKind::Tank) {
            s.enqueue(sim::Order{u.id, sim::orders::Attack{jammer}});
        }
    }
    for (int t = 0; t < 800 && s.find(jammer)->alive; ++t) {
        s.step();
    }
    ASSERT_FALSE(s.find(jammer)->alive) << "the Jammer never fell";
    s.step();
    EXPECT_FALSE(s.find(lung)->alive);
    for (int t = 0; t < 400; ++t) {
        s.step();
    }
    EXPECT_FALSE(s.find(lung)->alive) << "a Constructor restored the Lung of a wreck";
}

// ------------------------------------------------------------------------------ determinism

TEST(PowerLung, TheSameRaidGivesTheSameStateHashEveryTime) {
    const auto run = []() {
        Raid r = make_raid();
        (void)raid_the_lung(r);
        raiders_leave(r);
        for (int t = 0; t < 300; ++t) {
            r.sim.step();
        }
        return r.sim.state_hash();
    };
    EXPECT_EQ(run(), run());
}

// -------------------------------------------------- the attack list leaves it out, the pick finds
// it

struct Eye {
    game::SceneOptions options;
    float lung_px{};
    float lung_py{};
};

/// A camera east of the unit, looking west at it, and the pixel its centre lands on.
[[nodiscard]] Eye eye_on(const sim::Sim& s, const sim::Unit& lung) {
    Eye e;
    e.options.seed = kSeed;
    e.options.extent = game::Extent{1280, 720};
    e.options.radius_tiles = 60;
    const float x = static_cast<float>(lung.x.raw) / static_cast<float>(sim::Fixed::kOne)
                    * game::kMetresPerTile;
    const float z = static_cast<float>(lung.y.raw) / static_cast<float>(sim::Fixed::kOne)
                    * game::kMetresPerTile;
    const float ground = game::height_metres(kSeed, lung.x.floor_int(), lung.y.floor_int());
    const float middle = ground + (game::kPickLungHeightMetres * 0.5F);
    e.options.camera.x = x + 30.0F;
    e.options.camera.y = middle + 4.0F;
    e.options.camera.z = z;
    e.options.camera.pitch = 0.0F;
    // Yaw 0 looks along +Z; the sign that turns it to -X is found, not assumed.
    for (const float yaw : {1.5707963F, -1.5707963F}) {
        e.options.camera.yaw = yaw;
        const game::RayPoint ahead
            = game::ray_point(e.options.camera, e.options.extent, 640.0F, 360.0F, 30.0F);
        if (ahead.x < e.options.camera.x - 20.0F) {
            break;
        }
    }
    const game::Projected p = game::project(e.options.camera, e.options.extent, x, middle, z);
    e.lung_px = p.x;
    e.lung_py = p.y;
    (void)s;
    return e;
}

TEST(PowerLung, TheThreeAttackListNeverEnumeratesALungButThePickFindsIt) {
    const Block b = open_block();
    ASSERT_GE(b.x, 0);
    // A hostile Spire (with its Lung), a hostile Kiln (with its) and a hostile Factory (none), all
    // near the player's tank: the list is the nearest hostiles, and there are three of them.
    sim::Sim s = make(b, {{Side::Player, UnitKind::Tank, 4, 10},
                          {Side::Enemy, UnitKind::GunSpire, 16, 4},
                          {Side::Enemy, UnitKind::Kiln, 16, 16},
                          {Side::Enemy, UnitKind::Factory, 30, 10}});
    ASSERT_EQ(count_of(s, UnitKind::PowerLung), 2U) << "two Lungs stand in the world";
    const sim::UnitId tank = first_of(s, UnitKind::Tank, Side::Player)->id;
    const sim::Unit& spire = *first_of(s, UnitKind::GunSpire, Side::Enemy);
    const sim::Unit* lung = lung_for(s, spire.id);
    ASSERT_NE(lung, nullptr);

    // THE MENU HALF: not in the list, however near, however the list is bounded.
    const std::vector<game::TargetRow> rows = game::nearest_enemies(s, Side::Player, tank, 80);
    ASSERT_EQ(rows.size(), 3U) << "the Spire, the Kiln and the Factory, and nothing else";
    for (const game::TargetRow& row : rows) {
        EXPECT_NE(row.kind, UnitKind::PowerLung) << "the attack list named a Lung";
    }
    const Eye e = eye_on(s, *lung);
    const std::vector<game::TargetRow> in_view
        = game::nearest_enemies(s, Side::Player, tank, 80, &e.options);
    for (const game::TargetRow& row : in_view) {
        EXPECT_NE(row.kind, UnitKind::PowerLung);
    }

    // THE PICK HALF: pointing exactly at the Lung names the Lung.
    const game::Pick hit = game::pick(s, e.options, e.lung_px, e.lung_py);
    EXPECT_EQ(hit.unit.value, lung->id.value) << "pointing at the Lung did not pick it";
    // ...and SPACE's offer is the Lung too, for a player whose selection can shoot.
    game::SceneOptions aiming = e.options;
    aiming.aim = game::Aim{e.lung_px, e.lung_py};
    const std::array<sim::UnitId, 1> selected{tank};
    EXPECT_EQ(game::attack_target(s, aiming, selected, sim::UnitId{}).value, lung->id.value);

    // The assist cone does not find it: aimed at the Spire's own middle, SPACE offers the Spire.
    const float sx = static_cast<float>(spire.x.raw) / static_cast<float>(sim::Fixed::kOne)
                     * game::kMetresPerTile;
    const float sz = static_cast<float>(spire.y.raw) / static_cast<float>(sim::Fixed::kOne)
                     * game::kMetresPerTile;
    const float sg = game::height_metres(kSeed, spire.x.floor_int(), spire.y.floor_int());
    const game::Projected mid
        = game::project(e.options.camera, e.options.extent, sx, sg + 6.0F, sz);
    aiming.aim = game::Aim{mid.x, mid.y};
    const sim::UnitId offered = game::attack_target(s, aiming, selected, sim::UnitId{});
    EXPECT_NE(offered.value, lung->id.value) << "the assist cone fell on a Lung";
}

} // namespace
} // namespace protekt
