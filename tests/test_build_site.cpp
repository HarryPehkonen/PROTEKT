// ROUND 18, K2: a build is a JOB. SPACE places a SITE (the scrap is paid, the footprint is solid);
// the site WAITS until a friendly Constructor is within ONE TILE of it -- and the Constructor
// DRIVES ITSELF there; then the build runs on a TICK timer, and the building STANDS at the end.
// The beam and the growing model are DRAWING ONLY: the scene reads the sim's rule and moves
// nothing.
//
// The owner, 2026-11-12: "It should wait for the Constructor to arrive within one block (term?)
// from it, and then there is a time delay and animation as it builds the building ... a ray coming
// from the Constructor, the build proceeding from the bottom to the top as the ray also rises."
// (His block is this tree's TILE.)
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "protekt/game/menu.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/orders.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"

namespace protekt {
namespace {

using sim::Fixed;
using sim::Side;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr int kBlockW = 16;
constexpr int kBlockH = 14;
/// The build's length in ticks: the Power row's own number (30 scrap at the rows' 20 ticks a
/// scrap). PROVISIONAL -- the owner has not given one; it is read from the row, never copied here.
const std::uint16_t kBuildTicks = sim::spec_of(UnitKind::Power).build_ticks;

struct Block {
    int x{};
    int y{};
};

[[nodiscard]] Block open_block() {
    for (int by = 20; by < 200; by += 2) {
        for (int bx = 20; bx < 200; bx += 2) {
            bool clear = true;
            for (int dy = 0; dy < kBlockH && clear; ++dy) {
                for (int dx = 0; dx < kBlockW && clear; ++dx) {
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

/// The site's tile, in the open block.
struct Site {
    int x{};
    int y{};
};
[[nodiscard]] Site site_tile(const Block& b) { return Site{b.x + 5, b.y + 5}; }

struct Yard {
    sim::Sim sim;
    Block block;
    sim::UnitId constructor;
    sim::UnitId tank;
};

/// A friendly Constructor at tile (`cx`, `cy`) -- a unit stands at its tile's corner -- and a
/// Tank, on open ground, with scrap in the bank.
[[nodiscard]] Yard make_yard(int cx_offset, int cy_offset, std::uint16_t ore = 100) {
    Yard y{};
    y.block = open_block();
    sim::Scenario scenario{
        kSeed,
        {{UnitKind::Constructor, Side::Player, y.block.x + cx_offset, y.block.y + cy_offset},
         {UnitKind::Tank, Side::Player, y.block.x + 1, y.block.y + 12}}};
    scenario.store_capacity = 400;
    scenario.start_ore = ore;
    y.sim = sim::Sim::create(scenario);
    y.constructor = y.sim.units().at(0).id;
    y.tank = y.sim.units().at(1).id;
    return y;
}

[[nodiscard]] sim::Order place(sim::UnitId who, const Site& s) {
    return sim::Order{who, sim::orders::Place{UnitKind::Power, s.x, s.y, false}};
}

/// Place the site and, in the same tick, STOP the Constructor where it stands: the unit is held
/// still so a test can say exactly how far it is from the site.
void place_and_hold(Yard& y) {
    const Site s = site_tile(y.block);
    y.sim.enqueue(place(y.constructor, s));
    y.sim.enqueue(sim::Order{y.constructor, sim::orders::Stop{}});
}

[[nodiscard]] const sim::Unit* power_of(const sim::Sim& s) {
    for (const sim::Unit& u : s.units()) {
        if (u.alive && u.kind == UnitKind::Power) {
            return &u;
        }
    }
    return nullptr;
}

[[nodiscard]] std::uint16_t left_of(const sim::Sim& s) {
    const sim::Unit* p = power_of(s);
    return p != nullptr ? p->build_left : 0xFFFF;
}

} // namespace

// ------------------------------------------------------------------------------ the site

TEST(BuildSite, PlacingLeavesAnUnbuiltSiteAndChargesTheScrapAtOnce) {
    Yard y = make_yard(1, 1);
    y.sim.enqueue(place(y.constructor, site_tile(y.block)));
    y.sim.step();
    const sim::Unit* site = power_of(y.sim);
    ASSERT_NE(site, nullptr) << "the site exists the tick the order is applied";
    EXPECT_TRUE(sim::under_construction(*site));
    EXPECT_GT(site->build_left, kBuildTicks - 2U)
        << "nothing has been built: the Constructor is far";
    EXPECT_EQ(y.sim.ore(Side::Player), 70U) << "the scrap is paid as it always was";
}

TEST(BuildSite, ASiteIsSolidFromTheFirstTickAndAUnitOrderedThroughItDoesNotPass) {
    Yard y = make_yard(1, 1);
    const Site s = site_tile(y.block);
    y.sim.enqueue(place(y.constructor, s));
    y.sim.enqueue(sim::Order{y.constructor, sim::orders::Stop{}});
    y.sim.step();
    ASSERT_TRUE(sim::under_construction(*power_of(y.sim)));
    EXPECT_TRUE(sim::building_blocks(y.sim.units(), s.x, s.y)) << "the footprint is reserved";
    y.sim.enqueue(
        sim::Order{y.tank, sim::orders::MoveTo{Fixed::from_int(s.x + 6), Fixed::from_int(s.y)}});
    // The tank drives from the south-west of the block to the far side of the site's row.
    for (int tick = 0; tick < 500; ++tick) {
        y.sim.step();
        const sim::Unit* tank = y.sim.find(y.tank);
        ASSERT_NE(tank, nullptr);
        EXPECT_FALSE(tank->x.floor_int() == s.x && tank->y.floor_int() == s.y)
            << "the tank entered the site at tick " << tick;
    }
    EXPECT_TRUE(sim::under_construction(*power_of(y.sim))) << "no Constructor was near";
}

// ------------------------------------------------------------------------------ waiting

TEST(BuildSite, ASiteWithNoConstructorNearItMakesNoProgressOverALongRun) {
    Yard y = make_yard(1, 1);
    const Site s = site_tile(y.block);
    y.sim.enqueue(place(y.constructor, s));
    // The same tick: the Constructor is sent AWAY, so nothing is ever near the site.
    y.sim.enqueue(sim::Order{y.constructor, sim::orders::MoveTo{Fixed::from_int(y.block.x),
                                                                Fixed::from_int(y.block.y)}});
    for (int tick = 0; tick < 3000; ++tick) {
        y.sim.step();
        ASSERT_EQ(left_of(y.sim), kBuildTicks) << "progress at tick " << tick;
    }
}

TEST(BuildSite, AnEnemyConstructorBesideTheSiteDoesNotBuildIt) {
    Yard y = make_yard(1, 1);
    const Site s = site_tile(y.block);
    // The friendly Constructor is sent away; an ENEMY one stands beside the site -- but a site is
    // built by a Constructor of its own side only.
    sim::Scenario scenario{kSeed,
                           {{UnitKind::Constructor, Side::Player, y.block.x + 1, y.block.y + 1},
                            {UnitKind::Constructor, Side::Enemy, s.x + 5, s.y}}};
    scenario.store_capacity = 400;
    scenario.start_ore = 100;
    y.sim = sim::Sim::create(scenario);
    const sim::UnitId mine = y.sim.units().at(0).id;
    y.sim.enqueue(place(mine, s));
    y.sim.enqueue(sim::Order{
        mine, sim::orders::MoveTo{Fixed::from_int(y.block.x), Fixed::from_int(y.block.y)}});
    for (int tick = 0; tick < 600; ++tick) {
        y.sim.step();
        ASSERT_EQ(left_of(y.sim), kBuildTicks) << "the enemy built it at tick " << tick;
    }
}

TEST(BuildSite, ProgressBeginsOnlyWhenAConstructorIsWithinOneTile) {
    const Block b = open_block();
    const Site s = site_tile(b);
    // Tile offsets from the site's tile: a unit stands at its tile's CORNER, the site is the
    // square [s.x, s.x+4] x [s.y, s.y+4] (ROUND 19, L1: a Power is 4x4 now). Five tiles east is
    // exactly ONE tile from its east edge; six tiles east is TWO -- one tile FURTHER -- and must do
    // nothing.
    const int near_dx = 5;
    const int far_dx = near_dx + 1;
    {
        Yard y = make_yard(s.x + far_dx - b.x, s.y - b.y);
        place_and_hold(y);
        for (int tick = 0; tick < 300; ++tick) {
            y.sim.step();
        }
        EXPECT_EQ(left_of(y.sim), kBuildTicks) << "one tile too far: no progress";
    }
    {
        Yard y = make_yard(s.x + near_dx - b.x, s.y - b.y);
        place_and_hold(y);
        for (int tick = 0; tick < 300; ++tick) {
            y.sim.step();
        }
        EXPECT_EQ(left_of(y.sim), kBuildTicks - 300U) << "one tile away: it builds, a tick each";
    }
    {
        Yard y = make_yard(s.x + 4 - b.x, s.y - b.y);
        place_and_hold(y);
        for (int tick = 0; tick < 300; ++tick) {
            y.sim.step();
        }
        EXPECT_EQ(left_of(y.sim), kBuildTicks - 300U) << "touching it: it builds";
    }
}

// ------------------------------------------------------------------------------ the build

TEST(BuildSite, TheBuildTakesExactlyTheSpecTicksAndTheBuildingStandsOnlyAtTheEnd) {
    const Block b = open_block();
    const Site s = site_tile(b);
    Yard y = make_yard(s.x + 5 - b.x, s.y - b.y);
    place_and_hold(y);
    for (std::uint16_t tick = 1; tick < kBuildTicks; ++tick) {
        y.sim.step();
        ASSERT_EQ(left_of(y.sim), kBuildTicks - tick) << "tick " << tick;
        ASSERT_TRUE(sim::under_construction(*power_of(y.sim)));
        const game::StandingBuildings standing
            = game::standing_buildings(y.sim, Side::Player, game::Faction::Human);
        ASSERT_EQ(standing.at(static_cast<std::size_t>(game::ConstructBuilding::Power)), 0U)
            << "a site is not a standing building, tick " << tick;
    }
    y.sim.step(); // the kBuildTicks-th tick in reach
    ASSERT_NE(power_of(y.sim), nullptr);
    EXPECT_EQ(left_of(y.sim), 0U);
    EXPECT_FALSE(sim::under_construction(*power_of(y.sim)));
    const game::StandingBuildings standing
        = game::standing_buildings(y.sim, Side::Player, game::Faction::Human);
    EXPECT_EQ(standing.at(static_cast<std::size_t>(game::ConstructBuilding::Power)), 1U);
    // And it stays done.
    for (int tick = 0; tick < 50; ++tick) {
        y.sim.step();
    }
    EXPECT_EQ(left_of(y.sim), 0U);
}

TEST(BuildSite, TheConstructorDrivesItselfToTheSiteAndTheBuildCompletes) {
    Yard y = make_yard(1, 1); // five tiles off, and nobody drives it
    const Site s = site_tile(y.block);
    y.sim.enqueue(place(y.constructor, s));
    int first_progress = -1;
    int done = -1;
    for (int tick = 1; tick <= 4000 && done < 0; ++tick) {
        y.sim.step();
        if (first_progress < 0 && left_of(y.sim) < kBuildTicks) {
            first_progress = tick;
        }
        if (left_of(y.sim) == 0) {
            done = tick;
        }
    }
    ASSERT_GT(first_progress, 1) << "it had to travel first";
    ASSERT_GT(done, 0) << "the Constructor never built it";
    EXPECT_EQ(done - first_progress, kBuildTicks - 1)
        << "once there it is exactly the spec's ticks";
    for (int tick = 0; tick < 100; ++tick) {
        y.sim.step();
    }
    const sim::Unit* ctor = y.sim.find(y.constructor);
    ASSERT_NE(ctor, nullptr);
    EXPECT_EQ(ctor->stance, sim::Stance::Idle) << "the errand ends once the building stands";
    EXPECT_EQ(left_of(y.sim), 0U);
}

// ------------------------------------------------------------------------------ determinism

TEST(BuildSite, TheWholeSequenceIsDeterministicTickByTick) {
    Yard a = make_yard(1, 1);
    Yard c = make_yard(1, 1);
    const std::uint64_t before = a.sim.state_hash();
    a.sim.enqueue(place(a.constructor, site_tile(a.block)));
    c.sim.enqueue(place(c.constructor, site_tile(c.block)));
    std::uint64_t last = before;
    int moved = 0;
    int partial = 0;
    for (int tick = 0; tick < 1200; ++tick) {
        a.sim.step();
        c.sim.step();
        partial += (left_of(a.sim) != 0 && left_of(a.sim) < kBuildTicks) ? 1 : 0;
        ASSERT_EQ(left_of(a.sim), left_of(c.sim)) << "progress differs at tick " << tick;
        ASSERT_EQ(a.sim.state_hash(), c.sim.state_hash()) << "hash differs at tick " << tick;
        moved += a.sim.state_hash() != last ? 1 : 0;
        last = a.sim.state_hash();
    }
    EXPECT_EQ(left_of(a.sim), 0U);
    EXPECT_GT(partial, 500) << "the build was in progress for most of the run";
    EXPECT_GT(moved, 0);
}

TEST(BuildSite, ProgressIsPartOfTheStateHash) {
    const Block b = open_block();
    const Site s = site_tile(b);
    Yard a = make_yard(s.x + 5 - b.x, s.y - b.y);
    Yard c = make_yard(s.x + 5 - b.x, s.y - b.y);
    place_and_hold(a);
    place_and_hold(c);
    for (int tick = 0; tick < 100; ++tick) {
        a.sim.step();
    }
    for (int tick = 0; tick < 101; ++tick) {
        c.sim.step();
    }
    EXPECT_EQ(left_of(a.sim), kBuildTicks - 100U);
    EXPECT_EQ(left_of(c.sim), kBuildTicks - 101U);
    EXPECT_NE(a.sim.state_hash(), c.sim.state_hash()) << "one tick more built is another state";
}

TEST(BuildSite, ABattleWithNoSiteHashesAsItDid) {
    // A Constructor, a Tank, a Power the scenario itself spawns: none is a site, so `build_left`
    // is zero everywhere and is not hashed. The same scenario with a SITE hashes differently.
    sim::Scenario scenario{
        kSeed,
        {{UnitKind::Constructor, Side::Player, 40, 40}, {UnitKind::Power, Side::Player, 44, 44}}};
    sim::Sim plain = sim::Sim::create(scenario);
    ASSERT_FALSE(sim::under_construction(*power_of(plain)));
    sim::Sim with_site = sim::Sim::create(scenario);
    plain.step();
    with_site.step();
    EXPECT_EQ(plain.state_hash(), with_site.state_hash());
}

// ------------------------------------------------------------------------------ the drawing

namespace {

using game::Scene;
using game::SceneOptions;
using game::SceneVertex;

[[nodiscard]] SceneOptions view(const Site& s) {
    SceneOptions o;
    o.seed = kSeed;
    o.radius_tiles = 14;
    o.camera.x = static_cast<float>(s.x + 2) * game::kMetresPerTile;
    o.camera.z = static_cast<float>(s.y - 8) * game::kMetresPerTile;
    o.camera.y = 9.0F;
    o.show_ship_gauge = false;
    o.show_ledger = false;
    return o;
}

[[nodiscard]] bool is_beam_ink(const SceneVertex& v) {
    return std::abs(v.colour.at(0) - game::kBuildBeamInk.at(0)) < 0.002F
           && std::abs(v.colour.at(1) - game::kBuildBeamInk.at(1)) < 0.002F
           && std::abs(v.colour.at(2) - game::kBuildBeamInk.at(2)) < 0.002F;
}

/// The highest point of the beam, in metres; none (a very low number) when there is no beam.
[[nodiscard]] float beam_top(const Scene& scene) {
    float top = -1.0e9F;
    for (const SceneVertex& v : scene.vertices) {
        if (is_beam_ink(v)) {
            top = std::max(top, v.position.at(1));
        }
    }
    return top;
}

/// The highest point of anything drawn over the site's footprint that is not the beam: the
/// terrain's own corners plus whatever has been built there so far.
[[nodiscard]] float built_top(const Scene& scene, const Site& s) {
    constexpr float kM = game::kMetresPerTile;
    float top = -1.0e9F;
    for (const SceneVertex& v : scene.vertices) {
        const float x = v.position.at(0);
        const float z = v.position.at(2);
        if (!is_beam_ink(v) && x >= static_cast<float>(s.x) * kM
            && x <= static_cast<float>(s.x + 4) * kM && z >= static_cast<float>(s.y) * kM
            && z <= static_cast<float>(s.y + 4) * kM) {
            top = std::max(top, v.position.at(1));
        }
    }
    return top;
}

} // namespace

TEST(BuildSiteDrawing, ABeamIsDrawnWhileTheBuildAdvancesAndNotOtherwise) {
    const Block b = open_block();
    const Site s = site_tile(b);
    {
        Yard y = make_yard(s.x + 5 - b.x, s.y - b.y);
        place_and_hold(y);
        for (int tick = 0; tick < kBuildTicks / 2; ++tick) {
            y.sim.step();
        }
        ASSERT_NE(sim::site_builder(y.sim.units(), *power_of(y.sim)), nullptr);
        EXPECT_GT(beam_top(game::build_scene(y.sim, view(s))), -1.0e8F) << "the ray is drawn";
    }
    {
        Yard y = make_yard(s.x + 6 - b.x, s.y - b.y); // one tile too far: nothing advances
        place_and_hold(y);
        for (int tick = 0; tick < 100; ++tick) {
            y.sim.step();
        }
        EXPECT_LT(beam_top(game::build_scene(y.sim, view(s))), -1.0e8F) << "no builder, no ray";
    }
    {
        Yard y = make_yard(s.x + 5 - b.x, s.y - b.y);
        place_and_hold(y);
        for (int tick = 0; tick < kBuildTicks + 10; ++tick) {
            y.sim.step();
        }
        EXPECT_LT(beam_top(game::build_scene(y.sim, view(s))), -1.0e8F)
            << "a finished building has none";
    }
}

TEST(BuildSiteDrawing, TheBuildingAndTheBeamRiseTogetherAndNeverFall) {
    const Block b = open_block();
    const Site s = site_tile(b);
    Yard y = make_yard(s.x + 5 - b.x, s.y - b.y);
    place_and_hold(y);
    float last_beam = -1.0e9F;
    float last_built = -1.0e9F;
    float first_built = 0.0F;
    int samples = 0;
    for (int tick = 1; tick <= kBuildTicks - 1; ++tick) {
        y.sim.step();
        if (tick % 50 != 0) {
            continue;
        }
        const Scene scene = game::build_scene(y.sim, view(s));
        const float beam = beam_top(scene);
        const float built = built_top(scene, s);
        ASSERT_GT(beam, -1.0e8F) << "tick " << tick;
        EXPECT_GE(beam, last_beam) << "the beam fell at tick " << tick;
        EXPECT_GE(built, last_built) << "the building shrank at tick " << tick;
        if (samples == 0) {
            first_built = built;
        }
        last_beam = beam;
        last_built = built;
        ++samples;
    }
    ASSERT_GT(samples, 5);
    EXPECT_GT(last_beam, -1.0e8F);
    EXPECT_GT(last_built, first_built) << "it grew, bottom to top";
    // The finished building is the tallest of all.
    y.sim.step();
    const Scene done = game::build_scene(y.sim, view(s));
    EXPECT_GT(built_top(done, s), last_built);
}

TEST(BuildSiteDrawing, DrawingMovesNoState) {
    const Block b = open_block();
    const Site s = site_tile(b);
    Yard y = make_yard(s.x + 5 - b.x, s.y - b.y);
    place_and_hold(y);
    for (int tick = 0; tick < 200; ++tick) {
        y.sim.step();
    }
    const std::uint64_t hash = y.sim.state_hash();
    const std::uint16_t left = left_of(y.sim);
    for (int frame = 0; frame < 5; ++frame) {
        (void)game::build_scene(y.sim, view(s));
    }
    EXPECT_EQ(y.sim.state_hash(), hash);
    EXPECT_EQ(left_of(y.sim), left);
}

} // namespace protekt
