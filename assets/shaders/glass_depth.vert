#version 450

// Vertex Attributes
// We only need the position to calculate depth! The stride of your Vertex struct
// will still be handled correctly by the Vulkan pipeline.
layout(location = 0) in vec3 inPosition;
// (Locations 1, 2, 3 for normals/UVs/colors are ignored to save performance)

// SET 0: Global Camera UBO
layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 model; // Usually unused since we use push constants
    mat4 view;
    mat4 proj;
} ubo;

// PUSH CONSTANTS: Mesh Transform
layout(push_constant) uniform PushConstants {
    mat4 renderMatrix;
    vec4 baseColor;
    float metallicFactor;
    float roughnessFactor;
} push;

void main() {
    // Multiply the vertex position by the Model (Push Constant), View, and Projection (UBO) matrices
    gl_Position = ubo.proj * ubo.view * push.renderMatrix * vec4(inPosition, 1.0);
}