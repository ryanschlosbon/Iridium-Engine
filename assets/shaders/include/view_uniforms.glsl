#ifndef IRIDIUM_VIEW_UNIFORMS_GLSL
#define IRIDIUM_VIEW_UNIFORMS_GLSL

// The set-0 view uniform block: C++ `UniformBufferObject` (renderer/rhi/Mesh.h),
// filled by VulkanViewUniforms. The single GLSL declaration (M9 G5a);
// ShaderAbiContractTests checks every shader that uses it.
layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
    mat4 inverseView;
    mat4 inverseProjection;
    vec4 cameraPosition;
    vec4 depthRange;
    uvec4 renderInfo;
    vec4 worldUnits;
    // M9 G5b: raster stages and depth reconstruction use the jittered pair;
    // proj/inverseProjection stay unjittered (culling, Hi-Z, clusters).
    mat4 jitteredProjection;
    mat4 jitteredInverseProjection;
    mat4 previousViewProjection;   // previous turn, unjittered
    vec4 jitter;                   // xy current NDC jitter, zw previous
    uvec4 temporalInfo;            // x sequence index, y turns since cut, z flags
} ubo;

#endif
