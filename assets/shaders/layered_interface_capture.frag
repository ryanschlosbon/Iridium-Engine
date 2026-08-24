#version 450

#extension GL_EXT_nonuniform_qualifier : require
#include "include/packed_material.glsl"

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragTexCoord0;
layout(location = 5) in vec2 fragTexCoord1;

layout(location = 0) out uint outInterfaceIdentity;

layout(std430, set = 1, binding = 0) readonly buffer MaterialTable {
    PackedMaterial materials[];
};
layout(set = 1, binding = 1) uniform texture2D materialTextureViews[];
layout(set = 2, binding = 0) uniform sampler materialSamplers[];

// The capture set is deliberately separate from the shared material tables.
// Interface zero uses opaqueDepth. Every subsequent peel additionally requires
// a valid preceding interface and captures the nearest fragment behind it.
// Work identity and orientation may differ across passes for nested/crossing
// stacks; the CPU reduction validates closed-volume pairing after capture.
layout(set = 3, binding = 0) uniform sampler2D opaqueDepth;
layout(set = 3, binding = 1) uniform sampler2D previousInterfaceDepth;
layout(set = 3, binding = 2) uniform usampler2D previousInterfaceIdentity;
layout(set = 3, binding = 3) uniform usampler2D previousTileTermination;

layout(push_constant) uniform LayeredInterfaceCapturePushConstants {
    mat4 renderMatrix;
    uint materialIndex;
    uint workTableIndex;
    uint captureFlags;
    uint packedViewportOffset;
} push;

const uint IRIDIUM_LAYERED_CLASS = 5u;
const uint IRIDIUM_LAYERED_ORIENTATION_BIT = 0x80000000u;
const uint IRIDIUM_LAYERED_WORK_MASK = 0x7fffffffu;
const uint IRIDIUM_LAYERED_CAPTURE_MIRRORED = 1u << 0u;
const uint IRIDIUM_LAYERED_CAPTURE_HAS_PREVIOUS = 1u << 1u;
const uint IRIDIUM_LAYERED_CAPTURE_REQUIRE_PAIRED_ORIENTATION = 1u << 2u;
const uint IRIDIUM_LAYERED_CAPTURE_HAS_TERMINATION_MASK = 1u << 3u;
const uint IRIDIUM_DEEP_WORK_MASK = 0x00001fffu;
const uint IRIDIUM_DEEP_TRANSMISSION_SHIFT = 13u;
const uint IRIDIUM_DEEP_TRANSMISSION_MAXIMUM = 0x00003fffu;
const uint IRIDIUM_DEEP_OPEN_COUNT_SHIFT = 27u;
const uint IRIDIUM_DEEP_OPEN_COUNT_MASK = 0x78000000u;
const uint IRIDIUM_DEEP_INVALID_OPEN_COUNT = 15u;
const uint IRIDIUM_DEEP_TERMINATION_TILE_SIZE = 16u;

int unpackSigned16(uint value) {
    return int((value & 0xffffu) ^ 0x8000u) - 0x8000;
}

float materialCoverage(PackedMaterial material) {
    float alpha = material.baseColorFactor.a * fragColor.a;
    if (packedMaterialHasTexture(material, MATERIAL_TEXTURE_BASE_COLOR)) {
        uint semantic = MATERIAL_TEXTURE_BASE_COLOR;
        uint viewIndex = material.textureIndices[semantic];
        uint samplerIndex = packedMaterialSamplerIndex(material, semantic);
        vec2 uv = packedMaterialUv(material, semantic,
            fragTexCoord0, fragTexCoord1);
        alpha *= texture(sampler2D(
            materialTextureViews[nonuniformEXT(viewIndex)],
            materialSamplers[nonuniformEXT(samplerIndex)]), uv).a;
    }
    return clamp(alpha, 0.0, 1.0);
}

float materialTransmissionUpperBound(PackedMaterial material) {
    float transmission = 0.0;
    for (uint index = 0u; index < material.complexLobeCount; ++index) {
        PackedComplexLobe lobe = material.complexLobes[index];
        if (lobe.type != 4u)
            continue;
        float textureFactor = 1.0;
        if (packedMaterialHasTexture(material, 17u)) {
            uint viewIndex = material.textureIndices[17u];
            uint samplerIndex = packedMaterialSamplerIndex(material, 17u);
            vec2 uv = packedMaterialUv(material, 17u,
                fragTexCoord0, fragTexCoord1);
            textureFactor = texture(sampler2D(
                materialTextureViews[nonuniformEXT(viewIndex)],
                materialSamplers[nonuniformEXT(samplerIndex)]), uv).r;
        }
        transmission = max(transmission,
            clamp(lobe.parameters[0] * textureFactor, 0.0, 1.0));
    }
    return transmission;
}

void main() {
    ivec2 viewportOffset = ivec2(
        unpackSigned16(push.packedViewportOffset),
        unpackSigned16(push.packedViewportOffset >> 16u));
    ivec2 atlasPixel = ivec2(gl_FragCoord.xy);
    bool hasPreviousInterface = (push.captureFlags &
        IRIDIUM_LAYERED_CAPTURE_HAS_PREVIOUS) != 0u;
    bool requirePairedOrientation = (push.captureFlags &
        IRIDIUM_LAYERED_CAPTURE_REQUIRE_PAIRED_ORIENTATION) != 0u;
    bool hasTerminationMask = (push.captureFlags &
        IRIDIUM_LAYERED_CAPTURE_HAS_TERMINATION_MASK) != 0u;
    if (hasTerminationMask && !requirePairedOrientation) {
        ivec2 tilePixel = atlasPixel /
            int(IRIDIUM_DEEP_TERMINATION_TILE_SIZE);
        ivec2 tileExtent = textureSize(previousTileTermination, 0);
        if (all(greaterThanEqual(tilePixel, ivec2(0))) &&
            all(lessThan(tilePixel, tileExtent)) &&
            texelFetch(previousTileTermination, tilePixel, 0).r != 0u)
            discard;
    }

    PackedMaterial material = materials[push.materialIndex];
    if (material.schemaVersion != MATERIAL_SCHEMA_VERSION)
        discard;
    uint resolvedTransparencyClass =
        (material.transparencyPolicy >> 8u) & 0xffu;
    if (resolvedTransparencyClass != IRIDIUM_LAYERED_CLASS)
        discard;

    float coverage = materialCoverage(material);
    if (coverage <= 0.0 ||
        (material.alphaMode == 1u &&
            coverage < material.surfaceParameters.y))
        discard;

    uint oneBasedWork = push.workTableIndex + 1u;
    uint workMask = requirePairedOrientation
        ? IRIDIUM_LAYERED_WORK_MASK : IRIDIUM_DEEP_WORK_MASK;
    if (oneBasedWork == 0u || (oneBasedWork & ~workMask) != 0u)
        discard;

    ivec2 scenePixel = atlasPixel + viewportOffset;
    ivec2 opaqueExtent = textureSize(opaqueDepth, 0);
    if (any(lessThan(scenePixel, ivec2(0))) ||
        any(greaterThanEqual(scenePixel, opaqueExtent)))
        discard;
    float opaqueSample = texelFetch(opaqueDepth, scenePixel, 0).r;
    if (gl_FragCoord.z > opaqueSample)
        discard;

    bool mirrored = (push.captureFlags &
        IRIDIUM_LAYERED_CAPTURE_MIRRORED) != 0u;
    bool semanticEntry = gl_FrontFacing != mirrored;
    if (requirePairedOrientation &&
        semanticEntry == hasPreviousInterface)
        discard;

    if (hasPreviousInterface) {
        ivec2 previousExtent = textureSize(previousInterfaceDepth, 0);
        if (any(lessThan(atlasPixel, ivec2(0))) ||
            any(greaterThanEqual(atlasPixel, previousExtent)))
            discard;
        float previousDepth = texelFetch(previousInterfaceDepth,
            atlasPixel, 0).r;
        uint previousIdentity = texelFetch(previousInterfaceIdentity,
            atlasPixel, 0).r;
        if (gl_FragCoord.z <= previousDepth ||
            (previousIdentity & workMask) == 0u)
            discard;
        if (requirePairedOrientation &&
            ((previousIdentity & IRIDIUM_LAYERED_ORIENTATION_BIT) != 0u ||
             (previousIdentity & IRIDIUM_LAYERED_WORK_MASK) != oneBasedWork))
            discard;
    }

    if (requirePairedOrientation) {
        outInterfaceIdentity = oneBasedWork |
            (!semanticEntry ? IRIDIUM_LAYERED_ORIENTATION_BIT : 0u);
        return;
    }

    uint transmission = IRIDIUM_DEEP_TRANSMISSION_MAXIMUM;
    uint openCount = 0u;
    if (hasPreviousInterface) {
        uint previousIdentity = texelFetch(previousInterfaceIdentity,
            atlasPixel, 0).r;
        transmission = (previousIdentity >>
            IRIDIUM_DEEP_TRANSMISSION_SHIFT) &
            IRIDIUM_DEEP_TRANSMISSION_MAXIMUM;
        openCount = (previousIdentity & IRIDIUM_DEEP_OPEN_COUNT_MASK) >>
            IRIDIUM_DEEP_OPEN_COUNT_SHIFT;
    }
    if (openCount != IRIDIUM_DEEP_INVALID_OPEN_COUNT) {
        if (semanticEntry) {
            openCount = openCount < 8u
                ? openCount + 1u : IRIDIUM_DEEP_INVALID_OPEN_COUNT;
        }
        else if (openCount == 0u) {
            openCount = IRIDIUM_DEEP_INVALID_OPEN_COUNT;
        }
        else {
            --openCount;
            uint shellTransmission = uint(ceil(
                materialTransmissionUpperBound(material) *
                float(IRIDIUM_DEEP_TRANSMISSION_MAXIMUM)));
            uint numerator = transmission * shellTransmission +
                IRIDIUM_DEEP_TRANSMISSION_MAXIMUM - 1u;
            transmission = numerator /
                IRIDIUM_DEEP_TRANSMISSION_MAXIMUM;
        }
    }
    outInterfaceIdentity = oneBasedWork |
        (transmission << IRIDIUM_DEEP_TRANSMISSION_SHIFT) |
        (openCount << IRIDIUM_DEEP_OPEN_COUNT_SHIFT) |
        (!semanticEntry ? IRIDIUM_LAYERED_ORIENTATION_BIT : 0u);
}
