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
#include "protekt/app/time_accumulator.hpp"
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

/// N12: whether the measured frames copy the finished image out of the GPU. The game does that only
/// for `--capture`, so the default here is NO (what a played frame costs); `--readback`, anywhere
/// after the scenario, measures the phase the way every frame paid for it before N12.
bool readback_wanted(const bool set = false) {
    static bool wanted = false;
    wanted = wanted || set;
    return wanted;
}

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
        const render::FrameRequest request{.extent = game::Extent{1280, 720},
                                           .seed = kSeed,
                                           .tick = 0,
                                           .scene = &scene,
                                           .readback = readback_wanted()};
        render::FrameStats stats{};
        for (int i = 0; i < kWarmup; ++i) {
            (void)renderer->render(request, stats);
        }
        std::vector<double> render_ms;
        for (int i = 0; i < kFrames; ++i) {
            const auto t0 = Clock::now();
            (void)renderer->render(request, stats);
            render_ms.push_back(since_ms(t0));
        }
        double r_mean = 0.0;
        for (const double v : render_ms) {
            r_mean += v / static_cast<double>(render_ms.size());
        }
        std::cout << std::format("   -- renderer ({}), mean of {} frames after {} warm-up --\n",
                                 renderer->device_name(), kFrames, kWarmup);
        std::cout << std::format("   {:<22} {:8.3f} ms  (median {:.3f}; {} triangles submitted)\n",
                                 "Renderer::render", r_mean, median(render_ms),
                                 scene.vertices.size() / 3U);
        std::cout
            << "   (the old one-triangle 'renderer floor' row is gone: a point cannot "
               "separate a fixed cost from a per-vertex one. See `protekt-framecost floor`.)\n";
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

/// The value at fraction `p` (0..1) of an already-SORTED sample, nearest-rank.
[[nodiscard]] double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    const auto index
        = static_cast<std::size_t>(std::lround(p * static_cast<double>(sorted.size() - 1)));
    return sorted.at(std::min(index, sorted.size() - 1));
}

/// N17: THE SPREAD OF THE FRAME THE PLAYER SEES, not of the frame this tool steps. The tool above
/// steps one tick per frame, so every measured frame carries a `Sim::step`. The game does not: PLAY
/// mode runs the sim at 30 Hz off the wall clock, so at a higher frame rate most frames build the
/// scene and nothing else, and every few frames one also pays for the tick(s) that have come due.
/// That makes the frame time a two-humped thing whose MEAN hides the hump.
///
/// This replays the measured per-frame costs through the game's own `TimeAccumulator` on a VIRTUAL
/// clock that advances by the modelled frame's own duration (CPU only: the scene build, plus AI and
/// `Sim::step` on a frame with a tick due; no GPU, no present, no vsync). The samples are used in
/// the order measured and wrapped. It is a model, labelled as one: what it shows is the shape, and
/// the absolute milliseconds are this machine's CPU.
void report_spread(const std::string& name, const std::vector<double>& tick_ms,
                   const std::vector<double>& scene_ms) {
    if (tick_ms.empty() || tick_ms.size() != scene_ms.size()) {
        return;
    }
    using namespace std::chrono;
    protekt::app::TimeAccumulator clock(nanoseconds{1'000'000'000LL / 30},
                                        nanoseconds{250'000'000LL},
                                        protekt::app::TimeAccumulator::Clock::time_point{});
    auto now = protekt::app::TimeAccumulator::Clock::time_point{};
    constexpr int kModelFrames = 4000;
    std::vector<double> all;
    std::vector<double> quiet;
    std::vector<double> busy;
    double previous = 0.0;
    int multi = 0;
    for (int i = 0; i < kModelFrames; ++i) {
        const std::size_t k = static_cast<std::size_t>(i) % tick_ms.size();
        now += duration_cast<nanoseconds>(duration<double, std::milli>(previous));
        const std::uint32_t ticks = clock.steps_at(now);
        multi += ticks > 1 ? 1 : 0;
        const double cost = scene_ms.at(k) + (static_cast<double>(ticks) * tick_ms.at(k));
        (ticks > 0 ? busy : quiet).push_back(cost);
        all.push_back(cost);
        previous = cost;
    }
    std::vector<double> sorted = all;
    std::sort(sorted.begin(), sorted.end());
    const double mean = [&] {
        double sum = 0.0;
        for (const double v : all) {
            sum += v;
        }
        return sum / static_cast<double>(all.size());
    }();
    const auto mean_of = [](const std::vector<double>& v) {
        double sum = 0.0;
        for (const double x : v) {
            sum += x;
        }
        return v.empty() ? 0.0 : sum / static_cast<double>(v.size());
    };
    std::cout << std::format(
        "   -- N17: THE SPREAD of the frame the player sees ({}): scene + (AI + step on a tick "
        "frame), the sim on the 30 Hz clock, CPU only (a MODEL: no GPU, no vsync) --\n",
        name);
    std::cout << std::format(
        "   frame ms   mean {:6.2f}   p50 {:6.2f}   p95 {:6.2f}   p99 {:6.2f}   "
        "max {:6.2f}   ({:.0f} fps at the mean)\n",
        mean, percentile(sorted, 0.50), percentile(sorted, 0.95), percentile(sorted, 0.99),
        sorted.back(), 1000.0 / mean);
    std::cout << std::format(
        "   {:.0f}% of frames run a tick ({} of {} ran more than one): a tick frame averages "
        "{:.2f} ms, a frame with no tick {:.2f} ms ({:.1f}x)\n",
        100.0 * static_cast<double>(busy.size()) / static_cast<double>(all.size()), multi,
        all.size(), mean_of(busy), mean_of(quiet),
        mean_of(quiet) > 0.0 ? mean_of(busy) / mean_of(quiet) : 0.0);
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

/// N11: what GROWING the scene's vertex list costs, measured on the frame's own vertices, in the
/// frame's own shape: appended in chunks (a mesh at a time) into an empty vector, as `build_scene`
/// does; the same into a vector reserved to the final size; the same into a vector that keeps its
/// capacity from the last frame; and one straight copy. Each is timed with the vector's destruction
/// included, because the loop frees the last frame's list every frame. Not a change to build_scene:
/// a measurement of what a change to it could save.
void emission_micro(const std::vector<protekt::game::SceneVertex>& v) {
    using protekt::game::SceneVertex;
    constexpr std::size_t kChunk = 1000;
    constexpr int kReps = 60;
    const auto fill = [&](std::vector<SceneVertex>& out) {
        for (std::size_t at = 0; at < v.size(); at += kChunk) {
            const std::size_t end = std::min(v.size(), at + kChunk);
            out.insert(out.end(), v.begin() + static_cast<std::ptrdiff_t>(at),
                       v.begin() + static_cast<std::ptrdiff_t>(end));
        }
    };
    float sink = 0.0F;
    const auto time_it = [&](auto&& body) {
        Stat st;
        for (int i = 0; i < kReps + 5; ++i) {
            const auto t0 = Clock::now();
            body();
            const double ms = since_ms(t0);
            if (i >= 5) {
                st.add(ms);
            }
        }
        return st;
    };
    const Stat grow = time_it([&] {
        std::vector<SceneVertex> out;
        fill(out);
        sink += out.back().position.at(0);
    });
    const Stat reserved = time_it([&] {
        std::vector<SceneVertex> out;
        out.reserve(v.size());
        fill(out);
        sink += out.back().position.at(0);
    });
    std::vector<SceneVertex> kept;
    kept.reserve(v.size());
    const Stat reused = time_it([&] {
        kept.clear();
        fill(kept);
        sink += kept.back().position.at(0);
    });
    const Stat whole = time_it([&] {
        std::vector<SceneVertex> out(v);
        out.front().alpha = 0.0F; // written to, so the copy is real work
        sink += out.back().position.at(0);
    });
    std::cout << std::format(
        "   -- N11: what the vertex list's growth costs ({} vertices, {:.1f} MB, appended {} at a "
        "time, destruction included) --\n"
        "   empty vector, grows by doubling (build_scene's way)  {:7.3f} ms\n"
        "   reserved to the final size first                      {:7.3f} ms\n"
        "   a vector that kept last frame's capacity (clear())    {:7.3f} ms\n"
        "   one straight copy of the whole list                   {:7.3f} ms   (sink {:.0f})\n",
        v.size(), static_cast<double>(v.size() * sizeof(SceneVertex)) / 1.0e6, kChunk, grow.mean(),
        reserved.mean(), reused.mean(), whole.mean(), static_cast<double>(sink) * 0.0);
}

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
    Stat sky_scene, fog_scene, lookup_s, template_s, copy_s, live_s, live_n, built_n, live_v,
        cap_changes;
    Stat tbody_s, tshape_s, tsmooth_s, tverts_s;
    Stat up_s, rec_s, sub_s, wait_s, rb_s, grew_n;
    std::size_t last_capacity = 0;
    double inner_gap = 0.0; // |sum of step phases - step total|, worst frame
    double scene_gap = 0.0;
    int frames_run = 0;
    std::vector<double> tick_cost_ms;  // AI + step, per counted frame (N17)
    std::vector<double> scene_cost_ms; // build_scene, per counted frame (N17)
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
        render::FrameTimes rt{};
#if defined(PROTEKT_HAVE_VULKAN)
        if (renderer != nullptr) {
            const render::FrameRequest request{.extent = game::Extent{1280, 720},
                                               .seed = kSeed,
                                               .tick = 0,
                                               .scene = &scene,
                                               .readback = readback_wanted()};
            render::FrameStats stats{};
            const auto r0 = Clock::now();
            (void)renderer->render(request, stats);
            render_ms = since_ms(r0);
            rt = stats.times;
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
        tick_cost_ms.push_back(fr.observe_ms + fr.think_ms + ns_to_ms(st.total_ns));
        scene_cost_ms.push_back(scene_out);
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
        sky_scene.add(scene_t.sky_and_shadows_ms);
        fog_scene.add(scene_t.fog_ms);
        lookup_s.add(scene_t.units_lookup_ms);
        template_s.add(scene_t.units_template_ms);
        copy_s.add(scene_t.units_copy_ms);
        live_s.add(scene_t.units_live_ms);
        live_n.add(static_cast<double>(scene_t.units_live));
        built_n.add(static_cast<double>(scene_t.units_template_built));
        live_v.add(static_cast<double>(scene_t.live_vertices));
        cap_changes.add(static_cast<double>(scene_t.vertex_capacity_changes));
        last_capacity = scene_t.vertex_capacity;
        tbody_s.add(scene_t.template_body_ms);
        tshape_s.add(scene_t.template_shape_ms);
        tsmooth_s.add(scene_t.template_smooth_ms);
        tverts_s.add(static_cast<double>(scene_t.template_vertices));
        up_s.add(rt.upload_ms);
        rec_s.add(rt.record_ms);
        sub_s.add(rt.submit_ms);
        wait_s.add(rt.wait_ms);
        rb_s.add(rt.readback_ms);
        grew_n.add(rt.vertex_buffer_grew ? 1.0 : 0.0);
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
    row("  sky + shadows", sky_scene, "");
    row("  fog (both passes)", fog_scene, "");
    std::cout << std::format("   (worst frame's |sum of scene phases - scene total|: {:.4f} ms)\n",
                             scene_gap);
    // N11: the EMISSION, split. Inside "units + buildings".
    const double unit_parts = lookup_s.mean() + template_s.mean() + copy_s.mean() + live_s.mean();
    std::cout << "   -- N11: the emission, inside 'units + buildings' (a clock read per unit; "
                 "the sub-clocks cost, see the gap row) --\n";
    row("  cache key + find", lookup_s, "per unit: build the key, walk the std::map");
    row("  template build (miss)", template_s, "a miss: build the mesh, smooth it alone, insert");
    row("    of which builder", tbody_s, "build_unit_body / build_building_model");
    row("    of which scale + tilt", tshape_s, "scale_model, tilt_to_ground");
    row("    of which smooth alone", tsmooth_s,
        "smooth_vertex_normals on the miss: MOVED here by the cache, not removed");
    row(
        "    template insert etc.",
        [&] {
            Stat r;
            r.add(template_s.mean() - tbody_s.mean() - tshape_s.mean() - tsmooth_s.mean());
            return r;
        }(),
        "std::map insert, mesh move, the key");
    row("  copy into scene", copy_s, "vector::insert of the cached mesh (incl. any growth)");
    row("  live path", live_s, "uncached units: build, scale, cut, tilt");
    row(
        "  dispatch (the rest)",
        [&] {
            Stat r;
            r.add(units_scene.mean() - unit_parts);
            return r;
        }(),
        "units_ms less the four above: visibility, selection search, headings, gun_yaw");
    std::cout << std::format(
        "   per frame: {:.1f} cache misses (max {:.0f}), {:.1f} units on the live path "
        "({:.0f} vertices); scene vertex list: capacity {} ({:.1f} MB) for {} vertices, "
        "capacity changes seen {:.1f} per frame (lower bound)\n",
        built_n.mean(), built_n.max, live_n.mean(), live_v.mean(), last_capacity,
        static_cast<double>(last_capacity) * sizeof(game::SceneVertex) / 1.0e6,
        scene.vertices.size(), cap_changes.mean());
    std::cout << std::format("   misses build {:.0f} vertices per frame on average\n",
                             tverts_s.mean());
    std::cout << std::format(
        "   NOTE: build_scene returns a fresh Scene each frame and the loop move-assigns it over "
        "the last: the vertex list starts at capacity 0 EVERY frame; no reserve exists.\n");
    emission_micro(scene.vertices);
    std::cout << std::format("   CPU per frame (AI + step + build_scene): {:.3f} ms\n", cpu);
    std::cout << std::format("   of that: AI {:.1f}%, Sim::step {:.1f}%, build_scene {:.1f}%\n",
                             100.0 * ai_s.mean() / cpu, 100.0 * step_s.mean() / cpu,
                             100.0 * scene_s.mean() / cpu);
    report_spread(battle.name, tick_cost_ms, scene_cost_ms);
#if defined(PROTEKT_HAVE_VULKAN)
    if (renderer != nullptr) {
        std::cout << std::format("   -- renderer ({}) --\n", renderer->device_name());
        row("Renderer::render", render_s, "");
        row("  upload (memcpy)", up_s, "vertex buffer grow check + memcpy into mapped memory");
        row("  record", rec_s, "command buffer, host side");
        row("  submit", sub_s, "vkQueueSubmit");
        row("  wait (GPU)", wait_s, "vkWaitForFences: the GPU runs the frame + driver latency");
        row("  readback", rb_s, "allocate + copy 1280x720x4 out of the mapped buffer");
        std::cout << std::format(
            "   sum of the render phases {:.3f} ms (mean total {:.3f}); vertex buffer grew in "
            "{:.0f} of the counted frames; no present here (headless)\n",
            up_s.mean() + rec_s.mean() + sub_s.mean() + wait_s.mean() + rb_s.mean(),
            render_s.mean(), grew_n.sum);
        std::cout << std::format("   whole frame: CPU {:.3f} ms + render {:.3f} ms = {:.3f} ms "
                                 "= {:.1f} fps  (the loop's own wall clock per frame: mean "
                                 "{:.3f} ms, worst {:.3f} ms)\n",
                                 cpu, render_s.mean(), cpu + render_s.mean(),
                                 1000.0 / (cpu + render_s.mean()), frame_s.mean(), frame_s.max);
    }
#endif
    (void)frames_run;
}

#if defined(PROTEKT_HAVE_VULKAN)
// N11: THE RENDERER'S FLOOR AS A LINE. One triangle is one point, and a point cannot say whether
// 15 ms is a fixed per-frame cost (submit, the full-screen passes, the readback) or a per-vertex
// one. So: the renderer is timed on a sequence of triangle counts, each drawn from the vertices of
// a real scene (base C's world, every k-th triangle, or repeated past its end, so the mix of
// terrain, sky and models is the game's and not a synthetic one), and a least-squares line is
// fitted: the intercept is the cost of a frame with nothing in it, the slope the cost of each
// triangle.
struct Fit {
    double intercept{};
    double slope{}; ///< per x unit
    double r2{};
};

[[nodiscard]] Fit fit_line(const std::vector<double>& x, const std::vector<double>& y) {
    const double n = static_cast<double>(x.size());
    double sx = 0.0;
    double sy = 0.0;
    double sxx = 0.0;
    double sxy = 0.0;
    double syy = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        sx += x.at(i);
        sy += y.at(i);
        sxx += x.at(i) * x.at(i);
        sxy += x.at(i) * y.at(i);
        syy += y.at(i) * y.at(i);
    }
    const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
    const double intercept = (sy - slope * sx) / n;
    const double ss_tot = syy - sy * sy / n;
    double ss_res = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double e = y.at(i) - (intercept + slope * x.at(i));
        ss_res += e * e;
    }
    return Fit{intercept, slope, ss_tot > 0.0 ? 1.0 - ss_res / ss_tot : 1.0};
}

struct FloorPoint {
    std::size_t triangles{};
    double mean{}, median{}, min{}, upload{}, record{}, submit{}, wait{}, readback{};
};

[[nodiscard]] FloorPoint time_triangles(protekt::render::Renderer& renderer,
                                        const protekt::game::Scene& source, std::size_t triangles) {
    using namespace protekt;
    const std::size_t pool = source.world_vertex_count / 3U;
    game::Scene scene;
    scene.view_projection = source.view_projection;
    scene.vertices.reserve(triangles * 3U);
    for (std::size_t i = 0; i < triangles; ++i) {
        const std::size_t t = triangles <= pool ? i * pool / triangles : i % pool;
        for (std::size_t k = 0; k < 3U; ++k) {
            scene.vertices.push_back(source.vertices.at(t * 3U + k));
        }
    }
    scene.world_vertex_count = scene.vertices.size();
    const render::FrameRequest request{.extent = game::Extent{1280, 720},
                                       .seed = kSeed,
                                       .tick = 0,
                                       .scene = &scene,
                                       .readback = readback_wanted()};
    render::FrameStats stats{};
    for (int i = 0; i < 10; ++i) {
        (void)renderer.render(request, stats);
    }
    std::vector<double> ms;
    FloorPoint p;
    p.triangles = triangles;
    for (int i = 0; i < kFrames; ++i) {
        const auto t0 = Clock::now();
        (void)renderer.render(request, stats);
        ms.push_back(since_ms(t0));
        p.upload += stats.times.upload_ms / kFrames;
        p.record += stats.times.record_ms / kFrames;
        p.submit += stats.times.submit_ms / kFrames;
        p.wait += stats.times.wait_ms / kFrames;
        p.readback += stats.times.readback_ms / kFrames;
    }
    for (const double v : ms) {
        p.mean += v / static_cast<double>(ms.size());
    }
    p.median = median(ms);
    p.min = *std::ranges::min_element(ms);
    return p;
}

void measure_floor(protekt::render::Renderer& renderer) {
    using namespace protekt;
    const Base base = bases().back();
    const sim::Sim sim = sim::Sim::create(sim::skirmish(kSeed, base.data));
    float cx = 0.0F;
    float cy = 0.0F;
    int n = 0;
    for (const sim::Unit& unit : sim.units()) {
        if (unit.alive && unit.side == sim::Side::Player) {
            cx += static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne);
            cy += static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne);
            ++n;
        }
    }
    game::TerrainCache cache{kSeed};
    game::SceneOptions options{};
    options.seed = kSeed;
    options.extent = game::Extent{1280, 720};
    options.radius_tiles = kRadius;
    options.camera = game::Camera{cx / static_cast<float>(n) * game::kMetresPerTile, 8.0F,
                                  cy / static_cast<float>(n) * game::kMetresPerTile, 0.0F, 0.0F};
    options.terrain = &cache;
    const game::BuildingMeshCache building_cache;
    options.buildings = &building_cache;
    const game::Scene source = game::build_scene(sim, options);

    const std::vector<std::size_t> counts{0, 1, 100, 1000, 10000, 50000};
    std::cout << std::format(
        "\n== THE RENDERER'S FLOOR, AS A LINE ({})\n   triangles drawn from the world of '{}' "
        "({} world triangles; every k-th, repeated past the end), seed {}, 1280x720, {} warm-up "
        "+ {} counted frames per point; ms per Renderer::render call\n",
        renderer.device_name(), base.name, source.world_vertex_count / 3U, kSeed, 10, kFrames);
    const auto table = [&](const char* title, const std::vector<std::size_t>& order) {
        std::cout << std::format("   -- {} --\n   {:>9} {:>8} {:>8} {:>8} | {:>7} {:>7} {:>7} "
                                 "{:>8} {:>8}\n",
                                 title, "triangles", "mean", "median", "min", "upload", "record",
                                 "submit", "wait", "readbk");
        std::vector<FloorPoint> out;
        for (const std::size_t t : order) {
            const FloorPoint p = time_triangles(renderer, source, t);
            std::cout << std::format("   {:>9} {:8.3f} {:8.3f} {:8.3f} | {:7.3f} {:7.3f} {:7.3f} "
                                     "{:8.3f} {:8.3f}\n",
                                     p.triangles, p.mean, p.median, p.min, p.upload, p.record,
                                     p.submit, p.wait, p.readback);
            out.push_back(p);
        }
        return out;
    };
    const auto report = [&](const char* label, const std::vector<FloorPoint>& pts) {
        std::vector<double> x;
        std::vector<double> mean;
        std::vector<double> med;
        std::vector<double> wait;
        std::vector<double> upload;
        for (const FloorPoint& p : pts) {
            x.push_back(static_cast<double>(p.triangles));
            mean.push_back(p.mean);
            med.push_back(p.median);
            wait.push_back(p.wait);
            upload.push_back(p.upload);
        }
        const auto line = [&](const char* what, const std::vector<double>& y) {
            const Fit f = fit_line(x, y);
            std::cout << std::format("   fit {:<28} {:<7} intercept {:7.3f} ms, slope {:8.4f} ms "
                                     "per 1000 triangles ({:6.2f} ns/vertex), R^2 {:.4f}\n",
                                     label, what, f.intercept, f.slope * 1000.0,
                                     f.slope * 1.0e6 / 3.0, f.r2);
        };
        line("mean", mean);
        line("median", med);
        line("wait phase", wait);
        line("upload phase", upload);
    };
    const std::vector<FloorPoint> up = table("ascending", counts);
    std::vector<std::size_t> down(counts.rbegin(), counts.rend());
    const std::vector<FloorPoint> dn
        = table("descending (the same points, the other way round)", down);
    report("ascending run", up);
    report("descending run", dn);
    std::cout << "   (the old row timed ONE triangle in the HUD pass only, right after a full "
                 "scene's renders; compare it to the 1-triangle row above)\n";
}
#endif

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
    for (const char* a : args) {
        (void)readback_wanted(std::string_view(a) == "--readback");
    }
    const std::string_view what = args.size() > 1 ? args[1] : "all";
    std::cout << std::format("protekt-framecost: build type {}, flags: {}\n", PROTEKT_BUILD_TYPE,
                             PROTEKT_BUILD_FLAGS);
    const bool use_building_cache
        = !(args.size() > 2 && std::string_view(args[2]) == "--no-building-cache");
    std::cout << std::format("protekt-framecost: readback {}\n",
                             readback_wanted()
                                 ? "ON (--readback: the frame is copied out, as before N12)"
                                 : "off (as the game runs without --capture)");
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

#if defined(PROTEKT_HAVE_VULKAN)
    if ((what == "all" || what == "floor") && renderer) {
        measure_floor(*renderer);
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
