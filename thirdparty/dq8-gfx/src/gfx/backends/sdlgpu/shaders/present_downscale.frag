#version 450

// The picture made smaller with a Lanczos-2 filter stretched over each
// output pixel's footprint, one axis a pass (Rogue Galaxy: added). At 4x to
// 8x internal resolution a bilinear blit to the window reads only the four
// texels nearest each pixel's centre and skips the rest, which leaves the
// shimmer and stair-steps the extra resolution was there to remove; this
// reads every texel under the pixel (up to 2 x 8 a side at 8x), weighted
// the way a good image resizer does.
//
// `region` (x, y, width, height, in texels) of `source` becomes the target's
// `size.xy` along `size.z`'s axis (0 across, 1 down); the other axis is
// copied texel for texel, so the first pass's target is size.x x region.w.
layout(set = 2, binding = 0) uniform sampler2D source;
layout(set = 3, binding = 0) uniform DownscaleParams {
    vec4 region;
    vec4 size; // x, y: the output's size; z: the axis
} params;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265358979;

float lanczos2(float x) {
    x = abs(x);
    if (x < 1.0e-4)
        return 1.0;
    if (x >= 2.0)
        return 0.0;
    float px = PI * x;
    return 2.0 * sin(px) * sin(px * 0.5) / (px * px);
}

void main() {
    bool down = params.size.z > 0.5;
    ivec2 origin = ivec2(params.region.xy);
    ivec2 last = ivec2(params.region.zw) - 1;
    ivec2 p = ivec2(gl_FragCoord.xy);
    float extent = down ? params.region.w : params.region.z;
    float outExtent = down ? params.size.y : params.size.x;
    float ratio = extent / outExtent;
    // Enlarging: the kernel stays a texel wide (plain Lanczos-2).
    float stretch = max(ratio, 1.0);
    float centre = ((down ? float(p.y) : float(p.x)) + 0.5) * ratio;
    float radius = 2.0 * stretch;
    int first = int(floor(centre - radius));
    int count = min(int(ceil(2.0 * radius)) + 1, 72);
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int i = 0; i < count; ++i) {
        int at = first + i;
        float w = lanczos2((float(at) + 0.5 - centre) / stretch);
        if (w == 0.0)
            continue;
        ivec2 texel = down ? ivec2(p.x, clamp(at, 0, last.y)) : ivec2(clamp(at, 0, last.x), p.y);
        // The other axis: a texel of the region for each pixel.
        if (down)
            texel.x = clamp(texel.x, 0, last.x);
        else
            texel.y = clamp(texel.y, 0, last.y);
        sum += w * texelFetch(source, origin + texel, 0).rgb;
        total += w;
    }
    outColor = vec4(clamp(sum / max(total, 1.0e-5), 0.0, 1.0), 1.0);
}
