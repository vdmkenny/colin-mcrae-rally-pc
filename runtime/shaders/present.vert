#version 450
// Full-viewport quad showing u_src (x, y, w, h in texture coordinates) of a texture.
layout(set = 1, binding = 0) uniform Source { vec4 u_src; };
layout(location = 0) out vec2 v_uv;
layout(location = 1) flat out vec4 v_src;

const vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));

void main() {
    vec2 c = corners[gl_VertexIndex];
    v_uv = u_src.xy + c * u_src.zw;
    v_src = u_src;
    gl_Position = vec4(c.x * 2.0 - 1.0, 1.0 - c.y * 2.0, 0.0, 1.0);
}
