#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec3 fragWorldPos;
layout(location = 4) in vec4 fragTangent;

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 model;
    mat4 view;
    mat4 proj;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D albedoMap;
layout(set = 1, binding = 1) uniform sampler2D normalMap;
layout(set = 1, binding = 2) uniform sampler2D metallicRoughnessMap;

layout(set = 2, binding = 0) uniform sampler2D gDepth; 
layout(set = 2, binding = 1) uniform sampler2D gNormalRoughMetal;
layout(set = 2, binding = 2) uniform sampler2D gAlbedoEmissive;
layout(set = 2, binding = 3) uniform sampler2D hdriMap;
layout(set = 2, binding = 4) uniform sampler2D opaqueSceneCopyMap;
layout(set = 2, binding = 5) uniform sampler2D glassDepthMap;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    mat4 renderMatrix;
    vec4 baseColor;
    float metallicFactor;
    float roughnessFactor;
    vec2 padding;
} push;

// --- UTILITY FUNCTIONS ---
vec2 SampleSphericalMap(vec3 v) {
    vec2 invAtan = vec2(0.1591, 0.3183);
    vec2 uv = vec2(atan(v.z, v.x), asin(v.y));
    uv *= invAtan;
    uv += 0.5;
    return uv;
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 ACESFilm(vec3 x) {
    float a = 2.51f; float b = 0.03f; float c = 2.43f;
    float d = 0.59f; float e = 0.14f;
    return clamp((x*(a*x+b))/(x*(c*x+d)+e), 0.0, 1.0);
}

float LinearizeDepth(float depth) {
    float near = 0.1;
    float far = 1000.0;
    float z = depth * 2.0 - 1.0; 
    return (2.0 * near * far) / (far + near - z * (far - near));
}

void main() {
    vec4 texColor = texture(albedoMap, fragTexCoord);
    vec3 glassColor = push.baseColor.rgb * fragColor * texColor.rgb;
    
    // We still read roughness from the texture so frosted glass works!
    float roughness = push.roughnessFactor * texture(metallicRoughnessMap, fragTexCoord).g;

    vec3 cameraPos = inverse(ubo.view)[3].xyz;
    
    // ==========================================================
    // 1. NORMAL MAP DECODING (With Tangent Safeguard)
    // ==========================================================
    vec3 N_geom = normalize(fragNormal);
    if (!gl_FrontFacing) {
        N_geom = -N_geom;
    }

    vec3 T;
    if (length(fragTangent.xyz) < 0.001) {
        T = cross(N_geom, vec3(0.0, 1.0, 0.0));
        if (length(T) < 0.001) {
            T = cross(N_geom, vec3(1.0, 0.0, 0.0));
        }
        T = normalize(T);
    } else {
        T = normalize(fragTangent.xyz);
    }
    
    T = normalize(T - dot(T, N_geom) * N_geom); 
    float handedness = (abs(fragTangent.w) < 0.001) ? 1.0 : fragTangent.w;
    vec3 B = normalize(cross(N_geom, T)) * handedness;
    mat3 TBN = mat3(T, B, N_geom);

    vec3 normalSample = texture(normalMap, fragTexCoord).rgb;
    normalSample.g = 1.0 - normalSample.g;
    vec3 N = normalize(TBN * (normalSample * 2.0 - 1.0));

    // ==========================================================

    vec3 V = normalize(cameraPos - fragWorldPos);
    vec3 R = reflect(-V, N);
    R.y = -R.y; 

    vec2 screenSize = vec2(textureSize(opaqueSceneCopyMap, 0));
    vec2 screenUV = gl_FragCoord.xy / screenSize;

    float frontFaceDepth = LinearizeDepth(texture(glassDepthMap, screenUV).r);
    float currentDepth = LinearizeDepth(gl_FragCoord.z);
    float thickness = max(currentDepth - frontFaceDepth, 0.0);
    float effectiveThickness = thickness + 0.05; 

    // ==========================================================
    // 3. BEER-LAMBERT LAW & BASE TINT
    // ==========================================================
    vec3 absorbColor = vec3(1.0) - glassColor;
    vec3 physicalTransmittance = exp(-absorbColor * effectiveThickness * 0.5);

    float materialAlpha = push.baseColor.a * texColor.a;
    vec3 finalTransmittance = mix(physicalTransmittance, glassColor, materialAlpha);

    // ==========================================================
    // 4. SCREEN SPACE REFRACTION (True Snell's Law Physics)
    // ==========================================================
    // In physics, the Index of Refraction (IOR) determines how much light bends.
    float IOR_Air = 1.0;
    float IOR_Glass = 1.52; // Automotive windshields/headlights are typically 1.52
    float eta = IOR_Air / IOR_Glass;

    // The refract() function calculates the exact 3D trajectory of the bent light ray
    vec3 refractedRay = refract(-V, N, eta);

    // If the ray hits at an extreme grazing angle, it completely reflects (Total Internal Reflection)
    // refract() returns a zero vector if this happens, so we fallback to no distortion
    if (length(refractedRay) < 0.001) {
        refractedRay = -V;
    }

    // We take the X and Y trajectory of the bent ray, and multiply it by how far it 
    // travels through the glass (thickness) to get our physical UV offset!
    // The 0.25 is a scaling factor to convert 3D world space units to 2D screen UV space.
    vec2 physicalDistortion = refractedRay.xy * thickness * 0.25;
    
    // Add microscopic scattering for frosted glass (Roughness)
    float scatter = roughness * 0.01;

    vec2 distortedUV = screenUV + physicalDistortion + (N.xy * scatter);
    distortedUV = clamp(distortedUV, vec2(0.001), vec2(0.999));

    vec3 refractionColor = texture(opaqueSceneCopyMap, distortedUV).rgb;
    vec3 tintedRefraction = refractionColor * finalTransmittance;

// Fallback Alpha Blending
    vec3 baseSurface = glassColor;
    
    // Add reflections
    vec3 reflectionColor = texture(hdriMap, SampleSphericalMap(R)).rgb;
    reflectionColor = clamp(reflectionColor, 0.0, 5.0); 
    reflectionColor = ACESFilm(reflectionColor * 1.5);
    
    vec3 F0 = vec3(0.04); 
    vec3 F = FresnelSchlick(max(dot(N, V), 0.0), F0);
    vec3 finalColor = baseSurface + (reflectionColor * F.r);
    
    // Output true alpha so the Vulkan pipeline blends it with the background!
    outColor = vec4(finalColor, materialAlpha);
}