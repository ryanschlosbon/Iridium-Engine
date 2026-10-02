#include "GpuSceneLod.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Iridium {

    namespace {
        bool finitePositive(float value) noexcept {
            return std::isfinite(value) && value > 0.0f;
        }

        bool validLevels(std::span<const GpuSceneLodLevel> levels) noexcept {
            float previousError = -1.0f;
            for (const GpuSceneLodLevel& level : levels) {
                if (!std::isfinite(level.geometricError) ||
                    level.geometricError < 0.0f ||
                    level.geometricError < previousError) return false;
                previousError = level.geometricError;
            }
            return !levels.empty();
        }

        uint32_t firstResident(std::span<const GpuSceneLodLevel> levels,
            uint32_t limit) noexcept {
            for (uint32_t index = 0; index <= limit; ++index)
                if (levels[index].resident) return index;
            return InvalidGpuSceneIndex;
        }
    }

    GpuSceneLodSelection selectGpuSceneLod(
        const GpuSceneLodRequest& request) noexcept {
        GpuSceneLodSelection result;
        if (request.levels.empty()) return result;
        const uint32_t limit = (std::min)(request.maximumLod,
            static_cast<uint32_t>(request.levels.size() - 1u));
        const bool valid = validLevels(request.levels) &&
            finitePositive(request.distanceToBounds) &&
            finitePositive(request.viewportHeightPixels) &&
            finitePositive(request.targetErrorPixels) &&
            std::isfinite(request.verticalFieldOfViewRadians) &&
            request.verticalFieldOfViewRadians > 0.0f &&
            request.verticalFieldOfViewRadians < 3.14159265358979323846f &&
            std::isfinite(request.hysteresisFraction) &&
            request.hysteresisFraction >= 0.0f &&
            request.hysteresisFraction <= 0.5f;

        const uint32_t failVisible = firstResident(request.levels, limit);
        if (!valid) {
            if (failVisible == InvalidGpuSceneIndex) return result;
            const GpuSceneLodLevel& level = request.levels[failVisible];
            result.selectedLod = failVisible;
            result.geometryIndex = level.geometryIndex;
            result.triangleCount = level.triangleCount;
            result.reason = GpuSceneLodReason::InvalidInputFailVisible;
            result.drawable = true;
            result.changed = request.previousLod != failVisible;
            return result;
        }

        const float projectionScale = request.viewportHeightPixels /
            (2.0f * std::tan(request.verticalFieldOfViewRadians * 0.5f));
        if (!finitePositive(projectionScale)) {
            if (failVisible == InvalidGpuSceneIndex) return result;
            const GpuSceneLodLevel& level = request.levels[failVisible];
            result.selectedLod = failVisible;
            result.geometryIndex = level.geometryIndex;
            result.triangleCount = level.triangleCount;
            result.reason = GpuSceneLodReason::InvalidInputFailVisible;
            result.drawable = true;
            result.changed = request.previousLod != failVisible;
            return result;
        }
        const auto projectedError = [&](uint32_t lod) noexcept {
            return request.levels[lod].geometricError * projectionScale /
                request.distanceToBounds;
        };

        uint32_t selected = 0;
        for (uint32_t lod = 1; lod <= limit; ++lod) {
            const float errorPixels = projectedError(lod);
            if (!std::isfinite(errorPixels) ||
                errorPixels > request.targetErrorPixels) break;
            selected = lod;
        }

        GpuSceneLodReason reason = GpuSceneLodReason::ProjectedError;
        if (request.previousLod <= limit &&
            request.levels[request.previousLod].resident) {
            if (selected > request.previousLod &&
                projectedError(selected) > request.targetErrorPixels *
                    (1.0f - request.hysteresisFraction)) {
                selected = request.previousLod;
                reason = GpuSceneLodReason::HysteresisRetained;
            }
            else if (selected < request.previousLod &&
                projectedError(request.previousLod) <=
                    request.targetErrorPixels *
                    (1.0f + request.hysteresisFraction)) {
                selected = request.previousLod;
                reason = GpuSceneLodReason::HysteresisRetained;
            }
        }

        if (!request.levels[selected].resident) {
            uint32_t fallback = InvalidGpuSceneIndex;
            for (uint32_t lod = selected + 1u; lod <= limit; ++lod) {
                if (!request.levels[lod].resident) continue;
                fallback = lod;
                reason = GpuSceneLodReason::CoarserResidentFallback;
                break;
            }
            if (fallback == InvalidGpuSceneIndex) {
                for (uint32_t lod = selected; lod-- > 0u;) {
                    if (!request.levels[lod].resident) continue;
                    fallback = lod;
                    reason = GpuSceneLodReason::FinerResidentFallback;
                    break;
                }
            }
            if (fallback == InvalidGpuSceneIndex) return result;
            selected = fallback;
        }

        const GpuSceneLodLevel& level = request.levels[selected];
        result.selectedLod = selected;
        result.geometryIndex = level.geometryIndex;
        result.triangleCount = level.triangleCount;
        result.projectedErrorPixels = projectedError(selected);
        result.reason = reason;
        result.drawable = true;
        result.changed = request.previousLod != selected;
        return result;
    }

    float boundGpuSceneLodPixelError(const GpuSceneGeometryRecord& base,
        const glm::mat4& clipFromLocal, glm::vec2 viewportPixels,
        float geometricError) noexcept {
        constexpr float invalid = std::numeric_limits<float>::infinity();
        if (!finitePositive(viewportPixels.x) || !finitePositive(viewportPixels.y) ||
            !std::isfinite(geometricError) || geometricError < 0.0f) return invalid;
        const glm::vec3 minimum(base.localBoundsMin.x, base.localBoundsMin.y, base.localBoundsMin.z);
        const glm::vec3 maximum(base.localBoundsMax.x, base.localBoundsMax.y, base.localBoundsMax.z);
        for (uint32_t axis = 0; axis < 3; ++axis)
            if (!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) ||
                minimum[axis] > maximum[axis]) return invalid;
        float minimumW = invalid;
        float minimumZ = invalid;
        float minimumWMinusZ = invalid;
        glm::vec2 maximumAbsClip(0.0f);
        for (uint32_t corner = 0; corner < 8; ++corner) {
            const glm::vec4 clip = clipFromLocal * glm::vec4(
                (corner & 1u) ? maximum.x : minimum.x,
                (corner & 2u) ? maximum.y : minimum.y,
                (corner & 4u) ? maximum.z : minimum.z, 1.0f);
            for (uint32_t axis = 0; axis < 4; ++axis)
                if (!std::isfinite(clip[axis])) return invalid;
            minimumW = (std::min)(minimumW, clip.w);
            minimumZ = (std::min)(minimumZ, clip.z);
            minimumWMinusZ = (std::min)(minimumWMinusZ, clip.w - clip.z);
            maximumAbsClip = glm::max(maximumAbsClip, glm::abs(glm::vec2(clip)));
        }
        const auto rowLength = [&](uint32_t row) {
            return glm::length(glm::vec3(clipFromLocal[0][row],
                clipFromLocal[1][row], clipFromLocal[2][row]));
        };
        const float wLength = rowLength(3);
        const float wMinusZLength = glm::length(glm::vec3(
            clipFromLocal[0][3] - clipFromLocal[0][2],
            clipFromLocal[1][3] - clipFromLocal[1][2],
            clipFromLocal[2][3] - clipFromLocal[2][2]));
        const float denominator = minimumW - geometricError * wLength;
        if (!finitePositive(minimumW) || !finitePositive(denominator)) return invalid;
        // A small positional error can clip an entire grazing surface. Pin LOD0
        // if the error ball reaches either depth plane (also correct for reverse Z).
        if (!finitePositive(minimumZ - geometricError * rowLength(2)) ||
            !finitePositive(minimumWMinusZ - geometricError * wMinusZLength)) return invalid;
        const glm::vec2 displacement = geometricError *
            (glm::vec2(rowLength(0), rowLength(1)) + maximumAbsClip / minimumW * wLength) /
            denominator * viewportPixels * 0.5f;
        // Small rounding guard; identical guard in the device implementation.
        const float bound = glm::length(displacement) * 1.0001f;
        return std::isfinite(bound) ? bound : invalid;
    }

    uint32_t selectGpuSceneLodGeometry(
        std::span<const GpuSceneGeometryRecord> geometries, uint32_t baseIndex,
        const glm::mat4& clipFromLocal, glm::vec2 viewportPixels,
        float targetErrorPixels, uint32_t maximumLod,
        uint32_t previousLod, float hysteresisFraction) noexcept {
        if (baseIndex >= geometries.size()) return InvalidGpuSceneIndex;
        const auto& base = geometries[baseIndex];
        if (!finitePositive(targetErrorPixels) || base.localBoundsMin.w != 0.0f ||
            base.localBoundsMax.w != 0.0f || !std::isfinite(hysteresisFraction) ||
            hysteresisFraction < 0.0f || hysteresisFraction > 0.5f) return baseIndex;
        uint32_t selected = baseIndex;
        uint32_t current = baseIndex;
        float previousError = 0.0f;
        uint32_t previousCount = base.draw.y;
        const uint32_t limit = (std::min)(maximumLod, MaximumGpuSceneLodLevels - 1u);
        for (uint32_t lod = 1; lod <= limit; ++lod) {
            const uint32_t next = geometries[current].state.z;
            if (next == InvalidGpuSceneIndex) break;
            if (next >= geometries.size()) return baseIndex;
            const auto& child = geometries[next];
            const float error = child.localBoundsMin.w;
            if (!std::isfinite(error) || error < previousError ||
                child.localBoundsMax.w != static_cast<float>(lod) ||
                child.draw.w != base.draw.w || child.storage.z != base.storage.z ||
                child.draw.y == 0 || child.draw.y % 3u != 0 ||
                child.draw.y >= previousCount) return baseIndex;
            // Never relax the requested ceiling. Retain/refine at the ceiling;
            // entering a coarser level requires the additional safety margin.
            const float threshold = previousLod != InvalidGpuSceneIndex && lod > previousLod
                ? targetErrorPixels * (1.0f - hysteresisFraction) : targetErrorPixels;
            if (boundGpuSceneLodPixelError(base, clipFromLocal, viewportPixels, error) <=
                threshold) selected = next;
            previousError = error;
            previousCount = child.draw.y;
            current = next;
        }
        return selected;
    }

    uint32_t selectGpuSceneRadialLodGeometry(
        std::span<const GpuSceneGeometryRecord> geometries, uint32_t baseIndex,
        const GpuSceneInstanceRecord& instance, glm::vec3 consumerPosition,
        float faceResolutionPixels, float targetErrorPixels,
        uint32_t maximumLod) noexcept {
        if (baseIndex >= geometries.size()) return InvalidGpuSceneIndex;
        const auto& base = geometries[baseIndex];
        const glm::vec3 center(instance.worldBoundsSphere.x,
            instance.worldBoundsSphere.y, instance.worldBoundsSphere.z);
        const float worldRadius = instance.worldBoundsSphere.w;
        const float localRadius = base.localBoundsSphere.w;
        if (!finitePositive(faceResolutionPixels) ||
            !finitePositive(targetErrorPixels) ||
            !finitePositive(localRadius) || !finitePositive(worldRadius) ||
            !std::isfinite(consumerPosition.x) ||
            !std::isfinite(consumerPosition.y) ||
            !std::isfinite(consumerPosition.z) ||
            !std::isfinite(center.x) || !std::isfinite(center.y) ||
            !std::isfinite(center.z) || base.localBoundsMin.w != 0.0f ||
            base.localBoundsMax.w != 0.0f) return baseIndex;
        const float distanceToBounds = glm::length(center - consumerPosition) -
            worldRadius;
        if (!finitePositive(distanceToBounds)) return baseIndex;
        const float worldScale = worldRadius / localRadius;
        if (!finitePositive(worldScale)) return baseIndex;

        uint32_t selected = baseIndex;
        uint32_t current = baseIndex;
        float previousError = 0.0f;
        uint32_t previousCount = base.draw.y;
        const uint32_t limit = (std::min)(maximumLod,
            MaximumGpuSceneLodLevels - 1u);
        for (uint32_t lod = 1; lod <= limit; ++lod) {
            const uint32_t next = geometries[current].state.z;
            if (next == InvalidGpuSceneIndex) break;
            if (next >= geometries.size()) return baseIndex;
            const auto& child = geometries[next];
            const float error = child.localBoundsMin.w;
            if (!std::isfinite(error) || error < previousError ||
                child.localBoundsMax.w != static_cast<float>(lod) ||
                child.draw.w != base.draw.w ||
                child.storage.z != base.storage.z || child.draw.y == 0u ||
                child.draw.y % 3u != 0u || child.draw.y >= previousCount)
                return baseIndex;
            // A cubemap face has a 90-degree vertical FOV, so its projection
            // scale is resolution / (2 * tan(45 degrees)) = resolution / 2.
            const float pixelError = error * worldScale *
                faceResolutionPixels * 0.5f / distanceToBounds;
            if (std::isfinite(pixelError) && pixelError <= targetErrorPixels)
                selected = next;
            previousError = error;
            previousCount = child.draw.y;
            current = next;
        }
        return selected;
    }

    uint32_t selectGpuSceneDensityLodGeometry(
        std::span<const GpuSceneGeometryRecord> geometries, uint32_t baseIndex,
        const GpuSceneInstanceRecord& instance, float worldUnitsPerTexel,
        float targetErrorTexels, uint32_t maximumLod) noexcept {
        if (baseIndex >= geometries.size()) return InvalidGpuSceneIndex;
        const auto& base = geometries[baseIndex];
        const float localRadius = base.localBoundsSphere.w;
        const float worldRadius = instance.worldBoundsSphere.w;
        if (!finitePositive(worldUnitsPerTexel) ||
            !finitePositive(targetErrorTexels) ||
            !finitePositive(localRadius) || !finitePositive(worldRadius) ||
            base.localBoundsMin.w != 0.0f || base.localBoundsMax.w != 0.0f)
            return baseIndex;
        const float worldScale = worldRadius / localRadius;
        if (!finitePositive(worldScale)) return baseIndex;

        uint32_t selected = baseIndex;
        uint32_t current = baseIndex;
        float previousError = 0.0f;
        uint32_t previousCount = base.draw.y;
        const uint32_t limit = (std::min)(maximumLod,
            MaximumGpuSceneLodLevels - 1u);
        for (uint32_t lod = 1u; lod <= limit; ++lod) {
            const uint32_t next = geometries[current].state.z;
            if (next == InvalidGpuSceneIndex) break;
            if (next >= geometries.size()) return baseIndex;
            const auto& child = geometries[next];
            const float error = child.localBoundsMin.w;
            if (!std::isfinite(error) || error < previousError ||
                child.localBoundsMax.w != static_cast<float>(lod) ||
                child.draw.w != base.draw.w ||
                child.storage.z != base.storage.z || child.draw.y == 0u ||
                child.draw.y % 3u != 0u || child.draw.y >= previousCount)
                return baseIndex;
            const float errorTexels = error * worldScale /
                worldUnitsPerTexel;
            if (std::isfinite(errorTexels) &&
                errorTexels <= targetErrorTexels) selected = next;
            previousError = error;
            previousCount = child.draw.y;
            current = next;
        }
        return selected;
    }

    uint64_t GpuSceneLodHistory::newToken() {
        if (nextToken_ == UINT64_MAX) throw std::overflow_error("LOD history token exhausted");
        return ++nextToken_;
    }

    void GpuSceneLodHistory::invalidate() {
        for (auto& entry : entries_) if (entry.primitive.isValid()) {
            entry.token = newToken();
            entry.oracle = {};
        }
    }

    void GpuSceneLodHistory::resize(uint32_t capacity) {
        // The backend fences and replaces its matching device storage first.
        entries_.assign(capacity, {});
        live_.resize(capacity);
        slots_.clear();
        slots_.reserve(capacity);
        freeSlots_.clear();
        freeSlots_.reserve(capacity);
        denseSlots_.clear();
        denseSlots_.reserve(capacity);
        geometryRevisions_.clear();
        published_ = false;
    }

    void GpuSceneLodHistory::publish(const GpuScenePackedTables& scene) {
        if (published_ && sceneEpoch_ == scene.sceneEpoch && publicationRevision_ == scene.publicationRevision)
            return;
        if (scene.densePrimitiveHandles.size() != scene.primitives.size() ||
            scene.denseGeometryHandles.size() != scene.geometries.size())
            throw std::invalid_argument("LOD history requires stable packed handles");
        if (!published_ || sceneEpoch_ != scene.sceneEpoch) {
            entries_.assign(entries_.size(), {});
            slots_.clear();
        }
        else if (geometryRevisions_ != scene.geometryRevisions) {
            // Conservative whole-view reset on any chain edit/relocation. Material
            // and transform-only publications do not invalidate geometry history.
            invalidate();
        }
        sceneEpoch_ = scene.sceneEpoch;
        publicationRevision_ = scene.publicationRevision;
        geometryRevisions_ = scene.geometryRevisions;
        published_ = true;
        denseSlots_.clear();
        if (scene.primitives.size() > entries_.size()) {
            // No unbounded table growth. Oversized plans remain direct; a smaller
            // eligible opaque subset can still use the stateless safe selector.
            entries_.assign(entries_.size(), {});
            slots_.clear();
            return;
        }
        std::fill(live_.begin(), live_.end(), uint8_t{ 0 });
        const auto key = [](GpuScenePrimitiveHandle h) {
            return (static_cast<uint64_t>(h.generation) << 32u) | h.slot;
        };
        for (auto handle : scene.densePrimitiveHandles) {
            if (!handle.isValid()) throw std::invalid_argument("invalid LOD history primitive handle");
            if (auto it = slots_.find(key(handle)); it != slots_.end()) live_[it->second] = 1;
        }
        freeSlots_.clear();
        for (uint32_t slot = 0; slot < entries_.size(); ++slot) if (!live_[slot]) {
            if (entries_[slot].primitive.isValid()) slots_.erase(key(entries_[slot].primitive));
            entries_[slot] = {};
            freeSlots_.push_back(slot);
        }
        for (size_t dense = 0; dense < scene.primitives.size(); ++dense) {
            const auto handle = scene.densePrimitiveHandles[dense];
            const auto& primitive = scene.primitives[dense];
            if (primitive.binding.y >= scene.denseGeometryHandles.size())
                throw std::invalid_argument("invalid LOD history geometry binding");
            auto found = slots_.find(key(handle));
            uint32_t slot;
            if (found != slots_.end()) slot = found->second;
            else {
                slot = freeSlots_.back();
                freeSlots_.pop_back();
                slots_.emplace(key(handle), slot);
            }
            // Reject aliases: a dispatch must have at most one writer per slot.
            if (live_[slot] == 2) throw std::invalid_argument("duplicate LOD history primitive handle");
            live_[slot] = 2;
            auto& entry = entries_[slot];
            const auto geometry = scene.denseGeometryHandles[primitive.binding.y];
            if (entry.primitive != handle || entry.geometry != geometry ||
                entry.layoutRevision != primitive.revisions.x || entry.productRevision != primitive.revisions.y) {
                entry = { handle, geometry, primitive.revisions.x, primitive.revisions.y, newToken(), {} };
            }
            denseSlots_.push_back(slot);
        }
    }

    void GpuSceneLodHistory::updateView(const ViewTransportRecord& view, ViewHistoryContext context) {
        if (!viewValid_ || context != context_ || view.projection != view_.projection ||
            glm::uvec3(view.renderInfo) != glm::uvec3(view_.renderInfo) || view.worldUnits != view_.worldUnits ||
            frameSerial_ == UINT32_MAX) invalidate();
        view_ = view;
        context_ = context;
        viewValid_ = true;
        frameSerial_ = frameSerial_ == UINT32_MAX ? 1u : frameSerial_ + 1u;
    }

    GpuSceneLodHistoryBinding GpuSceneLodHistory::binding(uint32_t densePrimitive) const noexcept {
        if (densePrimitive >= denseSlots_.size()) return {};
        const uint32_t slot = denseSlots_[densePrimitive];
        return { slot, static_cast<uint32_t>(entries_[slot].token), static_cast<uint32_t>(entries_[slot].token >> 32u) };
    }

    uint32_t GpuSceneLodHistory::previous(GpuSceneLodHistoryBinding binding) const noexcept {
        if (binding.slot >= entries_.size() || frameSerial_ <= 1u) return InvalidGpuSceneIndex;
        const auto& previous = entries_[binding.slot].oracle;
        return previous.tokenLow == binding.tokenLow && previous.tokenHigh == binding.tokenHigh &&
            previous.lastFrame == frameSerial_ - 1u ? previous.selectedLod : InvalidGpuSceneIndex;
    }

    void GpuSceneLodHistory::record(GpuSceneLodHistoryBinding binding, uint32_t lod) noexcept {
        if (binding.slot < entries_.size())
            entries_[binding.slot].oracle = { binding.tokenLow, binding.tokenHigh, lod, frameSerial_ };
    }

} // namespace Iridium
