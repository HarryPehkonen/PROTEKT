#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "protekt/app/audio_pipeline.hpp"
#include "protekt/game/interaction.hpp"
#include "protekt/game/pick.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/weapon.hpp"
#include "protekt/input/input.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"
#include "protekt/world/world.hpp"

/// M5: an honest trigger (sound and message follow what the SIM did), the reload on the reticle,
/// and the landing marker.
namespace protekt::game {
namespace {

using input::Click;
using input::InputFrame;
using input::MouseButton;
using sim::Side;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr game::Extent kExtent{1280, 720};
constexpr float kHalfPi = 1.5707963F;

class CountingSink final : public app::SampleSink {
public:
    int plays{0};
    [[nodiscard]] bool ok() const noexcept override { return true; }
    void play(std::span<const float>, int) noexcept override { ++plays; }
};

[[nodiscard]] float as_float(sim::Fixed value) {
    return static_cast<float>(value.raw) / static_cast<float>(sim::Fixed::kOne);
}

/// A tile on a level row of 16 tiles (and the row beside it), none a crater.
struct Row {
    int x{};
    int y{};
};

[[nodiscard]] Row find_level_row() {
    constexpr int kLength = 16;
    constexpr int kTolerance = 6;
    for (int y = 5; y < sim::kMapTiles - 5; ++y) {
        for (int x = 5; x + kLength < sim::kMapTiles - 5; ++x) {
            const int h0 = world::sample(kSeed, x, y).height;
            bool level = true;
            for (int i = 0; i <= kLength && level; ++i) {
                const auto t = world::sample(kSeed, x + i, y);
                const auto below = world::sample(kSeed, x + i, y + 1);
                level = t.terrain != world::Terrain::Crater && std::abs(t.height - h0) <= kTolerance
                        && std::abs(below.height - h0) <= kTolerance;
            }
            if (level) {
                return Row{x, y};
            }
        }
    }
    return Row{-1, -1};
}

/// A frame of the game as `app/game.cpp` runs it, in miniature: the hands, the order, the pipeline,
/// and what the app does with what the pipeline learned.
struct Rig {
    sim::Sim sim;
    SceneOptions options;
    sim::UnitId tank{};
    Interaction player{sim::UnitId{}};
    CountingSink sink;
    app::AudioPipeline pipeline{&sink};
    HitFlash flash;
    HitMark mark;
    LandingMarks marks;
    int shots{0};
    int landings{0};
    sim::Tick last_landing_tick{};
    sim::Landing last_landing{};
};

[[nodiscard]] InputFrame trigger() {
    InputFrame frame{};
    frame.clicks.push_back(Click{640, 360, MouseButton::Left, true, false});
    return frame;
}

void look(Rig& rig, float pitch) {
    const sim::Unit* gun = rig.sim.find(rig.tank);
    rig.options.camera = Camera{as_float(gun->x) * kMetresPerTile,
                                ground_metres(kSeed, gun->x, gun->y) + kRideHeightMetres + 3.0F,
                                as_float(gun->y) * kMetresPerTile, kHalfPi, pitch};
}

/// The driven tank on a level row with the mortar up, its gun ready, the barrel along +x at
/// `pitch`.
void make_rig(Rig& rig, Weapon weapon, float pitch) {
    const Row row = find_level_row();
    ASSERT_GE(row.x, 0);
    const int far_y = row.y < 100 ? row.y + 40 : row.y - 40;
    rig.sim = sim::Sim::create(sim::Scenario{kSeed,
                                             {{UnitKind::Tank, Side::Player, row.x, row.y},
                                              {UnitKind::Tank, Side::Enemy, row.x, far_y}}});
    rig.tank = rig.sim.units().front().id;
    rig.player = Interaction{rig.tank};
    for (int tick = 0; tick < sim::profile(UnitKind::Tank).cooldown_ticks; ++tick) {
        rig.sim.step(); // a new unit starts with its gun reloading
    }
    rig.options.seed = kSeed;
    rig.options.extent = kExtent;
    rig.options.aim
        = Aim{static_cast<float>(kExtent.width) / 2.0F, static_cast<float>(kExtent.height) / 2.0F};
    rig.options.radius_tiles = 24;
    rig.options.viewer_unit = rig.tank;
    rig.options.weapon = weapon;
    look(rig, pitch);
    const int notches = weapon == Weapon::Gun ? 0 : (weapon == Weapon::Rocket ? 1 : 2);
    InputFrame wheel{};
    wheel.wheel = -notches;
    rig.player.handle(wheel, rig.sim, rig.options);
    ASSERT_EQ(rig.player.weapon(), weapon);
}

/// One frame, one tick: exactly the app's sequence.
void frame(Rig& rig, bool pull) {
    rig.player.handle(pull ? trigger() : InputFrame{}, rig.sim, rig.options);
    for (const sim::Order& order : rig.player.take_orders()) {
        rig.sim.enqueue(order);
    }
    const bool pulled = rig.player.take_shot_voice();
    const app::AudioFrame audio{Listener{}, pulled, true, true, rig.tank};
    const app::AdvanceResult result = rig.pipeline.advance(rig.sim, 1, audio, rig.flash, rig.mark);
    for (const sim::Sim::Shot& shot : rig.sim.shots()) {
        if (shot.shooter == rig.tank) {
            ++rig.shots;
        }
    }
    if (result.trigger_refused) {
        rig.player.trigger_refused(rig.sim);
    }
    for (const app::AdvanceResult::Touchdown& down : result.touchdowns) {
        rig.marks.land(down.tick, down.landing.tile_x, down.landing.tile_y);
        ++rig.landings;
        rig.last_landing_tick = down.tick;
        rig.last_landing = down.landing;
    }
}

// --- (a) an honest trigger ---------------------------------------------------------------------

TEST(HonestTrigger, ADoubleClickInsideTheMortarCooldownLeavesOneShotAndOneSound) {
    Rig rig;
    make_rig(rig, Weapon::Mortar, 0.6F);
    frame(rig, true);
    EXPECT_EQ(rig.shots, 1);
    EXPECT_EQ(rig.sink.plays, 1);
    EXPECT_TRUE(rig.player.message().empty()) << "an accepted trigger says nothing";
    for (sim::Tick idle = 0; idle < kManualShotIntervalTicks + 2; ++idle) {
        frame(rig, false);
    }
    ASSERT_TRUE(weapon_reloading(rig.sim, rig.tank)) << "the second click is inside the 45";
    frame(rig, true); // the button allows it; the gun does not
    EXPECT_EQ(rig.shots, 1) << "the sim launched nothing for the second click";
    EXPECT_EQ(rig.sink.plays, 1) << "and so nothing was heard";
    EXPECT_EQ(rig.sim.projectiles().size(), 1U);
}

TEST(HonestTrigger, ARefusedTriggerNamesTheReasonAndAnAcceptedOneDoesNot) {
    Rig rig;
    make_rig(rig, Weapon::Mortar, 0.6F);
    frame(rig, true);
    EXPECT_EQ(rig.player.message(), "");
    for (sim::Tick idle = 0; idle < kManualShotIntervalTicks + 2; ++idle) {
        frame(rig, false);
    }
    frame(rig, true);
    EXPECT_EQ(rig.player.message(), "the mortar is reloading.");
    frame(rig, false);
    EXPECT_EQ(rig.player.message(), "") << "a message describes the frame that just happened";
}

TEST(HonestTrigger, TheGunAndTheRocketNameThemselvesAndAnUnmountedTurretIsDeploying) {
    sim::Unit tank{};
    tank.kind = UnitKind::Tank;
    EXPECT_EQ(trigger_refusal(Weapon::Gun, tank), "the gun is reloading.");
    EXPECT_EQ(trigger_refusal(Weapon::Rocket, tank), "the rocket is reloading.");
    EXPECT_EQ(trigger_refusal(Weapon::Mortar, tank), "the mortar is reloading.");
    sim::Unit turret{};
    turret.kind = UnitKind::Turret;
    turret.still_ticks = 0;
    EXPECT_EQ(trigger_refusal(Weapon::Gun, turret), "the turret is deploying.");
    turret.still_ticks = sim::kMountTicks;
    EXPECT_EQ(trigger_refusal(Weapon::Gun, turret), "the gun is reloading.");
}

TEST(HonestTrigger, ATriggerBeforeTheGunIsLoadedIsSilentAndExplained) {
    Rig rig;
    make_rig(rig, Weapon::Gun, 0.0F);
    frame(rig, true);
    ASSERT_EQ(rig.shots, 1);
    for (sim::Tick idle = 0; idle < kManualShotIntervalTicks; ++idle) {
        frame(rig, false);
    }
    const int heard = rig.sink.plays;
    frame(rig, true); // 9 ticks in, the gun has 15
    EXPECT_EQ(rig.shots, 1);
    EXPECT_EQ(rig.sink.plays, heard);
    EXPECT_EQ(rig.player.message(), "the gun is reloading.");
}

// --- (b) the reload on the reticle -------------------------------------------------------------

[[nodiscard]] int ticks_until_ready(Rig& rig) {
    frame(rig, true);
    int ticks = 1; // the step that launched it
    while (weapon_reloading(rig.sim, rig.tank) && ticks < 200) {
        frame(rig, false);
        ++ticks;
    }
    return ticks;
}

TEST(ReloadReticle, TheMortarShellReloadsForFortyFiveTicksAndTheTanksBoltFifteen) {
    Rig mortar;
    make_rig(mortar, Weapon::Mortar, 0.6F);
    EXPECT_EQ(ticks_until_ready(mortar), 45);
    EXPECT_EQ(sim::profile(UnitKind::Mortar).cooldown_ticks, 45);
    Rig gun;
    make_rig(gun, Weapon::Gun, 0.0F);
    EXPECT_EQ(ticks_until_ready(gun), 15);
    EXPECT_EQ(sim::profile(UnitKind::Tank).cooldown_ticks, 15);
}

/// The dot at the reticle's tail, which every weapon draws.
[[nodiscard]] std::array<float, 3> dot_colour(const Scene& scene) {
    return scene.vertices.back().colour;
}

TEST(ReloadReticle, ItReadsSpentWhileReloadingAndNormalTheTickItIsReady) {
    for (const Weapon weapon : {Weapon::Gun, Weapon::Mortar}) {
        Rig rig;
        make_rig(rig, weapon, 0.6F);
        const int reload = weapon == Weapon::Gun ? 15 : 45;
        rig.options.reloading = false;
        const std::array<float, 3> ready = dot_colour(build_scene(rig.sim, rig.options));
        for (const float channel : ready) {
            EXPECT_GT(channel, 0.8F) << "ready is bright";
        }
        frame(rig, true);
        for (int age = 1; age <= reload; ++age) {
            rig.options.reloading = weapon_reloading(rig.sim, rig.tank);
            const std::array<float, 3> drawn = dot_colour(build_scene(rig.sim, rig.options));
            if (age < reload) {
                for (std::size_t c = 0; c < 3; ++c) {
                    EXPECT_LT(drawn.at(c), ready.at(c) * 0.45F) << "age " << age;
                }
                frame(rig, false);
            } else {
                EXPECT_EQ(drawn, ready) << "the tick the cooldown hits zero";
            }
        }
    }
}

TEST(ReloadReticle, TheWholeTickedReticleIsDimmedNotJustTheDot) {
    Rig rig;
    make_rig(rig, Weapon::Gun, 0.0F);
    const Scene ready = build_scene(rig.sim, rig.options);
    rig.options.reloading = true;
    const Scene spent = build_scene(rig.sim, rig.options);
    ASSERT_EQ(ready.vertices.size(), spent.vertices.size()) << "the same shape, ghosted";
    for (std::size_t i = ready.vertices.size() - kReticleVertexCount; i < ready.vertices.size();
         ++i) {
        EXPECT_EQ(ready.vertices[i].position, spent.vertices[i].position);
        EXPECT_LT(spent.vertices[i].colour[0], ready.vertices[i].colour[0] * 0.45F);
    }
}

// --- (c) the landing marker --------------------------------------------------------------------

struct Marked {
    std::vector<SceneVertex> marker;
    bool tail_untouched{false};
};

/// The marker's vertices: what appears in the scene when the marks are attached.
[[nodiscard]] Marked marker_vertices(Rig& rig) {
    rig.options.landing_marks = nullptr;
    const Scene bare = build_scene(rig.sim, rig.options);
    rig.options.landing_marks = &rig.marks;
    const Scene marked = build_scene(rig.sim, rig.options);
    rig.options.landing_marks = nullptr;
    Marked out;
    if (marked.vertices.size() < bare.vertices.size()) {
        return out;
    }
    std::size_t first = 0;
    while (first < bare.vertices.size() && first < marked.vertices.size()
           && bare.vertices[first].position == marked.vertices[first].position
           && bare.vertices[first].colour == marked.vertices[first].colour) {
        ++first;
    }
    const std::size_t extra = marked.vertices.size() - bare.vertices.size();
    out.marker.assign(marked.vertices.begin() + static_cast<std::ptrdiff_t>(first),
                      marked.vertices.begin() + static_cast<std::ptrdiff_t>(first + extra));
    out.tail_untouched = true;
    for (std::size_t i = first; i < bare.vertices.size(); ++i) {
        out.tail_untouched = out.tail_untouched
                             && bare.vertices[i].position == marked.vertices[i + extra].position;
    }
    return out;
}

void expect_marker_at(Rig& rig, float expected_tiles) {
    const float start = as_float(rig.sim.find(rig.tank)->x);
    const int tank_row = rig.sim.find(rig.tank)->y.floor_int();
    frame(rig, true);
    for (int tick = 0; tick < 300 && rig.landings == 0; ++tick) {
        frame(rig, false);
    }
    ASSERT_EQ(rig.landings, 1) << "the sim reported the touchdown";
    EXPECT_FALSE(rig.last_landing.hit);
    EXPECT_EQ(rig.last_landing.tile_y, tank_row);
    EXPECT_NEAR(static_cast<float>(rig.last_landing.tile_x) + 0.5F - start, expected_tiles, 1.1F);

    const sim::Tick landed = rig.last_landing_tick;
    ASSERT_EQ(rig.sim.tick(), landed) << "read the tick it came down on";
    const std::vector<LandingMarks::Mark> showing = rig.marks.active(landed);
    ASSERT_EQ(showing.size(), 1U);
    EXPECT_EQ(showing[0].tile_x, rig.last_landing.tile_x);
    EXPECT_EQ(showing[0].tile_y, rig.last_landing.tile_y);
    EXPECT_EQ(showing[0].level, 1.0F);

    // In the scene: one marker's worth of vertices, standing on the touchdown tile.
    const Marked drawn = marker_vertices(rig);
    ASSERT_EQ(drawn.marker.size(), kLandingMarkVertexCount);
    EXPECT_TRUE(drawn.tail_untouched) << "the reticle is still the tail";
    const float centre_x = (static_cast<float>(rig.last_landing.tile_x) + 0.5F) * kMetresPerTile;
    const float centre_z = (static_cast<float>(rig.last_landing.tile_y) + 0.5F) * kMetresPerTile;
    const float ground = ground_metres(kSeed, static_cast<float>(rig.last_landing.tile_x) + 0.5F,
                                       static_cast<float>(rig.last_landing.tile_y) + 0.5F);
    for (const SceneVertex& v : drawn.marker) {
        EXPECT_LE(std::abs(v.position[0] - centre_x), 3.0F);
        EXPECT_LE(std::abs(v.position[2] - centre_z), 3.0F);
        EXPECT_GE(v.position[1], ground - 0.5F);
        EXPECT_LE(v.position[1], ground + 6.0F);
    }

    // Seen from the level view the shot was fired from: the shell can land close enough that the
    // foot of the marker is below the bottom of the frame, but it stands 4 m tall, so its middle is
    // in the frame, a good part of it is on screen, and what is on screen is big.
    look(rig, 0.0F);
    float top = 1e9F;
    float bottom = -1e9F;
    std::size_t inside = 0;
    for (const SceneVertex& v : drawn.marker) {
        const Projected p
            = project(rig.options.camera, kExtent, v.position[0], v.position[1], v.position[2]);
        ASSERT_GT(p.depth, 0.0F);
        if (p.x >= 0.0F && p.x < static_cast<float>(kExtent.width) && p.y >= 0.0F
            && p.y < static_cast<float>(kExtent.height)) {
            ++inside;
        }
        top = std::min(top, p.y);
        bottom = std::max(bottom, p.y);
    }
    EXPECT_GE(inside * 3, drawn.marker.size()) << "at least a third of the marker is in the frame";
    const Projected middle = project(rig.options.camera, kExtent, centre_x,
                                     ground + (kLandingMarkHeight / 2.0F), centre_z);
    EXPECT_GE(middle.y, 0.0F);
    EXPECT_LT(middle.y, static_cast<float>(kExtent.height));
    EXPECT_GT(std::min(bottom, static_cast<float>(kExtent.height)) - std::max(top, 0.0F), 40.0F)
        << "not tiny: at least 40 px of it on screen at 720";

    // Gone after its lifetime.
    EXPECT_EQ(rig.marks.active(landed + kLandingMarkHoldTicks - 1).front().level, 1.0F);
    const float fading = rig.marks.active(landed + kLandingMarkHoldTicks + 5).front().level;
    EXPECT_GT(fading, 0.0F);
    EXPECT_LT(fading, 1.0F);
    EXPECT_TRUE(rig.marks.active(landed + kLandingMarkHoldTicks + kLandingMarkFadeTicks).empty());
    for (sim::Tick tick = 0; tick < kLandingMarkHoldTicks + kLandingMarkFadeTicks; ++tick) {
        frame(rig, false);
    }
    EXPECT_TRUE(marker_vertices(rig).marker.empty()) << "and out of the scene";
}

TEST(LandingMarker, TheLowestLadderPitchLandsAMarkerThatIsClearlyVisibleInFront) {
    Rig rig;
    make_rig(rig, Weapon::Mortar, kMortarRungPitches.front());
    expect_marker_at(rig, mortar_landing_tiles(kMortarRungPitches.front()));
}

TEST(LandingMarker, TheHighestLadderPitchLandsAMarkerThirteenPointNineTilesOut) {
    Rig rig;
    make_rig(rig, Weapon::Mortar, kMortarRungPitches.back());
    expect_marker_at(rig, mortar_landing_tiles(kMortarRungPitches.back()));
    EXPECT_NEAR(mortar_landing_tiles(kMortarRungPitches.back()), 13.9F, 0.1F);
}

TEST(LandingMarker, OnlyThePlayersOwnLobMakesOne) {
    Rig rig;
    make_rig(rig, Weapon::Gun, 0.0F);
    frame(rig, true); // a straight bolt into the ground, not a lob
    for (int tick = 0; tick < 200; ++tick) {
        frame(rig, false);
    }
    EXPECT_EQ(rig.landings, 0);
    EXPECT_TRUE(rig.marks.active(rig.sim.tick()).empty());
}

// --- (d) the shell's trail ---------------------------------------------------------------------

TEST(ShellTrail, AMortarShellDrawsThreeTrailBoxesBehindItAndAGunBoltDrawsNone) {
    constexpr std::size_t kBoxVertices = 36;
    for (const Weapon weapon : {Weapon::Gun, Weapon::Mortar}) {
        Rig rig;
        make_rig(rig, weapon, 0.6F);
        const std::size_t bare = build_scene(rig.sim, rig.options).vertices.size();
        frame(rig, true);
        frame(rig, false);
        ASSERT_EQ(rig.sim.projectiles().size(), 1U);
        const std::size_t flying = build_scene(rig.sim, rig.options).vertices.size();
        EXPECT_EQ(flying - bare, weapon == Weapon::Mortar ? 4 * kBoxVertices : kBoxVertices);
    }
}

// --- determinism -------------------------------------------------------------------------------

TEST(M5, TwoSameSeedRunsAreByteIdentical) {
    const auto run = [] {
        Rig rig;
        make_rig(rig, Weapon::Mortar, 0.6F);
        frame(rig, true);
        for (int tick = 0; tick < 12; ++tick) {
            frame(rig, tick == 9);
        }
        for (int tick = 0; tick < 60 && rig.landings == 0; ++tick) {
            frame(rig, false);
        }
        rig.options.reloading = weapon_reloading(rig.sim, rig.tank);
        rig.options.landing_marks = &rig.marks;
        const Scene scene = build_scene(rig.sim, rig.options);
        return std::pair{rig.sim.state_hash(), scene.vertices};
    };
    const auto a = run();
    const auto b = run();
    EXPECT_EQ(a.first, b.first);
    ASSERT_EQ(a.second.size(), b.second.size());
    for (std::size_t i = 0; i < a.second.size(); ++i) {
        EXPECT_EQ(a.second[i].position, b.second[i].position);
        EXPECT_EQ(a.second[i].colour, b.second[i].colour);
    }
}

} // namespace
} // namespace protekt::game
