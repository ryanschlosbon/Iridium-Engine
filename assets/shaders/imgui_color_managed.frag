#version 450 core

layout(location = 0) out vec4 fColor;
layout(set = 0, binding = 0) uniform sampler2D sTexture;
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;
layout(push_constant) uniform ImGuiPushConstants {
    layout(offset = 16) float displayColorScale;
    uint outputColorSpace;
} push;

vec3 decodeSrgb(vec3 encoded) {
    bvec3 low = lessThanEqual(encoded, vec3(0.04045));
    vec3 linearLow = encoded / 12.92;
    vec3 linearHigh = pow((encoded + 0.055) / 1.055, vec3(2.4));
    return mix(linearHigh, linearLow, low);
}

vec3 linearSrgbToLinearRec2020(vec3 value) {
    return mat3(
        0.627404, 0.069097, 0.016391,
        0.329283, 0.919540, 0.088013,
        0.043313, 0.011362, 0.895595) * value;
}

void main() {
    vec4 sampled = texture(sTexture, In.UV.st);
    vec3 vertexColor = decodeSrgb(In.Color.rgb);
    if (push.outputColorSpace == 1u) {
        vertexColor = linearSrgbToLinearRec2020(vertexColor);
    }
    fColor = vec4(vertexColor * sampled.rgb * push.displayColorScale,
        In.Color.a * sampled.a);
}
