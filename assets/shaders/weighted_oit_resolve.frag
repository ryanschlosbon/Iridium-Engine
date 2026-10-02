#version 450

layout(set = 0, binding = 0) uniform sampler2D weightedAccumulation;
layout(set = 0, binding = 1) uniform sampler2D weightedRevealage;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform WeightedOitResolvePushConstants {
    uint debugView;
} push;

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 accumulation = texelFetch(weightedAccumulation, pixel, 0);
    float revealage = texelFetch(weightedRevealage, pixel, 0).r;
    if (any(isnan(accumulation)) || any(isinf(accumulation)) ||
        isnan(revealage) || isinf(revealage)) {
        discard;
    }
    float coverage = clamp(1.0 - revealage, 0.0, 1.0);
    if (!(coverage > 0.0) || !(accumulation.a > 1.0e-8)) discard;
    if (push.debugView == 22u) {
        outColor = vec4(0.70, 0.15, 1.00, 1.0);
        return;
    }
    if (push.debugView == 23u) {
        float radianceLoad = max(abs(accumulation.r),
            max(abs(accumulation.g), abs(accumulation.b))) / 65504.0;
        float coverageLoad = accumulation.a / 256.0;
        float load = max(radianceLoad, coverageLoad);
        vec3 color = load >= 1.0
            ? vec3(1.0, 0.0, 1.0)
            : mix(vec3(0.05, 0.30, 0.08), vec3(1.0, 0.55, 0.0),
                clamp(load, 0.0, 1.0));
        outColor = vec4(color, 1.0);
        return;
    }
    vec3 averageRadiance = accumulation.rgb / accumulation.a;
    vec3 premultiplied = max(averageRadiance * coverage, vec3(0.0));
    outColor = vec4(premultiplied, coverage);
}
