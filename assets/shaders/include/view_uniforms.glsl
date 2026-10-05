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
} ubo;

#endif
