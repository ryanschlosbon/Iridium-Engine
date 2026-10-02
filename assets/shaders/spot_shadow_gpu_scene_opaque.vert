#version 450

layout(location = 0) in vec3 inPosition;

struct SpotShadowEntry {
    mat4 worldToShadowClip;
    vec4 atlasScaleBias;
    uvec4 metadata;
    vec4 biasParameters;
    vec4 projectionParameters;
    uvec4 filterMetadata;
};
layout(std140, set = 0, binding = 0) uniform SpotShadowData {
    SpotShadowEntry entries[256];
    uvec4 metadata;
} shadowData;
layout(push_constant) uniform CanonicalPushConstants {
    mat4 renderMatrix;
    uint materialIndex;
    uint shadowDataSlot;
    uint padding1;
    uint padding2;
} push;
struct GpuSceneTransform { vec4 row0; vec4 row1; vec4 row2; };
struct GpuSceneInstance {
    vec4 worldBoundsSphere; vec4 worldBoundsMin; vec4 worldBoundsMax;
    uvec4 references; uvec4 state;
};
struct GpuScenePrimitive { uvec4 binding; uvec4 state; uvec4 revisions; };
layout(std430, set = 3, binding = 0) readonly buffer Transforms {
    GpuSceneTransform values[];
} transforms;
layout(std430, set = 3, binding = 1) readonly buffer Instances {
    GpuSceneInstance values[];
} instances;
layout(std430, set = 3, binding = 2) readonly buffer Primitives {
    GpuScenePrimitive values[];
} primitives;

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
    gl_Position = shadowData.entries[push.shadowDataSlot].worldToShadowClip *
        renderMatrixForPrimitive(primitiveIndex) * vec4(inPosition, 1.0);
}
