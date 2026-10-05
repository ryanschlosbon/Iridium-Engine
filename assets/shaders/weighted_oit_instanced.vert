#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord0;
layout(location = 4) in vec4 inTangent;
layout(location = 5) in vec2 inTexCoord1;
layout(location = 6) in vec4 inInstanceMatrix0;
layout(location = 7) in vec4 inInstanceMatrix1;
layout(location = 8) in vec4 inInstanceMatrix2;
layout(location = 9) in vec4 inInstanceMatrix3;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragTexCoord0;
layout(location = 2) out vec3 fragNormal;
layout(location = 3) out vec3 fragWorldPos;
layout(location = 4) out vec4 fragTangent;
layout(location = 5) out vec2 fragTexCoord1;

#include "include/view_uniforms.glsl"

layout(push_constant) uniform CanonicalPushConstants {
    mat4 renderMatrix;
    uint materialIndex;
    uint padding0;
    uint padding1;
    uint padding2;
} push;

void main() {
    mat4 renderMatrix = mat4(inInstanceMatrix0, inInstanceMatrix1,
        inInstanceMatrix2, inInstanceMatrix3);
    vec4 worldPos = renderMatrix * vec4(inPosition, 1.0);
    gl_Position = ubo.proj * ubo.view * worldPos;
    fragColor = inColor;
    fragTexCoord0 = inTexCoord0;
    fragTexCoord1 = inTexCoord1;
    mat3 normalMatrix = transpose(inverse(mat3(renderMatrix)));
    fragNormal = normalMatrix * inNormal;
    fragTangent = vec4(mat3(renderMatrix) * inTangent.xyz, inTangent.w);
    fragWorldPos = worldPos.xyz;
}
