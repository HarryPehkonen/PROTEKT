#version 450
// A fullscreen triangle with no vertex buffer: three vertices from gl_VertexIndex that overhang the
// frame, so the rasteriser clips them to exactly the viewport and every pixel is covered once.
// This is the machinery every fullscreen pass shares; the fragment shader is what differs.
void main() {
    const vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
