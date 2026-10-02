// Shared conservative GPU-scene Hi-Z projection/query implementation.
// Callers provide the GPU-scene table declarations, global UBO, sampler, and
// GpuSceneDepthResult ABI structure before including this file.

const uint IridiumDepthRejectionNone = 0u;
const uint IridiumDepthRejectionInvalidExtent = 1u;
const uint IridiumDepthRejectionInvalidBounds = 2u;
const uint IridiumDepthRejectionInvalidProjection = 3u;
const uint IridiumDepthRejectionClipPlane = 5u;
const uint IridiumDepthRejectionOutsideView = 6u;
const uint IridiumDepthRejectionSmallBounds = 7u;

bool iridiumDepthFinite(float value) {
    return !isnan(value) && !isinf(value);
}

bool iridiumDepthFinite3(vec3 value) {
    return iridiumDepthFinite(value.x) && iridiumDepthFinite(value.y) &&
        iridiumDepthFinite(value.z);
}

bool iridiumDepthFinite4(vec4 value) {
    return iridiumDepthFinite(value.x) && iridiumDepthFinite(value.y) &&
        iridiumDepthFinite(value.z) && iridiumDepthFinite(value.w);
}

vec3 iridiumDepthTransformPoint(Transform transform, vec3 point) {
    return vec3(dot(transform.row0.xyz, point) + transform.row0.w,
        dot(transform.row1.xyz, point) + transform.row1.w,
        dot(transform.row2.xyz, point) + transform.row2.w);
}

uint iridiumDepthProportionalCeil(
    uint coordinate, uint targetExtent, uint sourceExtent) {
    uint quotient = coordinate / sourceExtent;
    uint remainder = coordinate % sourceExtent;
    return quotient * targetExtent +
        (remainder * targetExtent + sourceExtent - 1u) / sourceExtent;
}

GpuSceneDepthResult iridiumDepthFailVisible(
    uint primitiveIndex, uint rejection, uint reverseDepth, uint abiVersion) {
    GpuSceneDepthResult result;
    result.metadata = uvec4(abiVersion, 0u, 0u, 0u);
    result.farthestOccluderDepth = reverseDepth != 0u ? 0.0 : 1.0;
    result.occluded = 0u;
    result.reserved = uvec2(rejection, primitiveIndex);
    return result;
}

GpuSceneDepthResult iridiumQueryGpuSceneDepth(uint primitiveIndex,
    sampler2D depthPyramid, uvec4 tableCounts, uint reverseDepth,
    uint abiVersion) {
    if (primitiveIndex >= tableCounts.z) {
        return iridiumDepthFailVisible(primitiveIndex,
            IridiumDepthRejectionInvalidBounds, reverseDepth, abiVersion);
    }
    Primitive primitive = primitives.values[primitiveIndex];
    if (primitive.binding.x >= tableCounts.y ||
        primitive.binding.y >= tableCounts.w) {
        return iridiumDepthFailVisible(primitiveIndex,
            IridiumDepthRejectionInvalidBounds, reverseDepth, abiVersion);
    }
    Instance instance = instances.values[primitive.binding.x];
    if (instance.references.x >= tableCounts.x) {
        return iridiumDepthFailVisible(primitiveIndex,
            IridiumDepthRejectionInvalidBounds, reverseDepth, abiVersion);
    }

    uvec2 extent = uvec2(textureSize(depthPyramid, 0));
    if (any(equal(extent, uvec2(0u))) ||
        any(notEqual(extent, ubo.renderInfo.xy))) {
        return iridiumDepthFailVisible(primitiveIndex,
            IridiumDepthRejectionInvalidExtent, reverseDepth, abiVersion);
    }
    Geometry geometry = geometries.values[primitive.binding.y];
    vec3 minimum = geometry.localBoundsMin.xyz;
    vec3 maximum = geometry.localBoundsMax.xyz;
    if (!iridiumDepthFinite3(minimum) || !iridiumDepthFinite3(maximum) ||
        any(greaterThan(minimum, maximum))) {
        return iridiumDepthFailVisible(primitiveIndex,
            IridiumDepthRejectionInvalidBounds, reverseDepth, abiVersion);
    }

    Transform transform = transforms.values[instance.references.x];
    vec3 worldMinimum = vec3(3.402823466e+38);
    vec3 worldMaximum = vec3(-3.402823466e+38);
    for (uint corner = 0u; corner < 8u; ++corner) {
        vec3 local = vec3(
            (corner & 1u) != 0u ? maximum.x : minimum.x,
            (corner & 2u) != 0u ? maximum.y : minimum.y,
            (corner & 4u) != 0u ? maximum.z : minimum.z);
        vec3 world = iridiumDepthTransformPoint(transform, local);
        if (!iridiumDepthFinite3(world)) {
            return iridiumDepthFailVisible(primitiveIndex,
                IridiumDepthRejectionInvalidProjection, reverseDepth,
                abiVersion);
        }
        worldMinimum = min(worldMinimum, world);
        worldMaximum = max(worldMaximum, world);
    }

    // Match the CPU safety oracle's transformed world AABB. The two-pixel guard
    // and camera-ward depth margin intentionally retain more marginal work.
    mat4 clipFromWorld = ubo.proj * ubo.view;
    vec2 minimumNdc = vec2(1.0);
    vec2 maximumNdc = vec2(-1.0);
    float nearest = reverseDepth != 0u ? 0.0 : 1.0;
    vec4 clips[8];
    for (uint corner = 0u; corner < 8u; ++corner) {
        vec3 world = vec3(
            (corner & 1u) != 0u ? worldMaximum.x : worldMinimum.x,
            (corner & 2u) != 0u ? worldMaximum.y : worldMinimum.y,
            (corner & 4u) != 0u ? worldMaximum.z : worldMinimum.z);
        vec4 clip = clipFromWorld * vec4(world, 1.0);
        clips[corner] = clip;
        if (!iridiumDepthFinite4(clip)) {
            return iridiumDepthFailVisible(primitiveIndex,
                IridiumDepthRejectionInvalidProjection, reverseDepth,
                abiVersion);
        }
        if (clip.w <= 0.000001) {
            return iridiumDepthFailVisible(primitiveIndex,
                IridiumDepthRejectionClipPlane, reverseDepth, abiVersion);
        }
        vec3 ndc = clip.xyz / clip.w;
        if (!iridiumDepthFinite3(ndc)) {
            return iridiumDepthFailVisible(primitiveIndex,
                IridiumDepthRejectionInvalidProjection, reverseDepth,
                abiVersion);
        }
        if (ndc.z <= 0.0 || ndc.z >= 1.0) {
            return iridiumDepthFailVisible(primitiveIndex,
                IridiumDepthRejectionClipPlane, reverseDepth, abiVersion);
        }
        minimumNdc = min(minimumNdc, ndc.xy);
        maximumNdc = max(maximumNdc, ndc.xy);
        nearest = reverseDepth != 0u
            ? max(nearest, ndc.z) : min(nearest, ndc.z);
    }

    for (uint plane = 0u; plane < 6u; ++plane) {
        bool allOutside = true;
        for (uint corner = 0u; corner < 8u; ++corner) {
            vec4 clip = clips[corner];
            bool outside = plane == 0u ? clip.x < -clip.w :
                plane == 1u ? clip.x > clip.w :
                plane == 2u ? clip.y < -clip.w :
                plane == 3u ? clip.y > clip.w :
                plane == 4u ? clip.z < 0.0 : clip.z > clip.w;
            allOutside = allOutside && outside;
        }
        if (allOutside) {
            return iridiumDepthFailVisible(primitiveIndex,
                IridiumDepthRejectionOutsideView, reverseDepth, abiVersion);
        }
    }

    if (maximumNdc.x <= -1.0 || minimumNdc.x >= 1.0 ||
        maximumNdc.y <= -1.0 || minimumNdc.y >= 1.0) {
        return iridiumDepthFailVisible(primitiveIndex,
            IridiumDepthRejectionOutsideView, reverseDepth, abiVersion);
    }
    vec2 minimumUv = clamp(minimumNdc, vec2(-1.0), vec2(1.0)) * 0.5 + 0.5;
    vec2 maximumUv = clamp(maximumNdc, vec2(-1.0), vec2(1.0)) * 0.5 + 0.5;
    vec2 footprint = (maximumUv - minimumUv) * vec2(extent);
    if (footprint.x <= 1.0 || footprint.y <= 1.0) {
        return iridiumDepthFailVisible(primitiveIndex,
            IridiumDepthRejectionSmallBounds, reverseDepth, abiVersion);
    }

    const float guardPixels = 2.0;
    uvec2 first = uvec2(max(vec2(0.0),
        floor(minimumUv * vec2(extent) - guardPixels)));
    uvec2 end = uvec2(min(vec2(extent),
        ceil(maximumUv * vec2(extent) + guardPixels)));
    if (any(greaterThanEqual(first, end))) {
        return iridiumDepthFailVisible(primitiveIndex,
            IridiumDepthRejectionSmallBounds, reverseDepth, abiVersion);
    }
    nearest = reverseDepth != 0u
        ? min(1.0, nearest + 0.000002)
        : max(0.0, nearest - 0.000002);

    uint firstX = first.x;
    uint firstY = first.y;
    uint lastX = end.x - 1u;
    uint lastY = end.y - 1u;
    uvec2 selectedExtent = extent;
    uint selectedMip = 0u;
    uint mipCount = uint(textureQueryLevels(depthPyramid));
    for (uint mip = 0u; mip < mipCount; ++mip) {
        uvec2 candidateExtent = uvec2(textureSize(depthPyramid, int(mip)));
        if (mip != 0u) {
            firstX = iridiumDepthProportionalCeil(firstX + 1u,
                candidateExtent.x, selectedExtent.x) - 1u;
            firstY = iridiumDepthProportionalCeil(firstY + 1u,
                candidateExtent.y, selectedExtent.y) - 1u;
            lastX = iridiumDepthProportionalCeil(lastX + 1u,
                candidateExtent.x, selectedExtent.x) - 1u;
            lastY = iridiumDepthProportionalCeil(lastY + 1u,
                candidateExtent.y, selectedExtent.y) - 1u;
        }
        selectedMip = mip;
        selectedExtent = candidateExtent;
        if (lastX - firstX < 2u && lastY - firstY < 2u) break;
    }

    float sampled = reverseDepth != 0u ? 1.0 : 0.0;
    uint sampledTexels = 0u;
    for (uint y = firstY; y <= lastY; ++y) {
        for (uint x = firstX; x <= lastX; ++x) {
            float value = texelFetch(depthPyramid, ivec2(x, y),
                int(selectedMip)).r;
            sampled = reverseDepth != 0u
                ? min(sampled, value) : max(sampled, value);
            ++sampledTexels;
        }
    }

    GpuSceneDepthResult result;
    result.metadata = uvec4(abiVersion, selectedMip, sampledTexels,
        sampledTexels != 0u ? 1u : 0u);
    result.farthestOccluderDepth = sampled;
    result.occluded = reverseDepth != 0u
        ? (nearest < sampled - 0.00001 ? 1u : 0u)
        : (nearest > sampled + 0.00001 ? 1u : 0u);
    result.reserved = uvec2(IridiumDepthRejectionNone, primitiveIndex);
    return result;
}
