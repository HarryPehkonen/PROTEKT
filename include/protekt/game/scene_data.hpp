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

/// One vertex of a scene: position in world metres, a flat colour, then a texture coordinate. The
/// layout is part of the contract because the vertex buffer is uploaded verbatim. The fragment
/// stage multiplies the texel at (u, v) by the colour; nothing sets a UV yet, and the renderer's
/// one white texture makes (0, 0) an identity.
struct SceneVertex {
    std::array<float, 3> position{};
    std::array<float, 3> colour{};
    float u{};
    float v{};
};

/// A frame's worth of geometry, already in world space, plus whose camera it is.
struct Scene {
    std::vector<SceneVertex> vertices;       ///< triples: three vertices per triangle
    std::array<float, 16> view_projection{}; ///< column-major, what GLSL's mat4 expects
};

} // namespace protekt::game
