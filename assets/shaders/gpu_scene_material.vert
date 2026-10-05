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
#define IRIDIUM_MOTION_VERTEX 1
#include "include/motion_vectors.glsl"

layout(push_constant) uniform CanonicalPushConstants {
    mat4 renderMatrix;
    uint materialIndex;
    uint padding0;
    uint padding1;
    uint padding2;
} push;

struct GpuSceneTransform { vec4 row0; vec4 row1; vec4 row2; };
struct GpuSceneInstance {
    vec4 worldBoundsSphere; vec4 worldBoundsMin; vec4 worldBoundsMax;
    uvec4 references; uvec4 state;
};
struct GpuScenePrimitive { uvec4 binding; uvec4 state; uvec4 revisions; };
struct GpuSceneGeometry {
    vec4 localBoundsSphere; vec4 localBoundsMin; vec4 localBoundsMax;
    uvec4 draw; uvec4 storage; uvec4 state;
};
layout(std430, set = 4, binding = 0) readonly buffer Transforms {
    GpuSceneTransform values[];
} transforms;
layout(std430, set = 4, binding = 1) readonly buffer Instances {
    GpuSceneInstance values[];
} instances;
layout(std430, set = 4, binding = 2) readonly buffer Primitives {
    GpuScenePrimitive values[];
} primitives;
layout(std430, set = 4, binding = 3) readonly buffer Geometries {
    GpuSceneGeometry values[];
} geometries;

mat4 affineMatrix(uint transformIndex) {
    GpuSceneTransform value = transforms.values[transformIndex];
    return mat4(
        vec4(value.row0.x, value.row1.x, value.row2.x, 0.0),
        vec4(value.row0.y, value.row1.y, value.row2.y, 0.0),
        vec4(value.row0.z, value.row1.z, value.row2.z, 0.0),
        vec4(value.row0.w, value.row1.w, value.row2.w, 1.0));
}

void main() {
    uint instanceIndex = primitives.values[gl_InstanceIndex].binding.x;
    uvec4 references = instances.values[instanceIndex].references;
    mat4 renderMatrix = affineMatrix(references.x);
    vec4 worldPos = renderMatrix * vec4(inPosition, 1.0);
    // M9.1: slot 2d + 1 is last frame's transform (settled, M9 G3).
    iridiumEmitMotion(worldPos, affineMatrix(references.y) * vec4(inPosition, 1.0));
    gl_Position = ubo.jitteredProjection * ubo.view * worldPos;
    fragColor = inColor;
    fragTexCoord0 = inTexCoord0;
    fragTexCoord1 = inTexCoord1;
    mat3 normalMatrix = transpose(inverse(mat3(renderMatrix)));
    fragNormal = normalMatrix * inNormal;
    fragTangent = vec4(mat3(renderMatrix) * inTangent.xyz, inTangent.w);
    fragWorldPos = worldPos.xyz;
}
