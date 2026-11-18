// N10: THE SAME CACHE, FOR VEHICLES, AND THE FRAME IS THE SAME FRAME.
//
// A battle's scene is mostly vehicles, and `smooth_vertex_normals` re-averaged all of them every
// frame. A vehicle that holds still is now built once (`BuildingMeshCache`'s vehicle table), copied
// out on every later frame and stepped over by the pass. These tests pin what makes that safe: a
// cached vehicle's vertices are EXACTLY the live path's, bit for bit, for every kind, both
// factions and both sides, at any heading; a vehicle that moved, turned or was marked is never
// shown a stale mesh; the pass walks none of the cached ranges; and the pass no longer grows with
// the number of vehicles that stand still.
//
// Every number below is a LITERAL or a count taken from the live path, never read back from the
// code under test.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "protekt/game/building_cache.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/orders.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/units.hpp"

namespace protekt {
namespace {

using game::BuildingMeshCache;
using game::FrameTimings;
using game::Scene;
using game::SceneOptions;
using game::SceneVertex;
using sim::Side;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;

/// Every kind a vehicle is drawn as, a LITERAL list: all of them are cacheable, because the
/// builders read nothing that differs per unit but the id (the camo, in the key), the kind, the
/// side and the place. (`stands_level` names the ones that are NOT; this is the answer written
/// out, so a change to either shows here.)
constexpr std::array<UnitKind, 16> kVehicles{
    UnitKind::Tank,      UnitKind::HoverTank,   UnitKind::MortarBike,   UnitKind::RocketTank,
    UnitKind::Scavenger, UnitKind::Recycler,    UnitKind::Scout,        UnitKind::AssaultTank,
    UnitKind::Walker,    UnitKind::Apc,         UnitKind::Bomber,       UnitKind::MslScout,
    UnitKind::Tug,       UnitKind::Constructor, UnitKind::ServiceTruck, UnitKind::ServicePod};

struct Tile {
    int x{};
    int y{};
};

[[nodiscard]] Tile open_rect(int w, int h, int margin) {
    for (int y = 40; y < sim::kMapTiles - 60; ++y) {
        for (int x = 40; x < sim::kMapTiles - 60; ++x) {
            bool open = true;
            for (int ty = y - margin; ty < y + h + margin && open; ++ty) {
                for (int tx = x - margin; tx < x + w + margin && open; ++tx) {
                    open = sim::tile_passable(kSeed, tx, ty);
                }
            }
            if (open) {
                return Tile{x, y};
            }
        }
    }
    return Tile{-1, -1};
}

[[nodiscard]] sim::Sim world(std::vector<sim::Spawn> spawns) {
    return sim::Sim::create(sim::Scenario{kSeed, std::move(spawns)});
}

[[nodiscard]] SceneOptions view_of(const Tile& at, const BuildingMeshCache* cache,
                                   FrameTimings* timings = nullptr) {
    SceneOptions options{};
    options.seed = kSeed;
    options.radius_tiles = 30;
    options.camera.x = static_cast<float>(at.x + 33) * game::kMetresPerTile;
    options.camera.y = 40.0F;
    options.camera.z = static_cast<float>(at.y + 21) * game::kMetresPerTile;
    options.viewer_side = Side::Player;
    options.buildings = cache;
    options.timings = timings;
    return options;
}

[[nodiscard]] bool same_bits(float a, float b) {
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}
[[nodiscard]] bool same_bits(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return same_bits(a.at(0), b.at(0)) && same_bits(a.at(1), b.at(1))
           && same_bits(a.at(2), b.at(2));
}
[[nodiscard]] bool same_vertex(const SceneVertex& a, const SceneVertex& b) {
    return same_bits(a.position, b.position) && same_bits(a.colour, b.colour)
           && same_bits(a.alpha, b.alpha) && same_bits(a.u, b.u) && same_bits(a.v, b.v)
           && same_bits(a.fog, b.fog) && same_bits(a.normal, b.normal);
}
[[nodiscard]] bool same_bytes(const std::vector<SceneVertex>& a,
                              const std::vector<SceneVertex>& b) {
    return a.size() == b.size()
           && std::equal(
               a.begin(), a.end(), b.begin(),
               [](const SceneVertex& l, const SceneVertex& r) { return same_vertex(l, r); });
}

/// `count` vehicles of the kinds in `kVehicles`, 8 tiles apart, sides alternating.
[[nodiscard]] std::vector<sim::Spawn> fleet(const Tile& at, std::size_t count) {
    std::vector<sim::Spawn> spawns;
    spawns.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        spawns.push_back(sim::Spawn{
            kVehicles.at(i % kVehicles.size()), i % 2 == 0 ? Side::Player : Side::Enemy,
            at.x + 6 + (static_cast<int>(i % 6) * 8), at.y + 6 + (static_cast<int>(i / 6) * 8)});
    }
    return spawns;
}

constexpr int kW = 66;
constexpr int kH = 42;

TEST(VehicleCache, EveryVehicleKindIsCacheableAndNothingElseIs) {
    for (int raw = 0; raw < sim::kUnitKindCount; ++raw) {
        sim::Unit unit;
        unit.kind = static_cast<UnitKind>(raw);
        unit.build_left = 0;
        const bool expected
            = std::find(kVehicles.begin(), kVehicles.end(), unit.kind) != kVehicles.end();
        EXPECT_EQ(game::vehicle_is_cacheable(unit), expected) << "kind " << raw;
        // The two gates never both say yes: a unit is a building or a vehicle, not both.
        EXPECT_FALSE(game::vehicle_is_cacheable(unit) && game::building_is_cacheable(unit))
            << "kind " << raw;
    }
}

TEST(VehicleCache, AFrameFromTheCacheIsTheLiveFramesBytesForEveryKindBothSidesBothFactions) {
    const Tile at = open_rect(kW, kH, 2);
    ASSERT_GE(at.x, 0);
    for (const game::Faction faction : {game::Faction::Human, game::Faction::Alien}) {
        const sim::Sim sim = world(fleet(at, 32)); // each kind twice: both sides
        SceneOptions live_view = view_of(at, nullptr);
        live_view.factions = game::Factions{faction, faction};
        const Scene live = game::build_scene(sim, live_view);
        const BuildingMeshCache cache;
        SceneOptions cached_view = view_of(at, &cache);
        cached_view.factions = live_view.factions;
        const Scene first = game::build_scene(sim, cached_view);  // every vehicle a MISS
        const Scene second = game::build_scene(sim, cached_view); // every vehicle a HIT
        EXPECT_EQ(cache.vehicle_size(), 32U) << "every standing vehicle is held";
        EXPECT_GE(cache.vehicle_reused(), 32U) << "and the second frame read each back";
        EXPECT_TRUE(same_bytes(live.vertices, first.vertices)) << "the frame that fills it";
        EXPECT_TRUE(same_bytes(live.vertices, second.vertices)) << "the frame that reads it";
    }
}

TEST(VehicleCache, EachKindAloneIsExactlyTheLivePathsMeshOnBothSides) {
    const Tile at = open_rect(kW, kH, 2);
    ASSERT_GE(at.x, 0);
    for (const UnitKind kind : kVehicles) {
        for (const Side side : {Side::Player, Side::Enemy}) {
            const sim::Sim sim = world({{kind, side, at.x + 12, at.y + 12}});
            const Scene live = game::build_scene(sim, view_of(at, nullptr));
            const BuildingMeshCache cache;
            const Scene filled = game::build_scene(sim, view_of(at, &cache));
            const Scene read = game::build_scene(sim, view_of(at, &cache));
            EXPECT_EQ(cache.vehicle_size(), 1U) << static_cast<int>(kind);
            EXPECT_TRUE(same_bytes(live.vertices, filled.vertices))
                << "kind " << static_cast<int>(kind) << " side " << static_cast<int>(side);
            EXPECT_TRUE(same_bytes(live.vertices, read.vertices))
                << "kind " << static_cast<int>(kind) << " side " << static_cast<int>(side);
        }
    }
}

/// A tank sent east and stepped `ticks` ticks: it is MOVING (its heading is its velocity's).
struct Driving {
    sim::Sim sim;
    game::Headings headings;
};
[[nodiscard]] Driving driving(const Tile& at, int dx, int dy, int ticks) {
    Driving d{world({{UnitKind::Tank, Side::Player, at.x + 12, at.y + 12}}), {}};
    d.sim.enqueue(sim::Order{d.sim.units().front().id,
                             sim::orders::MoveTo{sim::Fixed::from_int(at.x + 12 + dx),
                                                 sim::Fixed::from_int(at.y + 12 + dy)}});
    for (int i = 0; i < ticks; ++i) {
        d.sim.step();
    }
    d.headings.observe(d.sim);
    return d;
}

TEST(VehicleCache, ATurnedInstanceIsExactlyTheLivePathsTurnedOutput) {
    // The same tank, driving four ways, through ONE cache: the heading is a key and a turned hull
    // is never shown the mesh of the first.
    const Tile at = open_rect(kW, kH, 2);
    ASSERT_GE(at.x, 0);
    const BuildingMeshCache cache;
    for (const std::array<int, 2> way : {std::array<int, 2>{6, 0}, std::array<int, 2>{0, -6},
                                         std::array<int, 2>{-5, 3}, std::array<int, 2>{4, -6}}) {
        const Driving d = driving(at, way.at(0), way.at(1), 3);
        ASSERT_NE(d.sim.units().front().vx.raw | d.sim.units().front().vy.raw, 0) << "moving";
        SceneOptions live_view = view_of(at, nullptr);
        live_view.headings = &d.headings;
        SceneOptions cached_view = view_of(at, &cache);
        cached_view.headings = &d.headings;
        const Scene live = game::build_scene(d.sim, live_view);
        EXPECT_TRUE(same_bytes(live.vertices, game::build_scene(d.sim, cached_view).vertices))
            << "heading " << way.at(0) << "," << way.at(1);
        EXPECT_TRUE(same_bytes(live.vertices, game::build_scene(d.sim, cached_view).vertices))
            << "again, read back: heading " << way.at(0) << "," << way.at(1);
        // THE SAME PLACE, ANOTHER HEADING, one cache: let the tank arrive and stand still. It
        // keeps the heading it last had; a `Headings` that never saw it move says 0. A key that
        // left the heading out would hand back the mesh cached for the turned hull.
        Driving stopped = driving(at, way.at(0), way.at(1), 1);
        for (int tick = 0; tick < 400
                           && (stopped.sim.units().front().vx.raw != 0
                               || stopped.sim.units().front().vy.raw != 0);
             ++tick) {
            stopped.sim.step();
            stopped.headings.observe(stopped.sim);
        }
        ASSERT_EQ(stopped.sim.units().front().vx.raw | stopped.sim.units().front().vy.raw, 0);
        const game::Headings unseen;
        SceneOptions remembered_cached = view_of(at, &cache);
        remembered_cached.headings = &stopped.headings;
        SceneOptions remembered_live = view_of(at, nullptr);
        remembered_live.headings = &stopped.headings;
        SceneOptions level_live = view_of(at, nullptr);
        level_live.headings = &unseen;
        SceneOptions level_cached = view_of(at, &cache);
        level_cached.headings = &unseen;
        const Scene turned = game::build_scene(stopped.sim, remembered_live);
        const Scene level = game::build_scene(stopped.sim, level_live);
        EXPECT_FALSE(same_bytes(turned.vertices, level.vertices))
            << "the two headings really are two meshes";
        EXPECT_TRUE(same_bytes(turned.vertices,
                               game::build_scene(stopped.sim, remembered_cached).vertices));
        EXPECT_TRUE(
            same_bytes(level.vertices, game::build_scene(stopped.sim, level_cached).vertices))
            << "same place, heading 0, after the turned one: " << way.at(0) << "," << way.at(1);
    }
}

TEST(VehicleCache, AVehicleThatMovesIsAMissEveryFrameAndStillTheLivePathsBytes) {
    const Tile at = open_rect(kW, kH, 2);
    ASSERT_GE(at.x, 0);
    Driving d = driving(at, 10, 0, 1);
    const BuildingMeshCache cache;
    for (int frame = 0; frame < 4; ++frame) {
        SceneOptions live_view = view_of(at, nullptr);
        live_view.headings = &d.headings;
        SceneOptions cached_view = view_of(at, &cache);
        cached_view.headings = &d.headings;
        EXPECT_TRUE(same_bytes(game::build_scene(d.sim, live_view).vertices,
                               game::build_scene(d.sim, cached_view).vertices))
            << "frame " << frame;
        EXPECT_EQ(cache.vehicle_size(), 1U) << "only this frame's pose is held";
        d.sim.step();
        d.headings.observe(d.sim);
    }
    EXPECT_EQ(cache.vehicle_reused(), 0U) << "a tank that moves every frame never hits";
    EXPECT_EQ(cache.vehicle_built(), 4U);
}

TEST(VehicleCache, TheKeyIsEverythingThatShapesTheMeshSoAReusedPlaceNeverShowsTheOldOne) {
    const Tile at = open_rect(kW, kH, 2);
    ASSERT_GE(at.x, 0);
    const BuildingMeshCache cache;
    for (const UnitKind kind : {UnitKind::Tank, UnitKind::Scavenger, UnitKind::Walker}) {
        const sim::Sim mine = world({{kind, Side::Player, at.x + 12, at.y + 12}});
        const sim::Sim theirs = world({{kind, Side::Enemy, at.x + 12, at.y + 12}});
        static_cast<void>(game::build_scene(mine, view_of(at, &cache)));
        EXPECT_TRUE(same_bytes(game::build_scene(theirs, view_of(at, nullptr)).vertices,
                               game::build_scene(theirs, view_of(at, &cache)).vertices))
            << "side, kind " << static_cast<int>(kind);
        SceneOptions other_ground = view_of(at, &cache);
        other_ground.seed = kSeed + 1;
        SceneOptions other_live = view_of(at, nullptr);
        other_live.seed = kSeed + 1;
        EXPECT_TRUE(same_bytes(game::build_scene(mine, other_live).vertices,
                               game::build_scene(mine, other_ground).vertices))
            << "seed, kind " << static_cast<int>(kind);
        // And the other faction's silhouette (the alien Scavenger is a Harvester).
        SceneOptions alien_cached = view_of(at, &cache);
        alien_cached.factions = game::Factions{game::Faction::Alien, game::Faction::Alien};
        SceneOptions alien_live = view_of(at, nullptr);
        alien_live.factions = alien_cached.factions;
        EXPECT_TRUE(same_bytes(game::build_scene(mine, alien_live).vertices,
                               game::build_scene(mine, alien_cached).vertices))
            << "faction, kind " << static_cast<int>(kind);
    }
}

TEST(VehicleCache, ASelectedVehicleIsDrawnLiveBecauseItsColourIsTheMark) {
    const Tile at = open_rect(kW, kH, 2);
    ASSERT_GE(at.x, 0);
    const sim::Sim sim = world({{UnitKind::Tank, Side::Player, at.x + 12, at.y + 12},
                                {UnitKind::Apc, Side::Player, at.x + 30, at.y + 12}});
    const std::array<sim::UnitId, 1> picked{sim.units().at(0).id};
    const BuildingMeshCache cache;
    FrameTimings timings;
    const Scene marked = game::build_scene(sim, view_of(at, &cache, &timings), picked);
    EXPECT_EQ(timings.cached_vehicles, 1U) << "all but the selected one";
    EXPECT_EQ(cache.vehicle_size(), 1U);
    EXPECT_TRUE(
        same_bytes(marked.vertices, game::build_scene(sim, view_of(at, nullptr), picked).vertices));
}

TEST(VehicleCache, TheDrivenVehicleIsDrawnLive) {
    const Tile at = open_rect(kW, kH, 2);
    ASSERT_GE(at.x, 0);
    const sim::Sim sim = world({{UnitKind::Tank, Side::Player, at.x + 12, at.y + 12}});
    SceneOptions live_view = view_of(at, nullptr);
    live_view.facing = SceneOptions::Facing{sim.units().front().id, 0.7F, 0.4F, 0.2F};
    const BuildingMeshCache cache;
    SceneOptions cached_view = view_of(at, &cache);
    cached_view.facing = live_view.facing;
    EXPECT_TRUE(same_bytes(game::build_scene(sim, live_view).vertices,
                           game::build_scene(sim, cached_view).vertices));
    EXPECT_EQ(cache.vehicle_size(), 0U) << "the player's own hull is never held";
}

TEST(VehicleCache, ThePassLeavesTheCachedVehicleRangesAloneAndCountsWhatItWalked) {
    // The scene: the pass walks what it walked, less the cached vehicles' vertices, and the
    // vertices it did NOT walk are the cache's bytes, final.
    const Tile at = open_rect(kW, kH, 2);
    ASSERT_GE(at.x, 0);
    const sim::Sim sim = world(fleet(at, 12));
    FrameTimings plain;
    static_cast<void>(game::build_scene(sim, view_of(at, nullptr, &plain)));
    const BuildingMeshCache cache;
    static_cast<void>(game::build_scene(sim, view_of(at, &cache))); // fill
    FrameTimings cached;
    static_cast<void>(game::build_scene(sim, view_of(at, &cache, &cached)));
    EXPECT_EQ(plain.cached_vehicles, 0U);
    EXPECT_EQ(cached.cached_vehicles, 12U) << "every one holds still";
    EXPECT_GT(cached.cached_vertices, 0U);
    EXPECT_EQ(plain.smoothed_vertices, cached.smoothed_vertices + cached.cached_vertices)
        << "the pass walks everything it walked, less the cached ranges";
}

TEST(VehicleCache, AKindThatMustStayLiveIsWalkedByThePass) {
    // A building under the BUILDING gate, a site under construction, and a marked vehicle are not
    // vehicle-cached: they are in the pass's walk. A vehicle that IS cached is not.
    const Tile at = open_rect(kW, kH, 2);
    ASSERT_GE(at.x, 0);
    const sim::Sim sim = world({{UnitKind::Tank, Side::Player, at.x + 12, at.y + 12},
                                {UnitKind::Turret, Side::Player, at.x + 30, at.y + 12}});
    FrameTimings plain;
    static_cast<void>(game::build_scene(sim, view_of(at, nullptr, &plain)));
    const BuildingMeshCache cache;
    static_cast<void>(game::build_scene(sim, view_of(at, &cache)));
    FrameTimings cached;
    const std::array<sim::UnitId, 1> picked{sim.units().at(0).id};
    static_cast<void>(game::build_scene(sim, view_of(at, &cache, &cached), picked));
    EXPECT_EQ(cached.cached_vehicles, 0U) << "the only vehicle is the selected one";
    EXPECT_EQ(cached.cached_vertices, 0U) << "and the turret aims: the pass walks both";
    EXPECT_EQ(cached.smoothed_vertices, plain.smoothed_vertices);
}

TEST(VehicleCache, BuildingTheSceneTwiceForTheSameSimGivesTheSameVertices) {
    const Tile at = open_rect(kW, kH, 2);
    ASSERT_GE(at.x, 0);
    const sim::Sim sim = world(fleet(at, 16));
    const BuildingMeshCache cache;
    const Scene a = game::build_scene(sim, view_of(at, &cache));
    const Scene b = game::build_scene(sim, view_of(at, &cache));
    const Scene c = game::build_scene(sim, view_of(at, &cache));
    EXPECT_TRUE(same_bytes(a.vertices, b.vertices));
    EXPECT_TRUE(same_bytes(b.vertices, c.vertices));
    // And order does not matter: a cache warmed by a reversed fleet gives the same frame.
    std::vector<sim::Spawn> reversed = fleet(at, 16);
    std::reverse(reversed.begin(), reversed.end());
    const BuildingMeshCache warmed;
    static_cast<void>(game::build_scene(world(reversed), view_of(at, &warmed)));
    EXPECT_TRUE(same_bytes(a.vertices, game::build_scene(sim, view_of(at, &warmed)).vertices));
}

TEST(VehicleCache, TheNumberOfStandingVehiclesNoLongerGrowsWhatThePassWalks) {
    // THE TEST THE OLD EVERY-FRAME PATH FAILS: with no cache the pass's walk grows by every
    // vertex of every added vehicle; with it, by what the cache does not hold (their shadows).
    const Tile at = open_rect(kW, kH, 2);
    ASSERT_GE(at.x, 0);
    const auto walked = [&](std::size_t vehicles, bool cached) {
        const sim::Sim sim = world(fleet(at, vehicles));
        const BuildingMeshCache cache;
        static_cast<void>(game::build_scene(sim, view_of(at, cached ? &cache : nullptr)));
        FrameTimings timings;
        static_cast<void>(game::build_scene(sim, view_of(at, cached ? &cache : nullptr, &timings)));
        return timings.smoothed_vertices;
    };
    const std::size_t plain_growth = walked(15, false) - walked(1, false);
    const std::size_t cached_growth = walked(15, true) - walked(1, true);
    EXPECT_GT(plain_growth, 10000U) << "fourteen more vehicles are thousands of vertices";
    EXPECT_LT(cached_growth * 10, plain_growth)
        << "cached: " << cached_growth << " plain: " << plain_growth;
}

TEST(VehicleCache, ADestroyedVehicleStopsBeingHeldAndNoBuildingEntryIsAffected) {
    const Tile at = open_rect(kW, kH, 2);
    ASSERT_GE(at.x, 0);
    sim::Sim sim = world({{UnitKind::Tank, Side::Player, at.x + 12, at.y + 12},
                          {UnitKind::Tank, Side::Player, at.x + 24, at.y + 12},
                          {UnitKind::Armory, Side::Player, at.x + 40, at.y + 30}});
    const BuildingMeshCache cache;
    static_cast<void>(game::build_scene(sim, view_of(at, &cache)));
    ASSERT_EQ(cache.vehicle_size(), 2U);
    ASSERT_EQ(cache.size(), 1U) << "the buildings' table is its own";
    // The tank at index 0 is gone from the scene when it is not alive; one frame later it is not
    // held.
    sim::Sim fewer = world({{UnitKind::Tank, Side::Player, at.x + 24, at.y + 12},
                            {UnitKind::Armory, Side::Player, at.x + 40, at.y + 30}});
    static_cast<void>(game::build_scene(fewer, view_of(at, &cache)));
    EXPECT_EQ(cache.vehicle_size(), 1U);
    EXPECT_EQ(cache.size(), 1U);
}

} // namespace
} // namespace protekt
