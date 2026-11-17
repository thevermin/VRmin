// Adapted from Snapdragon Game Super Resolution 1, RGBA GLSL variant.
// Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// See ../assets/licenses/snapdragon-gsr.txt for the full license.
R"glsl(
uniform highp vec2 sgsrSize;

// External decoder textures cannot use textureGather. Read the same green
// samples explicitly at texel centers, clamped to the eye before SBS packing.
float sgsrTap(highp vec2 pixel) {
    highp vec2 local = (clamp(pixel, vec2(0.0), sgsrSize - 1.0) + 0.5) / sgsrSize;
    return texture(tex, vec2(off + local.x * 0.5, local.y)).g;
}

vec2 sgsrWeight(vec2 offset, float contrast, float scale) {
    float x = dot(offset, offset) * 0.55 + clamp(abs(contrast) * scale, 0.0, 1.0);
    float a = x - 4.0;
    float weight = (x * a - a) * a * a;
    return vec2(weight, weight * contrast);
}

vec3 sgsrFilter(highp vec2 local, vec3 rgb) {
    highp vec2 grid = local * sgsrSize + vec2(-0.5, 0.5);
    highp vec2 origin = floor(grid);
    float leftY = sgsrTap(origin);
    float leftZ = sgsrTap(origin + vec2(0.0, -1.0));
    float edge = abs(leftZ - leftY) + abs(rgb.g - leftY) + abs(rgb.g - leftZ);
    if (edge <= 4.0 / 255.0) return rgb;

    vec4 left = vec4(sgsrTap(origin + vec2(-1.0, 0.0)), leftY, leftZ,
                     sgsrTap(origin + vec2(-1.0, -1.0)));
    vec4 right = vec4(sgsrTap(origin + vec2(1.0, 0.0)), sgsrTap(origin + vec2(2.0, 0.0)),
                      sgsrTap(origin + vec2(2.0, -1.0)), sgsrTap(origin + vec2(1.0, -1.0)));
    vec4 upDown = vec4(sgsrTap(origin + vec2(0.0, -2.0)), sgsrTap(origin + vec2(1.0, -2.0)),
                       sgsrTap(origin + vec2(1.0, 1.0)), sgsrTap(origin + vec2(0.0, 1.0)));

    float mean = (left.y + left.z + right.x + right.w) * 0.25;
    left -= mean;
    right -= mean;
    upDown -= mean;
    float scale = 2.181818 / max(dot(abs(left) + abs(right) + abs(upDown), vec4(1.0)), 0.0001);
    vec2 delta = fract(grid);
    vec2 sum = sgsrWeight(delta + vec2(0.0, 1.0), upDown.x, scale)
             + sgsrWeight(delta + vec2(-1.0, 1.0), upDown.y, scale)
             + sgsrWeight(delta + vec2(-1.0, -2.0), upDown.z, scale)
             + sgsrWeight(delta + vec2(0.0, -2.0), upDown.w, scale)
             + sgsrWeight(delta + vec2(1.0, -1.0), left.x, scale)
             + sgsrWeight(delta + vec2(0.0, -1.0), left.y, scale)
             + sgsrWeight(delta, left.z, scale)
             + sgsrWeight(delta + vec2(1.0, 0.0), left.w, scale)
             + sgsrWeight(delta + vec2(-1.0, -1.0), right.x, scale)
             + sgsrWeight(delta + vec2(-2.0, -1.0), right.y, scale)
             + sgsrWeight(delta + vec2(-2.0, 0.0), right.z, scale)
             + sgsrWeight(delta + vec2(-1.0, 0.0), right.w, scale);
    if (abs(sum.x) < 0.0001) return rgb;
    float lo = min(min(left.y, left.z), min(right.x, right.w));
    float hi = max(max(left.y, left.z), max(right.x, right.w));
    float filtered = clamp(2.0 * sum.y / sum.x, lo, hi) + mean;
    float correction = clamp(filtered - rgb.g, -23.0 / 255.0, 23.0 / 255.0);
    return clamp(rgb + correction, 0.0, 1.0);
}
)glsl"
