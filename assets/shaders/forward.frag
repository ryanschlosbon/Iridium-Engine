#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec3 fragWorldPos;
layout(location = 4) in vec4 fragTangent;

// 1. THE FIX: Removed cameraPos to prevent memory misalignment!
layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 model;
    mat4 view;
    mat4 proj;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D albedoMap;
layout(set = 1, binding = 1) uniform sampler2D normalMap;
layout(set = 1, binding = 2) uniform sampler2D metallicRoughnessMap;
layout(set = 2, binding = 3) uniform sampler2D hdriMap;
layout(set = 2, binding = 4) uniform sampler2D opaqueSceneCopyMap;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    mat4 renderMatrix;
    vec4 baseColor;
    float metallicFactor;
    float roughnessFactor;
    vec2 padding;
} push;

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
    float a = 2.51f; float b = 0.03f; float c = 2.43f; float d = 0.59f; float e = 0.14f;
    return clamp((x*(a*x+b))/(x*(c*x+d)+e), 0.0, 1.0);
}

void main() {
    vec4 texColor = texture(albedoMap, fragTexCoord);
    float baseAlpha = texColor.a * push.baseColor.a;
    baseAlpha = min(baseAlpha, 0.15); // Glass Hack
    
    vec3 cameraPos = inverse(ubo.view)[3].xyz;
    vec3 N = normalize(fragNormal);
    vec3 V = normalize(cameraPos - fragWorldPos);
    vec3 R = reflect(-V, N); 

    // 1. Calculate perfect Screen-Space UV coordinates
    vec2 screenSize = vec2(textureSize(opaqueSceneCopyMap, 0));
    vec2 screenUV = gl_FragCoord.xy / screenSize;

    // 2. REFRACTION MATH! 
    // We bend the UV coordinates based on the XY tilt of the surface normal.
    // The distortionStrength dictates how drastically the light bends.
    float distortionStrength = 0.05; 
    vec2 distortedUV = screenUV + (N.xy * distortionStrength);
    
    // Clamp the UVs so the refraction doesn't accidentally sample off the edge of the screen
    distortedUV = clamp(distortedUV, vec2(0.001), vec2(0.999));

    // Sample the distorted background (The Photograph!)
    vec3 refractionColor = texture(opaqueSceneCopyMap, distortedUV).rgb;

    // 3. REFLECTION MATH
    vec3 reflectionColor = texture(hdriMap, SampleSphericalMap(R)).rgb;
    reflectionColor = pow(ACESFilm(reflectionColor * 1.5), vec3(1.0/2.2));

    vec3 F0 = vec3(0.04); 
    vec3 F = FresnelSchlick(max(dot(N, V), 0.0), F0);

    // 4. MIXING IT ALL TOGETHER
    vec3 glassTint = push.baseColor.rgb * fragColor * texColor.rgb;
    
    // Multiply the refraction by the tint of the glass
    vec3 tintedRefraction = refractionColor * mix(vec3(1.0), glassTint, 0.5);

    // Mix the Refraction (Background) with the Reflection (Foreground) using Fresnel!
    vec3 finalColor = mix(tintedRefraction, reflectionColor, F.r);
    
    // We add the Fresnel to the base alpha so the edges look solid, but the center is clear!
    float finalAlpha = clamp(baseAlpha + F.r, 0.0, 1.0);

    outColor = vec4(finalColor, finalAlpha);
}