#version 450

// FSR 1's sharpening (RCAS, "robust contrast adaptive sharpening"), after
// AMD FidelityFX Super Resolution 1.0's ffx_fsr1.h (MIT, Copyright (c) 2021
// Advanced Micro Devices, Inc.), in plain GLSL at full precision (Rogue
// Galaxy: added).
//
// A cross of five texels:
//
//       b
//     d e f
//       h
//
// e less a share of its four neighbours, the share (the negative lobe) as
// large as it can be without taking e below the neighbours' minimum or
// above their maximum, and at most 0.1875, times the strength. So edges
// grow crisper without the halos a fixed unsharp mask leaves.
//
// The source is the picture at the output's size, at the origin; it lands
// in `rect` of the target (x, y, width, height in pixels), which the
// viewport and scissor limit the pass to.
layout(set = 2, binding = 0) uniform sampler2D source;
layout(set = 3, binding = 0) uniform RcasParams {
    vec4 rect;     // where the picture goes in the target
    vec4 strength; // x: 0 none to 1 the most (2 to the power of minus the stops)
} params;
layout(location = 0) out vec4 outColor;

void main() {
    ivec2 last = ivec2(params.rect.zw) - 1;
    ivec2 p = clamp(ivec2(gl_FragCoord.xy - params.rect.xy), ivec2(0), last);
    vec3 b = texelFetch(source, clamp(p + ivec2(0, -1), ivec2(0), last), 0).rgb;
    vec3 d = texelFetch(source, clamp(p + ivec2(-1, 0), ivec2(0), last), 0).rgb;
    vec3 e = texelFetch(source, p, 0).rgb;
    vec3 f = texelFetch(source, clamp(p + ivec2(1, 0), ivec2(0), last), 0).rgb;
    vec3 h = texelFetch(source, clamp(p + ivec2(0, 1), ivec2(0), last), 0).rgb;

    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));
    // The lobe that would just reach the minimum, and the one that would
    // just reach the maximum (FSR's peakC = (1, -4)); the small terms keep
    // flat black and flat white from dividing by zero.
    vec3 hitMin = min(mn4, e) / (4.0 * mx4 + 1.0e-5);
    vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, -1.0e-5);
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * params.strength.x;
    vec3 color = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
    outColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
