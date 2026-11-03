// THE STORE GAUGE (Harri, 2026-11-01): a vertical gauge beside the radar whose whole extent is the
// store's capacity, a green section for the base and a gold one added at the bottom for each
// deployed scavenger, a white bar up to the scrap held, and that number below. Drawn, never written
// as a fraction.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "protekt/game/pick.hpp"
#include "protekt/game/radar.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/store_gauge.hpp"
#include "protekt/game/world_map.hpp"
#include "protekt/sim/orders.hpp"
#include "protekt/sim/pool.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace {

using protekt::game::GaugeRect;
using protekt::game::StoreGaugeLayout;

constexpr float kPx = 0.75F; ///< a pixel's worth of slack

const protekt::game::RadarPanel kPanel = protekt::game::radar_panel({1280, 720});

StoreGaugeLayout gauge(int capacity, int band, int held) {
    return protekt::game::store_gauge_layout(kPanel, capacity, band, held, 2.0F);
}

float bottom(const GaugeRect& r) { return r.y + r.height; }

TEST(StoreGauge, ItSitsImmediatelyBesideTheRadarAndSharesItsBottom) {
    const StoreGaugeLayout g = gauge(60, 20, 49);
    EXPECT_NEAR(g.frame.x, kPanel.x + kPanel.width + protekt::game::kGaugeGapPixels, kPx)
        << "beside the radar, one gap from its right edge";
    EXPECT_NEAR(bottom(g.frame), kPanel.y + kPanel.height, kPx);
    EXPECT_GT(g.frame.height, g.frame.width * 2.0F) << "a vertical gauge";
}

TEST(StoreGauge, TheWholeExtentIsTheCapacityGreenOnTopAndGoldAddedAtTheBottom) {
    const StoreGaugeLayout g = gauge(60, 20, 49);
    ASSERT_GT(g.column.height, 0.0F);
    EXPECT_NEAR(g.green.height, g.column.height * 40.0F / 60.0F, kPx) << "the base 40";
    EXPECT_NEAR(g.gold.height, g.column.height * 20.0F / 60.0F, kPx)
        << "one deployed scavenger's 20";
    EXPECT_NEAR(g.green.y, g.column.y, kPx) << "green on top";
    EXPECT_NEAR(g.gold.y, bottom(g.green), kPx) << "gold directly under it";
    EXPECT_NEAR(bottom(g.gold), bottom(g.column), kPx) << "and at the very bottom";
    EXPECT_FLOAT_EQ(g.green.x, g.column.x);
    EXPECT_FLOAT_EQ(g.gold.width, g.column.width);
}

TEST(StoreGauge, EachDeployedScavengerAddsAnotherTwentyOfGoldAndTheExtentStaysTheCapacity) {
    const StoreGaugeLayout one = gauge(60, 20, 0);
    const StoreGaugeLayout three = gauge(100, 60, 0);
    EXPECT_NEAR(three.gold.height / three.column.height, 60.0F / 100.0F, 0.01F);
    EXPECT_NEAR(three.green.height / three.column.height, 40.0F / 100.0F, 0.01F);
    EXPECT_NEAR(one.column.height, three.column.height, kPx) << "the gauge never grows taller";
}

TEST(StoreGauge, WithNoDeployedScavengerItIsAllGreen) {
    const StoreGaugeLayout g = gauge(40, 0, 7);
    EXPECT_NEAR(g.green.height, g.column.height, kPx);
    EXPECT_FLOAT_EQ(g.gold.height, 0.0F);
}

TEST(StoreGauge, TheWhiteBarRisesFromTheBottomToTheScrapHeld) {
    const StoreGaugeLayout g = gauge(60, 20, 49);
    EXPECT_NEAR(bottom(g.level), bottom(g.column), kPx);
    EXPECT_NEAR(g.level.height, g.column.height * 49.0F / 60.0F, kPx);
    EXPECT_LT(g.level.width, g.column.width) << "inset inside the sections, as in his capture";
    EXPECT_GE(g.level.x, g.column.x);
    const StoreGaugeLayout empty = gauge(60, 20, 0);
    EXPECT_FLOAT_EQ(empty.level.height, 0.0F);
    const StoreGaugeLayout over = gauge(60, 20, 90);
    EXPECT_NEAR(over.level.height, over.column.height, kPx) << "a bank never draws past the top";
}

TEST(StoreGauge, TheNumberIsTheScrapHeldBelowTheColumn) {
    const StoreGaugeLayout g = gauge(60, 20, 49);
    EXPECT_EQ(g.text, "49") << "held, not capacity and not a fraction";
    EXPECT_GE(g.number.y, bottom(g.column) - kPx);
    EXPECT_LE(bottom(g.number), bottom(g.frame) + kPx);
    EXPECT_EQ(gauge(40, 0, 0).text, "0");
}

// ---- the chunks: one gold segment per deployed scavenger, a gap between each
// ---------------------

constexpr float kGap = protekt::game::kGaugeChunkGapCells * 2.0F; ///< the cell is 2.0 in `gauge()`

TEST(StoreGauge, ThereIsOneChunkForTheBaseAndOneForEveryDeployedScavenger) {
    // 40 base, then +20 a scavenger: counting the chunks counts the scavengers.
    EXPECT_EQ(gauge(40, 0, 0).chunks.size(), 1U) << "the base alone";
    EXPECT_EQ(gauge(60, 20, 0).chunks.size(), 2U) << "one scavenger";
    EXPECT_EQ(gauge(80, 40, 0).chunks.size(), 3U) << "two";
    EXPECT_EQ(gauge(160, 120, 0).chunks.size(), 7U) << "six";
    const StoreGaugeLayout three = gauge(100, 60, 0);
    ASSERT_EQ(three.chunks.size(), 4U);
    EXPECT_FALSE(three.chunks.at(0).gold) << "the base is the green one, on top";
    for (std::size_t i = 1; i < three.chunks.size(); ++i) {
        EXPECT_TRUE(three.chunks.at(i).gold) << i;
    }
}

TEST(StoreGauge, ASmallGapSeparatesEveryChunkFromTheNext) {
    const StoreGaugeLayout g = gauge(100, 60, 0);
    ASSERT_EQ(g.chunks.size(), 4U);
    for (std::size_t i = 0; i + 1 < g.chunks.size(); ++i) {
        const float gap = g.chunks.at(i + 1).box.y - bottom(g.chunks.at(i).box);
        EXPECT_NEAR(gap, kGap, 0.01F) << "between chunk " << i << " and " << (i + 1);
        EXPECT_GT(gap, 1.0F) << "a gap you can see";
        EXPECT_LT(gap, g.chunks.at(i).box.height * 0.25F) << "and a small one";
    }
    EXPECT_NEAR(g.chunks.front().box.y, g.column.y, 0.01F) << "the outer edges stay flush";
    EXPECT_NEAR(bottom(g.chunks.back().box), bottom(g.column), 0.01F);
}

TEST(StoreGauge, EachDeployedScavengersChunkIsTheSameSizeAndTheBaseIsTwiceOne) {
    const StoreGaugeLayout g = gauge(100, 60, 0);
    ASSERT_EQ(g.chunks.size(), 4U);
    const float pitch = g.column.height * 20.0F / 100.0F;
    EXPECT_NEAR(g.chunks.at(2).box.y - g.chunks.at(1).box.y, pitch, 0.01F);
    EXPECT_NEAR(g.chunks.at(3).box.y - g.chunks.at(2).box.y, pitch, 0.01F);
    EXPECT_NEAR(g.chunks.at(2).box.height, pitch - kGap, 0.01F) << "an inner gold chunk";
    EXPECT_NEAR(g.chunks.at(0).box.height + (kGap * 0.5F), pitch * 2.0F, 0.01F)
        << "the base is two scavengers' worth";
}

TEST(StoreGauge, TheWhiteBarIsCutByTheSameGapsAndReachesOnlyWhereTheScrapDoes) {
    // Full: one piece of white per chunk, with the gaps open between them.
    const StoreGaugeLayout full = gauge(100, 60, 100);
    ASSERT_EQ(full.fills.size(), full.chunks.size());
    for (std::size_t i = 0; i + 1 < full.fills.size(); ++i) {
        EXPECT_NEAR(full.fills.at(i + 1).y - bottom(full.fills.at(i)), kGap, 0.01F);
    }
    // 49 of 60 reaches into the base chunk only a little: one gold chunk full, the base partly.
    const StoreGaugeLayout part = gauge(60, 20, 49);
    ASSERT_EQ(part.fills.size(), 2U);
    EXPECT_NEAR(bottom(part.fills.back()), bottom(part.column), 0.01F);
    EXPECT_NEAR(part.fills.front().y, part.level.y, 0.01F) << "the top of the bar is the scrap";
    EXPECT_EQ(gauge(60, 20, 0).fills.size(), 0U) << "no scrap, no white";
    EXPECT_EQ(gauge(100, 60, 20).fills.size(), 1U) << "20 is the bottom chunk and no more";
}

TEST(StoreGauge, ThePillIsBandsThatTileTheFrameExactlyAndNoneTallerThanAChunkSlice) {
    const StoreGaugeLayout g = gauge(100, 60, 0);
    ASSERT_GT(g.pill.size(), g.chunks.size());
    float at = g.frame.y;
    float tallest = 0.0F;
    for (const GaugeRect& band : g.pill) {
        EXPECT_NEAR(band.y, at, 0.001F) << "a band starts where the last ended";
        EXPECT_FLOAT_EQ(band.x, g.frame.x);
        EXPECT_FLOAT_EQ(band.width, g.frame.width);
        EXPECT_GT(band.height, 0.0F);
        at = band.y + band.height;
        tallest = std::max(tallest, band.height);
    }
    EXPECT_NEAR(at, bottom(g.frame), 0.001F) << "and the last ends at the frame's bottom";
    EXPECT_LE(tallest, g.column.height * 40.0F / 100.0F) << "none taller than the base chunk";
}

TEST(StoreGauge, TheNumberFitsTheHalfSizeMapsNarrowPill) {
    // The map is half the radar's width now, 165 px: 16% of it (26 px) would clip "142".
    const protekt::game::WorldMapLayout map = protekt::game::world_map_layout({1280, 720});
    const protekt::game::RadarPanel square{map.x, map.y, map.size, map.size};
    const StoreGaugeLayout g = protekt::game::store_gauge_layout(square, 180, 140, 142, 2.0F);
    const float text_width = ((static_cast<float>(g.text.size()) * 6.0F) - 1.0F) * 2.0F;
    EXPECT_GE(g.frame.width, text_width + 4.0F);
    EXPECT_NEAR(g.frame.height, map.size, 0.01F) << "as tall as the map beside it";
}

// ---- in the scene ------------------------------------------------------------------------------

using protekt::game::Scene;
using protekt::game::SceneOptions;
using protekt::sim::Side;
using protekt::sim::Sim;

/// A battle whose player has one deployed scavenger and a few units banked from its pool.
Sim banked_battle() {
    protekt::sim::Scenario s;
    s.seed = 20261101;
    s.pools = {protekt::sim::Pool{70, 60}};
    s.spawns = {protekt::sim::Spawn{protekt::sim::UnitKind::Factory, Side::Player, 50, 60},
                protekt::sim::Spawn{protekt::sim::UnitKind::Scavenger, Side::Player, 70, 60}};
    Sim sim = Sim::create(s);
    sim.enqueue(protekt::sim::Order{protekt::sim::UnitId{2}, protekt::sim::orders::Deploy{70, 60}});
    for (int i = 0; i < 400 || sim.ore(Side::Player) < 5; ++i) {
        sim.step();
    }
    return sim;
}

SceneOptions rig() {
    SceneOptions o;
    o.seed = 20261101;
    o.radius_tiles = 8;
    o.camera.x = 100.0F;
    o.camera.z = 100.0F;
    o.camera.y = 8.0F;
    o.show_ship_gauge = false;
    o.show_ledger = true; // the layer that carries the gauge
    o.viewer_unit = protekt::sim::UnitId{1};
    return o;
}

/// The frame-pixel bounding box of every vertex drawn in `ink`; width 0 when there is none.
GaugeRect ink_box(const Scene& scene, const SceneOptions& options,
                  const std::array<float, 3>& ink) {
    float lo_x = 1.0e9F;
    float lo_y = 1.0e9F;
    float hi_x = -1.0e9F;
    float hi_y = -1.0e9F;
    for (const auto& v : scene.vertices) {
        if (std::abs(v.colour.at(0) - ink.at(0)) > 0.005F
            || std::abs(v.colour.at(1) - ink.at(1)) > 0.005F
            || std::abs(v.colour.at(2) - ink.at(2)) > 0.005F) {
            continue;
        }
        const auto p = protekt::game::project(options.camera, options.extent, v.position[0],
                                              v.position[1], v.position[2]);
        lo_x = std::min(lo_x, p.x);
        lo_y = std::min(lo_y, p.y);
        hi_x = std::max(hi_x, p.x);
        hi_y = std::max(hi_y, p.y);
    }
    if (hi_x < lo_x) {
        return {};
    }
    return GaugeRect{lo_x, lo_y, hi_x - lo_x, hi_y - lo_y};
}

void expect_box(const GaugeRect& drawn, const GaugeRect& wanted, const char* what) {
    EXPECT_NEAR(drawn.x, wanted.x, 1.5F) << what;
    EXPECT_NEAR(drawn.y, wanted.y, 1.5F) << what;
    EXPECT_NEAR(drawn.width, wanted.width, 1.5F) << what;
    EXPECT_NEAR(drawn.height, wanted.height, 1.5F) << what;
}

TEST(StoreGauge, TheSceneDrawsTheSimsOwnCapacityBandAndBankAsTheLayoutSays) {
    const Sim sim = banked_battle();
    ASSERT_EQ(sim.capacity(Side::Player), 60);
    ASSERT_EQ(sim.fast_band(Side::Player), 20);
    const SceneOptions options = rig();
    const Scene scene = protekt::game::build_scene(sim, options);
    const StoreGaugeLayout g = protekt::game::store_gauge_layout(
        protekt::game::radar_panel(options.extent), sim.capacity(Side::Player),
        sim.fast_band(Side::Player), sim.ore(Side::Player), 2.0F);
    ASSERT_GT(g.column.height, 0.0F);
    ASSERT_EQ(g.chunks.size(), 2U);
    expect_box(ink_box(scene, options, protekt::game::kGaugeGreen), g.chunks.at(0).box, "green");
    expect_box(ink_box(scene, options, protekt::game::kGaugeGold), g.chunks.at(1).box, "gold");
    expect_box(ink_box(scene, options, protekt::game::kGaugePill), g.frame, "pill");
    EXPECT_GT(ink_box(scene, options, protekt::game::kGaugeLevel).height, 0.0F) << "the white bar";
    EXPECT_GT(ink_box(scene, options, protekt::game::kGaugeInk).width, 0.0F) << "the number";
}

/// How many quads of `ink` the scene drew (six vertices each).
std::size_t quads_in(const Scene& scene, const std::array<float, 3>& ink) {
    std::size_t vertices = 0;
    for (const auto& v : scene.vertices) {
        if (std::abs(v.colour.at(0) - ink.at(0)) < 0.005F
            && std::abs(v.colour.at(1) - ink.at(1)) < 0.005F
            && std::abs(v.colour.at(2) - ink.at(2)) < 0.005F) {
            ++vertices;
        }
    }
    return vertices / 6U;
}

TEST(StoreGauge, TheSceneDrawsOneQuadPerChunkAndTheGapsBetweenThemInBothViews) {
    const Sim sim = banked_battle();
    for (const bool map : {false, true}) {
        SceneOptions options = rig();
        options.show_world_map = map;
        const Scene scene = protekt::game::build_scene(sim, options);
        EXPECT_EQ(quads_in(scene, protekt::game::kGaugeGreen), 1U) << "map " << map;
        EXPECT_EQ(quads_in(scene, protekt::game::kGaugeGold), 1U)
            << "one deployed scavenger, one gold chunk, map " << map;
        const GaugeRect green = ink_box(scene, options, protekt::game::kGaugeGreen);
        const GaugeRect gold = ink_box(scene, options, protekt::game::kGaugeGold);
        EXPECT_NEAR(gold.y - (green.y + green.height), kGap, 1.0F) << "the gap, map " << map;
    }
}

/// The projected height of each quad drawn in `ink`, six vertices to a quad.
std::vector<float> quad_heights(const Scene& scene, const SceneOptions& options,
                                const std::array<float, 3>& ink) {
    std::vector<float> heights;
    std::size_t in_quad = 0;
    float lo = 1.0e9F;
    float hi = -1.0e9F;
    for (const auto& v : scene.vertices) {
        if (std::abs(v.colour.at(0) - ink.at(0)) > 0.005F
            || std::abs(v.colour.at(1) - ink.at(1)) > 0.005F
            || std::abs(v.colour.at(2) - ink.at(2)) > 0.005F) {
            continue;
        }
        const auto p = protekt::game::project(options.camera, options.extent, v.position[0],
                                              v.position[1], v.position[2]);
        lo = std::min(lo, p.y);
        hi = std::max(hi, p.y);
        if (++in_quad == 6) {
            heights.push_back(hi - lo);
            in_quad = 0;
            lo = 1.0e9F;
            hi = -1.0e9F;
        }
    }
    return heights;
}

TEST(StoreGauge, NoPillQuadIsTallerThanAChunk) {
    // THE CAUSE of the map-mode bug, pinned on the vertices (Harri, 2026-11-03): the pill was one
    // quad the height of the panel beside the gauge, and a tall flat quad's depth beat the sections
    // a hair in front of it. Every pill quad is a band no taller than the base chunk's slice.
    const Sim sim = banked_battle();
    for (const bool map : {false, true}) {
        SceneOptions options = rig();
        options.show_world_map = map;
        const Scene scene = protekt::game::build_scene(sim, options);
        const GaugeRect pill = ink_box(scene, options, protekt::game::kGaugePill);
        const std::vector<float> bands = quad_heights(scene, options, protekt::game::kGaugePill);
        ASSERT_GT(bands.size(), 2U) << "the pill is bands, map " << map;
        const float tallest = *std::max_element(bands.begin(), bands.end());
        // The column is 60 of capacity and the base is 40 of it: two thirds of the column at most,
        // and the frame is the column plus its padding and the number's room.
        EXPECT_LT(tallest, pill.height * 0.75F) << "map " << map;
    }
}

TEST(StoreGauge, InMapModeTheGaugeIsAsTallAsTheHalfSizeMapAndTheNumberStaysInsideThePill) {
    const Sim sim = banked_battle();
    SceneOptions options = rig();
    options.show_world_map = true;
    const protekt::game::WorldMapLayout map = protekt::game::world_map_layout(options.extent);
    const Scene scene = protekt::game::build_scene(sim, options);
    const GaugeRect pill = ink_box(scene, options, protekt::game::kGaugePill);
    EXPECT_NEAR(pill.height, map.size, 1.5F);
    EXPECT_NEAR(pill.x, map.x + map.size + protekt::game::kGaugeGapPixels, 1.5F);
    const GaugeRect number = ink_box(scene, options, protekt::game::kGaugeInk);
    EXPECT_GE(number.x, pill.x - 0.5F);
    EXPECT_LE(number.x + number.width, pill.x + pill.width + 0.5F);
}

TEST(StoreGauge, WithoutADeployedScavengerTheSceneDrawsNoGold) {
    protekt::sim::Scenario s;
    s.seed = 20261101;
    s.spawns = {protekt::sim::Spawn{protekt::sim::UnitKind::Factory, Side::Player, 50, 60}};
    const Sim sim = Sim::create(s);
    const SceneOptions options = rig();
    const Scene scene = protekt::game::build_scene(sim, options);
    EXPECT_GT(ink_box(scene, options, protekt::game::kGaugeGreen).height, 0.0F);
    EXPECT_FLOAT_EQ(ink_box(scene, options, protekt::game::kGaugeGold).height, 0.0F);
}

TEST(StoreGauge, TheLayerIsOffWhereTheHudLayerIsOff) {
    const Sim sim = banked_battle();
    SceneOptions options = rig();
    options.show_ledger = false;
    const Scene scene = protekt::game::build_scene(sim, options);
    EXPECT_FLOAT_EQ(ink_box(scene, options, protekt::game::kGaugeGreen).height, 0.0F);
}

} // namespace
