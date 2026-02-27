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

layout(set = 2, binding = 0) uniform sampler2D opaquePositionMap; 
layout(set = 2, binding = 1) uniform sampler2D opaqueNormalMap;
layout(set = 2, binding = 2) uniform sampler2D opaqueAlbedoMap;
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
    
    float metallic = push.metallicFactor * texture(metallicRoughnessMap, fragTexCoord).b;
    float roughness = push.roughnessFactor * texture(metallicRoughnessMap, fragTexCoord).g;

    vec3 cameraPos = inverse(ubo.view)[3].xyz;
    vec3 N = normalize(fragNormal);
    
    // 1. THE ONE-SIDED FIX: Flip the normal if we are looking at the inside of the glass!
    // This prevents the backfaces from returning negative dot products and turning into 100% mirrors.
    if (!gl_FrontFacing) {
        N = -N;
    }

    vec3 V = normalize(cameraPos - fragWorldPos);
    vec3 R = reflect(-V, N);
    
    // 2. THE UPSIDE-DOWN FIX: Invert the Y-axis for Vulkan's coordinate system
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
    // Physics: Thick glass absorbs light.
    vec3 absorbColor = vec3(1.0) - glassColor;
    vec3 physicalTransmittance = exp(-absorbColor * effectiveThickness * 0.5);

    // Art Direction: The material's alpha defines a minimum tint, even if paper-thin!
    // If the glTF model says the glass should be 40% opaque, we enforce that tint.
    float materialAlpha = push.baseColor.a * texColor.a;
    vec3 finalTransmittance = mix(physicalTransmittance, glassColor, materialAlpha);

    // ==========================================================
    // 4. SCREEN SPACE REFRACTION (Pure Physics)
    // ==========================================================
    // Distortion is now strictly driven by physical thickness and surface angle.
    // Extremely thin glass (windshield) will have thickness near 0.0, meaning zero distortion.
    // Thick glass (headlamps) will multiply the distortion.
    
    // We use (1.0 - dot(N, V)) so looking at glass at a steep angle refracts more than looking dead-on
    float angleOfIncidence = 1.0 - max(dot(N, V), 0.0);
    float physicalDistortion = (thickness * 0.1) * angleOfIncidence;
    
    // Add a tiny bit of scatter for roughness, but no artificial boost!
    float distortionStrength = physicalDistortion + (roughness * 0.01);

    vec2 distortedUV = screenUV + (N.xy * distortionStrength);
    distortedUV = clamp(distortedUV, vec2(0.001), vec2(0.999));

    vec3 refractionColor = texture(opaqueSceneCopyMap, distortedUV).rgb;
    vec3 tintedRefraction = refractionColor * finalTransmittance;

    vec3 reflectionColor = texture(hdriMap, SampleSphericalMap(R)).rgb;
    reflectionColor = pow(ACESFilm(reflectionColor * 1.5), vec3(1.0/2.2));

    vec3 F0 = mix(vec3(0.04), glassColor, metallic); 
    vec3 F = FresnelSchlick(max(dot(N, V), 0.0), F0);

    vec3 finalColor = mix(tintedRefraction, reflectionColor, F.r);

    // ==========================================================
    // 5. THE GHOSTING FIX & BASELINE OPACITY
    // ==========================================================
    // Ensure the baseline opacity is never lower than what the material requested
    float finalAlpha = clamp(materialAlpha + F.r + (thickness * 0.5) + roughness, 0.0, 1.0);

    // Because we already manually painted the background (refractionColor) into our finalColor,
    // we MUST tell Vulkan this is an opaque pixel. If alpha < 1.0, Vulkan blends the straight, 
    // un-refracted background back on top, causing the double image!
    outColor = vec4(finalColor, 1.0);
}