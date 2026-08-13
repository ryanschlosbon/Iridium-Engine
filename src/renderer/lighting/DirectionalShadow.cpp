#include "renderer/lighting/DirectionalShadow.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace Iridium {
namespace {

    bool finite(glm::vec3 value) noexcept {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
            std::isfinite(value.z);
    }

    glm::mat4 shadowMatrix(glm::vec3 right, glm::vec3 up, glm::vec3 forward,
        glm::vec2 center, float radius, float minimumDepth, float maximumDepth) {
        const float depthRange = maximumDepth - minimumDepth;
        glm::mat4 result(0.0f);
        result[0][0] = right.x / radius;
        result[1][0] = right.y / radius;
        result[2][0] = right.z / radius;
        result[3][0] = -center.x / radius;
        result[0][1] = up.x / radius;
        result[1][1] = up.y / radius;
        result[2][1] = up.z / radius;
        result[3][1] = -center.y / radius;
        result[0][2] = forward.x / depthRange;
        result[1][2] = forward.y / depthRange;
        result[2][2] = forward.z / depthRange;
        result[3][2] = -minimumDepth / depthRange;
        result[3][3] = 1.0f;
        return result;
    }

} // namespace

std::optional<DirectionalShadowSelection> selectDirectionalShadowLight(
    const LightingFramePacket& lighting) {
    std::vector<DirectionalShadowSelection> selected =
        selectDirectionalShadowLights(lighting, 1);
    if (selected.empty()) return std::nullopt;
    return selected.front();
}

std::vector<DirectionalShadowSelection> selectDirectionalShadowLights(
    const LightingFramePacket& lighting, uint32_t maximumLights) {
    std::vector<DirectionalShadowSelection> candidates;
    for (uint32_t slot : lighting.activeSlots) {
        if (slot >= lighting.records.size() ||
            slot >= lighting.selectionMetadata.size())
            throw std::invalid_argument(
                "Directional shadow selection received an invalid light slot.");
        const PackedGpuLight& light = lighting.records[slot];
        const uint32_t metadata = std::bit_cast<uint32_t>(
            light.shapeMetadata.z);
        if ((metadata & 3u) !=
                static_cast<uint32_t>(PackedGpuLightType::Directional) ||
            (metadata & PackedGpuLightCastsShadows) == 0)
            continue;
        const LightSelectionMetadata& selection =
            lighting.selectionMetadata[slot];
        if (selection.owner.isNil()) continue;
        candidates.push_back(DirectionalShadowSelection{
            .owner = selection.owner,
            .lightSlot = slot,
            .quality = (metadata & PackedGpuLightShadowQualityMask) >>
                PackedGpuLightShadowQualityShift,
            .priority = selection.priority,
            .lightForward = glm::vec3(light.directionOuterCos),
        });
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const DirectionalShadowSelection& left,
            const DirectionalShadowSelection& right) {
            if (left.priority != right.priority)
                return left.priority > right.priority;
            return left.owner < right.owner;
        });
    const uint32_t selectedCount = (std::min)(maximumLights,
        static_cast<uint32_t>(candidates.size()));
    const uint32_t omitted = static_cast<uint32_t>(candidates.size()) -
        selectedCount;
    candidates.resize(selectedCount);
    for (DirectionalShadowSelection& selection : candidates)
        selection.omittedShadowDirectionalLights = omitted;
    return candidates;
}

DirectionalShadowCascadePlan buildDirectionalShadowCascades(
    const DirectionalShadowCamera& sourceCamera, glm::vec3 lightForward,
    DirectionalShadowConfig config) {
    if (config.resolution == 0 || !std::isfinite(config.splitLambda) ||
        config.splitLambda < 0.0f || config.splitLambda > 1.0f ||
        !std::isfinite(config.guardBandFraction) ||
        config.guardBandFraction < 0.0f ||
        !std::isfinite(config.depthPaddingMeters) ||
        config.depthPaddingMeters < 0.0f || !finite(sourceCamera.position) ||
        !finite(sourceCamera.forward) || !finite(sourceCamera.up) ||
        !finite(lightForward) || sourceCamera.verticalFovRadians <= 0.0f ||
        sourceCamera.verticalFovRadians >= 3.13f ||
        sourceCamera.aspectRatio <= 0.0f || sourceCamera.nearPlane <= 0.0f ||
        sourceCamera.farPlane <= sourceCamera.nearPlane ||
        glm::dot(sourceCamera.forward, sourceCamera.forward) < 1.0e-8f ||
        glm::dot(sourceCamera.up, sourceCamera.up) < 1.0e-8f ||
        glm::dot(lightForward, lightForward) < 1.0e-8f)
        throw std::invalid_argument("Directional shadow inputs are invalid.");

    const glm::vec3 cameraForward = glm::normalize(sourceCamera.forward);
    glm::vec3 cameraRight = glm::cross(cameraForward, sourceCamera.up);
    if (glm::dot(cameraRight, cameraRight) < 1.0e-8f)
        throw std::invalid_argument("Directional shadow camera basis is degenerate.");
    cameraRight = glm::normalize(cameraRight);
    const glm::vec3 cameraUp = glm::normalize(
        glm::cross(cameraRight, cameraForward));
    lightForward = glm::normalize(lightForward);
    const glm::vec3 lightUpReference = std::abs(lightForward.y) < 0.99f
        ? glm::vec3(0.0f, 1.0f, 0.0f)
        : glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 lightRight = glm::normalize(
        glm::cross(lightUpReference, lightForward));
    const glm::vec3 lightUp = glm::normalize(
        glm::cross(lightForward, lightRight));

    DirectionalShadowCascadePlan result;
    for (uint32_t index = 0; index < kDirectionalShadowCascadeCount; ++index) {
        const float fraction = static_cast<float>(index + 1u) /
            kDirectionalShadowCascadeCount;
        const float logarithmic = sourceCamera.nearPlane * std::pow(
            sourceCamera.farPlane / sourceCamera.nearPlane, fraction);
        const float uniform = sourceCamera.nearPlane +
            (sourceCamera.farPlane - sourceCamera.nearPlane) * fraction;
        result.splitFar[index] = std::lerp(
            uniform, logarithmic, config.splitLambda);
    }

    const float tangent = std::tan(sourceCamera.verticalFovRadians * 0.5f);
    float splitNear = sourceCamera.nearPlane;
    for (uint32_t cascadeIndex = 0;
        cascadeIndex < kDirectionalShadowCascadeCount; ++cascadeIndex) {
        const float splitFar = result.splitFar[cascadeIndex];
        std::array<glm::vec3, 8> corners{};
        size_t cornerIndex = 0;
        for (float distance : { splitNear, splitFar }) {
            const float halfHeight = tangent * distance;
            const float halfWidth = halfHeight * sourceCamera.aspectRatio;
            const glm::vec3 center = sourceCamera.position +
                cameraForward * distance;
            for (float vertical : { -1.0f, 1.0f })
                for (float horizontal : { -1.0f, 1.0f })
                    corners[cornerIndex++] = center +
                        cameraRight * (horizontal * halfWidth) +
                        cameraUp * (vertical * halfHeight);
        }
        glm::vec3 center{};
        for (glm::vec3 corner : corners) center += corner;
        center /= static_cast<float>(corners.size());
        float radius = 0.0f;
        for (glm::vec3 corner : corners)
            radius = (std::max)(radius, glm::length(corner - center));
        radius = std::ceil(radius * 16.0f) / 16.0f;
        radius *= 1.0f + config.guardBandFraction;
        const float texelSize = 2.0f * radius / config.resolution;
        const glm::vec2 lightCenter{
            glm::dot(lightRight, center), glm::dot(lightUp, center) };
        const glm::vec2 snappedCenter = glm::round(lightCenter / texelSize) *
            texelSize;
        float minimumDepth = std::numeric_limits<float>::max();
        float maximumDepth = std::numeric_limits<float>::lowest();
        for (glm::vec3 corner : corners) {
            const float depth = glm::dot(lightForward, corner);
            minimumDepth = (std::min)(minimumDepth, depth);
            maximumDepth = (std::max)(maximumDepth, depth);
        }
        minimumDepth -= config.depthPaddingMeters;
        maximumDepth += config.depthPaddingMeters;
        DirectionalShadowCascade& cascade = result.cascades[cascadeIndex];
        cascade.splitNear = splitNear;
        cascade.splitFar = splitFar;
        cascade.radiusMeters = radius;
        cascade.worldUnitsPerTexel = texelSize;
        cascade.depthSpanMeters = maximumDepth - minimumDepth;
        cascade.snappedLightCenter = snappedCenter;
        cascade.worldToShadowClip = shadowMatrix(lightRight, lightUp,
            lightForward, snappedCenter, radius, minimumDepth, maximumDepth);
        splitNear = splitFar;
    }
    return result;
}

DirectionalShadowCascadeBlend directionalShadowCascadeBlend(
    const DirectionalShadowCascadePlan& plan, float viewDepth,
    uint32_t sampleableMask) noexcept {
    DirectionalShadowCascadeBlend result;
    while (result.primaryCascade + 1u < kDirectionalShadowCascadeCount &&
        viewDepth > plan.splitFar[result.primaryCascade])
        ++result.primaryCascade;
    result.secondaryCascade = result.primaryCascade;
    const uint32_t primaryBit = 1u << result.primaryCascade;
    if ((sampleableMask & primaryBit) == 0u ||
        result.primaryCascade + 1u == kDirectionalShadowCascadeCount)
        return result;
    const uint32_t secondary = result.primaryCascade + 1u;
    if ((sampleableMask & (1u << secondary)) == 0u) return result;
    const float splitNear = result.primaryCascade == 0u ? 0.0f :
        plan.splitFar[result.primaryCascade - 1u];
    const float splitFar = plan.splitFar[result.primaryCascade];
    const float width = (splitFar - splitNear) * 0.1f;
    if (!(width > 0.0f)) return result;
    const float linear = std::clamp(
        (viewDepth - (splitFar - width)) / width, 0.0f, 1.0f);
    result.secondaryCascade = secondary;
    result.secondaryWeight = linear * linear * (3.0f - 2.0f * linear);
    return result;
}

float directionalShadowTentWeight(int32_t x, int32_t y) noexcept {
    if (x < -2 || x > 2 || y < -2 || y > 2) return 0.0f;
    constexpr std::array<float, 5> Weights{ 1.0f, 2.0f, 3.0f, 2.0f, 1.0f };
    return Weights[static_cast<size_t>(x + 2)] *
        Weights[static_cast<size_t>(y + 2)] / 81.0f;
}

glm::vec3 directionalShadowNormalOffset(glm::vec3 worldPosition,
    glm::vec3 surfaceNormal, float worldUnitsPerTexel, float scale) noexcept {
    return worldPosition + surfaceNormal * worldUnitsPerTexel * scale;
}

DirectionalShadowSchedule DirectionalShadowCache::schedule(
    const DirectionalShadowCacheInput& input,
    uint32_t maximumCascadeUpdates) {
    if (input.selection.owner.isNil())
        throw std::invalid_argument(
            "Directional shadow cache requires a stable light owner.");
    DirectionalShadowSchedule result;
    constexpr uint32_t AllCascades =
        (1u << kDirectionalShadowCascadeCount) - 1u;
    for (uint32_t index = 0; index < kDirectionalShadowCascadeCount; ++index) {
        const CascadeState& state = states_[index];
        const bool matrixChanged = !state.valid || std::memcmp(
            &state.worldToShadowClip, &input.plan.cascades[index].worldToShadowClip,
            sizeof(glm::mat4)) != 0;
        const bool dirty = !state.valid || state.owner != input.selection.owner ||
            state.lightRevision != input.lightRevision ||
            state.casterRevision != input.casterRevision ||
            state.pipelineRevision != input.pipelineRevision || matrixChanged;
        if (dirty) result.dirtyMask |= 1u << index;
        else result.sampleableMask |= 1u << index;
    }
    result.invalidatedCount = std::popcount(result.dirtyMask);
    result.cacheHitCount = kDirectionalShadowCascadeCount -
        result.invalidatedCount;
    uint32_t remaining = maximumCascadeUpdates;
    for (uint32_t index = 0; index < kDirectionalShadowCascadeCount && remaining;
        ++index) {
        const uint32_t bit = 1u << index;
        if ((result.dirtyMask & bit) == 0) continue;
        result.updateMask |= bit;
        --remaining;
    }
    result.sampleableMask |= result.updateMask;
    result.sampleableMask &= AllCascades;
    result.deferredCount = std::popcount(
        result.dirtyMask & ~result.updateMask);
    pending_ = input;
    pendingUpdateMask_ = result.updateMask;
    return result;
}

void DirectionalShadowCache::markRendered(uint32_t renderedMask) {
    if (!pending_ || (renderedMask & ~pendingUpdateMask_) != 0)
        throw std::invalid_argument(
            "Directional shadow completion did not match scheduled cascades.");
    for (uint32_t index = 0; index < kDirectionalShadowCascadeCount; ++index) {
        const uint32_t bit = 1u << index;
        if ((renderedMask & bit) == 0) continue;
        CascadeState& state = states_[index];
        state.owner = pending_->selection.owner;
        state.worldToShadowClip =
            pending_->plan.cascades[index].worldToShadowClip;
        state.lightRevision = pending_->lightRevision;
        state.casterRevision = pending_->casterRevision;
        state.pipelineRevision = pending_->pipelineRevision;
        state.valid = true;
    }
    pending_.reset();
    pendingUpdateMask_ = 0;
}

void DirectionalShadowCache::reset() noexcept {
    states_ = {};
    pending_.reset();
    pendingUpdateMask_ = 0;
}

} // namespace Iridium
