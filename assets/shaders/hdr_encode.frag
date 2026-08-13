#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D displayLinear;
layout(push_constant) uniform HdrEncodePushConstants {
    float paperWhiteNits;
    float peakNits;
} push;

vec3 encodeSt2084FromNits(vec3 nits) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}

void main() {
    vec3 rec2020Nits = min(max(texture(displayLinear, fragTexCoord).rgb, 0.0) *
        push.paperWhiteNits, vec3(push.peakNits));
    outColor = vec4(encodeSt2084FromNits(rec2020Nits), 1.0);
}
