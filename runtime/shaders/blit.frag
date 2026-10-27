#version 450
layout(location = 0) in vec2 v_pos;
layout(location = 1) flat in uvec4 v_src;
layout(std430, set = 2, binding = 0) readonly buffer Pixels { uint words[]; } u_px;
layout(location = 0) out vec4 o_col;

void main() {
    ivec2 p = ivec2(floor(v_pos)) & ivec2(1023, 511);
    uint i = v_src.x + uint(p.y - int(v_src.w)) * v_src.y + uint(p.x - int(v_src.z));
    uint w = u_px.words[i >> 1];
    uint t = (i & 1u) != 0u ? w >> 16 : w & 0xFFFFu;
    o_col = vec4(float(t & 31u), float((t >> 5) & 31u), float((t >> 10) & 31u), 31.0) / 31.0;
}
