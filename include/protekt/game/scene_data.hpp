#pragma once

#include <array>
#include <cstdint>
#include <vector>

/// Plain data the game builds and the renderer consumes. The game owns these definitions so the
/// layers below the renderer never include it.
namespace protekt::game {

struct Extent {
    std::uint32_t width{};
    std::uint32_t height{};
};

/// The inset of the bottom-left panel corner, in pixels of a 720-high frame. The radar and the
/// world map both live in that corner and swap places in it, so they share one margin rather than
/// each carrying its own.
inline constexpr float kPanelMarginPixels = 24.0F;

/// One vertex of a scene: position in world metres, a flat colour, an alpha, then a texture
/// coordinate. The layout is part of the contract because the vertex buffer is uploaded verbatim.
/// The fragment stage multiplies the texel at (u, v) by the colour and outputs the alpha, so a quad
/// drawn with an alpha under 1 blends with whatever is already behind it. Alpha defaults to 1, so
/// every existing caller is unchanged and every world triangle stays opaque.
struct SceneVertex {
    std::array<float, 3> position{};
    std::array<float, 3> colour{};
    float alpha{1.0F};
    float u{};
    float v{};
};

/// A frame's worth of geometry, already in world space, plus whose camera it is.
struct Scene {
    std::vector<SceneVertex> vertices;       ///< triples: three vertices per triangle
    std::array<float, 16> view_projection{}; ///< column-major, what GLSL's mat4 expects
};

/// The radar's wireframe grid pitch in tiles, for RADAR3D-2: 2 gives a 41x41 grid of tile corners
/// for the 40-tile radius (3,280 segments before clipping) and reads as BZ2's density at 1280.
/// It lives in this lowest game header rather than beside the mesh in `radar.hpp` because BOTH
/// sides need it: `radar_mesh` defaults to it, and `SceneOptions` (scene.hpp) offers it as a dial,
/// priced in `protekt-meshcost`. `scene.hpp` may not include `radar.hpp` -- `nav_marks.hpp`
/// includes `scene.hpp` and `radar.hpp` includes `nav_marks.hpp`, so putting the constant here is
/// what keeps that triangle of headers from closing.
inline constexpr int kRadarMeshStepTiles = 2;

} // namespace protekt::game
