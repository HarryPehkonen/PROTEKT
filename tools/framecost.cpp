// N6: WHERE THE FRAME'S TIME GOES. "I played for a bit, but the framerate went down a lot" -- and
// an outside analysis blamed the per-frame scene rebuild and `smooth_vertex_normals`, with numbers
// nobody had checked. This is the project's Rule 6, measure before claiming: a HEADLESS benchmark
// in real milliseconds (`std::chrono::steady_clock`), by phase, on bases of growing size.
//
// It changes no behaviour and optimises nothing. `game::build_scene` carries an optional
// `FrameTimings*` (null reads no clock); everything else here is read from the public surface.
//
// WHAT IT MEASURES, per scenario (a seed, a composition, a camera at the base, radius 24 -- the
// app's own):
//   terrain          build_terrain                              (inside build_scene)
//   sky+shadows      the sky, the pick, the unit shadows        (inside build_scene)
//   units            every unit and building emitted            (inside build_scene)
//   world effects    projectiles, scrap, pools, beams, motes... (inside build_scene)
//   smooth normals   smooth_vertex_normals over the whole world (inside build_scene)
//   fog              both fog passes                            (inside build_scene)
//   hud              radar, bars, panels, everything after      (inside build_scene)
//   build_scene      the whole call, timed from OUTSIDE too, so the phases are checked against it
//   renderer         Renderer::render: the vertex upload, the GPU, the readback of the frame
// and a "renderer floor": the same render with an (almost) empty scene, the part of the renderer's
// cost that does not depend on the geometry.
//
// THE BUILD TYPE IS PRINTED FIRST, and a configuration without optimisation is announced loudly:
// a silent -O0 is the classic cause of exactly this complaint.
//
// RECORDED NUMBERS live in docs/FRAMECOST.md beside the scenario, seed, composition and frame
// count. Run: build-release/protekt-framecost   (a Debug build prints a warning and its numbers
// mean nothing about the game the player runs).
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "protekt/ai/scripted.hpp"
#include "protekt/ai/utility.hpp"
#include "protekt/game/atlas.hpp"
#include "protekt/game/building_cache.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/terrain_cache.hpp"
#include "protekt/sim/commander.hpp"
#include "protekt/sim/observer.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#if defined(PROTEKT_HAVE_VULKAN)
#include "protekt/render/renderer.hpp"
#endif

#ifndef PROTEKT_BUILD_TYPE
#define PROTEKT_BUILD_TYPE "unknown"
#endif
#ifndef PROTEKT_BUILD_FLAGS
#define PROTEKT_BUILD_FLAGS "unknown"
#endif

namespace {

using protekt::game::FrameTimings;
using protekt::sim::Group;
using protekt::sim::UnitKind;
using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kSeed = 42;
constexpr int kWarmup = 5;
constexpr int kFrames = 60;
constexpr int kRadius = 24; ///< the app's own terrain radius, in tiles

struct Base {
    std::string name;
    std::string composition; ///< for the record
    protekt::sim::ScenarioData data;
};

/// `sim::StepTimings` is integer nanoseconds (the sim has no float); the tool's tables are in ms.
[[nodiscard]] double ns_to_ms(std::int64_t ns) { return static_cast<double>(ns) / 1.0e6; }

[[nodiscard]] double since_ms(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

/// The mean of a field over the sampled frames.
template <class F> [[nodiscard]] double mean_of(const std::vector<FrameTimings>& v, F field) {
    double sum = 0.0;
    for (const FrameTimings& t : v) {
        sum += field(t);
    }
    return sum / static_cast<double>(v.size());
}

[[nodiscard]] double median(std::vector<double> v) {
    std::ranges::sort(v);
    return v[v.size() / 2U];
}

/// Buildings are four times as wide as they were, so the wishes are spread: the placement rule
/// nudges each to the nearest free passable tile, and everything stays inside the radius-24 mesh.
[[nodiscard]] std::vector<Base> bases() {
    std::vector<Base> out;

    // The floor: no base at all, so what a frame costs over bare ground (terrain, sky, HUD).
    Base bare;
    bare.name = "0: bare ground";
    bare.composition = "1 Tank and nothing else";
    bare.data.groups = {{UnitKind::Tank, 1, 0, 0}};
    out.push_back(bare);

    Base small;
    small.name = "A: default skirmish";
    small.composition = "3 Tank, HoverTank, Factory, 2 Turret, MortarBike, RocketTank, Scavenger, "
                        "Recycler (undeployed)";
    small.data = protekt::sim::default_data();
    out.push_back(small);

    // HIS CASE: a Power plant, a Gun Tower, a factory, a service bay and the rest, standing, plus a
    // deployed recycler, and the tanks.
    const std::vector<Group> core = {
        {UnitKind::Factory, 1, 0, 0},       {UnitKind::DeployedRecycler, 1, -10, -6},
        {UnitKind::Power, 1, 6, -9},        {UnitKind::GunTower, 1, 10, 4},
        {UnitKind::ServiceBay, 1, -6, 8},   {UnitKind::RelayBunker, 1, 12, -4},
        {UnitKind::Armory, 1, -12, 4},      {UnitKind::TrainingCentre, 1, 4, 11},
        {UnitKind::TechCentre, 1, -4, -12}, {UnitKind::BomberBay, 1, 13, 11},
        {UnitKind::Tank, 3, 8, 2},          {UnitKind::HoverTank, 1, 5, 5},
        {UnitKind::MortarBike, 1, -4, -1},  {UnitKind::RocketTank, 1, -1, -4},
        {UnitKind::Scavenger, 1, 1, -3},
    };
    Base big;
    big.name = "B: his base";
    big.composition
        = "Factory, DeployedRecycler, Power, GunTower, ServiceBay, RelayBunker, Armory, "
          "TrainingCentre, TechCentre, BomberBay (10 buildings) + 3 Tank, HoverTank, "
          "MortarBike, RocketTank, Scavenger";
    big.data.groups = core;
    out.push_back(big);

    // The same base twice over: more of every building, at wider offsets, to see the GROWTH.
    Base huge;
    huge.name = "C: his base x2";
    huge.composition = "B, plus a second Power, GunTower, ServiceBay, RelayBunker, Armory, "
                       "TrainingCentre, TechCentre, BomberBay, Factory, Turret x2 (19 buildings) "
                       "+ the same units";
    huge.data.groups = core;
    const std::vector<Group> more = {
        {UnitKind::Power, 1, -14, -14},     {UnitKind::GunTower, 1, 16, -12},
        {UnitKind::ServiceBay, 1, -16, 12}, {UnitKind::RelayBunker, 1, 16, 8},
        {UnitKind::Armory, 1, 0, 16},       {UnitKind::TrainingCentre, 1, -16, -2},
        {UnitKind::TechCentre, 1, 0, -17},  {UnitKind::BomberBay, 1, 8, -15},
        {UnitKind::Factory, 1, 17, 0},      {UnitKind::Turret, 2, -8, 15},
    };
    huge.data.groups.insert(huge.data.groups.end(), more.begin(), more.end());
    out.push_back(huge);
    return out;
}

/// N8: `use_building_cache` is whether the standing buildings' meshes come from `BuildingMeshCache`
/// (the app's own setting); `protekt-framecost <what> --no-building-cache` measures the old path,
/// same binary.
void measure(const Base& base, protekt::render::Renderer* renderer, bool use_building_cache) {
    using namespace protekt;
    const sim::Sim sim = sim::Sim::create(sim::skirmish(kSeed, base.data));
    sim::UnitId viewer{};
    int units_alive = 0;
    int buildings = 0;
    float cx = 0.0F;
    float cy = 0.0F;
    for (const sim::Unit& unit : sim.units()) {
        if (unit.alive && unit.side == sim::Side::Player) {
            if (viewer == sim::UnitId{}) {
                viewer = unit.id;
            }
            ++units_alive;
            buildings += sim::has_footprint(unit.kind) ? 1 : 0;
            cx += static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne);
            cy += static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne);
        }
    }
    cx /= static_cast<float>(units_alive);
    cy /= static_cast<float>(units_alive);

    game::TerrainCache cache{kSeed};
    FrameTimings timings{};
    game::SceneOptions options{};
    options.seed = kSeed;
    options.extent = game::Extent{1280, 720};
    options.radius_tiles = kRadius;
    options.viewer_unit = viewer;
    options.camera
        = game::Camera{cx * game::kMetresPerTile, 8.0F, cy * game::kMetresPerTile, 0.0F, 0.0F};
    options.terrain = &cache;
    const game::BuildingMeshCache building_cache;
    options.buildings = use_building_cache ? &building_cache : nullptr;
    options.timings = &timings;

    std::cout << std::format("\n== {}\n   composition: {}\n   seed {}, {} player units standing "
                             "({} of them buildings), radius {} tiles, 1280x720\n",
                             base.name, base.composition, kSeed, units_alive, buildings, kRadius);

    // The terrain build ALONE, no terrain cache (the app builds the cache once per seed; the
    // frame's own terrain pass is the cached one measured below under "terrain").
    const auto cold0 = Clock::now();
    const game::TerrainCache cold{kSeed};
    const double cache_build_ms = since_ms(cold0);
    (void)cold;

    for (int i = 0; i < kWarmup; ++i) {
        (void)game::build_scene(sim, options);
    }
    std::vector<FrameTimings> samples;
    std::vector<double> outside;
    game::Scene scene;
    for (int i = 0; i < kFrames; ++i) {
        const auto t0 = Clock::now();
        scene = game::build_scene(sim, options);
        outside.push_back(since_ms(t0));
        samples.push_back(timings);
    }
    const auto field = [&](double FrameTimings::* member) {
        return mean_of(samples, [&](const FrameTimings& t) { return t.*member; });
    };
    const double terrain = field(&FrameTimings::terrain_ms);
    const double shadows = field(&FrameTimings::sky_and_shadows_ms);
    const double units = field(&FrameTimings::units_ms);
    const double effects = field(&FrameTimings::world_effects_ms);
    const double smooth = field(&FrameTimings::smooth_ms);
    const double fog = field(&FrameTimings::fog_ms);
    const double hud = field(&FrameTimings::hud_ms);
    const double total = field(&FrameTimings::total_ms);
    const double sum = terrain + shadows + units + effects + smooth + fog + hud;
    double outer_mean = 0.0;
    for (const double v : outside) {
        outer_mean += v / static_cast<double>(outside.size());
    }

    const auto row = [&](const char* label, double ms) {
        std::cout << std::format("   {:<22} {:8.3f} ms  {:5.1f}% of build_scene\n", label, ms,
                                 100.0 * ms / total);
    };
    std::cout << std::format("   -- game::build_scene, mean of {} frames after {} warm-up --\n",
                             kFrames, kWarmup);
    row("terrain", terrain);
    row("sky + shadows", shadows);
    row("units + buildings", units);
    row("world effects", effects);
    row("smooth_vertex_normals", smooth);
    row("fog (both passes)", fog);
    row("hud", hud);
    std::cout << std::format("   {:<22} {:8.3f} ms  (inner total)\n", "sum of the phases", sum);
    std::cout << std::format("   {:<22} {:8.3f} ms  (timed inside)\n", "build_scene", total);
    std::cout << std::format("   {:<22} {:8.3f} ms  (timed outside; median {:.3f})\n",
                             "build_scene", outer_mean, median(outside));
    std::cout << std::format("   vertices: {} total, {} world, {} terrain; terrain cache built "
                             "once: {:.3f} ms\n",
                             scene.vertices.size(), scene.world_vertex_count,
                             samples.back().terrain_vertices, cache_build_ms);
    std::cout << std::format("   mesh cache {}: {} buildings + {} vehicles drawn from it "
                             "({} vertices), smooth_vertex_normals walked {} of {} vertices\n",
                             use_building_cache ? "ON" : "off", samples.back().cached_buildings,
                             samples.back().cached_vehicles, samples.back().cached_vertices,
                             samples.back().smoothed_vertices, scene.vertices.size());

#if defined(PROTEKT_HAVE_VULKAN)
    if (renderer != nullptr) {
        const render::FrameRequest request{
            .extent = game::Extent{1280, 720}, .seed = kSeed, .tick = 0, .scene = &scene};
        render::FrameStats stats{};
        render::FrameStats floor_stats{};
        for (int i = 0; i < kWarmup; ++i) {
            (void)renderer->render(request, stats);
        }
        std::vector<double> render_ms;
        for (int i = 0; i < kFrames; ++i) {
            const auto t0 = Clock::now();
            (void)renderer->render(request, stats);
            render_ms.push_back(since_ms(t0));
        }
        game::Scene floor_scene;
        floor_scene.vertices.assign(3, game::SceneVertex{});
        floor_scene.view_projection = scene.view_projection;
        const render::FrameRequest floor_request{
            .extent = game::Extent{1280, 720}, .seed = kSeed, .tick = 0, .scene = &floor_scene};
        for (int i = 0; i < kWarmup; ++i) {
            (void)renderer->render(floor_request, floor_stats);
        }
        std::vector<double> floor_ms;
        for (int i = 0; i < kFrames; ++i) {
            const auto t0 = Clock::now();
            (void)renderer->render(floor_request, floor_stats);
            floor_ms.push_back(since_ms(t0));
        }
        double r_mean = 0.0;
        for (const double v : render_ms) {
            r_mean += v / static_cast<double>(render_ms.size());
        }
        double f_mean = 0.0;
        for (const double v : floor_ms) {
            f_mean += v / static_cast<double>(floor_ms.size());
        }
        std::cout << std::format("   -- renderer ({}), mean of {} frames after {} warm-up --\n",
                                 renderer->device_name(), kFrames, kWarmup);
        std::cout << std::format("   {:<22} {:8.3f} ms  (median {:.3f}; {} triangles submitted)\n",
                                 "Renderer::render", r_mean, median(render_ms),
                                 scene.vertices.size() / 3U);
        std::cout << std::format("   {:<22} {:8.3f} ms  (one triangle: upload/GPU/readback "
                                 "that does not scale with geometry)\n",
                                 "renderer floor", f_mean);
        std::cout << std::format("   whole frame, CPU build + render: {:.3f} ms  =  {:.1f} fps\n",
                                 outer_mean + r_mean, 1000.0 / (outer_mean + r_mean));
    }
#else
    (void)renderer;
    std::cout << "   (no renderer in this build: build_scene only)\n";
#endif
}

// ---------------------------------------------------------------------------------------------
// N7: THE BATTLE. His bar: "battles in the middle of a compound with dozens of vehicles, tons of
// mortars and bolts flying at the same time". Everything below goes through the REAL sim: the
// projectiles are the sim's own, fired by its own guns, advanced by `Sim::step`.
//
// THE FIGHT: both sides are built up (buildings standing, vehicles, mortar bikes). The ENEMY is
// the app's own opponent (`ScriptedCommander`: every combatant attacks the nearest enemy), so it
// marches on the player's compound; the PLAYER's side is held by the utility AI (`UtilityCommander`
// -- what the human's units would be doing while they are not driven). So the fight happens
// INSIDE the player's compound. The sim runs, un-timed, until the sides are in contact; then the
// measured frames start, and each is what one frame of the game is:
//     observe + think (both sides)  ->  Sim::step  ->  build_scene  ->  Renderer::render
// ---------------------------------------------------------------------------------------------

struct Battle {
    std::string name;
    std::string composition; ///< for the record
    protekt::sim::ScenarioData data;
};

constexpr int kBattleWarmup = 20;  ///< measured-style frames not counted, once in contact
constexpr int kBattleFrames = 200; ///< counted frames (30 Hz: 6.7 s of game time)
constexpr int kMaxPreRoll = 6000;  ///< ticks to wait for contact before giving up

/// The compound: the same ten buildings as N6's scenario B, plus two turrets, per side.
[[nodiscard]] std::vector<Group> compound() {
    return {
        {UnitKind::Factory, 1, 0, 0},       {UnitKind::DeployedRecycler, 1, -10, -6},
        {UnitKind::Power, 1, 6, -9},        {UnitKind::GunTower, 1, 10, 4},
        {UnitKind::ServiceBay, 1, -6, 8},   {UnitKind::RelayBunker, 1, 12, -4},
        {UnitKind::Armory, 1, -12, 4},      {UnitKind::TrainingCentre, 1, 4, 11},
        {UnitKind::TechCentre, 1, -4, -12}, {UnitKind::BomberBay, 1, 13, 11},
        {UnitKind::Turret, 1, 8, 8},        {UnitKind::Turret, 1, 12, 0},
    };
}

[[nodiscard]] Battle make_battle(const char* name, int tanks, int hovers, int mortars,
                                 int rockets) {
    Battle b;
    b.name = name;
    b.data.groups = compound();
    b.data.groups.push_back({UnitKind::Tank, tanks, 8, 2});
    b.data.groups.push_back({UnitKind::HoverTank, hovers, 5, 5});
    b.data.groups.push_back({UnitKind::MortarBike, mortars, -4, -1});
    b.data.groups.push_back({UnitKind::RocketTank, rockets, -1, -4});
    b.composition
        = std::format("PER SIDE: 10 buildings (Factory, DeployedRecycler, Power, GunTower, "
                      "ServiceBay, RelayBunker, Armory, TrainingCentre, TechCentre, "
                      "BomberBay) + 2 Turret + {} Tank, {} HoverTank, {} MortarBike, "
                      "{} RocketTank ({} vehicles); enemy = ScriptedCommander, player = "
                      "UtilityCommander",
                      tanks, hovers, mortars, rockets, tanks + hovers + mortars + rockets);
    return b;
}

[[nodiscard]] std::vector<Battle> battles() {
    std::vector<Battle> out;
    out.push_back(make_battle("S: skirmish", 5, 2, 3, 2));
    out.push_back(make_battle("M: battle", 12, 4, 6, 4));
    out.push_back(make_battle("H: heavy battle", 24, 8, 10, 6));
    return out;
}

struct Stat {
    double sum{};
    double max{};
    double min{1e300};
    int n{};
    void add(double v) {
        sum += v;
        max = std::max(max, v);
        min = std::min(min, v);
        ++n;
    }
    [[nodiscard]] double mean() const { return n > 0 ? sum / n : 0.0; }
};

void measure_battle(const Battle& battle, protekt::render::Renderer* renderer,
                    bool use_building_cache) {
    using namespace protekt;
    sim::Sim sim = sim::Sim::create(sim::skirmish(kSeed, battle.data));
    ai::UtilityCommander player_ai;
    ai::ScriptedCommander enemy_ai;
    std::vector<sim::Order> orders;

    int vehicles_start = 0;
    int buildings_start = 0;
    for (const sim::Unit& unit : sim.units()) {
        if (unit.alive && unit.side == sim::Side::Player) {
            (sim::has_footprint(unit.kind) ? buildings_start : vehicles_start) += 1;
        }
    }

    // One frame of the game's CPU side, with the phases timed. Returns nothing: the caller reads
    // the accumulators.
    struct Frame {
        double observe_ms{}, think_ms{};
    };
    const auto commanders = [&]() {
        Frame f{};
        orders.clear();
        const auto t0 = Clock::now();
        const sim::Observation own = sim::observe(sim, sim::Side::Player);
        const sim::Observation foe = sim::observe(sim, sim::Side::Enemy);
        const auto t1 = Clock::now();
        player_ai.think(own, orders);
        enemy_ai.think(foe, orders);
        const auto t2 = Clock::now();
        f.observe_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        f.think_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
        for (const sim::Order& order : orders) {
            sim.enqueue(order);
        }
        return f;
    };

    // PRE-ROLL, un-timed: until the two sides are in contact (a player unit has been hit).
    int pre_roll = 0;
    bool contact = false;
    while (pre_roll < kMaxPreRoll && !contact) {
        (void)commanders();
        sim.step();
        ++pre_roll;
        contact = !sim.impacts().empty();
    }

    game::TerrainCache cache{kSeed};
    FrameTimings scene_t{};
    game::SceneOptions options{};
    options.seed = kSeed;
    options.extent = game::Extent{1280, 720};
    options.radius_tiles = kRadius;
    options.terrain = &cache;
    const game::BuildingMeshCache building_cache;
    options.buildings = use_building_cache ? &building_cache : nullptr;
    options.timings = &scene_t;

    Stat observe_s, think_s, ai_s, step_s, orders_s, move_s, steer_s, bolts_s, hit_s, fire_s,
        rest_s, line_s, probe_s, line_n, probe_n, refused_n, blocked_n, scene_s, render_s, frame_s;
    Stat proj_s, shell_s, units_s, vert_s, impacts_s, shots_s;
    Stat units_scene, effects_scene, smooth_scene, hud_scene, terrain_scene;
    double inner_gap = 0.0; // |sum of step phases - step total|, worst frame
    double scene_gap = 0.0;
    int frames_run = 0;
    game::Scene scene;
    const int total_frames = kBattleWarmup + kBattleFrames;
    for (int i = 0; i < total_frames; ++i) {
        const bool counted = i >= kBattleWarmup;
        const auto f0 = Clock::now();
        const Frame fr = commanders();
        sim::StepTimings st{};
        sim.step(&st);

        // The camera: the middle of the player's compound (the app's camera follows the driven
        // unit, which is in the compound).
        float cx = 0.0F;
        float cy = 0.0F;
        int n = 0;
        sim::UnitId viewer{};
        std::size_t shells = 0;
        for (const sim::Unit& unit : sim.units()) {
            if (unit.alive && unit.side == sim::Side::Player) {
                if (viewer == sim::UnitId{}) {
                    viewer = unit.id;
                }
                cx += static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne);
                cy += static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne);
                ++n;
            }
        }
        for (const sim::Projectile& p : sim.projectiles()) {
            shells += p.gravity.raw > 0 ? 1U : 0U;
        }
        if (n > 0) {
            cx /= static_cast<float>(n);
            cy /= static_cast<float>(n);
        }
        options.viewer_unit = viewer;
        options.camera
            = game::Camera{cx * game::kMetresPerTile, 8.0F, cy * game::kMetresPerTile, 0.0F, 0.0F};
        const auto s0 = Clock::now();
        scene = game::build_scene(sim, options);
        const double scene_out = since_ms(s0);

        double render_ms = 0.0;
#if defined(PROTEKT_HAVE_VULKAN)
        if (renderer != nullptr) {
            const render::FrameRequest request{
                .extent = game::Extent{1280, 720}, .seed = kSeed, .tick = 0, .scene = &scene};
            render::FrameStats stats{};
            const auto r0 = Clock::now();
            (void)renderer->render(request, stats);
            render_ms = since_ms(r0);
        }
#else
        (void)renderer;
#endif
        const double frame_ms = since_ms(f0);
        ++frames_run;
        if (!counted) {
            continue;
        }
        observe_s.add(fr.observe_ms);
        think_s.add(fr.think_ms);
        ai_s.add(fr.observe_ms + fr.think_ms);
        step_s.add(ns_to_ms(st.total_ns));
        orders_s.add(ns_to_ms(st.orders_ns));
        move_s.add(ns_to_ms(st.movement_ns));
        steer_s.add(ns_to_ms(st.steer_ns));
        bolts_s.add(ns_to_ms(st.bolts_ns));
        hit_s.add(ns_to_ms(st.hit_tests_ns));
        fire_s.add(ns_to_ms(st.fire_ns));
        line_s.add(ns_to_ms(st.line_ns));
        probe_s.add(ns_to_ms(st.probe_ns));
        line_n.add(static_cast<double>(st.line_calls));
        probe_n.add(static_cast<double>(st.probe_calls));
        refused_n.add(static_cast<double>(st.probe_refused));
        blocked_n.add(static_cast<double>(st.line_blocked));
        rest_s.add(ns_to_ms(st.rest_ns));
        scene_s.add(scene_out);
        render_s.add(render_ms);
        frame_s.add(frame_ms);
        proj_s.add(static_cast<double>(st.projectiles_in));
        shell_s.add(static_cast<double>(shells));
        units_s.add(static_cast<double>(st.units_alive));
        vert_s.add(static_cast<double>(scene.vertices.size()));
        impacts_s.add(static_cast<double>(sim.impacts().size()));
        shots_s.add(static_cast<double>(sim.shots().size()));
        units_scene.add(scene_t.units_ms);
        effects_scene.add(scene_t.world_effects_ms);
        smooth_scene.add(scene_t.smooth_ms);
        hud_scene.add(scene_t.hud_ms);
        terrain_scene.add(scene_t.terrain_ms);
        inner_gap = std::max(inner_gap, std::abs(ns_to_ms(st.orders_ns) + ns_to_ms(st.movement_ns)
                                                 + ns_to_ms(st.bolts_ns) + ns_to_ms(st.fire_ns)
                                                 + ns_to_ms(st.rest_ns) - ns_to_ms(st.total_ns)));
        scene_gap = std::max(scene_gap, std::abs(scene_t.terrain_ms + scene_t.sky_and_shadows_ms
                                                 + scene_t.units_ms + scene_t.world_effects_ms
                                                 + scene_t.smooth_ms + scene_t.fog_ms
                                                 + scene_t.hud_ms - scene_t.total_ms));
    }

    int vehicles_end = 0;
    int enemy_vehicles_end = 0;
    for (const sim::Unit& unit : sim.units()) {
        if (unit.alive && !sim::has_footprint(unit.kind)) {
            (unit.side == sim::Side::Player ? vehicles_end : enemy_vehicles_end) += 1;
        }
    }

    std::cout << std::format(
        "\n== {}\n   {}\n   seed {}, {} player vehicles + {} player buildings at the start "
        "(enemy the same); contact after {} un-timed ticks; {} frames after {} warm-up (all in "
        "contact), 1280x720, radius {}\n   alive at the end: {} player vehicles, {} enemy "
        "vehicles\n",
        battle.name, battle.composition, kSeed, vehicles_start, buildings_start, pre_roll,
        kBattleFrames, kBattleWarmup, kRadius, vehicles_end, enemy_vehicles_end);
    const auto count_row = [&](const char* label, const Stat& s) {
        std::cout << std::format("   {:<26} mean {:8.1f}   min {:6.0f}   max {:6.0f}\n", label,
                                 s.mean(), s.min, s.max);
    };
    std::cout << "   -- what is on the field, per measured frame --\n";
    count_row("living units (all, both sides)", units_s);
    count_row("projectiles in flight", proj_s);
    count_row("  of which mortar shells", shell_s);
    count_row("shots fired per tick", shots_s);
    count_row("impacts per tick", impacts_s);
    count_row("scene vertices", vert_s);
    const double cpu = ai_s.mean() + step_s.mean() + scene_s.mean();
    const auto row = [&](const char* label, const Stat& s, const char* note) {
        std::cout << std::format("   {:<26} mean {:7.3f} ms   max {:7.3f} ms   {}\n", label,
                                 s.mean(), s.max, note);
    };
    std::cout << "   -- the SIM's side of the frame (mean and worst of the counted frames) --\n";
    row("AI: observe (2 sides)", observe_s, "sim::observe, player + enemy");
    row("AI: think (2 sides)", think_s, "UtilityCommander + ScriptedCommander");
    row("AI total", ai_s, "");
    row("Sim::step", step_s, "(timed inside)");
    row("  orders", orders_s, "");
    row("  movement", move_s, "deploy, scavenge, steer, move");
    row("    of which steer", steer_s, "inside movement");
    row("  bolts (advance)", bolts_s, "advance_projectiles");
    row("    of which hit tests", hit_s, "inside bolts: units + buildings, per projectile");
    row("  fire (guns)", fire_s, "target search, line of sight, launch");
    row("    of which has_line", line_s, "inside fire: line of sight (clock per call: reads high)");
    row("    of which friend probe", probe_s,
        "inside fire: shot_hurts_friend, a whole flight each");
    row("  rest", rest_s, "lungs, repair, wrecks, store, queues");
    std::cout << std::format(
        "   per tick: {:.1f} has_line calls (max {:.0f}), {:.2f} friend probes "
        "(max {:.0f}); a probe costs {:.1f} us on average; a projectile's "
        "hit tests {:.1f} us\n",
        line_n.mean(), line_n.max, probe_n.mean(), probe_n.max,
        probe_n.sum > 0 ? 1000.0 * probe_s.sum / probe_n.sum : 0.0,
        proj_s.sum > 0 ? 1000.0 * hit_s.sum / proj_s.sum : 0.0);
    std::cout << std::format(
        "   per tick: {:.1f} has_line calls found no line; {:.1f} friend probes "
        "refused the shot; {:.2f} shots were actually fired\n",
        blocked_n.mean(), refused_n.mean(), shots_s.mean());
    std::cout << std::format("   {:<26} {:7.3f} ms   (worst frame's |sum of step phases - step "
                             "total|: {:.4f} ms)\n",
                             "sum of step phases",
                             orders_s.mean() + move_s.mean() + bolts_s.mean() + fire_s.mean()
                                 + rest_s.mean(),
                             inner_gap);
    std::cout << std::format(
        "   mesh cache {}: last frame {} buildings + {} vehicles drawn from it ({} vertices); "
        "smooth_vertex_normals walked {} of {} vertices\n",
        use_building_cache ? "ON" : "off", scene_t.cached_buildings, scene_t.cached_vehicles,
        scene_t.cached_vertices, scene_t.smoothed_vertices, scene.vertices.size());
    std::cout << "   -- the scene (game::build_scene) --\n";
    row("build_scene", scene_s, "(timed outside)");
    row("  units + buildings", units_scene, "");
    row("  world effects", effects_scene, "projectiles, scrap, pools, beams, motes");
    row("  smooth_vertex_normals", smooth_scene, "");
    row("  hud", hud_scene, "");
    row("  terrain", terrain_scene, "");
    std::cout << std::format("   (worst frame's |sum of scene phases - scene total|: {:.4f} ms)\n",
                             scene_gap);
    std::cout << std::format("   CPU per frame (AI + step + build_scene): {:.3f} ms\n", cpu);
    std::cout << std::format("   of that: AI {:.1f}%, Sim::step {:.1f}%, build_scene {:.1f}%\n",
                             100.0 * ai_s.mean() / cpu, 100.0 * step_s.mean() / cpu,
                             100.0 * scene_s.mean() / cpu);
#if defined(PROTEKT_HAVE_VULKAN)
    if (renderer != nullptr) {
        std::cout << std::format("   -- renderer ({}) --\n", renderer->device_name());
        row("Renderer::render", render_s, "");
        std::cout << std::format("   whole frame: CPU {:.3f} ms + render {:.3f} ms = {:.3f} ms "
                                 "= {:.1f} fps  (the loop's own wall clock per frame: mean "
                                 "{:.3f} ms, worst {:.3f} ms)\n",
                                 cpu, render_s.mean(), cpu + render_s.mean(),
                                 1000.0 / (cpu + render_s.mean()), frame_s.mean(), frame_s.max);
    }
#endif
    (void)frames_run;
}

// N9: THE DRIFT. A beat on the guns' target search changes WHEN a unit decides, so a fight is not
// bit-identical to the one before it. This runs each battle headless (no scene, no renderer: the
// sim and the two commanders only) for a fixed number of ticks and prints, per tick, a signature
// that does not depend on the sim's own hash (shots fired, living units, the sum of every hp), so
// the same tool run against the old and the new sim can be diffed line by line: how many shots,
// kills and frames differ, and who is left standing.
//   protekt-framecost drift [out-dir [seed...]]   (with an out-dir, writes <size><seed>.ticks)
constexpr int kDriftTicks = 3000; ///< 100 s of game time

void measure_drift(const Battle& battle, const char* out_dir, std::uint64_t seed) {
    using namespace protekt;
    sim::Sim sim = sim::Sim::create(sim::skirmish(seed, battle.data));
    ai::UtilityCommander player_ai;
    ai::ScriptedCommander enemy_ai;
    std::vector<sim::Order> orders;
    std::int64_t shots = 0;
    std::int64_t kills = 0;
    std::size_t alive_before = 0;
    for (const sim::Unit& unit : sim.units()) {
        alive_before += unit.alive ? 1U : 0U;
    }
    std::string lines;
    int first_impact = -1;
    int vehicles_gone_tick = -1;
    for (int tick = 0; tick < kDriftTicks; ++tick) {
        orders.clear();
        const sim::Observation own = sim::observe(sim, sim::Side::Player);
        const sim::Observation foe = sim::observe(sim, sim::Side::Enemy);
        player_ai.think(own, orders);
        enemy_ai.think(foe, orders);
        for (const sim::Order& order : orders) {
            sim.enqueue(order);
        }
        sim.step();
        std::size_t alive = 0;
        std::int64_t hp = 0;
        std::array<int, 2> vehicles{};
        for (const sim::Unit& unit : sim.units()) {
            if (unit.alive) {
                ++alive;
                hp += unit.hp;
                if (!sim::has_footprint(unit.kind)) {
                    ++vehicles.at(unit.side == sim::Side::Player ? 0U : 1U);
                }
            }
        }
        shots += static_cast<std::int64_t>(sim.shots().size());
        kills += static_cast<std::int64_t>(alive_before - alive);
        alive_before = alive;
        if (first_impact < 0 && !sim.impacts().empty()) {
            first_impact = tick;
        }
        if (vehicles_gone_tick < 0 && (vehicles.at(0) == 0 || vehicles.at(1) == 0)) {
            vehicles_gone_tick = tick;
        }
        lines += std::format("{} {} {} {}\n", tick, sim.shots().size(), alive, hp);
        if (tick == kDriftTicks - 1) {
            std::cout << std::format(
                "== {} seed {}\n   {} ticks: shots {}, kills {}, first impact at tick {}, alive at "
                "the end: {} player vehicles, {} enemy vehicles; a side's vehicles all gone at "
                "tick {}\n",
                battle.name, seed, kDriftTicks, shots, kills, first_impact, vehicles.at(0),
                vehicles.at(1), vehicles_gone_tick);
        }
    }
    if (out_dir != nullptr) {
        const std::string path = std::format("{}/{}{}.ticks", out_dir, battle.name.substr(0, 1),
                                             seed == kSeed ? "" : std::to_string(seed));
        std::ofstream(path) << lines;
    }
}

} // namespace

// A probe, run by hand: a throw is the report. No state to unwind.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv) {
    // `protekt-framecost [bases|battle]`: both by default.
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    const std::string_view what = args.size() > 1 ? args[1] : "all";
    std::cout << std::format("protekt-framecost: build type {}, flags: {}\n", PROTEKT_BUILD_TYPE,
                             PROTEKT_BUILD_FLAGS);
    const bool use_building_cache
        = !(args.size() > 2 && std::string_view(args[2]) == "--no-building-cache");
    std::cout << std::format("protekt-framecost: building cache {}\n",
                             use_building_cache ? "ON" : "OFF (--no-building-cache)");
    const std::string flags = PROTEKT_BUILD_FLAGS;
    const bool optimised = flags.find("-O2") != std::string::npos
                           || flags.find("-O3") != std::string::npos
                           || flags.find("-O1") != std::string::npos;
    if (!optimised) {
        std::cout
            << "\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
               "!!  THIS BUILD IS NOT OPTIMISED (no -O1/-O2/-O3 in the flags above).    !!\n"
               "!!  These numbers say nothing about the game a player runs. Use the     !!\n"
               "!!  Release build directory.                                            !!\n"
               "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n";
    }

    if (what == "drift") {
        // `drift [out-dir [seed...]]`: seed 42 (the framecost battles' own) unless others are
        // named.
        std::vector<std::uint64_t> seeds;
        for (std::size_t i = 3; i < args.size(); ++i) {
            seeds.push_back(std::stoull(args[i]));
        }
        if (seeds.empty()) {
            seeds.push_back(kSeed);
        }
        for (const std::uint64_t seed : seeds) {
            for (const Battle& battle : battles()) {
                measure_drift(battle, args.size() > 2 ? args[2] : nullptr, seed);
            }
        }
        return 0;
    }

    std::unique_ptr<protekt::render::Renderer> renderer;
#if defined(PROTEKT_HAVE_VULKAN)
    protekt::render::CreateOptions create;
    create.validation = false; // the player's frame has no validation layer
    create.extent = protekt::game::Extent{1280, 720};
    const auto atlas = protekt::game::load_game_atlas(std::cerr);
    create.atlas_rgba = atlas.pixels;
    create.atlas_side = protekt::game::kAtlasSide;
    std::string error;
    renderer = protekt::render::create(create, error);
    if (!renderer) {
        std::cout << "no renderer (" << error << "): the renderer rows are skipped\n";
    }
#endif

    if (what == "all" || what == "bases") {
        for (const Base& base : bases()) {
            measure(base, renderer.get(), use_building_cache);
        }
    }
    if (what == "all" || what == "battle") {
        for (const Battle& battle : battles()) {
            measure_battle(battle, renderer.get(), use_building_cache);
        }
    }
    return 0;
}
