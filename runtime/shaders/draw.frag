#version 450
// v_tex: x = texpage x | y << 16, y = clut x | y << 16, z = texture window, w = flags
// flags: 0-1 depth, 2 textured, 3 raw, 4-5 pass (0 all, 1 opaque texels, 2 translucent texels),
//        6 bilinear filtering
layout(location = 0) centroid in vec4 v_col;
layout(location = 1) centroid in vec2 v_uv;
layout(location = 2) flat in uvec4 v_tex;
layout(location = 3) flat in uint v_uvlim;
// 1x VRAM, two 16-bit pixels per word (integer textures cannot be sampled in SDL_GPU)
layout(std430, set = 2, binding = 0) readonly buffer Vram { uint words[]; } u_vram;
layout(location = 0) out vec4 o_col;

uint fetch16(int x, int y) {
    uint i = uint(y & 511) * 1024u + uint(x & 1023);
    uint w = u_vram.words[i >> 1];
    return (i & 1u) != 0u ? w >> 16 : w & 0xFFFFu;
}

uint texel(ivec2 c) {
    uint tw = v_tex.z;
    uint mx = tw & 31u, my = (tw >> 5) & 31u, ox = (tw >> 10) & 31u, oy = (tw >> 15) & 31u;
    uint u = uint(c.x) & 255u, v = uint(c.y) & 255u;
    u = (u & ~(mx * 8u)) | ((ox & mx) * 8u);
    v = (v & ~(my * 8u)) | ((oy & my) * 8u);
    int tx = int(v_tex.x & 0xFFFFu), ty = int(v_tex.x >> 16);
    int cx = int(v_tex.y & 0xFFFFu), cy = int(v_tex.y >> 16);
    uint depth = v_tex.w & 3u;
    if (depth == 0u) {
        uint w = fetch16(tx + int(u >> 2), ty + int(v));
        return fetch16(cx + int((w >> ((u & 3u) * 4u)) & 15u), cy);
    } else if (depth == 1u) {
        uint w = fetch16(tx + int(u >> 1), ty + int(v));
        return fetch16(cx + int((w >> ((u & 1u) * 8u)) & 255u), cy);
    }
    return fetch16(tx + int(u), ty + int(v));
}

vec4 decode(uint t) {
    return vec4(float(t & 31u), float((t >> 5) & 31u), float((t >> 10) & 31u), 31.0) / 31.0;
}

void main() {
    uint flags = v_tex.w;
    vec4 col = v_col;
    if ((flags & 4u) != 0u) {
        uint pass = (flags >> 4) & 3u;
        vec4 t;
        bool stp;
        if ((flags & 64u) != 0u) {
            vec2 p = v_uv - 0.5;
            ivec2 b = ivec2(floor(p));
            vec2 f = p - vec2(b);
            ivec2 lo = ivec2(int(v_uvlim & 255u), int((v_uvlim >> 8) & 255u));
            ivec2 hi = ivec2(int((v_uvlim >> 16) & 255u), int(v_uvlim >> 24));
            ivec2 b0 = clamp(b, lo, hi), b1 = clamp(b + 1, lo, hi);
            uint t00 = texel(b0), t10 = texel(ivec2(b1.x, b0.y)), t01 = texel(ivec2(b0.x, b1.y)), t11 = texel(b1);
            vec4 c00 = t00 == 0u ? vec4(0) : decode(t00), c10 = t10 == 0u ? vec4(0) : decode(t10);
            vec4 c01 = t01 == 0u ? vec4(0) : decode(t01), c11 = t11 == 0u ? vec4(0) : decode(t11);
            t = mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
            if (t.a < 0.5) discard;
            t.rgb /= t.a;
            // semi-transparent if most of the visible texels mixed here are (the nearest texel
            // alone may be a transparent one at the edge of a translucent shape)
            vec4 w = vec4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
            float s = dot(w, vec4((t00 & 0x8000u) != 0u, (t10 & 0x8000u) != 0u, (t01 & 0x8000u) != 0u, (t11 & 0x8000u) != 0u));
            stp = s >= 0.5 * t.a;
        } else {
            uint tn = texel(ivec2(floor(v_uv)));
            if (tn == 0u) discard;
            t = decode(tn);
            stp = (tn & 0x8000u) != 0u;
        }
        if (pass == 1u && stp) discard;
        if (pass == 2u && !stp) discard;
        if ((flags & 8u) != 0u) col = vec4(t.rgb, 1.0);
        else col = vec4(min(t.rgb * col.rgb * (255.0 / 128.0), vec3(1.0)), 1.0);
    }
    o_col = vec4(col.rgb, 1.0);
}
