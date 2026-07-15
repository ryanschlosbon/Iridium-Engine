#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec3 fragWorldPos;
layout(location = 4) in vec4 fragTangent;

// The 3 Input Textures (Unchanged)
layout(set = 1, binding = 0) uniform sampler2D albedoMap;
layout(set = 1, binding = 1) uniform sampler2D normalMap;
layout(set = 1, binding = 2) uniform sampler2D metallicRoughnessMap;

// ==========================================================
// THE NEW 2-TARGET OUTPUTS (96 bits total per pixel)
// ==========================================================
// Target 0: Normal X, Normal Y, Roughness, Metallic
layout(location = 0) out vec4 outNormalRoughMetal;

// Target 1: Albedo R, G, B, Emissive Intensity
layout(location = 1) out vec4 outAlbedoEmissive;

layout(push_constant) uniform PushConstants {
    mat4 renderMatrix;
    vec4 baseColor;
    float metallicFactor;
    float roughnessFactor;
    float emissiveFactor; // <--- Claimed from padding!
    float padding;        // <--- Keeps the struct 4-byte aligned
} push;

vec2 OctWrap(vec2 v) {
    return (1.0 - abs(v.yx)) * mix(vec2(-1.0), vec2(1.0), step(vec2(0.0), v));
}

void main() {
    // 1. ALBEDO & ALPHA DISCARD
    vec4 texColor = texture(albedoMap, fragTexCoord);
    if (texColor.a < 0.1) { discard; } 

    // 2. METALLIC / ROUGHNESS 
    vec4 mrSample = texture(metallicRoughnessMap, fragTexCoord);
    float roughness = mrSample.g * push.roughnessFactor;
    float metallic = mrSample.b * push.metallicFactor;

    // 3. TRUE TANGENT SPACE NORMAL MAPPING
    vec3 N = normalize(fragNormal);
    vec3 T;

    // Polyhaven Safeguard
    if (length(fragTangent.xyz) < 0.001) {
        T = cross(N, vec3(0.0, 1.0, 0.0));
        if (length(T) < 0.001) {
            T = cross(N, vec3(1.0, 0.0, 0.0));
        }
        T = normalize(T);
    } else {
        T = normalize(fragTangent.xyz);
    }

    float handedness = (abs(fragTangent.w) < 0.001) ? 1.0 : fragTangent.w;
    vec3 B = normalize(cross(N, T)) * handedness;
    mat3 TBN = mat3(T, B, N);

    vec3 normalSample = texture(normalMap, fragTexCoord).rgb;
    normalSample.g = 1.0 - normalSample.g;
    N = normalize(TBN * (normalSample * 2.0 - 1.0));

    // ==========================================================
    // 4. THE G-BUFFER PACKING 
    // ==========================================================
    // Fold the 3D normal into a 2D Octahedron
    vec3 n = N;
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    n.xy = n.z >= 0.0 ? n.xy : OctWrap(n.xy);

    // FIX: Because we upgraded back to SFLOAT, we don't need the UNORM * 0.5 + 0.5 bias!
    // We just write the pure, flawless float data directly to VRAM.
    outNormalRoughMetal = vec4(n.xy, roughness, metallic);
    vec3 finalColor = texColor.rgb * push.baseColor.rgb;
    outAlbedoEmissive = vec4(finalColor, push.emissiveFactor);

}