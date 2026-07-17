#version 450

// Inputs from Vertex Buffer
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;
layout(location = 4) in vec4 inTangent;

// Outputs to Fragment Shader (Must match fragment shader inputs exactly!)
layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) out vec3 fragNormal;
layout(location = 3) out vec3 fragWorldPos;
layout(location = 4) out vec4 fragTangent;

layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
} ubo;

layout(push_constant) uniform PushConstants {
    mat4 renderMatrix;
    vec4 baseColor;
    vec4 emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float normalScale;
    float alphaCutoff;
    float transmissionFactor;
    float padding;
} push;

void main() {
    // 1. Calculate World Position
    vec4 worldPos = push.renderMatrix * vec4(inPosition, 1.0);
    gl_Position = ubo.proj * ubo.view * worldPos;

    // 2. Pass standard data
    fragColor = inColor;
    fragTexCoord = inTexCoord;
    
    // 3. Calculate accurate World Normals 
    // (Using transpose-inverse prevents normals from warping if the object is scaled)
    mat3 normalMatrix = transpose(inverse(mat3(push.renderMatrix)));
    fragNormal = normalMatrix * inNormal;
    
    // 4. Rotate the tangent to world space
    fragTangent = vec4(mat3(push.renderMatrix) * inTangent.xyz, inTangent.w);

    // 5. Pass the world position to the fragment shader
    fragWorldPos = worldPos.xyz;
}
