#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 3) in vec2 inTexCoord0;
layout(location = 5) in vec2 inTexCoord1;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragTexCoord0;
layout(location = 2) out vec2 fragTexCoord1;
layout(location = 3) flat out uint fragMaterialIndex;

struct PointShadowEntry {
    mat4 worldToShadowClip[6];
    vec4 lightPositionFar;
    uvec4 metadata;
    vec4 depthBias;
    vec4 filterParameters;
    uvec4 filterMetadata;
};
layout(std140, set = 0, binding = 0) uniform PointShadowData {
    PointShadowEntry entries[56];
    uvec4 metadata;
} shadowData;

layout(push_constant) uniform CanonicalPushConstants {
    mat4 renderMatrix;
    uint materialIndex;
    uint shadowFaceSlot;
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
layout(std430, set = 3, binding = 0) readonly buffer Transforms {
    GpuSceneTransform values[];
} transforms;
layout(std430, set = 3, binding = 1) readonly buffer Instances {
    GpuSceneInstance values[];
} instances;
layout(std430, set = 3, binding = 2) readonly buffer Primitives {
    GpuScenePrimitive values[];
} primitives;
layout(std430, set = 3, binding = 3) readonly buffer Geometries {
    GpuSceneGeometry values[];
} geometries;

mat4 renderMatrixForPrimitive(uint primitiveIndex) {
    uint instanceIndex = primitives.values[primitiveIndex].binding.x;
    uint transformIndex = instances.values[instanceIndex].references.x;
    GpuSceneTransform value = transforms.values[transformIndex];
    return mat4(
        vec4(value.row0.x, value.row1.x, value.row2.x, 0.0),
        vec4(value.row0.y, value.row1.y, value.row2.y, 0.0),
        vec4(value.row0.z, value.row1.z, value.row2.z, 0.0),
        vec4(value.row0.w, value.row1.w, value.row2.w, 1.0));
}

void main() {
    uint primitiveIndex = gl_InstanceIndex;
    GpuScenePrimitive primitive = primitives.values[primitiveIndex];
    uint entry = push.shadowFaceSlot / 6u;
    uint face = push.shadowFaceSlot % 6u;
    gl_Position = shadowData.entries[entry].worldToShadowClip[face] *
        renderMatrixForPrimitive(primitiveIndex) * vec4(inPosition, 1.0);
    fragColor = inColor;
    fragTexCoord0 = inTexCoord0;
    fragTexCoord1 = inTexCoord1;
    fragMaterialIndex = primitive.binding.z;
}
