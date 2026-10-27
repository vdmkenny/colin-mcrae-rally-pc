#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 1) flat in vec4 v_src;
layout(set = 2, binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 o_col;

void main() {
    // filtering stays inside the shown area: the texels around it are other VRAM, or never
    // written at all around a video frame
    vec2 half_texel = 0.5 / vec2(textureSize(u_tex, 0));
    vec2 uv = clamp(v_uv, v_src.xy + half_texel, v_src.xy + v_src.zw - half_texel);
    o_col = vec4(texture(u_tex, uv).rgb, 1.0);
}
