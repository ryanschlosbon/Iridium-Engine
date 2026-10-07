#ifndef IRIDIUM_MOTION_VECTORS_GLSL
#define IRIDIUM_MOTION_VECTORS_GLSL

// M9.1 per-pixel motion. velocity = current UV - previous UV, both from
// unjittered projections (current: ubo.proj * ubo.view; previous: the view's
// previous-turn view-projection), so a consumer finds the previous position
// at uv - velocity. A surface point that was behind the previous camera has
// no valid previous position: it reports IridiumMotionNoHistory, which lies
// far off screen, so temporal consumers reject it.
// Requires include/view_uniforms.glsl. The velocity derives only from the
// current/previous world positions, so indexed, visibility-resolve and
// mesh-shader emitters produce identical vectors (ADR-0006).

const float IridiumMotionNoHistory = 4.0;

#if defined(IRIDIUM_MOTION_VERTEX)
layout(location = 6) out vec4 fragMotionCurrentClip;
layout(location = 7) out vec4 fragMotionPreviousClip;

void iridiumEmitMotion(vec4 worldPosition, vec4 previousWorldPosition) {
    fragMotionCurrentClip = ubo.proj * ubo.view * worldPosition;
    fragMotionPreviousClip = ubo.previousViewProjection * previousWorldPosition;
}
#endif

#if defined(IRIDIUM_MOTION_FRAGMENT)
layout(location = 6) in vec4 fragMotionCurrentClip;
layout(location = 7) in vec4 fragMotionPreviousClip;

vec2 iridiumMotionVector() {
    if (fragMotionCurrentClip.w <= 0.0 || fragMotionPreviousClip.w <= 0.0)
        return vec2(IridiumMotionNoHistory);
    vec2 current = fragMotionCurrentClip.xy / fragMotionCurrentClip.w;
    vec2 previous = fragMotionPreviousClip.xy / fragMotionPreviousClip.w;
    return (current - previous) * 0.5;
}
#endif

#endif
