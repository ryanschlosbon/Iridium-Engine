#version 450

layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
} ubo;

// NEW: Receive the matrix from the Command Buffer
layout(push_constant) uniform PushConstants {
    mat4 renderMatrix;
} pushConst;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord;

void main() {
    // USE pushConst.renderMatrix HERE:
    gl_Position = ubo.proj * ubo.view * pushConst.renderMatrix * vec4(inPosition, 1.0);
    
    fragColor = inColor;
    fragTexCoord = inTexCoord;
}