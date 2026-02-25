#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D gPosition;
layout(set = 0, binding = 1) uniform sampler2D gNormal;
layout(set = 0, binding = 2) uniform sampler2D gAlbedo;
layout(set = 0, binding = 3) uniform sampler2D hdriMap;

layout(push_constant) uniform PushConstants {
    vec3 viewPos;
    mat4 invView;
    mat4 invProj;
} push;

const float PI = 3.14159265359;

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
    float a = 2.51f; float b = 0.03f; float c = 2.43f; float d = 0.59f; float e = 0.14f;
    return clamp((x*(a*x+b))/(x*(c*x+d)+e), 0.0, 1.0);
}

void main() {
    vec4 albedoSample = texture(gAlbedo, fragTexCoord);
    vec4 normalSample = texture(gNormal, fragTexCoord);
    vec4 positionSample = texture(gPosition, fragTexCoord);

    // CRITICAL: Linearize the Albedo NOW, after it safely left the 8-bit G-Buffer!
    vec3 Albedo = pow(albedoSample.rgb, vec3(2.2));
    
    vec3 N = normalSample.rgb;
    vec3 fragPos = positionSample.xyz;

    float Metallic  = albedoSample.a;
    float Roughness = normalSample.a;
    
    vec3 V = normalize(push.viewPos - fragPos);

    // Skybox render check
    if (length(N) < 0.1) {
        vec2 ndc = fragTexCoord * 2.0 - 1.0;
        vec4 eye = push.invProj * vec4(ndc, 1.0, 1.0);
        vec3 viewDir = normalize((push.invView * vec4(eye.xy, -1.0, 0.0)).xyz);
        vec3 envColor = texture(hdriMap, SampleSphericalMap(viewDir)).rgb;
        envColor = pow(ACESFilm(envColor * 1.5), vec3(1.0/2.2)); 
        outColor = vec4(envColor, 1.0);
        return;
    }

    N = normalize(N);
    vec3 F0 = mix(vec3(0.04), Albedo, Metallic);

    vec3 R = reflect(-V, N); 
    vec3 reflectionColor = texture(hdriMap, SampleSphericalMap(R)).rgb;

    // Fake Directional Light
    vec3 L = normalize(vec3(1.0, 1.0, 1.0)); 
    vec3 H = normalize(V + L);
    vec3 radiance = vec3(3.0, 3.0, 3.0); 

    float NDF = DistributionGGX(N, H, Roughness);   
    float G   = GeometrySmith(N, V, L, Roughness);      
    vec3 F    = FresnelSchlick(max(dot(H, V), 0.0), F0);       
    
    vec3 specular = (NDF * G * F) / (4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001);
    vec3 kD = (vec3(1.0) - F) * (1.0 - Metallic); 
    
    vec3 Lo = (kD * Albedo / PI + specular) * radiance * max(dot(N, L), 0.0);

    // AMBIENT IBL (Heavily Boosted so dark objects are visible)
    vec3 kS_IBL = FresnelSchlick(max(dot(N, V), 0.0), F0);
    vec3 kD_IBL = (1.0 - kS_IBL) * (1.0 - Metallic); 

    vec3 ambient = (kD_IBL * (Albedo * 1.5)) + (reflectionColor * kS_IBL);

    outColor = vec4(pow(ACESFilm(ambient + Lo), vec3(1.0/2.2)), 1.0);
}