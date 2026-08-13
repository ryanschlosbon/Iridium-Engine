#include "include/scene_color.glsl"
#include "include/packed_material.glsl"
#include "include/material_normal.glsl"

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragTexCoord0;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec3 fragWorldPos;
layout(location = 4) in vec4 fragTangent;
layout(location = 5) in vec2 fragTexCoord1;

#if defined(IRIDIUM_INDEXED_MATERIAL_TEXTURES)
#extension GL_EXT_nonuniform_qualifier : require
layout(std430, set = 1, binding = 0) readonly buffer MaterialTable {
    PackedMaterial materials[];
};
layout(set = 1, binding = 1) uniform texture2D materialTextureViews[];
layout(set = 2, binding = 0) uniform sampler materialSamplers[];
#else
layout(set = 1, binding = 0) uniform sampler2D baseColorMap;
layout(set = 1, binding = 1) uniform sampler2D metallicRoughnessMap;
layout(set = 1, binding = 2) uniform sampler2D normalMap;
layout(set = 1, binding = 3) uniform sampler2D occlusionMap;
layout(set = 1, binding = 4) uniform sampler2D emissiveMap;
layout(set = 1, binding = 5) uniform sampler2D diffuseMap;
layout(set = 1, binding = 6) uniform sampler2D specularGlossinessMap;
layout(set = 1, binding = 12) uniform sampler2D specularMap;
layout(set = 1, binding = 13) uniform sampler2D specularColorMap;
layout(set = 1, binding = 17) uniform sampler2D transmissionMap;
layout(std430, set = 1, binding = 21) readonly buffer MaterialTable {
    PackedMaterial materials[];
};
#endif

#if defined(IRIDIUM_PACKED_GBUFFER)
layout(location = 0) out vec2 outNormalF90;
#else
layout(location = 0) out vec4 outNormalF90;
#endif
layout(location = 1) out vec4 outDiffuseAo;
layout(location = 2) out vec4 outEmissive;
layout(location = 3) out vec4 outF0Roughness;
layout(location = 4) out uint outMaterialFlags;

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
#if defined(IRIDIUM_INDEXED_MATERIAL_TEXTURES)
    uint viewIndex = material.textureIndices[semantic];
    uint samplerIndex = packedMaterialSamplerIndex(material, semantic);
    return texture(sampler2D(
        materialTextureViews[nonuniformEXT(viewIndex)],
        materialSamplers[nonuniformEXT(samplerIndex)]), uv);
#else
    if (semantic == 0u) return texture(baseColorMap, uv);
    if (semantic == 1u) return texture(metallicRoughnessMap, uv);
    if (semantic == 2u) return texture(normalMap, uv);
    if (semantic == 3u) return texture(occlusionMap, uv);
    if (semantic == 4u) return texture(emissiveMap, uv);
    if (semantic == 5u) return texture(diffuseMap, uv);
    if (semantic == 6u) return texture(specularGlossinessMap, uv);
    if (semantic == 12u) return texture(specularMap, uv);
    if (semantic == 13u) return texture(specularColorMap, uv);
    if (semantic == 17u) return texture(transmissionMap, uv);
    return vec4(1.0);
#endif
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
    if (material.alphaMode == 1u && alpha < material.surfaceParameters.y) discard;
    vec3 baseColor = linearSrgbToAcesCg(baseSample.rgb *
        material.baseColorFactor.rgb * fragColor.rgb);

    vec4 mrSample = sampleOrOne(
        material, MATERIAL_TEXTURE_METALLIC_ROUGHNESS);
    float roughness = clamp(mrSample.g *
        material.metallicRoughnessIorSpecular.y, 0.0, 1.0);
    float metallic = clamp(mrSample.b *
        material.metallicRoughnessIorSpecular.x, 0.0, 1.0);
    float ior = max(material.metallicRoughnessIorSpecular.z, 1.0);
    float specularWeight = material.metallicRoughnessIorSpecular.w *
        sampleOrOne(material, 12u).a;
    vec3 specularColor = material.specularColorNormalScale.rgb *
        sampleOrOne(material, 13u).rgb;
    float dielectricScalar = pow((ior - 1.0) / (ior + 1.0), 2.0);
    vec3 dielectricF0 = linearSrgbToAcesCg(
        dielectricScalar * specularColor) * specularWeight;
    vec3 f0 = mix(dielectricF0, baseColor, metallic);
    float f90 = mix(specularWeight, 1.0, metallic);
    vec3 diffuse = baseColor * (1.0 - metallic);

    if (material.workflow == 1u) {
        vec4 diffuseSample = sampleOrOne(material, 5u);
        vec4 specGlossSample = sampleOrOne(material, 6u);
        diffuse = linearSrgbToAcesCg(diffuseSample.rgb *
            material.diffuseFactor.rgb * fragColor.rgb);
        f0 = linearSrgbToAcesCg(specGlossSample.rgb *
            material.specularGlossinessFactorGloss.rgb);
        f90 = 1.0;
        roughness = 1.0 - material.specularGlossinessFactorGloss.a *
            specGlossSample.a;
    }

    float handedness = abs(fragTangent.w) < 0.001 ? 1.0 : fragTangent.w;
    MaterialTangentFrame frame = materialBuildTangentFrame(fragNormal,
        fragTangent.xyz, handedness, material.doubleSided != 0u, gl_FrontFacing);
    if (packedMaterialHasTexture(material, MATERIAL_TEXTURE_NORMAL)) {
        vec3 encodedNormal = sampleMaterialTexture(
            material, MATERIAL_TEXTURE_NORMAL).rgb;
        if (packedMaterialReconstructNormalZ(
            material, MATERIAL_TEXTURE_NORMAL))
            encodedNormal =
                materialReconstructEncodedNormalZ(
                    encodedNormal);
        frame.normal = materialApplyTangentNormal(frame,
            encodedNormal,
            packedMaterialScalar(material, MATERIAL_TEXTURE_NORMAL));
    }

    float ao = 1.0;
    if (packedMaterialHasTexture(material, MATERIAL_TEXTURE_OCCLUSION)) {
        float sampledAo = sampleMaterialTexture(
            material, MATERIAL_TEXTURE_OCCLUSION).r;
        ao = mix(1.0, sampledAo,
            packedMaterialScalar(material, MATERIAL_TEXTURE_OCCLUSION));
    }

    vec3 emissive = material.emissiveFactorStrength.rgb *
        material.emissiveFactorStrength.a;
    if (packedMaterialHasTexture(material, MATERIAL_TEXTURE_EMISSIVE))
        emissive *= sampleMaterialTexture(
            material, MATERIAL_TEXTURE_EMISSIVE).rgb;

#if defined(IRIDIUM_PACKED_GBUFFER)
    vec3 octNormal = frame.normal / (abs(frame.normal.x) + abs(frame.normal.y) +
        abs(frame.normal.z));
    if (octNormal.z < 0.0) {
        octNormal.xy = (1.0 - abs(octNormal.yx)) * sign(octNormal.xy);
    }
    outNormalF90 = octNormal.xy;
#else
    outNormalF90 = vec4(frame.normal, f90);
#endif
    outDiffuseAo = vec4(diffuse, ao);
    outEmissive = vec4(linearSrgbToAcesCg(emissive), 0.0);
    outF0Roughness = vec4(f0, clamp(roughness, 0.0, 1.0));
    uint packedFlags = (push.materialIndex & 0x000fffffu) |
        ((material.featureFlags & 0x000003ffu) << 20u) |
        ((material.closureClass & 0x3u) << 30u);
#if defined(IRIDIUM_PACKED_GBUFFER)
    // Q/C intentionally exercise the frozen 16-bit metadata proposal. The
    // reference comparison rejects any source that cannot survive this truncation.
    outMaterialFlags = packedFlags & 0xffffu;
#else
    outMaterialFlags = packedFlags;
#endif
}
