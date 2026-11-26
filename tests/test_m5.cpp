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
/// and the shell's burst where the landing marker was (G11).
namespace protekt::game {
namespace {

using input::Click;
using input::InputFrame;
using input::MouseButton;
using sim::Side;
using sim::UnitKind;

/// The flattest and the steepest pitch the shell's burst is tested at (radians, up positive): a
/// level aim, and 1.28 rad, just under `kLookPitchLimit`.
constexpr float kLevelPitch = 0.0F;
constexpr float kSteepPitch = 1.28F;

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
    RadarActivity radar;
    ShellBursts bursts;
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
    // The weapon is picked the way a player picks it now: one right CLICK per step of the wrap
    // (2026-10-23 -- `Q` and the wheel stopped switching).
    for (int step = 0; step < 8 && rig.player.weapon() != weapon; ++step) {
        InputFrame click{};
        click.clicks.push_back(input::Click{0, 0, input::MouseButton::Right, true, false});
        rig.player.handle(click, rig.sim, rig.options);
    }
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
    const app::AdvanceResult result
        = rig.pipeline.advance(rig.sim, 1, audio, rig.flash, rig.mark, rig.radar);
    for (const sim::Sim::Shot& shot : rig.sim.shots()) {
        if (shot.shooter == rig.tank) {
            ++rig.shots;
        }
    }
    if (result.trigger_refused) {
        rig.player.trigger_refused(rig.sim);
    }
    for (const app::AdvanceResult::Touchdown& down : result.touchdowns) {
        rig.bursts.land(down.tick, down.landing.tile_x, down.landing.tile_y);
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
    EXPECT_EQ(sim::profile(UnitKind::MortarBike).cooldown_ticks, 45);
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

// --- (c) G11: THE SHELL'S BURST, in the place of the deleted landing marker -------------------

struct Drawn {
    std::vector<SceneVertex> burst;
    bool tail_untouched{false};
};

/// The burst's vertices: what appears in the scene when the touchdowns are attached.
[[nodiscard]] Drawn burst_vertices(Rig& rig) {
    rig.options.shell_bursts = nullptr;
    const Scene bare = build_scene(rig.sim, rig.options);
    rig.options.shell_bursts = &rig.bursts;
    const Scene burst = build_scene(rig.sim, rig.options);
    rig.options.shell_bursts = nullptr;
    Drawn out;
    if (burst.vertices.size() < bare.vertices.size()) {
        return out;
    }
    std::size_t first = 0;
    while (first < bare.vertices.size() && first < burst.vertices.size()
           && bare.vertices[first].position == burst.vertices[first].position
           && bare.vertices[first].colour == burst.vertices[first].colour) {
        ++first;
    }
    const std::size_t extra = burst.vertices.size() - bare.vertices.size();
    out.burst.assign(burst.vertices.begin() + static_cast<std::ptrdiff_t>(first),
                     burst.vertices.begin() + static_cast<std::ptrdiff_t>(first + extra));
    out.tail_untouched = true;
    for (std::size_t i = first; i < bare.vertices.size(); ++i) {
        out.tail_untouched
            = out.tail_untouched && bare.vertices[i].position == burst.vertices[i + extra].position;
    }
    return out;
}

/// Fires, and steps until the sim's own touchdown: the sim is left on the tick it came down.
void fire_until_landed(Rig& rig) {
    frame(rig, true);
    for (int tick = 0; tick < 300 && rig.landings == 0; ++tick) {
        frame(rig, false);
    }
}

/// The landing tile's centre, in metres, and the ground there.
struct Spot {
    float x{};
    float z{};
    float ground{};
};

[[nodiscard]] Spot landing_spot(const Rig& rig) {
    const float tx = static_cast<float>(rig.last_landing.tile_x) + 0.5F;
    const float ty = static_cast<float>(rig.last_landing.tile_y) + 0.5F;
    return Spot{tx * kMetresPerTile, ty * kMetresPerTile, ground_metres(kSeed, tx, ty)};
}

void expect_burst_at(Rig& rig, float expected_tiles) {
    const float start = as_float(rig.sim.find(rig.tank)->x);
    const int tank_row = rig.sim.find(rig.tank)->y.floor_int();
    fire_until_landed(rig);
    ASSERT_EQ(rig.landings, 1) << "the sim reported the touchdown";
    EXPECT_FALSE(rig.last_landing.hit);
    EXPECT_EQ(rig.last_landing.tile_y, tank_row);
    EXPECT_NEAR(static_cast<float>(rig.last_landing.tile_x) + 0.5F - start, expected_tiles, 1.1F);

    const sim::Tick landed = rig.last_landing_tick;
    ASSERT_EQ(rig.sim.tick(), landed) << "read the tick it came down on";
    const std::vector<ShellBursts::Burst> showing = rig.bursts.active(landed);
    ASSERT_EQ(showing.size(), 1U);
    EXPECT_EQ(showing[0].tile_x, rig.last_landing.tile_x);
    EXPECT_EQ(showing[0].tile_y, rig.last_landing.tile_y);
    EXPECT_EQ(showing[0].age, 0);

    // In the scene, on the touchdown tile.
    const Drawn drawn = burst_vertices(rig);
    ASSERT_FALSE(drawn.burst.empty());
    EXPECT_TRUE(drawn.tail_untouched) << "the reticle is still the tail";
    const Spot spot = landing_spot(rig);
    for (const SceneVertex& v : drawn.burst) {
        EXPECT_LE(std::abs(v.position[0] - spot.x), 3.0F);
        EXPECT_LE(std::abs(v.position[2] - spot.z), 3.0F);
        EXPECT_GE(v.position[1], spot.ground - 0.5F);
        EXPECT_LE(v.position[1], spot.ground + 3.0F);
    }

    // Seen from the level view a third of a second on, the cloud climbing: what the marker's 4 m
    // height was for -- a shell that lands close is still in the frame.
    for (int tick = 0; tick < 10; ++tick) {
        frame(rig, false);
    }
    look(rig, 0.0F);
    std::size_t inside = 0;
    const Drawn rising = burst_vertices(rig);
    for (const SceneVertex& v : rising.burst) {
        const Projected p
            = project(rig.options.camera, kExtent, v.position[0], v.position[1], v.position[2]);
        ASSERT_GT(p.depth, 0.0F);
        if (p.x >= 0.0F && p.x < static_cast<float>(kExtent.width) && p.y >= 0.0F
            && p.y < static_cast<float>(kExtent.height)) {
            ++inside;
        }
    }
    EXPECT_GE(inside * 3, rising.burst.size()) << "at least a third of it is in the frame";

    // Gone after its lifetime, from the record and from the scene.
    EXPECT_EQ(rig.bursts.active(landed + kShellBurstTicks - 1).size(), 1U);
    EXPECT_TRUE(rig.bursts.active(landed + kShellBurstTicks).empty());
    while (rig.sim.tick() < landed + kShellBurstTicks) {
        frame(rig, false);
    }
    EXPECT_TRUE(burst_vertices(rig).burst.empty()) << "and out of the scene";
}

TEST(ShellBurst, TheLowestPitchBurstsInFrontAndClearsItself) {
    Rig rig;
    make_rig(rig, Weapon::Mortar, kLevelPitch);
    expect_burst_at(rig, mortar_landing_tiles(kLevelPitch));
}

TEST(ShellBurst, TheHighestPitchBurstsEightPointOneTilesOut) {
    // The fixed muzzle speed (2026-10-31): the steepest pitch is the HIGH, SHORT lob, 8.06 tiles.
    Rig rig;
    make_rig(rig, Weapon::Mortar, kSteepPitch);
    expect_burst_at(rig, mortar_landing_tiles(kSteepPitch));
    EXPECT_NEAR(mortar_landing_tiles(kSteepPitch), 8.1F, 0.1F);
}

TEST(ShellBurst, ItLastsASecondAndAHalf) {
    // G11: a puff, not an object. The marker held for 120 + 20 ticks; the burst is 45, and every
    // layer of it is over by then.
    EXPECT_EQ(kShellBurstTicks, 45);
    EXPECT_LE(kShellFlashTicks, kShellBurstTicks);
    EXPECT_LE(kShellSparkTicks, kShellBurstTicks);
}

TEST(ShellBurst, OnlyThePlayersOwnLobMakesOne) {
    Rig rig;
    make_rig(rig, Weapon::Gun, 0.0F);
    frame(rig, true); // a straight bolt into the ground, not a lob
    for (int tick = 0; tick < 200; ++tick) {
        frame(rig, false);
    }
    EXPECT_EQ(rig.landings, 0);
    EXPECT_TRUE(rig.bursts.active(rig.sim.tick()).empty());
}

/// Magenta-dominant: red and blue both well over green. The mark was {1.0, 0.1, 0.85}; the fog
/// only pulls a colour toward the sky, so a fogged mark still reads this way. Sparks are warm
/// (blue under green) and the cloud is grey-brown (the three close), so neither does. The WORLD's
/// vertices only: the HUD's shotgun label is a lilac {0.90, 0.45, 0.80} that reads this way too.
[[nodiscard]] std::size_t world_magenta(const Scene& scene) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < scene.world_vertex_count && i < scene.vertices.size(); ++i) {
        const SceneVertex& v = scene.vertices[i];
        count += v.colour[0] > v.colour[1] + 0.3F && v.colour[2] > v.colour[1] + 0.3F ? 1U : 0U;
    }
    return count;
}

TEST(ShellBurst, ALandingDrawsNoMarkButABurstOfSparksAndACloudThatClearsItself) {
    Rig rig;
    make_rig(rig, Weapon::Mortar, kLevelPitch);
    fire_until_landed(rig);
    ASSERT_EQ(rig.landings, 1) << "the sim's own landing list reported the touchdown";
    const sim::Tick landed = rig.last_landing_tick;
    ASSERT_EQ(rig.sim.tick(), landed);
    const Spot spot = landing_spot(rig);

    // The frame of the hit: no mark anywhere in the world, and a burst at the touchdown tile.
    rig.options.shell_bursts = &rig.bursts;
    EXPECT_EQ(world_magenta(build_scene(rig.sim, rig.options)), 0U) << "no X, not even fogged";
    const Drawn burst = burst_vertices(rig);
    ASSERT_FALSE(burst.burst.empty()) << "a landing draws something";
    EXPECT_TRUE(burst.tail_untouched);
    std::size_t sparks = 0;
    for (const SceneVertex& v : burst.burst) {
        EXPECT_LE(std::abs(v.position[0] - spot.x), 4.0F) << "at the touchdown tile";
        EXPECT_LE(std::abs(v.position[2] - spot.z), 4.0F);
        EXPECT_GE(v.position[1], spot.ground - 0.5F);
        EXPECT_LE(v.position[1], spot.ground + 8.0F);
        sparks += v.colour == kSparkHotColour ? 1U : 0U; // a spark is unfogged and white-hot at 0
    }
    EXPECT_GE(sparks, 8U * kSparkVertices) << "the sparks are the wreck's and the weld's own";

    // Later: the cloud rises. Its top is read after the flash (gone at `kShellFlashTicks`, and at
    // the hit it stands taller than the young cloud) and without the sparks (green 0.8 and over;
    // the dust is under 0.5), at +10 and again at +30.
    const auto cloud_top = [&] {
        float top = -1e9F;
        for (const SceneVertex& v : burst_vertices(rig).burst) {
            if (v.colour[1] < 0.7F) {
                top = std::max(top, v.position[1]);
            }
        }
        return top;
    };
    while (rig.sim.tick() < landed + 10) {
        frame(rig, false);
    }
    const float top_early = cloud_top();
    EXPECT_GT(top_early, spot.ground) << "the cloud is drawn";
    while (rig.sim.tick() < landed + 30) {
        frame(rig, false);
    }
    EXPECT_GT(cloud_top(), top_early + 0.5F) << "the cloud rises";

    // It clears itself up: still there on its last tick, gone the tick after.
    while (rig.sim.tick() < landed + kShellBurstTicks - 1) {
        frame(rig, false);
    }
    EXPECT_FALSE(burst_vertices(rig).burst.empty()) << "its last tick";
    frame(rig, false);
    EXPECT_TRUE(burst_vertices(rig).burst.empty()) << "a puff, not an object";
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
        rig.options.shell_bursts = &rig.bursts;
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
