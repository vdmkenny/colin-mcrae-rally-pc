#version 450
// A rectangle of 16-bit pixels drawn into the high-resolution target: u_rect is where (x, y, w, h
// in VRAM units); u_src where the pixels are in the source buffer: (first pixel, pixels per row,
// x, y of that first pixel). VRAM itself is (0, 1024, 0, 0).
layout(set = 1, binding = 0) uniform Rect { vec4 u_rect; uvec4 u_src; };
layout(location = 0) out vec2 v_pos;
layout(location = 1) flat out uvec4 v_src;

const vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));

void main() {
    vec2 p = u_rect.xy + corners[gl_VertexIndex] * u_rect.zw;
    v_pos = p;
    v_src = u_src;
    gl_Position = vec4(p.x / 1024.0 * 2.0 - 1.0, 1.0 - p.y / 512.0 * 2.0, 0.0, 1.0);
}
