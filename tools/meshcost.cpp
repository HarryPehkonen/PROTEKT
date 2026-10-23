// RADAR3D-2's frame-cost dial: what the radar's terrain wireframe costs, and whether step 1 is
// affordable.
//
// The brief's warning was "~3300 segments = ~20k vertices if each line is a quad -- budget a
// measurement before choosing step 1". This measures the mesh alone and the whole frame around it,
// at the app's own radius. Numbers are printed, not asserted: this is a probe, run by hand.
#include <chrono>
#include <cstddef>
#include <format>
#include <iostream>
#include <vector>

#include "protekt/game/radar.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/terrain_cache.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace {

template <class F> double ms_each(std::size_t n, const F& f) {
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < n; ++i) {
        f();
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / static_cast<double>(n);
}

/// The first living unit of the player's side: the frame's own radar centre, found the way the app
/// finds its driven unit, without needing a Session.
[[nodiscard]] protekt::sim::UnitId player_viewer(const protekt::sim::Sim& sim) {
    for (const protekt::sim::Unit& unit : sim.units()) {
        if (unit.alive && unit.side == protekt::sim::Side::Player) {
            return unit.id;
        }
    }
    return protekt::sim::UnitId{};
}

} // namespace

// A probe, run by hand: if the stream or the formatting machinery throws, the runtime's own message
// and a non-zero exit ARE the report -- there is no state to unwind and nothing to clean up.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
    using namespace protekt;
    const game::RadarPanel panel = game::radar_panel(game::Extent{1280, 720});
    std::cout << std::format("radar panel: {:.0f} x {:.0f} px at 1280x720\n", panel.width,
                             panel.height);

    for (const int step : {4, 3, 2, 1}) {
        std::vector<game::RadarSegment> segments;
        const double mesh_ms = ms_each(200, [&] {
            segments
                = game::radar_mesh(42, 100.0F, 100.0F, 0.3F, panel, game::kRadarRadiusTiles, step);
        });
        std::cout << std::format("  mesh alone, step {}: {:4} segments  {:7.3f} ms  "
                                 "({:5.1f} us a segment, {:5} vertices as thin quads)\n",
                                 step, segments.size(), mesh_ms,
                                 (mesh_ms * 1000.0) / static_cast<double>(segments.size()),
                                 segments.size() * 6U);
    }

    const sim::Sim sim = sim::Sim::create(sim::skirmish(42));
    const sim::UnitId viewer = player_viewer(sim);
    std::cout << "  the frame, through the terrain cache, radius 24 (the app's own). The mesh\n"
                 "  alone is cheap; it is the VERTICES that cost a frame here, so the dial is\n"
                 "  priced in frames:\n";
    for (const int step : {0, 4, 3, 2, 1}) {
        game::TerrainCache cache{42};
        game::SceneOptions options{};
        options.seed = 42;
        options.extent = game::Extent{1280, 720};
        options.radius_tiles = 24;
        options.viewer_unit = viewer;
        options.camera = game::Camera{100.0F, 8.0F, 104.0F, 0.0F, 0.0F};
        options.terrain = &cache;
        options.show_radar_mesh = step != 0;
        options.radar_mesh_step_tiles = step == 0 ? game::kRadarMeshStepTiles : step;
        std::size_t vertices = 0;
        const double frame_ms
            = ms_each(60, [&] { vertices = game::build_scene(sim, options).vertices.size(); });
        const std::string label = step == 0 ? "off" : std::format("step {}", step);
        std::cout << std::format("    mesh {}: {:6} vertices  {:7.3f} ms a frame\n", label,
                                 vertices, frame_ms);
    }
    return 0;
}