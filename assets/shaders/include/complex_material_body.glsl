#include "include/scene_color.glsl"
#include "include/packed_material.glsl"
#include "include/material_normal.glsl"
#include "include/material_complex.glsl"
#include "include/transparency_transport.glsl"

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragTexCoord0;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec3 fragWorldPos;
layout(location = 4) in vec4 fragTangent;
layout(location = 5) in vec2 fragTexCoord1;

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 model;
    mat4 view;
    mat4 proj;
    mat4 inverseView;
    mat4 inverseProjection;
    vec4 cameraPosition;
    vec4 depthRange;
    uvec4 renderInfo;
    vec4 worldUnits;
} ubo;

#if defined(IRIDIUM_INDEXED_MATERIAL_TEXTURES)
#extension GL_EXT_nonuniform_qualifier : require
layout(std430, set = 1, binding = 0) readonly buffer MaterialTable {
    PackedMaterial materials[];
};
layout(set = 1, binding = 1) uniform texture2D materialTextureViews[];
layout(set = 2, binding = 0) uniform sampler materialSamplers[];
#else
layout(set = 1, binding = 0) uniform sampler2D texture0;
layout(set = 1, binding = 1) uniform sampler2D texture1;
layout(set = 1, binding = 2) uniform sampler2D texture2;
layout(set = 1, binding = 3) uniform sampler2D texture3;
layout(set = 1, binding = 4) uniform sampler2D texture4;
layout(set = 1, binding = 5) uniform sampler2D texture5;
layout(set = 1, binding = 6) uniform sampler2D texture6;
layout(set = 1, binding = 7) uniform sampler2D texture7;
layout(set = 1, binding = 8) uniform sampler2D texture8;
layout(set = 1, binding = 9) uniform sampler2D texture9;
layout(set = 1, binding = 10) uniform sampler2D texture10;
layout(set = 1, binding = 11) uniform sampler2D texture11;
layout(set = 1, binding = 12) uniform sampler2D texture12;
layout(set = 1, binding = 13) uniform sampler2D texture13;
layout(set = 1, binding = 14) uniform sampler2D texture14;
layout(set = 1, binding = 15) uniform sampler2D texture15;
layout(set = 1, binding = 16) uniform sampler2D texture16;
layout(set = 1, binding = 17) uniform sampler2D texture17;
layout(set = 1, binding = 18) uniform sampler2D texture18;
layout(set = 1, binding = 19) uniform sampler2D texture19;
layout(set = 1, binding = 20) uniform sampler2D texture20;
layout(std430, set = 1, binding = 21) readonly buffer MaterialTable {
    PackedMaterial materials[];
};
#endif

#if defined(IRIDIUM_INDEXED_MATERIAL_TEXTURES)
#define IRIDIUM_SCENE_SET 3
#else
#define IRIDIUM_SCENE_SET 2
#endif
#define IRIDIUM_LIGHTING_SET IRIDIUM_SCENE_SET
#include "include/clustered_light_access.glsl"
#include "include/environment_ibl.glsl"
#include "include/directional_shadow.glsl"
#include "include/spot_shadow.glsl"
#include "include/point_shadow.glsl"

layout(set = IRIDIUM_SCENE_SET, binding = 0) uniform sampler2D gDepth;
layout(set = IRIDIUM_SCENE_SET, binding = 1) uniform sampler2D gNormalRoughMetal;
layout(set = IRIDIUM_SCENE_SET, binding = 2) uniform sampler2D gAlbedoEmissive;
#ifndef IRIDIUM_OPAQUE_FORWARD
layout(set = IRIDIUM_SCENE_SET, binding = 4) uniform sampler2D
    refractionColorPyramid;
layout(set = IRIDIUM_SCENE_SET, binding = 5) uniform sampler2D
    refractionDepthPyramid;
#endif

#if defined(IRIDIUM_LAYERED_ORDINARY2_COMPOSITION) || \
    defined(IRIDIUM_LAYERED_DEEP_COMPOSITION) || \
    defined(IRIDIUM_LAYERED_DEEP_RESIDUAL)
#define IRIDIUM_LAYERED_LOCAL_COMPOSITION 1
#endif

#ifdef IRIDIUM_LAYERED_ORDINARY2_COMPOSITION
// Kept outside the shared scene set so the ordinary forward pipelines retain
// their existing descriptor-layout compatibility. The composition object owns
// this small, conditionally resident set with the packed-atlas interface data.
layout(set = 4, binding = 0) uniform sampler2D layeredEntryDepth;
layout(set = 4, binding = 1) uniform usampler2D layeredEntryIdentity;
layout(set = 4, binding = 2) uniform sampler2D layeredExitDepth;
layout(set = 4, binding = 3) uniform usampler2D layeredExitIdentity;
#elif defined(IRIDIUM_LAYERED_DEEP_COMPOSITION) || \
    defined(IRIDIUM_LAYERED_DEEP_RESIDUAL)
// Fixed capacity keeps the descriptor layout identical for Hero4 and
// Cinematic8. Hero4 repeats its final descriptor in unused slots, which the
// pushed interface count makes unreachable.
layout(set = 4, binding = 0) uniform sampler2D
    layeredInterfaceDepth[8];
layout(set = 4, binding = 1) uniform usampler2D
    layeredInterfaceIdentity[8];
#endif

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform CanonicalPushConstants {
    mat4 renderMatrix;
    uint materialIndex;
    uint padding0;
    uint padding1;
    uint padding2;
} push;

#ifdef IRIDIUM_LAYERED_LOCAL_COMPOSITION
const uint IRIDIUM_LAYERED_ORIENTATION_BIT = 0x80000000u;
const uint IRIDIUM_LAYERED_WORK_MASK = 0x7fffffffu;
const uint IRIDIUM_LAYERED_DEEP_WORK_MASK = 0x00001fffu;

int iridiumLayeredUnpackSigned16(uint value) {
    return int((value & 0xffffu) ^ 0x8000u) - 0x8000;
}

ivec2 iridiumLayeredAtlasPixel() {
    return ivec2(gl_FragCoord.xy);
}

ivec2 iridiumLayeredScenePixel() {
    ivec2 viewportOffset = ivec2(
        iridiumLayeredUnpackSigned16(push.padding2),
        iridiumLayeredUnpackSigned16(push.padding2 >> 16u));
    return iridiumLayeredAtlasPixel() + viewportOffset;
}

bool iridiumLayeredMirrored() {
    return (push.padding1 & 1u) != 0u;
}

#ifdef IRIDIUM_LAYERED_ORDINARY2_COMPOSITION

bool iridiumLayeredOrdinary2PairIsValid() {
    ivec2 atlasPixel = iridiumLayeredAtlasPixel();
    ivec2 atlasExtent = textureSize(layeredEntryDepth, 0);
    if (any(lessThan(atlasPixel, ivec2(0))) ||
        any(greaterThanEqual(atlasPixel, atlasExtent)))
        return false;
    uint oneBasedWork = push.padding0 + 1u;
    if (oneBasedWork == 0u ||
        (oneBasedWork & ~IRIDIUM_LAYERED_WORK_MASK) != 0u)
        return false;
    uint entryIdentity = texelFetch(layeredEntryIdentity,
        atlasPixel, 0).r;
    uint exitIdentity = texelFetch(layeredExitIdentity,
        atlasPixel, 0).r;
    if (entryIdentity != oneBasedWork ||
        exitIdentity != (oneBasedWork | IRIDIUM_LAYERED_ORIENTATION_BIT))
        return false;
    float entryDepth = texelFetch(layeredEntryDepth, atlasPixel, 0).r;
    float exitDepth = texelFetch(layeredExitDepth, atlasPixel, 0).r;
    return !(isnan(entryDepth) || isinf(entryDepth) ||
        isnan(exitDepth) || isinf(exitDepth)) && exitDepth > entryDepth;
}

float iridiumLayeredOrdinary2PathMeters(float authoredMaximumMeters) {
    ivec2 scenePixel = iridiumLayeredScenePixel();
    ivec2 sceneExtent = textureSize(gDepth, 0);
    if (any(lessThan(scenePixel, ivec2(0))) ||
        any(greaterThanEqual(scenePixel, sceneExtent)))
        return 0.0;
    ivec2 atlasPixel = iridiumLayeredAtlasPixel();
    float entryDepth = texelFetch(layeredEntryDepth, atlasPixel, 0).r;
    float exitDepth = texelFetch(layeredExitDepth, atlasPixel, 0).r;
    vec2 sceneUv = (vec2(scenePixel) + vec2(0.5)) / vec2(sceneExtent);
    vec3 entryView = iridiumReconstructViewPosition(sceneUv,
        entryDepth, ubo.inverseProjection);
    vec3 exitView = iridiumReconstructViewPosition(sceneUv,
        exitDepth, ubo.inverseProjection);
    float measuredChordMeters = length(exitView - entryView) *
        max(iridiumTransparencyFiniteOr(ubo.worldUnits.x, 1.0), 0.0);
    authoredMaximumMeters = max(iridiumTransparencyFiniteOr(
        authoredMaximumMeters, 0.0), 0.0);
    return authoredMaximumMeters > 0.0
        ? min(measuredChordMeters, authoredMaximumMeters)
        : measuredChordMeters;
}
#else
uint iridiumLayeredDeepInterfaceIndex() {
    return (push.padding1 >> 8u) & 0xffu;
}

uint iridiumLayeredDeepInterfaceCount() {
    return (push.padding1 >> 16u) & 0xffu;
}

bool iridiumLayeredDeepFindPair(out float entryDepth,
    out float exitDepth) {
    ivec2 atlasPixel = iridiumLayeredAtlasPixel();
    ivec2 atlasExtent = textureSize(layeredInterfaceDepth[0], 0);
    if (any(lessThan(atlasPixel, ivec2(0))) ||
        any(greaterThanEqual(atlasPixel, atlasExtent)))
        return false;
    uint interfaceIndex = iridiumLayeredDeepInterfaceIndex();
    uint interfaceCount = iridiumLayeredDeepInterfaceCount();
    if (interfaceCount < 2u || interfaceCount > 8u ||
        interfaceIndex >= interfaceCount)
        return false;
    uint oneBasedWork = push.padding0 + 1u;
    if (oneBasedWork == 0u ||
        (oneBasedWork & ~IRIDIUM_LAYERED_WORK_MASK) != 0u)
        return false;
    uint entryIdentity = texelFetch(
        layeredInterfaceIdentity[interfaceIndex], atlasPixel, 0).r;
    if ((entryIdentity & IRIDIUM_LAYERED_DEEP_WORK_MASK) != oneBasedWork)
        return false;
    entryDepth = texelFetch(layeredInterfaceDepth[interfaceIndex],
        atlasPixel, 0).r;
    if (isnan(entryDepth) || isinf(entryDepth))
        return false;
    // Identity alone is insufficient for a non-convex work item that appears
    // more than once in the same pixel. Matching the rerasterized fragment to
    // the captured slot prevents duplicate shading.
    float depthTolerance = max(2.0e-6, abs(entryDepth) * 2.0e-6);
    if (abs(gl_FragCoord.z - entryDepth) > depthTolerance)
        return false;
    for (uint candidate = interfaceIndex + 1u;
        candidate < interfaceCount; ++candidate) {
        uint identity = texelFetch(layeredInterfaceIdentity[candidate],
            atlasPixel, 0).r;
        if ((identity & IRIDIUM_LAYERED_DEEP_WORK_MASK) != oneBasedWork)
            continue;
        if ((identity & IRIDIUM_LAYERED_ORIENTATION_BIT) == 0u)
            return false;
        exitDepth = texelFetch(layeredInterfaceDepth[candidate],
            atlasPixel, 0).r;
        return !(isnan(exitDepth) || isinf(exitDepth)) &&
            exitDepth > entryDepth;
    }
    return false;
}

bool iridiumLayeredDeepWorkOpenAtCapacity() {
    ivec2 atlasPixel = iridiumLayeredAtlasPixel();
    ivec2 atlasExtent = textureSize(layeredInterfaceDepth[0], 0);
    if (any(lessThan(atlasPixel, ivec2(0))) ||
        any(greaterThanEqual(atlasPixel, atlasExtent)))
        return false;
    uint interfaceCount = iridiumLayeredDeepInterfaceCount();
    if (interfaceCount < 2u || interfaceCount > 8u)
        return false;
    uint oneBasedWork = push.padding0 + 1u;
    if (oneBasedWork == 0u ||
        (oneBasedWork & ~IRIDIUM_LAYERED_WORK_MASK) != 0u)
        return false;
    uint finalIdentity = texelFetch(
        layeredInterfaceIdentity[interfaceCount - 1u], atlasPixel, 0).r;
    if ((finalIdentity & IRIDIUM_LAYERED_DEEP_WORK_MASK) == 0u)
        return false;
    int balance = 0;
    for (uint candidate = 0u; candidate < interfaceCount; ++candidate) {
        uint identity = texelFetch(
            layeredInterfaceIdentity[candidate], atlasPixel, 0).r;
        if ((identity & IRIDIUM_LAYERED_DEEP_WORK_MASK) != oneBasedWork)
            continue;
        balance += (identity & IRIDIUM_LAYERED_ORIENTATION_BIT) == 0u
            ? 1 : -1;
    }
    return balance > 0;
}

bool iridiumLayeredDeepResidualEntryIsValid() {
    ivec2 atlasPixel = iridiumLayeredAtlasPixel();
    ivec2 atlasExtent = textureSize(layeredInterfaceDepth[0], 0);
    if (any(lessThan(atlasPixel, ivec2(0))) ||
        any(greaterThanEqual(atlasPixel, atlasExtent)))
        return false;
    uint interfaceCount = iridiumLayeredDeepInterfaceCount();
    if (interfaceCount < 2u || interfaceCount > 8u)
        return false;
    uint finalIdentity = texelFetch(
        layeredInterfaceIdentity[interfaceCount - 1u], atlasPixel, 0).r;
    if ((finalIdentity & IRIDIUM_LAYERED_DEEP_WORK_MASK) == 0u)
        return false;
    float finalDepth = texelFetch(
        layeredInterfaceDepth[interfaceCount - 1u], atlasPixel, 0).r;
    if (isnan(finalDepth) || isinf(finalDepth))
        return false;
    float depthTolerance = max(2.0e-6, abs(finalDepth) * 2.0e-6);
    return gl_FragCoord.z > finalDepth + depthTolerance;
}

float iridiumLayeredDeepPathMeters(float authoredMaximumMeters) {
    float entryDepth = 0.0;
    float exitDepth = 0.0;
    if (!iridiumLayeredDeepFindPair(entryDepth, exitDepth))
        return 0.0;
    ivec2 scenePixel = iridiumLayeredScenePixel();
    ivec2 sceneExtent = textureSize(gDepth, 0);
    if (any(lessThan(scenePixel, ivec2(0))) ||
        any(greaterThanEqual(scenePixel, sceneExtent)))
        return 0.0;
    vec2 sceneUv = (vec2(scenePixel) + vec2(0.5)) / vec2(sceneExtent);
    vec3 entryView = iridiumReconstructViewPosition(sceneUv,
        entryDepth, ubo.inverseProjection);
    vec3 exitView = iridiumReconstructViewPosition(sceneUv,
        exitDepth, ubo.inverseProjection);
    float measuredChordMeters = length(exitView - entryView) *
        max(iridiumTransparencyFiniteOr(ubo.worldUnits.x, 1.0), 0.0);
    authoredMaximumMeters = max(iridiumTransparencyFiniteOr(
        authoredMaximumMeters, 0.0), 0.0);
    return authoredMaximumMeters > 0.0
        ? min(measuredChordMeters, authoredMaximumMeters)
        : measuredChordMeters;
}
#endif

#define IRIDIUM_MATERIAL_SCENE_PIXEL uvec2(iridiumLayeredScenePixel())
#else
#define IRIDIUM_MATERIAL_SCENE_PIXEL uvec2(gl_FragCoord.xy)
#endif

vec4 sampleMaterialTexture(PackedMaterial material, uint semantic) {
    vec2 uv = packedMaterialUv(material, semantic, fragTexCoord0, fragTexCoord1);
#if defined(IRIDIUM_INDEXED_MATERIAL_TEXTURES)
    uint viewIndex = material.textureIndices[semantic];
    uint samplerIndex = packedMaterialSamplerIndex(material, semantic);
    return texture(sampler2D(
        materialTextureViews[nonuniformEXT(viewIndex)],
        materialSamplers[nonuniformEXT(samplerIndex)]), uv);
#else
    if (semantic == 0u) return texture(texture0, uv);
    if (semantic == 1u) return texture(texture1, uv);
    if (semantic == 2u) return texture(texture2, uv);
    if (semantic == 3u) return texture(texture3, uv);
    if (semantic == 4u) return texture(texture4, uv);
    if (semantic == 5u) return texture(texture5, uv);
    if (semantic == 6u) return texture(texture6, uv);
    if (semantic == 7u) return texture(texture7, uv);
    if (semantic == 8u) return texture(texture8, uv);
    if (semantic == 9u) return texture(texture9, uv);
    if (semantic == 10u) return texture(texture10, uv);
    if (semantic == 11u) return texture(texture11, uv);
    if (semantic == 12u) return texture(texture12, uv);
    if (semantic == 13u) return texture(texture13, uv);
    if (semantic == 14u) return texture(texture14, uv);
    if (semantic == 15u) return texture(texture15, uv);
    if (semantic == 16u) return texture(texture16, uv);
    if (semantic == 17u) return texture(texture17, uv);
    if (semantic == 18u) return texture(texture18, uv);
    if (semantic == 19u) return texture(texture19, uv);
    return texture(texture20, uv);
#endif
}

vec4 materialSampleOrOne(PackedMaterial material, uint semantic) {
    return packedMaterialHasTexture(material, semantic)
        ? sampleMaterialTexture(material, semantic) : vec4(1.0);
}

void main() {
    PackedMaterial material = materials[push.materialIndex];
    if (material.schemaVersion != MATERIAL_SCHEMA_VERSION) discard;
    bool layeredNonRefractiveResidual = false;
#ifdef IRIDIUM_LAYERED_ORDINARY2_COMPOSITION
    uint resolvedTransparencyClass =
        (material.transparencyPolicy >> 8u) & 0xffu;
    bool mirrored = push.padding1 != 0u;
    bool semanticEntry = gl_FrontFacing != mirrored;
    if (resolvedTransparencyClass != 5u || !semanticEntry ||
        !iridiumLayeredOrdinary2PairIsValid())
        discard;
#elif defined(IRIDIUM_LAYERED_DEEP_COMPOSITION)
    uint resolvedTransparencyClass =
        (material.transparencyPolicy >> 8u) & 0xffu;
    bool mirrored = iridiumLayeredMirrored();
    bool semanticEntry = gl_FrontFacing != mirrored;
    float layeredEntryDepth = 0.0;
    float layeredExitDepth = 0.0;
    bool hasExactPair = iridiumLayeredDeepFindPair(
        layeredEntryDepth, layeredExitDepth);
    if (resolvedTransparencyClass != 5u || !semanticEntry ||
        !hasExactPair)
        discard;
#elif defined(IRIDIUM_LAYERED_DEEP_RESIDUAL)
    uint resolvedTransparencyClass =
        (material.transparencyPolicy >> 8u) & 0xffu;
    bool mirrored = iridiumLayeredMirrored();
    bool semanticEntry = gl_FrontFacing != mirrored;
    bool closesOpenPrefix = !semanticEntry &&
        iridiumLayeredDeepWorkOpenAtCapacity();
    layeredNonRefractiveResidual = true;
    if (resolvedTransparencyClass != 5u ||
        (!semanticEntry && !closesOpenPrefix) ||
        !iridiumLayeredDeepResidualEntryIsValid())
        discard;
#endif

    vec4 baseSample = materialSampleOrOne(material, 0u);
    vec3 baseColor = linearSrgbToAcesCg(
        baseSample.rgb * material.baseColorFactor.rgb * fragColor.rgb);
    float alpha = clamp(baseSample.a * material.baseColorFactor.a *
        fragColor.a, 0.0, 1.0);
    if (material.alphaMode == 1u && alpha < material.surfaceParameters.y) discard;

    vec4 mrSample = materialSampleOrOne(material, 1u);
    float metallic = clamp(mrSample.b *
        material.metallicRoughnessIorSpecular.x, 0.0, 1.0);
    float roughness = clamp(mrSample.g *
        material.metallicRoughnessIorSpecular.y, 0.0, 1.0);
    float ior = max(material.metallicRoughnessIorSpecular.z, 1.0);
    float specularWeight = material.metallicRoughnessIorSpecular.w *
        materialSampleOrOne(material, 12u).a;
    vec3 specularColor = material.specularColorNormalScale.rgb *
        materialSampleOrOne(material, 13u).rgb;
    float dielectricScalar = pow((ior - 1.0) / (ior + 1.0), 2.0);
    vec3 dielectricF0 = linearSrgbToAcesCg(
        dielectricScalar * specularColor) * specularWeight;
    vec3 f0 = mix(dielectricF0, baseColor, metallic);
    vec3 f90 = vec3(mix(specularWeight, 1.0, metallic));

    if (material.workflow == 1u) {
        vec4 diffuseSample = materialSampleOrOne(material, 5u);
        vec4 specGlossSample = materialSampleOrOne(material, 6u);
        baseColor = linearSrgbToAcesCg(diffuseSample.rgb *
            material.diffuseFactor.rgb * fragColor.rgb);
        vec3 specular = linearSrgbToAcesCg(specGlossSample.rgb *
            material.specularGlossinessFactorGloss.rgb);
        baseColor *= 1.0 - max(specular.r, max(specular.g, specular.b));
        f0 = specular;
        f90 = vec3(1.0);
        metallic = 0.0;
        roughness = 1.0 - material.specularGlossinessFactorGloss.a *
            specGlossSample.a;
        alpha = diffuseSample.a * material.diffuseFactor.a * fragColor.a;
    }

    float handedness = abs(fragTangent.w) < 0.001 ? 1.0 : fragTangent.w;
#ifdef IRIDIUM_LAYERED_LOCAL_COMPOSITION
    if (iridiumLayeredMirrored())
#else
    if (push.padding1 != 0u)
#endif
        handedness = -handedness;
    MaterialTangentFrame frame = materialBuildTangentFrame(fragNormal,
        fragTangent.xyz, handedness, material.doubleSided != 0u, gl_FrontFacing);
    if (packedMaterialHasTexture(material, 2u)) {
        vec3 encodedNormal =
            sampleMaterialTexture(material, 2u).rgb;
        if (packedMaterialReconstructNormalZ(material, 2u))
            encodedNormal =
                materialReconstructEncodedNormalZ(
                    encodedNormal);
        frame.normal = materialApplyTangentNormal(frame,
            encodedNormal,
            material.specularColorNormalScale.a);
    }

    float ao = packedMaterialHasTexture(material, 3u)
        ? mix(1.0, sampleMaterialTexture(material, 3u).r,
            material.surfaceParameters.x) : 1.0;
    vec3 emissive = material.emissiveFactorStrength.rgb *
        material.emissiveFactorStrength.a;
    if (packedMaterialHasTexture(material, 4u))
        emissive *= sampleMaterialTexture(material, 4u).rgb;
    emissive = linearSrgbToAcesCg(emissive);

#ifndef IRIDIUM_LAYERED_LOCAL_COMPOSITION
    if (push.padding0 != 0u && push.padding0 != 15u &&
        push.padding0 != 16u && push.padding0 != 17u) {
        if (push.padding0 == 1u) outColor = vec4(baseColor, 1.0);
        else if (push.padding0 == 2u)
            outColor = vec4(frame.normal * 0.5 + 0.5, 1.0);
        else if (push.padding0 == 3u)
            outColor = vec4(vec3(roughness), 1.0);
        else if (push.padding0 == 4u)
            outColor = vec4(vec3(metallic), 1.0);
        else if (push.padding0 == 5u) outColor = vec4(emissive, 1.0);
        else if (push.padding0 == 7u) outColor = vec4(vec3(ao), 1.0);
        else if (push.padding0 == 8u) outColor = vec4(f0, 1.0);
        else if (push.padding0 == 9u) outColor = vec4(f90, 1.0);
        else if (push.padding0 == 10u ||
            push.padding0 == 11u || push.padding0 == 12u) {
            uint value = push.materialIndex;
            if (push.padding0 == 11u) value = material.featureFlags;
            else if (push.padding0 == 12u) value = material.closureClass;
            uint hash = value * 1664525u + 1013904223u;
            outColor = vec4(vec3(float(hash & 255u),
                float((hash >> 8u) & 255u),
                float((hash >> 16u) & 255u)) / 255.0, 1.0);
        }
        else outColor = vec4(vec3(gl_FragCoord.z), 1.0);
        return;
    }
#endif

    float shadowViewDepth = -(ubo.view * vec4(fragWorldPos, 1.0)).z;
    if (push.padding0 == 16u) {
        outColor = vec4(iridiumDirectionalShadowCascadeDebug(
            shadowViewDepth), alpha);
        return;
    }

    if (material.closureClass == 3u) {
        outColor = push.padding0 == 15u
            ? vec4(0.0, 0.0, 0.0, alpha)
            : push.padding0 == 17u ? vec4(1.0, 1.0, 1.0, alpha)
            : vec4(baseColor, alpha);
        return;
    }

    vec3 cameraPosition = ubo.cameraPosition.xyz;
    vec3 view = normalize(cameraPosition - fragWorldPos);

    vec3 reflection = textureLod(iridiumEnvironmentPrefiltered,
        reflect(-view, frame.normal), roughness * max(float(textureQueryLevels(
            iridiumEnvironmentPrefiltered)) - 1.0, 0.0)).rgb;
    vec3 baseLayerAttenuation = vec3(1.0);
    uint directLobeTypes[8];
    vec4 directLobeData[8];
    vec3 directLobeNormals[8];

#ifndef IRIDIUM_OPAQUE_FORWARD
    float transmission = 0.0;
    float transmissionIor = ior;
    float transmissionSpecularWeight = specularWeight;
    vec3 transmissionSpecularColor = specularColor;
    float volumeThickness = 0.0;
    float attenuationDistance = 3.402823e38;
    vec3 attenuationColor = vec3(1.0);
    float diffuseTransmission = 0.0;
    vec3 diffuseTransmissionColor = vec3(1.0);
#endif

    // Resolve view-dependent layer attenuation and transmission parameters once.
    // Every direct light below then evaluates the same authored closure stack.
    for (uint index = 0u; index < material.complexLobeCount; ++index) {
        PackedComplexLobe lobe = material.complexLobes[index];
        directLobeTypes[index] = lobe.type;
        if (lobe.type == 0u) {
            float factor = lobe.parameters[0] * materialSampleOrOne(material, 7u).r;
            float coatRoughness = lobe.parameters[1] *
                materialSampleOrOne(material, 8u).g;
            MaterialTangentFrame coatFrame = frame;
            if (packedMaterialHasTexture(material, 9u)) {
                vec3 encodedCoatNormal =
                    sampleMaterialTexture(material, 9u).rgb;
                if (packedMaterialReconstructNormalZ(
                    material, 9u))
                    encodedCoatNormal =
                        materialReconstructEncodedNormalZ(
                            encodedCoatNormal);
                coatFrame.normal = materialApplyTangentNormal(frame,
                    encodedCoatNormal,
                    lobe.parameters[2]);
            }
            vec3 coatF = materialFresnelSchlick(vec3(0.04), vec3(1.0),
                max(dot(coatFrame.normal, view), 0.0)) * factor;
            baseLayerAttenuation *= vec3(1.0) - coatF;
            directLobeData[index] = vec4(factor, coatRoughness, 0.0, 0.0);
            directLobeNormals[index] = coatFrame.normal;
        }
        else if (lobe.type == 1u) {
            vec3 color = linearSrgbToAcesCg(vec3(lobe.parameters[0],
                lobe.parameters[1], lobe.parameters[2]) *
                materialSampleOrOne(material, 10u).rgb);
            float sheenRoughness = lobe.parameters[3] *
                materialSampleOrOne(material, 11u).a;
            directLobeData[index] = vec4(color, sheenRoughness);
        }
        else if (lobe.type == 2u) {
            vec3 anisotropySample = materialSampleOrOne(material, 14u).rgb;
            float strength = clamp(lobe.parameters[0] * anisotropySample.b,
                0.0, 1.0);
            float textureRotation = atan(anisotropySample.y * 2.0 - 1.0,
                anisotropySample.x * 2.0 - 1.0);
            directLobeData[index] = vec4(strength,
                lobe.parameters[1] + textureRotation, 0.0, 0.0);
        }
        else if (lobe.type == 3u) {
            float factor = lobe.parameters[0] *
                materialSampleOrOne(material, 15u).r;
            float thicknessMix = materialSampleOrOne(material, 16u).g;
            float thickness = mix(lobe.parameters[2], lobe.parameters[3],
                thicknessMix);
            vec3 tint = materialIridescenceTint(lobe.parameters[1], thickness,
                max(dot(frame.normal, view), 0.0));
            directLobeData[index] = vec4(mix(f0, tint, factor), 0.0);
        }
#ifndef IRIDIUM_OPAQUE_FORWARD
        else if (lobe.type == 4u) {
            transmission = clamp(lobe.parameters[0] *
                materialSampleOrOne(material, 17u).r, 0.0, 1.0);
            transmissionIor = lobe.parameters[1];
            transmissionSpecularWeight = lobe.parameters[2];
            transmissionSpecularColor = vec3(lobe.parameters[3],
                lobe.parameters[4], lobe.parameters[5]);
        }
        else if (lobe.type == 5u) {
            volumeThickness = lobe.parameters[0] *
                materialSampleOrOne(material, 18u).g;
            attenuationDistance = lobe.parameters[1];
            attenuationColor = vec3(lobe.parameters[2], lobe.parameters[3],
                lobe.parameters[4]);
        }
        else if (lobe.type == 7u) {
            diffuseTransmission = lobe.parameters[0] *
                materialSampleOrOne(material, 19u).a;
            diffuseTransmissionColor = linearSrgbToAcesCg(
                vec3(lobe.parameters[1], lobe.parameters[2], lobe.parameters[3]) *
                materialSampleOrOne(material, 20u).rgb);
        }
#endif
    }

    vec3 result = iridiumEvaluateStandardIbl(baseColor, f0, f90, metallic,
        roughness, frame.normal, view, ao, fragWorldPos,
        IRIDIUM_MATERIAL_SCENE_PIXEL) * baseLayerAttenuation;
    vec3 iblLobes = vec3(0.0);
    for (uint lobeIndex = 0u; lobeIndex < material.complexLobeCount;
        ++lobeIndex) {
        uint lobeType = directLobeTypes[lobeIndex];
        if (lobeType == 0u) {
            float factor = directLobeData[lobeIndex].x;
            iblLobes += iridiumSceneSpecular(vec3(0.04 * factor),
                vec3(factor), directLobeData[lobeIndex].y,
                directLobeNormals[lobeIndex], view, fragWorldPos,
                IRIDIUM_MATERIAL_SCENE_PIXEL) * ao;
        }
        else if (lobeType == 1u) {
            // Product v1 has no Charlie sheen convolution. Use the named
            // isotropic GGX approximation until dedicated evidence exists.
            vec3 color = directLobeData[lobeIndex].rgb;
            iblLobes += iridiumSceneSpecular(color, color,
                directLobeData[lobeIndex].a, frame.normal, view,
                fragWorldPos, IRIDIUM_MATERIAL_SCENE_PIXEL) * ao *
                baseLayerAttenuation;
        }
        else if (lobeType == 2u) {
            // Product v1 is isotropic; the standard-base result is the explicit
            // anisotropic IBL approximation while direct light remains exact.
        }
        else if (lobeType == 3u) {
            vec3 filmF0 = directLobeData[lobeIndex].rgb;
            iblLobes += (iridiumSceneSpecular(filmF0, f90, roughness,
                frame.normal, view, fragWorldPos,
                    IRIDIUM_MATERIAL_SCENE_PIXEL) -
                iridiumSceneSpecular(f0, f90, roughness, frame.normal, view,
                    fragWorldPos, IRIDIUM_MATERIAL_SCENE_PIXEL)) * ao *
                baseLayerAttenuation;
        }
    }
    result += iblLobes;
    vec3 directContribution = vec3(0.0);
    float shadowVisibility = 1.0;

    IridiumDirectLightRange lightRange = iridiumDirectLightRange(
        fragWorldPos, IRIDIUM_MATERIAL_SCENE_PIXEL);
    uint directLightCount = iridiumDirectLightCount(lightRange);
    for (uint lightIndex = 0u; lightIndex < directLightCount; ++lightIndex) {
        uint lightSlot = iridiumDirectLightSlot(lightRange, lightIndex);
        IridiumDirectLightSample directLight = iridiumEvaluateDirectLightSlot(
            lightSlot, fragWorldPos,
            frame.normal);
        vec3 light = directLight.direction;
        float visibility = iridiumDirectionalShadowVisibility(lightSlot,
            fragWorldPos, frame.normal, light, shadowViewDepth);
        PackedGpuLight lightRecord = iridiumLights[lightSlot];
        if ((floatBitsToUint(lightRecord.shapeMetadata.z) & 3u) ==
            IRIDIUM_LIGHT_TYPE_SPOT)
            visibility *= iridiumSpotShadowVisibility(lightSlot,
                lightRecord, fragWorldPos, frame.normal, light);
        else if ((floatBitsToUint(lightRecord.shapeMetadata.z) & 3u) ==
            IRIDIUM_LIGHT_TYPE_POINT)
            visibility *= iridiumPointShadowVisibility(lightSlot,
                lightRecord, fragWorldPos, frame.normal, light);
        shadowVisibility = min(shadowVisibility, visibility);
        vec3 radiance = directLight.radiance * visibility;
        float noL = max(dot(frame.normal, light), 0.0);

        if (noL > 0.0) {
            directContribution += materialEvaluateStandardBrdf(
                baseColor, f0, f90,
                metallic, roughness, frame.normal, view, light) * radiance *
                noL * baseLayerAttenuation;
        }

        for (uint lobeIndex = 0u; lobeIndex < material.complexLobeCount;
            ++lobeIndex) {
            uint lobeType = directLobeTypes[lobeIndex];
            if (lobeType == 0u) {
                float factor = directLobeData[lobeIndex].x;
                float coatRoughness = directLobeData[lobeIndex].y;
                MaterialTangentFrame coatFrame = frame;
                coatFrame.normal = directLobeNormals[lobeIndex];
                float coatNoL = max(dot(coatFrame.normal, light), 0.0);
                if (coatNoL > 0.0) {
                    directContribution += materialEvaluateSpecularLobe(
                        vec3(0.04) * factor, vec3(factor), coatRoughness,
                        coatFrame.normal, view, light) * radiance * coatNoL;
                }
            }
            else if (lobeType == 1u && noL > 0.0) {
                vec3 color = directLobeData[lobeIndex].rgb;
                float sheenRoughness = directLobeData[lobeIndex].a;
                directContribution += materialEvaluateSheen(
                    color, sheenRoughness,
                    frame.normal, view, light) * radiance * noL *
                    baseLayerAttenuation;
            }
            else if (lobeType == 2u && noL > 0.0) {
                float strength = directLobeData[lobeIndex].x;
                float rotation = directLobeData[lobeIndex].y;
                vec3 isotropic = materialEvaluateSpecularLobe(f0, f90,
                    roughness, frame.normal, view, light);
                vec3 anisotropic = materialEvaluateAnisotropicSpecular(f0,
                    f90, roughness, strength, rotation, frame, view, light);
                directContribution += (anisotropic - isotropic) * radiance * noL *
                    baseLayerAttenuation;
            }
            else if (lobeType == 3u && noL > 0.0) {
                vec3 filmF0 = directLobeData[lobeIndex].rgb;
                directContribution += (materialEvaluateSpecularLobe(
                    filmF0, f90,
                    roughness, frame.normal, view, light) -
                    materialEvaluateSpecularLobe(f0, f90, roughness,
                        frame.normal, view, light)) * radiance * noL *
                    baseLayerAttenuation;
            }
#ifndef IRIDIUM_OPAQUE_FORWARD
            else if (lobeType == 7u && diffuseTransmission > 0.0) {
                directContribution += diffuseTransmissionColor * baseColor *
                    diffuseTransmission / MATERIAL_PI * radiance *
                    max(dot(-frame.normal, light), 0.0) *
                    baseLayerAttenuation;
            }
#endif
        }
    }
    if (push.padding0 == 17u) {
        outColor = vec4(vec3(shadowVisibility), alpha);
        return;
    }
    if (push.padding0 == 15u) {
        outColor = vec4(max(directContribution, vec3(0.0)), alpha);
        return;
    }
    result += directContribution;
    float outputAlpha = alpha;

#ifndef IRIDIUM_OPAQUE_FORWARD

    if (transmission > 0.0) {
        uvec2 refractionExtent = uvec2(textureSize(
            refractionColorPyramid, 0));
        vec3 geometricNormal = normalize(fragNormal);
        if (material.doubleSided != 0u && !gl_FrontFacing)
            geometricNormal = -geometricNormal;
#ifdef IRIDIUM_LAYERED_ORDINARY2_COMPOSITION
        // The peeled entry/exit chord is authoritative. glTF volume thickness
        // (factor times the green texture channel, resolved above) is only a
        // maximum cap; zero means no cap rather than zero absorption.
        float opticalPathMeters =
            iridiumLayeredOrdinary2PathMeters(volumeThickness);
#elif defined(IRIDIUM_LAYERED_DEEP_COMPOSITION) || \
    defined(IRIDIUM_LAYERED_DEEP_RESIDUAL)
        // Each accepted entry uses the matching later exit from the bounded
        // front-to-back interface list. The same shared transport code below
        // then evaluates the measured chord.
        float worldThicknessScale = ubo.worldUnits.x /
            max(length(fragNormal), 1.0e-7);
        float residualSheetMeters = material.thinSheetThicknessMeters;
        if (residualSheetMeters <= 0.0)
            residualSheetMeters = volumeThickness;
        float opticalPathMeters = layeredNonRefractiveResidual
            ? iridiumThinSheetPathLength(residualSheetMeters,
                worldThicknessScale, dot(geometricNormal, view))
            : iridiumLayeredDeepPathMeters(volumeThickness);
#else
        float worldThicknessScale = ubo.worldUnits.x /
            max(length(fragNormal), 1.0e-7);
        uint resolvedTransparencyClass =
            (material.transparencyPolicy >> 8u) & 0xffu;
        float sheetThicknessMeters = material.thinSheetThicknessMeters;
        // Until Ordinary2 composition is selected, LayeredGlass preserves its
        // authored cap as a safe thin-sheet compatibility path.
        if (resolvedTransparencyClass == 5u && sheetThicknessMeters <= 0.0)
            sheetThicknessMeters = volumeThickness;
        float opticalPathMeters = iridiumThinSheetPathLength(
            sheetThicknessMeters, worldThicknessScale,
            dot(geometricNormal, view));
#endif
        // The shared transport helper sanitizes every sampled/authored input
        // before evaluating the per-channel extinction coefficients.
        vec3 attenuation = iridiumBeerLambert(attenuationColor,
            attenuationDistance, opticalPathMeters);
        bool totalInternalReflection = false;
        vec3 opticalNormal = dot(frame.normal, geometricNormal) >= 0.0
            ? frame.normal : -frame.normal;
        vec3 transmittedDirection = iridiumRefractTransparencyRay(
            -view, opticalNormal, 1.0, transmissionIor,
            totalInternalReflection);
        if (layeredNonRefractiveResidual) {
            transmittedDirection = -view;
            totalInternalReflection = false;
        }
        float interfaceFresnel = iridiumDielectricFresnel(1.0,
            transmissionIor, dot(frame.normal, view),
            totalInternalReflection);
        vec3 fresnelTint = linearSrgbToAcesCg(
            transmissionSpecularColor) * transmissionSpecularWeight;
        vec3 fresnel = totalInternalReflection ? vec3(1.0) :
            clamp(interfaceFresnel * fresnelTint, vec3(0.0), vec3(1.0));
        bool classifiedLocalInterface =
            (material.featureFlags & (1u << 19u)) != 0u &&
            opticalPathMeters <= 1.0e-7;
        if (classifiedLocalInterface) {
            // A zero-distance sheet has no displacement, rough-refraction
            // footprint, or absorption. Compose only its opaque fraction and
            // reflected interface here; fixed-function destination blending
            // supplies transmission from the current scene, including sorted
            // transparent reflector surfaces rendered after the pyramid.
            bool geometricTotalInternalReflection = false;
            float geometricInterfaceFresnel = iridiumDielectricFresnel(
                1.0, transmissionIor, dot(geometricNormal, view),
                geometricTotalInternalReflection);
            vec3 geometricFresnel = geometricTotalInternalReflection
                ? vec3(1.0) : clamp(geometricInterfaceFresnel *
                    fresnelTint, vec3(0.0), vec3(1.0));
            vec2 weights = iridiumThinGlassLocalCompositionWeights(
                transmission, metallic, geometricFresnel);
            outputAlpha = 1.0 - weights.y;
            vec3 premultipliedInterface =
                result * (1.0 - weights.x) +
                reflection * fresnel * weights.x;
            result = outputAlpha > 1.0e-7
                ? premultipliedInterface / outputAlpha : vec3(0.0);
        }
        else {
            vec3 environmentTransmission =
                iridiumSampleTransmissionEnvironment(fragWorldPos,
                    transmittedDirection, roughness,
                    IRIDIUM_MATERIAL_SCENE_PIXEL);
            vec3 sceneTransmission = environmentTransmission;
            float sceneConfidence = 0.0;
            if (layeredNonRefractiveResidual) {
                sceneTransmission = texelFetch(refractionColorPyramid,
                    ivec2(IRIDIUM_MATERIAL_SCENE_PIXEL), 0).rgb;
                sceneConfidence = 1.0;
            }
            else if ((ubo.renderInfo.w &
                    IRIDIUM_VIEW_REFRACTION_PYRAMIDS_AVAILABLE) != 0u) {
                uint pyramidLevels = uint(textureQueryLevels(
                    refractionColorPyramid));
                IridiumRefractionProjection refraction =
                    iridiumProjectTransparencyRay(fragWorldPos,
                        transmittedDirection, opticalPathMeters,
                        ubo.worldUnits.x, roughness, 1.0 /
                        max(transmissionIor, IRIDIUM_TRANSPARENCY_MIN_IOR),
                        ubo.view, ubo.proj, refractionExtent, pyramidLevels);
                if (refraction.onScreen) {
                    float selectedLod = refraction.lod;
                    float nearestDepth = textureLod(refractionDepthPyramid,
                        refraction.sampleUv, selectedLod).r;
                    float depthTolerance = max(0.05,
                        refraction.expectedViewDepthMeters * 0.01);
                    bool foregroundLeak = nearestDepth + depthTolerance <
                        refraction.expectedViewDepthMeters;
                    if (foregroundLeak && selectedLod > 0.0) {
                        selectedLod = max(selectedLod - 1.0, 0.0);
                        nearestDepth = textureLod(refractionDepthPyramid,
                            refraction.sampleUv, selectedLod).r;
                        foregroundLeak = nearestDepth + depthTolerance <
                            refraction.expectedViewDepthMeters;
                    }
                    if (!foregroundLeak) {
                        sceneTransmission = textureLod(
                            refractionColorPyramid,
                            refraction.sampleUv, selectedLod).rgb;
                        sceneConfidence = refraction.edgeConfidence;
                    }
                }
            }
            vec3 transmitted = mix(environmentTransmission,
                sceneTransmission, sceneConfidence) * attenuation;
#if defined(IRIDIUM_LAYERED_DEEP_COMPOSITION) || \
    defined(IRIDIUM_LAYERED_DEEP_RESIDUAL)
            // A deep atlas must retain residual transmission so later entry
            // slots can be composed behind this one. Encode a premultiplied
            // source term plus scalar residual coverage in RGBA. The refracted
            // scene sample is preserved as a delta from the unrefracted source
            // pixel; colored extinction is reduced conservatively to luminance
            // because the current local product has one transmittance channel.
            float transportWeight = transmission * (1.0 - metallic);
            vec3 desired = mix(result,
                transmitted * (vec3(1.0) - fresnel) +
                    reflection * fresnel,
                transportWeight);
            vec3 sourceScene = texelFetch(refractionColorPyramid,
                ivec2(IRIDIUM_MATERIAL_SCENE_PIXEL), 0).rgb;
            vec3 channelResidual = clamp(transportWeight *
                (vec3(1.0) - fresnel) * attenuation,
                vec3(0.0), vec3(1.0));
            float residualTransmission = clamp(dot(channelResidual,
                vec3(0.2722287, 0.6740818, 0.0536895)), 0.0, 1.0);
            outputAlpha = 1.0 - residualTransmission;
            vec3 premultipliedSource = max(desired -
                sourceScene * residualTransmission, vec3(0.0));
            result = outputAlpha > 1.0e-7
                ? premultipliedSource / outputAlpha : vec3(0.0);
#else
            result = mix(result, transmitted * (vec3(1.0) - fresnel) +
                reflection * fresnel,
                transmission * (1.0 - metallic));
            // Nonlocal transport is already composited against the pyramid;
            // replace the current destination rather than blending it twice.
            outputAlpha = max(alpha, transmission);
#endif
        }
    }
#endif

    vec3 outputColor = max(result + emissive, vec3(0.0));
#if defined(IRIDIUM_LAYERED_DEEP_COMPOSITION) || \
    defined(IRIDIUM_LAYERED_DEEP_RESIDUAL)
    // Deep local blending always consumes premultiplied source-over output.
    outputColor *= outputAlpha;
#elif defined(IRIDIUM_LAYERED_ORDINARY2_COMPOSITION)
    // The atlas has one blending contract regardless of source alpha mode.
    // Its geometry-addressed scene resolve uses premultiplied ONE / ONE_MINUS_A.
    if ((material.featureFlags & (1u << 19u)) == 0u)
        outputColor *= outputAlpha;
#else
    if ((material.featureFlags & (1u << 19u)) != 0u)
        outputColor *= outputAlpha;
#endif
    outColor = vec4(outputColor, outputAlpha);
}
