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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "protekt/game/atlas.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/terrain_cache.hpp"
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

void measure(const Base& base, protekt::render::Renderer* renderer) {
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

} // namespace

// A probe, run by hand: a throw is the report. No state to unwind.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
    std::cout << std::format("protekt-framecost: build type {}, flags: {}\n", PROTEKT_BUILD_TYPE,
                             PROTEKT_BUILD_FLAGS);
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

    for (const Base& base : bases()) {
        measure(base, renderer.get());
    }
    return 0;
}
