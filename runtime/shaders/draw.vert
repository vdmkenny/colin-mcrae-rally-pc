#version 450
// Primitives in VRAM coordinates; w is the view depth for perspective-correct texturing.
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_col;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in uvec4 a_tex;
layout(location = 4) in uint a_uvlim;
// centroid: with MSAA, a pixel the triangle only partly covers is shaded inside the triangle, so
// texture coordinates are never extrapolated past the polygon's texture area
layout(location = 0) centroid out vec4 v_col;
layout(location = 1) centroid out vec2 v_uv;
layout(location = 2) flat out uvec4 v_tex;
layout(location = 3) flat out uint v_uvlim;

void main() {
    vec2 ndc = vec2(a_pos.x / 1024.0 * 2.0 - 1.0, 1.0 - a_pos.y / 512.0 * 2.0);
    gl_Position = vec4(ndc * a_pos.z, 0.0, a_pos.z);
    v_col = a_col;
    v_uv = a_uv;
    v_tex = a_tex;
    v_uvlim = a_uvlim;
}
