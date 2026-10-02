#ifndef IRIDIUM_MATERIAL_COMPLEX_GLSL
#define IRIDIUM_MATERIAL_COMPLEX_GLSL

#include "material_bsdf.glsl"

vec3 materialEvaluateSpecularLobe(vec3 f0, vec3 f90,
    float perceptualRoughness, vec3 normal, vec3 view, vec3 light) {
    vec3 halfVector = normalize(view + light);
    float noV = max(dot(normal, view), 0.0);
    float noL = max(dot(normal, light), 0.0);
    float distribution = materialDistributionGgx(
        max(dot(normal, halfVector), 0.0), perceptualRoughness);
    float geometry = materialGeometrySmith(noV, noL, perceptualRoughness);
    vec3 fresnel = materialFresnelSchlick(f0, f90,
        max(dot(halfVector, view), 0.0));
    return distribution * geometry * fresnel /
        max(4.0 * noV * noL, MATERIAL_BSDF_DENOMINATOR_EPSILON);
}

float materialDistributionAnisotropicGgx(vec3 normal, vec3 tangent,
    vec3 bitangent, vec3 halfVector, float perceptualRoughness,
    float anisotropy) {
    float alpha = materialGgxAlpha(perceptualRoughness);
    float aspect = sqrt(max(1.0 - 0.9 * clamp(anisotropy, 0.0, 1.0), 0.1));
    float alphaX = max(alpha / aspect, 0.001);
    float alphaY = max(alpha * aspect, 0.001);
    float tx = dot(tangent, halfVector) / alphaX;
    float by = dot(bitangent, halfVector) / alphaY;
    float nh = max(dot(normal, halfVector), 0.0);
    float denominator = tx * tx + by * by + nh * nh;
    return 1.0 / max(MATERIAL_PI * alphaX * alphaY *
        denominator * denominator, MATERIAL_BSDF_DENOMINATOR_EPSILON);
}

vec3 materialEvaluateAnisotropicSpecular(vec3 f0, vec3 f90,
    float perceptualRoughness, float anisotropy, float rotation,
    MaterialTangentFrame frame, vec3 view, vec3 light) {
    float c = cos(rotation);
    float s = sin(rotation);
    vec3 tangent = frame.tangent * c + frame.bitangent * s;
    vec3 bitangent = -frame.tangent * s + frame.bitangent * c;
    vec3 halfVector = normalize(view + light);
    float noV = max(dot(frame.normal, view), 0.0);
    float noL = max(dot(frame.normal, light), 0.0);
    float distribution = materialDistributionAnisotropicGgx(frame.normal,
        tangent, bitangent, halfVector, perceptualRoughness, anisotropy);
    float geometry = materialGeometrySmith(noV, noL, perceptualRoughness);
    vec3 fresnel = materialFresnelSchlick(f0, f90,
        max(dot(halfVector, view), 0.0));
    return distribution * geometry * fresnel /
        max(4.0 * noV * noL, MATERIAL_BSDF_DENOMINATOR_EPSILON);
}

vec3 materialEvaluateSheen(vec3 color, float perceptualRoughness,
    vec3 normal, vec3 view, vec3 light) {
    vec3 halfVector = normalize(view + light);
    float inverseHalf = clamp(1.0 - max(dot(normal, halfVector), 0.0), 0.0, 1.0);
    float grazing = pow(inverseHalf, mix(5.0, 1.0,
        materialSanitizePerceptualRoughness(perceptualRoughness)));
    return color * grazing / MATERIAL_PI;
}

vec3 materialIridescenceTint(float ior, float thicknessNm, float cosTheta) {
    vec3 wavelengths = vec3(650.0, 510.0, 475.0);
    vec3 phase = 4.0 * MATERIAL_PI * max(ior, 1.0) * max(thicknessNm, 0.0) *
        max(cosTheta, 0.0) / wavelengths;
    return clamp(vec3(0.5) + 0.5 * cos(phase), vec3(0.0), vec3(1.0));
}

#endif
