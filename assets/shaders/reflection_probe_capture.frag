#version 450
#extension GL_EXT_nonuniform_qualifier : require

#include "include/scene_color.glsl"
#include "include/packed_material.glsl"
#include "include/material_normal.glsl"
#include "include/material_bsdf.glsl"

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragTexCoord0;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec3 fragWorldPos;
layout(location = 4) in vec4 fragTangent;
layout(location = 5) in vec2 fragTexCoord1;
layout(location = 0) out vec4 outColor;

layout(std140, set = 0, binding = 0) uniform CaptureFaceData {
    mat4 worldToClip;
    mat4 clipToWorld;
    vec4 capturePositionNear;
    uvec4 metadata;
} capture;
layout(std430, set = 1, binding = 0) readonly buffer MaterialTable {
    PackedMaterial materials[];
};
layout(set = 1, binding = 1) uniform texture2D materialTextureViews[];
layout(set = 2, binding = 0) uniform sampler materialSamplers[];

#define IRIDIUM_LIGHTING_SET 3
#include "include/clustered_light_access.glsl"
#include "include/environment_ibl.glsl"
#include "include/directional_shadow.glsl"
#include "include/spot_shadow.glsl"
#include "include/point_shadow.glsl"

layout(push_constant) uniform CanonicalPushConstants {
    mat4 renderMatrix;
    uint materialIndex;
    uint padding0;
    uint padding1;
    uint padding2;
} push;

vec4 sampleMaterialTexture(PackedMaterial material, uint semantic) {
    vec2 uv = packedMaterialUv(material, semantic,
        fragTexCoord0, fragTexCoord1);
    return texture(sampler2D(
        materialTextureViews[nonuniformEXT(material.textureIndices[semantic])],
        materialSamplers[nonuniformEXT(
            packedMaterialSamplerIndex(material, semantic))]), uv);
}

vec4 sampleOrOne(PackedMaterial material, uint semantic) {
    return packedMaterialHasTexture(material, semantic)
        ? sampleMaterialTexture(material, semantic) : vec4(1.0);
}

void main() {
    PackedMaterial material = materials[push.materialIndex];
    if (material.schemaVersion != MATERIAL_SCHEMA_VERSION) discard;
    vec4 baseSample = sampleOrOne(material, MATERIAL_TEXTURE_BASE_COLOR);
    float alpha = baseSample.a * material.baseColorFactor.a * fragColor.a;
    if (material.alphaMode == 1u && alpha < material.surfaceParameters.y)
        discard;
    vec3 baseColor = linearSrgbToAcesCg(baseSample.rgb *
        material.baseColorFactor.rgb * fragColor.rgb);
    vec4 mrSample = sampleOrOne(material,
        MATERIAL_TEXTURE_METALLIC_ROUGHNESS);
    float roughness = clamp(mrSample.g *
        material.metallicRoughnessIorSpecular.y, 0.0, 1.0);
    float metallic = clamp(mrSample.b *
        material.metallicRoughnessIorSpecular.x, 0.0, 1.0);
    float ior = max(material.metallicRoughnessIorSpecular.z, 1.0);
    float specularWeight = material.metallicRoughnessIorSpecular.w *
        sampleOrOne(material, 12u).a;
    vec3 specularColor = material.specularColorNormalScale.rgb *
        sampleOrOne(material, 13u).rgb;
    float dielectric = pow((ior - 1.0) / (ior + 1.0), 2.0);
    vec3 f0 = mix(linearSrgbToAcesCg(dielectric * specularColor) *
        specularWeight, baseColor, metallic);
    vec3 f90 = vec3(mix(specularWeight, 1.0, metallic));
    vec3 diffuse = baseColor * (1.0 - metallic);
    if (material.workflow == 1u) {
        vec4 diffuseSample = sampleOrOne(material, 5u);
        vec4 specGlossSample = sampleOrOne(material, 6u);
        diffuse = linearSrgbToAcesCg(diffuseSample.rgb *
            material.diffuseFactor.rgb * fragColor.rgb);
        f0 = linearSrgbToAcesCg(specGlossSample.rgb *
            material.specularGlossinessFactorGloss.rgb);
        f90 = vec3(1.0);
        roughness = 1.0 - material.specularGlossinessFactorGloss.a *
            specGlossSample.a;
    }
    float handedness = abs(fragTangent.w) < 0.001 ? 1.0 : fragTangent.w;
    MaterialTangentFrame frame = materialBuildTangentFrame(fragNormal,
        fragTangent.xyz, handedness, material.doubleSided != 0u,
        gl_FrontFacing);
    if (packedMaterialHasTexture(material, MATERIAL_TEXTURE_NORMAL)) {
        vec3 encoded = sampleMaterialTexture(
            material, MATERIAL_TEXTURE_NORMAL).rgb;
        if (packedMaterialReconstructNormalZ(material,
                MATERIAL_TEXTURE_NORMAL))
            encoded = materialReconstructEncodedNormalZ(encoded);
        frame.normal = materialApplyTangentNormal(frame, encoded,
            packedMaterialScalar(material, MATERIAL_TEXTURE_NORMAL));
    }
    float ao = 1.0;
    if (packedMaterialHasTexture(material, MATERIAL_TEXTURE_OCCLUSION))
        ao = mix(1.0, sampleMaterialTexture(material,
            MATERIAL_TEXTURE_OCCLUSION).r,
            packedMaterialScalar(material, MATERIAL_TEXTURE_OCCLUSION));
    vec3 emissive = material.emissiveFactorStrength.rgb *
        material.emissiveFactorStrength.a;
    if (packedMaterialHasTexture(material, MATERIAL_TEXTURE_EMISSIVE))
        emissive *= sampleMaterialTexture(material,
            MATERIAL_TEXTURE_EMISSIVE).rgb;
    emissive = linearSrgbToAcesCg(emissive);

    vec3 N = normalize(frame.normal);
    vec3 V = normalize(capture.capturePositionNear.xyz - fragWorldPos);
    vec3 direct = vec3(0.0);
    uint lightCount = min(capture.metadata.x, iridiumClusterInput.x);
    float viewDepth = length(capture.capturePositionNear.xyz - fragWorldPos);
    for (uint lightSlot = 0u; lightSlot < lightCount; ++lightSlot) {
        PackedGpuLight record = iridiumLights[lightSlot];
        IridiumDirectLightSample light = iridiumEvaluateDirectLight(
            record, fragWorldPos, N);
        float noL = max(dot(N, light.direction), 0.0);
        if (!(noL > 0.0)) continue;
        float visibility = iridiumDirectionalShadowVisibility(lightSlot,
            fragWorldPos, N, light.direction, viewDepth);
        uint type = iridiumPackedLightType(record);
        if (type == IRIDIUM_LIGHT_TYPE_SPOT)
            visibility *= iridiumSpotShadowVisibility(lightSlot, record,
                fragWorldPos, N, light.direction);
        else if (type == IRIDIUM_LIGHT_TYPE_POINT)
            visibility *= iridiumPointShadowVisibility(lightSlot, record,
                fragWorldPos, N, light.direction);
        direct += materialEvaluateCanonicalBrdf(diffuse, f0, f90,
            roughness, N, V, light.direction) * light.radiance * noL *
            visibility;
    }
    vec3 ambient = (iridiumEnvironmentDiffuse(diffuse, f0, f90, N, V) +
        iridiumEnvironmentSpecular(f0, f90, roughness, N, V)) *
        clamp(ao, 0.0, 1.0);
    outColor = vec4(max(ambient + direct + emissive, vec3(0.0)), 1.0);
}
