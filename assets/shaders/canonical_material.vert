#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord0;
layout(location = 4) in vec4 inTangent;
layout(location = 5) in vec2 inTexCoord1;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragTexCoord0;
layout(location = 2) out vec3 fragNormal;
layout(location = 3) out vec3 fragWorldPos;
layout(location = 4) out vec4 fragTangent;
layout(location = 5) out vec2 fragTexCoord1;

#include "include/view_uniforms.glsl"
#include "include/motion_vectors.glsl"

layout(push_constant) uniform CanonicalPushConstants {
    mat4 renderMatrix;
    uint materialIndex;
    uint padding0;
    uint padding1;
    uint padding2;
#if defined(IRIDIUM_MOTION_VERTEX)
    // M9.1: CanonicalMotionPushConstants (G-buffer and forward-opaque).
    mat4 previousRenderMatrix;
#endif
} push;

void main() {
    vec4 worldPos = push.renderMatrix * vec4(inPosition, 1.0);
#if defined(IRIDIUM_UNJITTERED_RASTER)
    // The selection mask is read after temporal resolve: never jittered.
    gl_Position = ubo.proj * ubo.view * worldPos;
#else
    gl_Position = ubo.jitteredProjection * ubo.view * worldPos;
#endif
    fragColor = inColor;
    fragTexCoord0 = inTexCoord0;
    fragTexCoord1 = inTexCoord1;
    mat3 normalMatrix = transpose(inverse(mat3(push.renderMatrix)));
    fragNormal = normalMatrix * inNormal;
    fragTangent = vec4(mat3(push.renderMatrix) * inTangent.xyz, inTangent.w);
    fragWorldPos = worldPos.xyz;
#if defined(IRIDIUM_MOTION_VERTEX)
    iridiumEmitMotion(worldPos, push.previousRenderMatrix * vec4(inPosition, 1.0));
#endif
}
