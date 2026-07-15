#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

// THE NEW OPTIMIZED BINDINGS (96 bits total!)
layout(set = 0, binding = 0) uniform sampler2D gDepth; 
layout(set = 0, binding = 1) uniform sampler2D gNormalRoughMetal;
layout(set = 0, binding = 2) uniform sampler2D gAlbedoEmissive;
layout(set = 0, binding = 3) uniform sampler2D hdriMap;

layout(push_constant) uniform PushConstants {
    vec3 viewPos;
    mat4 invView;
    mat4 invProj;
} push;

const float PI = 3.14159265359;

// --- PBR UTILITY FUNCTIONS ---
float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float nom   = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    return nom / max(PI * denom * denom, 0.0000001);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    float nom   = NdotV;
    float denom = NdotV * (1.0 - k) + k;
    return nom / denom;
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotL, roughness) * GeometrySchlickGGX(NdotV, roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

const vec2 invAtan = vec2(0.1591, 0.3183);
vec2 SampleSphericalMap(vec3 v) {
    vec2 uv = vec2(atan(v.z, v.x), asin(v.y));
    uv *= invAtan;
    uv += 0.5;
    return uv;
}

vec3 ACESFilm(vec3 x) {
    float a = 2.51f; float b = 0.03f; float c = 2.43f; 
    float d = 0.59f; float e = 0.14f;
    return clamp((x*(a*x+b))/(x*(c*x+d)+e), 0.0, 1.0);
}

// ==========================================================
// MAGIC TRICK 1: WORLD POSITION RECONSTRUCTION
// ==========================================================
vec3 ReconstructWorldPos(vec2 uv, float depth) {
    vec4 clipSpace = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 viewSpace = push.invProj * clipSpace;
    viewSpace /= viewSpace.w; 
    vec4 worldSpace = push.invView * viewSpace;
    return worldSpace.xyz;
}

void main() {
    // 1. READ THE RAW VRAM DATA
    float rawDepth = texture(gDepth, fragTexCoord).r;
    vec4 nrmSample = texture(gNormalRoughMetal, fragTexCoord);
    vec4 aeSample  = texture(gAlbedoEmissive, fragTexCoord);

    // 2. UNPACK & RECONSTRUCT EVERYTHING
    vec3 fragPos = ReconstructWorldPos(fragTexCoord, rawDepth);
    
    // ==========================================================
    // MAGIC TRICK 2: OCTAHEDRAL Z-NORMAL DECODING
    // ==========================================================
    // FIX: No more UNORM decoding! We just read the flawless float data directly.
    vec2 f = nrmSample.xy; 
    
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    vec3 N = normalize(n);
    
    // Extract the rest of the material properties
    float Roughness = nrmSample.b;
    float Metallic  = nrmSample.a;
    vec3 Albedo = pow(aeSample.rgb, vec3(2.2));

   // ==========================================================
    // MAGIC TRICK 3: EMISSIVE GLOW
    // ==========================================================
    // No fluff needed! Because your G-Buffer is SFLOAT, aeSample.a holds the true 1000.0 HDR value!

    float emissiveIntensity = aeSample.a;
    vec3 Emissive = max((Albedo * emissiveIntensity), 0.0);

    // 3. SKYBOX / BACKGROUND CHECK
    vec3 V = normalize(push.viewPos - fragPos);

    if (rawDepth == 1.0) { // If depth is 1.0, we are looking at the sky!
        vec2 ndc = fragTexCoord * 2.0 - 1.0;
        vec4 eye = push.invProj * vec4(ndc, 1.0, 1.0);
        vec3 viewDir = normalize((push.invView * vec4(eye.xy, -1.0, 0.0)).xyz);
        vec3 envColor = texture(hdriMap, SampleSphericalMap(viewDir)).rgb;
        envColor = envColor = ACESFilm(envColor * 1.5);
        envColor = clamp(envColor, 0.0, 10.0);
        outColor = vec4(envColor, 1.0);
        return;
    }

    // 4. PBR LIGHTING EQUATION
    vec3 F0 = mix(vec3(0.04), Albedo, Metallic);

    vec3 R = reflect(-V, N);
    vec3 reflectionColor = texture(hdriMap, SampleSphericalMap(R)).rgb;
    reflectionColor *= (1.0 - Roughness);

    // Fake Directional Light (Sun)
    vec3 L = normalize(vec3(1.0, 1.0, 1.0));
    vec3 H = normalize(V + L);
    vec3 radiance = vec3(3.0, 3.0, 3.0); 

    float NDF = DistributionGGX(N, H, Roughness);
    float G   = GeometrySmith(N, V, L, Roughness);      
    vec3 F    = FresnelSchlick(max(dot(H, V), 0.0), F0);
    vec3 specular = (NDF * G * F) / (4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001);
    vec3 kD = (vec3(1.0) - F) * (1.0 - Metallic);
    vec3 Lo = (kD * Albedo / PI + specular) * radiance * max(dot(N, L), 0.0);
    
    // Ambient IBL
    vec3 kS_IBL = FresnelSchlick(max(dot(N, V), 0.0), F0);
    vec3 kD_IBL = (1.0 - kS_IBL) * (1.0 - Metallic);
    vec3 ambient = (kD_IBL * (Albedo * 1.5)) + (reflectionColor * kS_IBL);

    // Combine Lighting + Emissive Glow!
    vec3 finalLitColor = ambient + Lo + Emissive;

    // --- NEW: CAMERA EXPOSURE ---
    // Multiply all incoming light by a small decimal to "stop down" the camera lens.
    // This stops the tonemapper from crushing the HDR values, revealing the true emissive steps!
    // float exposure = 0.05; 
    // finalLitColor *= exposure;

    outColor = vec4(ACESFilm(finalLitColor), 1.0);
}