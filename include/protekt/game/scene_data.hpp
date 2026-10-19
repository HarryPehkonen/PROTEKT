#pragma once

#include <array>
#include <cstdint>
#include <vector>

/// Plain data the game builds and the renderer consumes. The game owns these definitions so the
/// layers below the renderer never include it; the namespace stays `render` because that is what
/// every caller already spells.
namespace protekt::render {

struct Extent {
    std::uint32_t width{};
    std::uint32_t height{};
};

/// One vertex of a scene: position in world metres, then a flat colour. The layout is part of the
/// contract because the vertex buffer is uploaded verbatim.
struct SceneVertex {
    std::array<float, 3> position{};
    std::array<float, 3> colour{};
};

/// A frame's worth of geometry, already in world space, plus whose camera it is.
struct Scene {
    std::vector<SceneVertex> vertices;       ///< triples: three vertices per triangle
    std::array<float, 16> view_projection{}; ///< column-major, what GLSL's mat4 expects
};

} // namespace protekt::render
