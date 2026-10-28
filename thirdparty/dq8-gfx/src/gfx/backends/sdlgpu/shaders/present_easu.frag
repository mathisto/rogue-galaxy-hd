#version 450

// FSR 1's upscale (EASU, "edge adaptive spatial upsampling"), after AMD
// FidelityFX Super Resolution 1.0's ffx_fsr1.h (MIT, Copyright (c) 2021
// Advanced Micro Devices, Inc.), written out in plain GLSL at full precision
// (Rogue Galaxy: added).
//
// Each output pixel reads the 12 input texels around it:
//
//       b c
//     e f g h
//     i j k l
//       n o
//
// From the luma of the four nearest (f g j k) and their neighbours it
// estimates an edge's direction and how strong it is; a Lanczos-like lobe,
// stretched along that edge and narrowed across it, then weighs the 12, and
// the result is held between the four nearest's minimum and maximum so the
// lobe cannot ring. Only for enlarging: the caller draws a picture larger
// than its source with it, and anything else the usual way.
//
// The output covers the target from its origin; the input is the region
// `region` (x, y, width, height, in texels) of `source`, read with texelFetch
// (the sampler's filter is not used).
layout(set = 2, binding = 0) uniform sampler2D source;
layout(set = 3, binding = 0) uniform EasuParams {
    vec4 region; // the input's x, y, width, height in texels
    vec4 outSize; // the output's width and height
} params;
layout(location = 0) out vec4 outColor;

ivec2 g_origin;
ivec2 g_last;

vec3 tap(ivec2 at) {
    return texelFetch(source, g_origin + clamp(at, ivec2(0), g_last), 0).rgb;
}

// Luma times two, as FSR takes it: green plus half of red and blue.
float luma(vec3 c) {
    return c.b * 0.5 + (c.r * 0.5 + c.g);
}

// The direction and the edge's length, from one of the four nearest (C)
// and its neighbours above (A), left (B), right (D) and below (E), weighed
// by how close the sample point is to it.
void accumulate(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = lenX > 0.0 ? 1.0 / lenX : 0.0;
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    lenX *= lenX;
    len += lenX * w;

    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = lenY > 0.0 ? 1.0 / lenY : 0.0;
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    lenY *= lenY;
    len += lenY * w;
}

void lobeTap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec3 c) {
    // Rotated into the edge's frame, and scaled: stretched along it.
    vec2 v;
    v.x = off.x * dir.x + off.y * dir.y;
    v.y = off.x * -dir.y + off.y * dir.x;
    v *= len2;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    // (25/16 * (2/5 * x^2 - 1)^2 - (25/16 - 1)) * (1/4 * x^2 - 1)^2, with the
    // second lobe's width `lob` instead of 1/4.
    float wB = 2.0 / 5.0 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

void main() {
    g_origin = ivec2(params.region.xy);
    g_last = ivec2(params.region.zw) - 1;
    // The output pixel's centre in input texels, from the input's first
    // texel centre: pp = (x + 0.5) * in / out - 0.5.
    vec2 scale = params.region.zw / params.outSize.xy;
    vec2 pp = gl_FragCoord.xy * scale - 0.5;
    vec2 fp = floor(pp);
    pp -= fp;
    ivec2 f0 = ivec2(fp);

    vec3 b = tap(f0 + ivec2(0, -1));
    vec3 c = tap(f0 + ivec2(1, -1));
    vec3 e = tap(f0 + ivec2(-1, 0));
    vec3 f = tap(f0 + ivec2(0, 0));
    vec3 g = tap(f0 + ivec2(1, 0));
    vec3 h = tap(f0 + ivec2(2, 0));
    vec3 i = tap(f0 + ivec2(-1, 1));
    vec3 j = tap(f0 + ivec2(0, 1));
    vec3 k = tap(f0 + ivec2(1, 1));
    vec3 l = tap(f0 + ivec2(2, 1));
    vec3 n = tap(f0 + ivec2(0, 2));
    vec3 o = tap(f0 + ivec2(1, 2));

    float bL = luma(b), cL = luma(c), eL = luma(e), fL = luma(f), gL = luma(g), hL = luma(h);
    float iL = luma(i), jL = luma(j), kL = luma(k), lL = luma(l), nL = luma(n), oL = luma(o);

    vec2 dir = vec2(0.0);
    float len = 0.0;
    accumulate(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    accumulate(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    accumulate(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    accumulate(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);

    // The direction normalised; none at all reads as along x.
    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    if (dirR < 1.0 / 32768.0) {
        dir = vec2(1.0, 0.0);
    } else {
        dir *= inversesqrt(dirR);
    }
    // The edge's strength, 0 to 1, shapes the kernel: stretched along a
    // diagonal edge, narrowed across a strong one.
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;

    vec3 aC = vec3(0.0);
    float aW = 0.0;
    lobeTap(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    lobeTap(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    lobeTap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
    lobeTap(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    lobeTap(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    lobeTap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    lobeTap(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    lobeTap(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    lobeTap(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    lobeTap(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    lobeTap(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
    lobeTap(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, n);

    // Held within the four nearest: no ringing.
    vec3 min4 = min(min(f, g), min(j, k));
    vec3 max4 = max(max(f, g), max(j, k));
    vec3 color = aW != 0.0 ? aC / aW : f;
    outColor = vec4(clamp(color, min4, max4), 1.0);
}
