#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec3 fragWorldPos;
layout(location = 4) in vec4 fragTangent;

layout(set = 1, binding = 0) uniform sampler2D albedoMap;
layout(set = 1, binding = 1) uniform sampler2D normalMap;
layout(set = 1, binding = 2) uniform sampler2D metallicRoughnessMap;

layout(location = 0) out vec4 outPosition;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outAlbedo;

layout(push_constant) uniform PushConstants {
    mat4 renderMatrix;
    vec4 baseColor;
    float metallicFactor;
    float roughnessFactor;
    vec2 padding;
} push;

void main() {
    // 1. ALBEDO & ALPHA DISCARD (No SRGB linearization here to prevent crushing!)
    vec4 texColor = texture(albedoMap, fragTexCoord);
    if (texColor.a < 0.1) { discard; } 

    // 2. METALLIC / ROUGHNESS 
    vec4 mrSample = texture(metallicRoughnessMap, fragTexCoord);
    float roughness = mrSample.g * push.roughnessFactor;
    float metallic = mrSample.b * push.metallicFactor;

    // 3. TRUE TANGENT SPACE NORMAL MAPPING
    vec3 N = normalize(fragNormal);
    vec3 T;
    
    // SAFEGUARD: Prevent division-by-zero NaN explosions on Polyhaven models!
    if (length(fragTangent.xyz) < 0.001) {
        T = cross(N, vec3(0.0, 1.0, 0.0));
        if (length(T) < 0.001) {
            T = cross(N, vec3(1.0, 0.0, 0.0));
        }
        T = normalize(T);
    } else {
        T = normalize(fragTangent.xyz);
    }
    
    // Calculate Bitangent and TBN matrix safely
    float handedness = (abs(fragTangent.w) < 0.001) ? 1.0 : fragTangent.w;
    vec3 B = normalize(cross(N, T)) * handedness;
    mat3 TBN = mat3(T, B, N);

    // Read and apply the normal map
    vec3 tangentNormal = texture(normalMap, fragTexCoord).rgb * 2.0 - 1.0;
    vec3 finalNormal = normalize(TBN * tangentNormal);

    // 4. PACK DATA INTO G-BUFFER
    outPosition = vec4(fragWorldPos, 1.0);
    outNormal   = vec4(finalNormal, roughness); // Pack Roughness in Alpha
    
    // Multiply push constants, vertex colors (for the Alfa), and textures!
    vec3 finalAlbedo = push.baseColor.rgb * fragColor * texColor.rgb;
    outAlbedo   = vec4(finalAlbedo, metallic); // Pack Metallic in Alpha
}