#ifndef IRIDIUM_MATERIAL_BSDF_GLSL
#define IRIDIUM_MATERIAL_BSDF_GLSL

const float MATERIAL_PI = 3.14159265358979323846;
const float MATERIAL_MIN_PERCEPTUAL_ROUGHNESS = 0.04;
const float MATERIAL_BSDF_DENOMINATOR_EPSILON = 1.0e-7;

float materialSanitizePerceptualRoughness(float value) {
    return clamp(value, 0.0, 1.0);
}

float materialGgxAlpha(float perceptualRoughness) {
    float roughness = max(materialSanitizePerceptualRoughness(
        perceptualRoughness), MATERIAL_MIN_PERCEPTUAL_ROUGHNESS);
    return roughness * roughness;
}

vec3 materialFresnelSchlick(vec3 f0, vec3 f90, float cosTheta) {
    float weight = pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
    return f0 + (f90 - f0) * weight;
}

float materialDistributionGgx(float noH, float perceptualRoughness) {
    float alpha = materialGgxAlpha(perceptualRoughness);
    float alphaSquared = alpha * alpha;
    float clampedNoH = clamp(noH, 0.0, 1.0);
    float denominatorTerm = clampedNoH * clampedNoH *
        (alphaSquared - 1.0) + 1.0;
    return alphaSquared / max(MATERIAL_PI * denominatorTerm *
        denominatorTerm, MATERIAL_BSDF_DENOMINATOR_EPSILON);
}

float materialGeometrySchlickGgx(float noX, float perceptualRoughness) {
    float roughness = materialSanitizePerceptualRoughness(perceptualRoughness);
    float radius = roughness + 1.0;
    float k = radius * radius / 8.0;
    float clampedNoX = clamp(noX, 0.0, 1.0);
    return clampedNoX / max(clampedNoX * (1.0 - k) + k,
        MATERIAL_BSDF_DENOMINATOR_EPSILON);
}

float materialGeometrySmith(float noV, float noL,
    float perceptualRoughness) {
    return materialGeometrySchlickGgx(noV, perceptualRoughness) *
        materialGeometrySchlickGgx(noL, perceptualRoughness);
}

vec3 materialDiffuseWeight(vec3 fresnel, float metallic) {
    return (vec3(1.0) - fresnel) * (1.0 - clamp(metallic, 0.0, 1.0));
}

vec3 materialEvaluateStandardBrdf(vec3 baseColor, vec3 f0, vec3 f90,
    float metallic, float perceptualRoughness, vec3 normal, vec3 view,
    vec3 light) {
    vec3 halfVector = normalize(view + light);
    float noV = max(dot(normal, view), 0.0);
    float noL = max(dot(normal, light), 0.0);
    float distribution = materialDistributionGgx(
        max(dot(normal, halfVector), 0.0), perceptualRoughness);
    float geometry = materialGeometrySmith(noV, noL, perceptualRoughness);
    vec3 fresnel = materialFresnelSchlick(f0, f90,
        max(dot(halfVector, view), 0.0));
    vec3 specular = distribution * geometry * fresnel /
        max(4.0 * noV * noL, MATERIAL_BSDF_DENOMINATOR_EPSILON);
    return materialDiffuseWeight(fresnel, metallic) * baseColor /
        MATERIAL_PI + specular;
}

vec3 materialEvaluateCanonicalBrdf(vec3 diffuseAlbedo, vec3 f0, vec3 f90,
    float perceptualRoughness, vec3 normal, vec3 view, vec3 light) {
    vec3 halfVector = normalize(view + light);
    float noV = max(dot(normal, view), 0.0);
    float noL = max(dot(normal, light), 0.0);
    float distribution = materialDistributionGgx(
        max(dot(normal, halfVector), 0.0), perceptualRoughness);
    float geometry = materialGeometrySmith(noV, noL, perceptualRoughness);
    vec3 fresnel = materialFresnelSchlick(f0, f90,
        max(dot(halfVector, view), 0.0));
    vec3 specular = distribution * geometry * fresnel /
        max(4.0 * noV * noL, MATERIAL_BSDF_DENOMINATOR_EPSILON);
    return (vec3(1.0) - fresnel) * diffuseAlbedo / MATERIAL_PI + specular;
}

#endif
