#pragma once

#include <array>
#include <cstddef>
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

/// One vertex of a scene: position in world metres, a colour (the surface's UNLIT colour where it
/// has a normal), an alpha, a texture coordinate, a fog amount and a normal. The layout is part of
/// the contract because the vertex buffer is uploaded verbatim. The fragment stage multiplies the
/// texel at (u, v) by the colour and outputs the alpha, so a quad drawn with an alpha under 1
/// blends with whatever is already behind it. Alpha defaults to 1, so every existing caller is
/// unchanged and every world triangle stays opaque.
struct SceneVertex {
    std::array<float, 3> position{};
    std::array<float, 3> colour{};
    float alpha{1.0F};
    float u{};
    float v{};
    /// How much of the distance fog covers this vertex, 0..1 (`fog_amount`). The fragment shader
    /// mixes the fog colour in AFTER the texture; 0 is the old expression exactly.
    float fog{};
    /// The surface's outward unit normal, for the fragment stage's light (AP). All zero means "not
    /// lit by a normal": the sky, every HUD plate, the particles and the shadows keep exactly the
    /// colour they carry, so the zero default leaves every existing caller as it was.
    std::array<float, 3> normal{};
};

/// A frame's worth of geometry, already in world space, plus whose camera it is.
struct Scene {
    std::vector<SceneVertex> vertices;       ///< triples: three vertices per triangle
    std::array<float, 16> view_projection{}; ///< column-major, what GLSL's mat4 expects
    /// How many of the leading vertices are the WORLD (sky, terrain, units, effects); the rest are
    /// the HUD. The renderer smooths the world's edges and draws the HUD after, so no panel, glyph,
    /// dot or reticle is ever softened. The default means everything is world, which is what a
    /// hand-built scene with no HUD wants. Both lists are triples, so a boundary is a multiple
    /// of 3.
    std::size_t world_vertex_count{SIZE_MAX};
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
