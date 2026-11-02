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
    expect_box(ink_box(scene, options, protekt::game::kGaugeGreen), g.green, "green");
    expect_box(ink_box(scene, options, protekt::game::kGaugeGold), g.gold, "gold");
    expect_box(ink_box(scene, options, protekt::game::kGaugePill), g.frame, "pill");
    EXPECT_GT(ink_box(scene, options, protekt::game::kGaugeLevel).height, 0.0F) << "the white bar";
    EXPECT_GT(ink_box(scene, options, protekt::game::kGaugeInk).width, 0.0F) << "the number";
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
