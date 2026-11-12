// A BUILDING'S FOOTPRINT IS A RECTANGLE (the owner, 2026-11-11: "the building footprints... they
// are not all completely square? Some are rectangular?"). A width and a height, per kind. Only the
// deployed recycler stands on one today, and it keeps its 2x2 (his ruling of 2026-10-26); the
// rectangles below are probes for the rules, not buildings.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#include "protekt/game/deploy_aim.hpp"
#include "protekt/game/interaction.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/units.hpp"

namespace {

using protekt::sim::Footprint;
using protekt::sim::footprint_at;
using protekt::sim::footprint_of;
using protekt::sim::FootprintSize;
using protekt::sim::kMapTiles;
using protekt::sim::Side;
using protekt::sim::Sim;
using protekt::sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr FootprintSize kWide{3, 2}; ///< wider than tall
constexpr FootprintSize kTall{2, 4}; ///< taller than wide

/// The north-west tile of an open (all passable) `size` rectangle with `margin` tiles of open
/// ground all round it.
std::pair<int, int> open_rect(FootprintSize size, int margin) {
    for (int y = 40; y < kMapTiles - 40; ++y) {
        for (int x = 40; x < kMapTiles - 40; ++x) {
            bool open = true;
            for (int ty = y - margin; ty < y + size.height + margin && open; ++ty) {
                for (int tx = x - margin; tx < x + size.width + margin && open; ++tx) {
                    open = protekt::sim::tile_passable(kSeed, tx, ty);
                }
            }
            if (open) {
                return {x, y};
            }
        }
    }
    return {-1, -1};
}

Sim world(std::vector<protekt::sim::Spawn> spawns) {
    return Sim::create(protekt::sim::Scenario{kSeed, std::move(spawns)});
}

TEST(Footprint, ARectanglesTilesAreExactlyWidthByHeightFromTheNorthWestCorner) {
    for (const FootprintSize size : {kWide, kTall, FootprintSize{1, 5}}) {
        const Footprint f = footprint_at(size, 60, 70);
        EXPECT_EQ(f.x, 60);
        EXPECT_EQ(f.y, 70);
        EXPECT_EQ(f.width, size.width);
        EXPECT_EQ(f.height, size.height);
        int count = 0;
        for (int y = 60; y < 80; ++y) {
            for (int x = 50; x < 70; ++x) {
                const bool inside
                    = x >= 60 && x < 60 + size.width && y >= 70 && y < 70 + size.height;
                EXPECT_EQ(f.covers(x, y), inside) << x << "," << y;
                count += f.covers(x, y) ? 1 : 0;
            }
        }
        EXPECT_EQ(count, size.width * size.height);
        // The tile just outside each edge is NOT in it.
        EXPECT_FALSE(f.covers(59, 70));
        EXPECT_FALSE(f.covers(60 + size.width, 70));
        EXPECT_FALSE(f.covers(60, 69));
        EXPECT_FALSE(f.covers(60, 70 + size.height));
        // And the walk visits exactly those tiles, once each.
        std::set<std::pair<int, int>> walked;
        EXPECT_TRUE(f.all_tiles([&](int x, int y) { return walked.insert({x, y}).second; }));
        EXPECT_EQ(walked.size(), static_cast<std::size_t>(size.width * size.height));
    }
}

TEST(Footprint, ANonSquareFootprintIsNeverSquaredByOneNumberServingBothAxes) {
    const Footprint wide = footprint_at(kWide, 10, 10);
    EXPECT_TRUE(wide.covers(12, 10)) << "three wide: x = 12 is the third column";
    EXPECT_FALSE(wide.covers(10, 12)) << "two tall: y = 12 is OFF it";
    const Footprint tall = footprint_at(kTall, 10, 10);
    EXPECT_FALSE(tall.covers(12, 10)) << "two wide: x = 12 is OFF it";
    EXPECT_TRUE(tall.covers(10, 13)) << "four tall: y = 13 is the fourth row";
    // The centre is per axis; the clip onto the map is per axis.
    EXPECT_EQ(wide.centre_x(), 11);
    EXPECT_EQ(wide.centre_y(), 11);
    EXPECT_EQ(tall.centre_x(), 11);
    EXPECT_EQ(tall.centre_y(), 12);
    const Footprint corner = footprint_at(kTall, kMapTiles - 1, kMapTiles - 1);
    EXPECT_EQ(corner.x, kMapTiles - 2);
    EXPECT_EQ(corner.y, kMapTiles - 4);
    // The preview grid's strokes: height + 1 along x of `width` tiles, width + 1 along z.
    EXPECT_EQ(protekt::game::deploy_preview_vertex_count(kWide), (3U * 3U + 4U * 2U) * 6U);
    EXPECT_EQ(protekt::game::deploy_preview_vertex_count(kTall), (5U * 2U + 3U * 4U) * 6U);
    EXPECT_NE(protekt::game::deploy_preview_vertex_count(kWide),
              protekt::game::deploy_preview_vertex_count(kTall));
}

TEST(Footprint, TheDeployedRecyclerIsStillTwoByTwoAndPowerIsTheOnlyOtherKindWithOne) {
    // The pin "the only kind with one" moved deliberately with round 17, J3: POWER is the first
    // placed building. Its 4x2 was provisional and the owner has since ruled: ONE tile (K1).
    EXPECT_EQ(footprint_of(UnitKind::DeployedRecycler), (FootprintSize{2, 2}));
    EXPECT_TRUE(protekt::sim::has_footprint(UnitKind::DeployedRecycler));
    EXPECT_EQ(footprint_of(UnitKind::Power), (FootprintSize{1, 1}));
    EXPECT_TRUE(protekt::sim::has_footprint(UnitKind::Power));
    for (int k = 0; k < 64; ++k) {
        const auto kind = static_cast<UnitKind>(k);
        if (kind == UnitKind::DeployedRecycler || kind == UnitKind::Power) {
            continue;
        }
        EXPECT_FALSE(protekt::sim::has_footprint(kind)) << k;
        EXPECT_EQ(footprint_of(kind), (FootprintSize{})) << k << " covers no tile";
    }
    const Footprint f = footprint_at(10, 20);
    EXPECT_EQ(f.width, 2);
    EXPECT_EQ(f.height, 2);
}

TEST(SiteRules, ARectangleThatFitsIsAcceptedWhateverItsShape) {
    for (const FootprintSize size : {kWide, kTall}) {
        const auto [x, y] = open_rect(size, 2);
        ASSERT_GE(x, 0);
        const Sim sim = world({});
        EXPECT_TRUE(protekt::sim::site_ok(kSeed, sim.units(), size, x, y))
            << size.width << "x" << size.height;
    }
}

TEST(SiteRules, ARectangleOverlappingABuildingsFootprintIsRefusedAndOneBesideItIsNot) {
    const auto [x, y] = open_rect(kWide, 6);
    ASSERT_GE(x, 0);
    // A deployed recycler (2x2) with its north-west tile at (x + 3, y + 1): centre (x + 4, y + 2).
    const Sim sim = world({{UnitKind::DeployedRecycler, Side::Player, x + 3, y + 1}});
    const Footprint rec = protekt::sim::footprint_around(
        UnitKind::DeployedRecycler, sim.units().front().x, sim.units().front().y);
    ASSERT_EQ(rec.x, x + 3);
    ASSERT_EQ(rec.y, y + 1);
    // The 3x2 at (x, y) covers x..x+2: its east edge touches the recycler's west edge, no overlap.
    EXPECT_TRUE(protekt::sim::site_ok(kSeed, sim.units(), kWide, x, y));
    // One tile east it overlaps the recycler's column x + 3 on row y + 1.
    EXPECT_FALSE(protekt::sim::site_ok(kSeed, sim.units(), kWide, x + 1, y));
    // Directly above it, rows y-1 and y clear the recycler's rows y+1, y+2: accepted.
    EXPECT_TRUE(protekt::sim::site_ok(kSeed, sim.units(), kWide, x + 3, y - 1));
    // One row lower, the rectangle's SECOND row (y + 1) is the recycler's first: refused.
    EXPECT_FALSE(protekt::sim::site_ok(kSeed, sim.units(), kWide, x + 3, y));
    // A tall one beside it overlaps only by reaching down its fourth row.
    EXPECT_TRUE(protekt::sim::site_ok(kSeed, sim.units(), kTall, x + 3, y - 3));
    EXPECT_FALSE(protekt::sim::site_ok(kSeed, sim.units(), kTall, x + 3, y - 2));
}

TEST(SiteRules, ARectangleRunningOffTheMapIsRefusedAndAnOffMapAimIsRefusedNotClipped) {
    const Sim sim = world({});
    // Aimed off the map: refused, whatever the size.
    EXPECT_FALSE(protekt::sim::site_ok(kSeed, sim.units(), kWide, -1, 50));
    EXPECT_FALSE(protekt::sim::site_ok(kSeed, sim.units(), kTall, 50, kMapTiles));
    // Aimed at the last tile on the map: the rectangle is pulled in per axis, and the verdict is
    // then the ground's (the clip is exact: the last tiles in each axis).
    const Footprint tall = footprint_at(kTall, kMapTiles - 1, 10);
    EXPECT_EQ(tall.x + tall.width, kMapTiles);
    const Footprint wide = footprint_at(kWide, 10, kMapTiles - 1);
    EXPECT_EQ(wide.y + wide.height, kMapTiles);
    EXPECT_EQ(wide.x, 10);
    EXPECT_EQ(footprint_at(kWide, kMapTiles - 1, 10).x, kMapTiles - 3);
}

TEST(SiteRules, ARectangleOnGroundNoUnitMayEnterIsRefusedEvenWithOnlyItsFarTileThere) {
    const auto [x, y] = open_rect(kWide, 2);
    ASSERT_GE(x, 0);
    const Sim sim = world({});
    // Find an impassable tile and aim rectangles so it is the LAST tile (south-east corner) of one
    // and so reached only because width and height are walked separately.
    int bad_x = -1;
    int bad_y = -1;
    for (int ty = 10; ty < kMapTiles - 10 && bad_x < 0; ++ty) {
        for (int tx = 10; tx < kMapTiles - 10; ++tx) {
            if (!protekt::sim::tile_passable(kSeed, tx, ty)) {
                bad_x = tx;
                bad_y = ty;
                break;
            }
        }
    }
    if (bad_x < 0) {
        GTEST_SKIP() << "seed has no impassable tile";
    }
    EXPECT_FALSE(protekt::sim::site_ok(kSeed, sim.units(), kWide, bad_x - 2, bad_y - 1))
        << "the bad tile is the south-east corner of a 3x2";
    EXPECT_FALSE(protekt::sim::site_ok(kSeed, sim.units(), kTall, bad_x - 1, bad_y - 3))
        << "the bad tile is the south-east corner of a 2x4";
    EXPECT_FALSE(protekt::sim::site_ok(kSeed, sim.units(), kWide, bad_x, bad_y));
}

// ---------------------------------------------------------------- ROUND 18, K1: POWER is ONE tile
// The owner, verbatim: "I think Power should take up only 1 x 1.  At least that's what their
// footprint seems to be in BattleZone 2."  A footprint is measured in TILES.

TEST(OneTile, TheTableReadsOneByOneForPowerAndPinsEveryOtherKindExactlyAsItWas) {
    EXPECT_EQ(footprint_of(UnitKind::Power), (FootprintSize{1, 1}));
    EXPECT_EQ(protekt::sim::kPowerFootprint, (FootprintSize{1, 1}));
    EXPECT_EQ(footprint_of(UnitKind::DeployedRecycler), (FootprintSize{2, 2})) << "untouched";
    // Only two kinds have a table row today: the other eight Builder rows have no UnitKind and so
    // no PROVISIONAL rectangle YET. This pins the whole table, so the day a kind is given one (a
    // later slice's real value) is a deliberate edit of this test, not a drift.
    for (int k = 0; k < 64; ++k) {
        const auto kind = static_cast<UnitKind>(k);
        if (kind == UnitKind::DeployedRecycler || kind == UnitKind::Power) {
            continue;
        }
        EXPECT_EQ(footprint_of(kind), (FootprintSize{})) << k;
    }
}

TEST(OneTile, AOneTileFootprintIsOneTileWhichTurningCannotChange) {
    const Footprint f = footprint_at(FootprintSize{1, 1}, 70, 80);
    EXPECT_TRUE(f.covers(70, 80));
    EXPECT_FALSE(f.covers(71, 80));
    EXPECT_FALSE(f.covers(69, 80));
    EXPECT_FALSE(f.covers(70, 81));
    EXPECT_FALSE(f.covers(70, 79));
    EXPECT_EQ(f.centre_x(), 70);
    EXPECT_EQ(f.centre_y(), 80);
    // The clip onto the map: the last tile is a whole footprint, so nothing is pulled back.
    const Footprint corner = footprint_at(FootprintSize{1, 1}, kMapTiles - 1, kMapTiles - 1);
    EXPECT_EQ(corner.x, kMapTiles - 1);
    EXPECT_EQ(corner.y, kMapTiles - 1);
}

TEST(OneTile, AOneByOneSiteIsAcceptedInASingleTileGapAndRefusedWhenThatTileIsTaken) {
    const auto [x, y] = open_rect(FootprintSize{5, 3}, 2);
    ASSERT_GE(x, 0);
    // Two POWERs with exactly one tile between them: (x, y+1) and (x+2, y+1); the gap is (x+1,
    // y+1).
    const Sim sim = world(
        {{UnitKind::Power, Side::Player, x, y + 1}, {UnitKind::Power, Side::Player, x + 2, y + 1}});
    const auto& units = sim.units();
    constexpr FootprintSize kOne{1, 1};
    EXPECT_TRUE(protekt::sim::site_ok(kSeed, units, kOne, x + 1, y + 1)) << "the gap takes it";
    EXPECT_TRUE(protekt::sim::place_ok(kSeed, units, UnitKind::Power, false, x + 1, y + 1));
    // ...where nothing two tiles wide can stand: not the recycler's square, not a 2x1 either way.
    EXPECT_FALSE(protekt::sim::site_ok(kSeed, units, FootprintSize{2, 2}, x + 1, y + 1));
    EXPECT_FALSE(protekt::sim::site_ok(kSeed, units, FootprintSize{2, 1}, x + 1, y + 1));
    EXPECT_FALSE(protekt::sim::site_ok(kSeed, units, FootprintSize{2, 1}, x, y + 1));
    EXPECT_FALSE(protekt::sim::deploy_site_ok(kSeed, units, x + 1, y + 1));
    // The tile taken is refused, and so is the tile of either neighbour building.
    EXPECT_FALSE(protekt::sim::site_ok(kSeed, units, kOne, x, y + 1));
    EXPECT_FALSE(protekt::sim::site_ok(kSeed, units, kOne, x + 2, y + 1));
    // Fill the gap and it is refused too.
    const Sim full = world({{UnitKind::Power, Side::Player, x, y + 1},
                            {UnitKind::Power, Side::Player, x + 1, y + 1},
                            {UnitKind::Power, Side::Player, x + 2, y + 1}});
    EXPECT_FALSE(protekt::sim::site_ok(kSeed, full.units(), kOne, x + 1, y + 1));
    EXPECT_FALSE(protekt::sim::place_ok(kSeed, full.units(), UnitKind::Power, false, x + 1, y + 1));
    // Off the map and off the ground are still refused for one tile.
    EXPECT_FALSE(protekt::sim::site_ok(kSeed, units, kOne, -1, 5));
    EXPECT_FALSE(protekt::sim::site_ok(kSeed, units, kOne, 5, kMapTiles));
}

TEST(OneTile, TheUnitBlockingReadsTheSameOneTile) {
    const auto [x, y] = open_rect(FootprintSize{3, 3}, 2);
    ASSERT_GE(x, 0);
    const Sim sim = world({{UnitKind::Power, Side::Player, x + 1, y + 1}});
    const auto& units = sim.units();
    int blocked = 0;
    for (int ty = y - 1; ty <= y + 3; ++ty) {
        for (int tx = x - 1; tx <= x + 3; ++tx) {
            const bool is_it = tx == x + 1 && ty == y + 1;
            EXPECT_EQ(protekt::sim::building_blocks(units, tx, ty), is_it) << tx << "," << ty;
            blocked += protekt::sim::building_blocks(units, tx, ty) ? 1 : 0;
        }
    }
    EXPECT_EQ(blocked, 1);
    // It stands on its tile's centre, so the floor of its position is that tile.
    EXPECT_EQ(units.front().x.floor_int(), x + 1);
    EXPECT_EQ(units.front().y.floor_int(), y + 1);
    EXPECT_EQ(units.front().x.raw,
              (x + 1) * protekt::sim::Fixed::kOne + (protekt::sim::Fixed::kOne / 2))
        << "centred on the tile, not on its north-west corner";
}

TEST(OneTile, ThePreviewDrawsOneTileWithFourEdgesNotASquareOfFour) {
    constexpr FootprintSize kOne{1, 1};
    EXPECT_EQ(protekt::game::deploy_preview_vertex_count(kOne), 4U * 6U) << "four strokes";
    EXPECT_NE(protekt::game::deploy_preview_vertex_count(kOne),
              protekt::game::deploy_preview_vertex_count(FootprintSize{2, 2}));
    // The SCENE draws exactly that many, and the preview the game hands it is the one tile.
    const Sim sim = world({{UnitKind::Tank, Side::Player, 60, 60}});
    protekt::game::SceneOptions plain;
    plain.seed = kSeed;
    plain.radius_tiles = 8;
    plain.camera.x = 60.0F * protekt::game::kMetresPerTile;
    plain.camera.z = 60.0F * protekt::game::kMetresPerTile;
    plain.camera.y = 8.0F;
    plain.show_ship_gauge = false;
    plain.show_ledger = false;
    protekt::game::SceneOptions previewed = plain;
    const protekt::game::Placing armed{UnitKind::Power, false, 30, footprint_of(UnitKind::Power)};
    previewed.deploy_preview = protekt::game::placement_preview(sim, armed, 63, 58);
    EXPECT_EQ(previewed.deploy_preview->site.width, 1);
    EXPECT_EQ(previewed.deploy_preview->site.height, 1);
    const auto without = protekt::game::build_scene(sim, plain);
    const auto with = protekt::game::build_scene(sim, previewed);
    EXPECT_EQ(with.vertices.size(), without.vertices.size() + 24U);
}

TEST(OneTile, ThePickBoxIsTheOneTileAndNotWiderThanIt) {
    const auto [x, y] = open_rect(FootprintSize{5, 5}, 2);
    ASSERT_GE(x, 0);
    const Sim sim = world({{UnitKind::Power, Side::Player, x + 2, y + 2}});
    const auto& power = sim.units().front();
    const float cx = static_cast<float>(power.x.raw) / static_cast<float>(protekt::sim::Fixed::kOne)
                     * protekt::game::kMetresPerTile;
    const float cz = static_cast<float>(power.y.raw) / static_cast<float>(protekt::sim::Fixed::kOne)
                     * protekt::game::kMetresPerTile;
    const float ground = protekt::game::ground_metres(kSeed, power.x, power.y);
    protekt::game::Camera camera{};
    camera.x = cx;
    camera.y = ground + 0.9F;
    camera.z = cz - 24.0F;
    camera.yaw = 0.0F;
    camera.pitch = 0.0F;
    protekt::game::SceneOptions options{};
    options.seed = kSeed;
    options.camera = camera;
    const protekt::game::Extent extent{1280, 720};
    const auto pick_at = [&](float dx_metres) {
        const auto p = protekt::game::project(camera, extent, cx + dx_metres, ground + 0.9F, cz);
        return protekt::game::pick(sim, options, p.x, p.y);
    };
    EXPECT_TRUE(pick_at(0.0F).has_unit()) << "its own tile's middle";
    EXPECT_TRUE(pick_at(0.8F).has_unit()) << "inside the tile (a tile is 2 m: +-1 m)";
    // 1.4 m out is the NEIGHBOUR tile's: the old 0.75-tile radius (1.5 m) reached into it.
    EXPECT_FALSE(pick_at(1.4F).has_unit()) << "into the next tile";
    EXPECT_FALSE(pick_at(-1.4F).has_unit());
}

} // namespace
