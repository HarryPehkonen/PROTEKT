// N8: THE STANDING BUILDINGS' MESHES ARE CACHED, AND THE FRAME IS THE SAME FRAME.
//
// `smooth_vertex_normals` was 62.7% of `build_scene` at the x2 base (docs/FRAMECOST.md),
// re-averaging every vertex of every building every frame. A building that stands cannot change, so
// its mesh -- shape, scale and smoothed normals -- is built once (`BuildingMeshCache`), copied out
// on every later frame, and left out of the smoothing pass. These tests pin what makes that safe:
// only a building that stands level is cached; its vertices are EXACTLY the live path's, bit for
// bit; the pass skips the cached ranges; nothing depends on the order the cache was filled in; a
// building that is gone stops being held; and the pass no longer grows with the number of
// buildings.
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

struct Tile {
    int x{};
    int y{};
};

/// The north-west tile of an open (all passable) `w` x `h` rectangle with `margin` tiles all round.
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
    options.radius_tiles = 30; // the whole compound is in the drawn box
    options.camera.x = static_cast<float>(at.x + 33) * game::kMetresPerTile;
    options.camera.y = 40.0F;
    options.camera.z = static_cast<float>(at.y + 21) * game::kMetresPerTile;
    options.viewer_side = Side::Player;
    options.buildings = cache;
    options.timings = timings;
    return options;
}

/// Bit-exact: every float of a vertex, member by member, no tolerance (a -0 is not a +0).
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

/// Every kind the building model draws and may be cached, as a LITERAL list: the factory, the power
/// plant, the fifteen... minus the ones drawn elsewhere. (`stands_level` names them; this is the
/// answer written out, so a change to either shows here.)
constexpr std::array<UnitKind, 17> kCacheable{UnitKind::Factory,
                                              UnitKind::Power,
                                              UnitKind::RelayBunker,
                                              UnitKind::Armory,
                                              UnitKind::ServiceBay,
                                              UnitKind::GunTower,
                                              UnitKind::TrainingCentre,
                                              UnitKind::TechCentre,
                                              UnitKind::BomberBay,
                                              UnitKind::Kiln,
                                              UnitKind::GunSpire,
                                              UnitKind::Antenna,
                                              UnitKind::Dower,
                                              UnitKind::Stronghold,
                                              UnitKind::Jammer,
                                              UnitKind::PowerLung,
                                              UnitKind::DeployedScavenger};

/// How many of the sim's living units are of a cacheable kind: a building can come with a
/// component (the alien kinds raise a Power Lung beside them), so spawns and units are not 1:1.
[[nodiscard]] std::size_t cacheable_units(const sim::Sim& sim) {
    std::size_t count = 0;
    for (const sim::Unit& unit : sim.units()) {
        if (unit.alive
            && std::find(kCacheable.begin(), kCacheable.end(), unit.kind) != kCacheable.end()) {
            ++count;
        }
    }
    return count;
}

/// A compound: a grid of buildings 12 tiles apart, `count` of them, kinds cycling, sides
/// alternating.
[[nodiscard]] std::vector<sim::Spawn> compound(const Tile& at, std::size_t count) {
    std::vector<sim::Spawn> spawns;
    spawns.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        spawns.push_back(sim::Spawn{
            kCacheable.at(i % kCacheable.size()), i % 2 == 0 ? Side::Player : Side::Enemy,
            at.x + 6 + (static_cast<int>(i % 5) * 12), at.y + 6 + (static_cast<int>(i / 5) * 12)});
    }
    return spawns;
}

constexpr int kW = 66;
constexpr int kH = 42;

TEST(BuildingCache, OnlyABuildingThatStandsLevelAndIsDrawnByTheModelIsCached) {
    // THE GATE. Every kind of the enum is asked; the cacheable ones are the literal list above.
    for (int raw = 0; raw < static_cast<int>(sim::kUnitKindCount); ++raw) {
        sim::Unit unit;
        unit.kind = static_cast<UnitKind>(raw);
        unit.build_left = 0;
        const bool expected
            = std::find(kCacheable.begin(), kCacheable.end(), unit.kind) != kCacheable.end();
        EXPECT_EQ(game::building_is_cacheable(unit), expected) << "kind " << raw;
    }
    // A turret stands level but AIMS; a deployed recycler is drawn as the hull that drove there;
    // a vehicle tilts with the ground. None of them may come from the cache.
    for (const UnitKind kind : {UnitKind::Turret, UnitKind::DeployedTurret,
                                UnitKind::DeployedRecycler, UnitKind::Tank, UnitKind::Recycler}) {
        sim::Unit unit;
        unit.kind = kind;
        EXPECT_FALSE(game::building_is_cacheable(unit)) << static_cast<int>(kind);
    }
    // A site that is still being built is cut to its growth every frame: not cached.
    sim::Unit site;
    site.kind = UnitKind::Armory;
    site.build_left = 5;
    EXPECT_TRUE(sim::under_construction(site));
    EXPECT_FALSE(game::building_is_cacheable(site));
}

TEST(BuildingCache, AFrameFromTheCacheIsTheLiveFramesBytesForEveryCacheableKind) {
    const Tile at = open_rect(kW, kH, 2);
    ASSERT_GE(at.x, 0);
    const sim::Sim sim = world(compound(at, kCacheable.size()));
    ASSERT_GE(cacheable_units(sim), kCacheable.size()) << "every kind placed";
    const Scene live = game::build_scene(sim, view_of(at, nullptr));
    const BuildingMeshCache cache;
    const Scene first = game::build_scene(sim, view_of(at, &cache));  // every building a MISS
    const Scene second = game::build_scene(sim, view_of(at, &cache)); // every building a HIT
    EXPECT_EQ(cache.size(), cacheable_units(sim)) << "every standing building is held";
    EXPECT_GT(cache.reused(), 0U) << "and the second frame read it back";
    EXPECT_TRUE(same_bytes(live.vertices, first.vertices)) << "the frame that fills the cache";
    EXPECT_TRUE(same_bytes(live.vertices, second.vertices)) << "the frame that reads it";
}

TEST(BuildingCache, EachKindAloneIsExactlyTheLivePathsMeshOnBothSides) {
    // The comparison, kind by kind and side by side (the enemy's yaw is a half turn from the
    // player's), so one kind's difference cannot hide in a crowd.
    const Tile at = open_rect(kW, kH, 2);
    ASSERT_GE(at.x, 0);
    for (const UnitKind kind : kCacheable) {
        for (const Side side : {Side::Player, Side::Enemy}) {
            const sim::Sim sim = world({{kind, side, at.x + 12, at.y + 12}});
            const Scene live = game::build_scene(sim, view_of(at, nullptr));
            const BuildingMeshCache cache;
            const Scene filled = game::build_scene(sim, view_of(at, &cache));
            const Scene read = game::build_scene(sim, view_of(at, &cache));
            EXPECT_EQ(cache.size(), cacheable_units(sim)) << static_cast<int>(kind);
            EXPECT_TRUE(same_bytes(live.vertices, filled.vertices))
                << "kind " << static_cast<int>(kind) << " side " << static_cast<int>(side);
            EXPECT_TRUE(same_bytes(live.vertices, read.vertices))
                << "kind " << static_cast<int>(kind) << " side " << static_cast<int>(side);
        }
    }
}

TEST(BuildingCache, TheKeyIsEverythingThatShapesTheMeshSoAReusedPlaceNeverShowsTheOldOne) {
    // ONE cache, the same kind at the same place, but another side (another colour and a half
    // turn of yaw), then the same sim under another world seed (another ground). A key that left
    // either out would hand back the first one's mesh: a wrong frame, not a crash.
    const Tile at = open_rect(kW, kH, 2);
    ASSERT_GE(at.x, 0);
    const BuildingMeshCache cache;
    for (const UnitKind kind : {UnitKind::Armory, UnitKind::Power, UnitKind::Factory}) {
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
    }
}

TEST(BuildingCache, ABuildingThatDoesNotStandIsNeverTakenFromTheCache) {
    const Tile at = open_rect(kW, kH, 2);
    ASSERT_GE(at.x, 0);
    // Turrets aim, a deployed recycler is a hull: they go through the live path whatever the cache.
    const sim::Sim sim = world({{UnitKind::Turret, Side::Player, at.x + 6, at.y + 6},
                                {UnitKind::DeployedRecycler, Side::Player, at.x + 24, at.y + 12},
                                {UnitKind::Tank, Side::Player, at.x + 8, at.y + 20}});
    const BuildingMeshCache cache;
    FrameTimings timings;
    const Scene scene = game::build_scene(sim, view_of(at, &cache, &timings));
    EXPECT_EQ(cache.size(), 0U);
    EXPECT_EQ(timings.cached_buildings, 0U);
    EXPECT_TRUE(same_bytes(scene.vertices, game::build_scene(sim, view_of(at, nullptr)).vertices));
}

TEST(BuildingCache, ASelectedBuildingIsDrawnLiveBecauseItsColourIsTheMark) {
    const Tile at = open_rect(kW, kH, 2);
    ASSERT_GE(at.x, 0);
    const sim::Sim sim = world({{UnitKind::Armory, Side::Player, at.x + 12, at.y + 12},
                                {UnitKind::Power, Side::Player, at.x + 30, at.y + 12}});
    const std::array<sim::UnitId, 1> picked{sim.units().at(0).id};
    const BuildingMeshCache cache;
    FrameTimings timings;
    const Scene marked = game::build_scene(sim, view_of(at, &cache, &timings), picked);
    EXPECT_EQ(timings.cached_buildings, cacheable_units(sim) - 1U) << "all but the selected one";
    EXPECT_TRUE(
        same_bytes(marked.vertices, game::build_scene(sim, view_of(at, nullptr), picked).vertices));
}

TEST(BuildingCache, ThePassLeavesTheSkippedRangesAloneAndCountsWhatItWalked) {
    // Three runs of one quad (six vertices) each, all sharing the corner positions of a unit quad
    // and each carrying a hard-coded face normal that is NOT the smoothed one. The middle run is
    // skipped: its normals must come back untouched while the others are blended with each other.
    const auto quad = [](float lift) {
        std::vector<SceneVertex> out;
        const std::array<std::array<float, 3>, 6> corners{
            {{0, lift, 0}, {1, lift, 0}, {1, lift, 1}, {0, lift, 0}, {1, lift, 1}, {0, lift, 1}}};
        for (const auto& c : corners) {
            SceneVertex v;
            v.position = c;
            v.normal = {0.0F, 1.0F, 0.0F};
            out.push_back(v);
        }
        return out;
    };
    std::vector<SceneVertex> list = quad(0.0F);
    const std::vector<SceneVertex> middle = quad(0.0F);
    list.insert(list.end(), middle.begin(), middle.end());
    // Tilt the skipped run's normals so a blend would be visible, and the outer ones' too.
    for (std::size_t i = 6; i < 12; ++i) {
        list.at(i).normal = {0.6F, 0.8F, 0.0F};
    }
    const std::vector<SceneVertex> last = quad(0.0F);
    list.insert(list.end(), last.begin(), last.end());
    const std::vector<SceneVertex> before = list;
    const std::array<game::VertexRange, 1> skip{{{6, 12}}};
    const std::size_t walked
        = game::smooth_vertex_normals(list, 0, list.size(), game::kCreaseAngleDegrees, skip);
    EXPECT_EQ(walked, 12U) << "18 vertices less the 6 skipped";
    for (std::size_t i = 6; i < 12; ++i) {
        EXPECT_TRUE(same_vertex(list.at(i), before.at(i)))
            << "skipped vertex " << i << " was touched";
    }
    // The outer two runs, which DO see each other, blended: their normal is no longer straight up
    // ... not necessarily; what must hold is that the skipped run took no part: with the middle
    // run taken out they see only each other (both straight up), so up stays up exactly.
    for (const std::size_t i : {0U, 1U, 2U, 3U, 4U, 5U, 12U, 13U, 14U, 15U, 16U, 17U}) {
        EXPECT_EQ(list.at(i).normal, (std::array<float, 3>{0.0F, 1.0F, 0.0F}))
            << "the skipped, tilted run leaked into vertex " << i;
    }
    // And with nothing skipped the tilted run DOES blend into them, so the check above bites.
    std::vector<SceneVertex> all = before;
    EXPECT_EQ(game::smooth_vertex_normals(all, 0, all.size(), game::kCreaseAngleDegrees, {}), 18U);
    EXPECT_NE(all.at(0).normal, (std::array<float, 3>{0.0F, 1.0F, 0.0F}));
}

TEST(BuildingCache, TheSceneSmoothsNoCachedVertex) {
    const Tile at = open_rect(kW, kH, 2);
    ASSERT_GE(at.x, 0);
    const sim::Sim sim = world(compound(at, 10));
    FrameTimings plain;
    static_cast<void>(game::build_scene(sim, view_of(at, nullptr, &plain)));
    const BuildingMeshCache cache;
    static_cast<void>(game::build_scene(sim, view_of(at, &cache))); // fill
    FrameTimings cached;
    static_cast<void>(game::build_scene(sim, view_of(at, &cache, &cached)));
    EXPECT_EQ(plain.cached_buildings, 0U);
    EXPECT_EQ(cached.cached_buildings, cacheable_units(sim)) << "every one stands";
    EXPECT_GT(cached.cached_vertices, 0U);
    EXPECT_EQ(plain.smoothed_vertices, cached.smoothed_vertices + cached.cached_vertices)
        << "the pass walks everything it walked, less the cached ranges";
}

TEST(BuildingCache, TheNumberOfBuildingsNoLongerGrowsWhatThePassWalks) {
    // The per-frame cost of the pass is what it walks. One building and fifteen: with the
    // cache the pass's walk grows only by what the cache does not hold (the buildings' shadows);
    // without it, by every vertex of every building.
    const Tile at = open_rect(kW, kH, 2);
    ASSERT_GE(at.x, 0);
    const auto walked = [&](std::size_t buildings, bool cached) {
        const sim::Sim sim = world(compound(at, buildings));
        const BuildingMeshCache cache;
        static_cast<void>(game::build_scene(sim, view_of(at, cached ? &cache : nullptr)));
        FrameTimings timings;
        static_cast<void>(game::build_scene(sim, view_of(at, cached ? &cache : nullptr, &timings)));
        return timings.smoothed_vertices;
    };
    const std::size_t plain_growth = walked(15, false) - walked(1, false);
    const std::size_t cached_growth = walked(15, true) - walked(1, true);
    EXPECT_GT(plain_growth, 10000U) << "fourteen more buildings are thousands of vertices";
    EXPECT_LT(cached_growth * 10, plain_growth)
        << "cached: " << cached_growth << " plain: " << plain_growth;
}

TEST(BuildingCache, FillingTheCacheInAnotherOrderChangesNothing) {
    // No order dependence: scene A built from a cache that already served scene B (other buildings,
    // other places) is the bytes of A built from a cold cache and of A built with no cache at all.
    const Tile at = open_rect(kW, kH, 2);
    ASSERT_GE(at.x, 0);
    const sim::Sim a = world(compound(at, 8));
    std::vector<sim::Spawn> reversed = compound(at, 8);
    std::reverse(reversed.begin(), reversed.end());
    const sim::Sim b = world(reversed);
    const Scene live = game::build_scene(a, view_of(at, nullptr));
    const BuildingMeshCache warmed;
    static_cast<void>(game::build_scene(b, view_of(at, &warmed)));
    static_cast<void>(game::build_scene(b, view_of(at, &warmed)));
    const Scene after_b = game::build_scene(a, view_of(at, &warmed));
    const BuildingMeshCache cold;
    const Scene from_cold = game::build_scene(a, view_of(at, &cold));
    EXPECT_TRUE(same_bytes(live.vertices, after_b.vertices));
    EXPECT_TRUE(same_bytes(live.vertices, from_cold.vertices));
    // The same sim twice from one cache, too.
    EXPECT_TRUE(same_bytes(after_b.vertices, game::build_scene(a, view_of(at, &warmed)).vertices));
}

TEST(BuildingCache, ADestroyedBuildingStopsBeingHeld) {
    const Tile at = open_rect(kW, kH, 2);
    ASSERT_GE(at.x, 0);
    // A one-hit-point Armory with an enemy turret 12 tiles from it: the sim itself destroys it.
    sim::Spawn wounded{UnitKind::Armory, Side::Player, at.x + 12, at.y + 12};
    wounded.hp = 1;
    sim::Sim sim = world({wounded,
                          {UnitKind::Power, Side::Player, at.x + 40, at.y + 30},
                          {UnitKind::Turret, Side::Enemy, at.x + 12, at.y + 22}});
    const BuildingMeshCache cache;
    static_cast<void>(game::build_scene(sim, view_of(at, &cache)));
    ASSERT_EQ(cache.size(), cacheable_units(sim)) << "all held while they stand";
    ASSERT_GE(cache.size(), 2U);
    const std::size_t standing = cache.size();
    int ticks = 0;
    while (sim.units().at(0).alive && ticks < 900) {
        sim.step();
        ++ticks;
    }
    ASSERT_FALSE(sim.units().at(0).alive) << "the turret killed it within " << ticks << " ticks";
    static_cast<void>(game::build_scene(sim, view_of(at, &cache)));
    EXPECT_EQ(cache.size(), cacheable_units(sim)) << "the wreck is not held";
    EXPECT_LT(cache.size(), standing) << "one fewer than while it stood";
    EXPECT_GE(cache.size(), 1U) << "the power plant still is";
}

} // namespace
} // namespace protekt
