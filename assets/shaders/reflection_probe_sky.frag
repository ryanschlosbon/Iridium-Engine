#version 450

layout(location = 0) in vec2 clipPosition;
layout(location = 0) out vec4 outColor;
layout(std140, set = 0, binding = 0) uniform CaptureFaceData {
    mat4 worldToClip;
    mat4 clipToWorld;
    vec4 capturePositionNear;
    uvec4 metadata;
} capture;
layout(std140, set = 3, binding = 15) uniform ClusterParameters {
    mat4 view;
    mat4 projection;
    uvec4 grid;
    vec4 depth;
    uvec4 limits;
    uvec4 inputData;
    vec4 environmentSettings;
};
layout(set = 3, binding = 19) uniform samplerCube environmentRadiance;

void main() {
    if (capture.metadata.y == 0u) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec4 world = capture.clipToWorld * vec4(clipPosition, 1.0, 1.0);
    vec3 direction = normalize(world.xyz / world.w -
        capture.capturePositionNear.xyz);
    float sineYaw = sin(environmentSettings.z);
    float cosineYaw = cos(environmentSettings.z);
    direction = vec3(cosineYaw * direction.x - sineYaw * direction.z,
        direction.y, sineYaw * direction.x + cosineYaw * direction.z);
    int flags = int(environmentSettings.w + 0.5);
    float scale = (flags & 1) != 0 ? environmentSettings.y : 0.0;
    outColor = vec4(texture(environmentRadiance, direction).rgb * scale, 1.0);
}
