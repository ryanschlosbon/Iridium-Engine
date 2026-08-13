#include "renderer/lighting/LocalShadow.h"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace Iridium {
namespace {

    struct RankedRequest {
        LocalShadowRequest request;
        uint32_t resolution = 0;
    };

    std::vector<RankedRequest> validateAndRank(
        std::span<const LocalShadowRequest> requests, LocalShadowKind kind) {
        std::vector<RankedRequest> ranked;
        ranked.reserve(requests.size());
        std::unordered_set<SceneEntityUuid, SceneEntityUuidHash> owners;
        for (const LocalShadowRequest& request : requests) {
            if (request.kind != kind) continue;
            if (request.owner.isNil() || request.quality > 3u ||
                !std::isfinite(request.conservativeContribution) ||
                request.conservativeContribution < 0.0f ||
                !owners.insert(request.owner).second)
                throw std::invalid_argument("Local shadow request is invalid.");
            ranked.push_back({ request,
                localShadowResolution(kind, request.quality) });
        }
        std::sort(ranked.begin(), ranked.end(),
            [](const RankedRequest& left, const RankedRequest& right) {
                return localShadowRequestPrecedes(left.request, right.request);
            });
        return ranked;
    }

    bool overlaps(const SpotShadowTile& left,
        const SpotShadowTile& right) noexcept {
        return left.x < right.x + right.size && right.x < left.x + left.size &&
            left.y < right.y + right.size && right.y < left.y + left.size;
    }

    bool placeSpot(SpotShadowTile& tile, uint32_t atlasResolution,
        std::span<const SpotShadowTile> occupied) {
        for (uint32_t y = 0; y + tile.size <= atlasResolution; y += tile.size) {
            for (uint32_t x = 0; x + tile.size <= atlasResolution; x += tile.size) {
                SpotShadowTile candidate = tile;
                candidate.x = x;
                candidate.y = y;
                bool free = true;
                for (const SpotShadowTile& allocation : occupied) {
                    if (overlaps(candidate, allocation)) {
                        free = false;
                        break;
                    }
                }
                if (free) {
                    tile = candidate;
                    return true;
                }
            }
        }
        return false;
    }

    uint32_t pointPoolIndex(uint32_t resolution) {
        if (resolution == 256u) return 0u;
        if (resolution == 512u) return 1u;
        if (resolution == 1024u) return 2u;
        throw std::invalid_argument("Unsupported point shadow resolution.");
    }

} // namespace

uint32_t localShadowResolution(LocalShadowKind kind, uint32_t quality) {
    if (quality > 3u)
        throw std::invalid_argument("Local shadow quality is invalid.");
    constexpr std::array<uint32_t, 4> Spot{ 512, 1024, 2048, 4096 };
    constexpr std::array<uint32_t, 4> Point{ 256, 512, 512, 1024 };
    return kind == LocalShadowKind::Spot ? Spot[quality] : Point[quality];
}

bool localShadowRequestPrecedes(const LocalShadowRequest& left,
    const LocalShadowRequest& right) noexcept {
    if (left.quality != right.quality) return left.quality > right.quality;
    if (left.priority != right.priority) return left.priority > right.priority;
    if (left.conservativeContribution != right.conservativeContribution)
        return left.conservativeContribution > right.conservativeContribution;
    return left.owner < right.owner;
}

std::vector<LocalShadowRequest> buildLocalShadowRequests(
    const LightingFramePacket& lighting, glm::vec3 cameraPosition) {
    if (!std::isfinite(cameraPosition.x) || !std::isfinite(cameraPosition.y) ||
        !std::isfinite(cameraPosition.z))
        throw std::invalid_argument("Local shadow camera position is invalid.");
    std::vector<LocalShadowRequest> result;
    result.reserve(lighting.activeSlots.size());
    for (uint32_t slot : lighting.activeSlots) {
        if (slot >= lighting.records.size() ||
            slot >= lighting.selectionMetadata.size())
            throw std::invalid_argument(
                "Local shadow request extraction received an invalid light slot.");
        const PackedGpuLight& record = lighting.records[slot];
        const uint32_t metadata = std::bit_cast<uint32_t>(record.shapeMetadata.z);
        const uint32_t type = metadata & 3u;
        if (type != static_cast<uint32_t>(PackedGpuLightType::Point) &&
            type != static_cast<uint32_t>(PackedGpuLightType::Spot)) continue;
        const LightSelectionMetadata& selection = lighting.selectionMetadata[slot];
        if ((metadata & PackedGpuLightCastsShadows) == 0u ||
            !selection.castsShadows || selection.owner.isNil()) continue;
        const glm::vec3 toCamera = cameraPosition -
            glm::vec3(record.positionRange);
        const float distanceSquared = glm::dot(toCamera, toCamera);
        const float radius = (std::max)(record.shapeMetadata.x, 0.01f);
        const float intensity = (std::max)(record.colorIntensity.w, 0.0f);
        const float chromaPeak = (std::max)({ record.colorIntensity.x,
            record.colorIntensity.y, record.colorIntensity.z, 0.0f });
        const float contribution = intensity * chromaPeak /
            (std::max)(distanceSquared, radius * radius);
        result.push_back({
            .owner = selection.owner,
            .kind = type == static_cast<uint32_t>(PackedGpuLightType::Point)
                ? LocalShadowKind::Point : LocalShadowKind::Spot,
            .lightSlot = slot,
            .quality = (metadata & PackedGpuLightShadowQualityMask) >>
                PackedGpuLightShadowQualityShift,
            .priority = selection.priority,
            .conservativeContribution = contribution,
        });
    }
    std::sort(result.begin(), result.end(), localShadowRequestPrecedes);
    return result;
}

StableSpotShadowAtlas::StableSpotShadowAtlas(SpotShadowAtlasConfig config)
    : config_(config) {
    if (config_.minimumTileResolution == 0u ||
        config_.atlasResolution < config_.minimumTileResolution ||
        config_.atlasResolution % config_.minimumTileResolution != 0u ||
        config_.guardTexels * 2u >= config_.minimumTileResolution)
        throw std::invalid_argument("Spot shadow atlas configuration is invalid.");
}

LocalShadowAllocationStats StableSpotShadowAtlas::reconcile(
    std::span<const LocalShadowRequest> requests) {
    const std::vector<RankedRequest> ranked = validateAndRank(
        requests, LocalShadowKind::Spot);
    LocalShadowAllocationStats stats;
    stats.requested = static_cast<uint32_t>(ranked.size());

    // First compute the capacity-selected set in a canonical fresh layout.
    std::vector<SpotShadowTile> ideal;
    ideal.reserve(ranked.size());
    for (const RankedRequest& candidate : ranked) {
        SpotShadowTile tile{ .owner = candidate.request.owner,
            .lightSlot = candidate.request.lightSlot,
            .size = candidate.resolution,
            .guardTexels = config_.guardTexels };
        if (placeSpot(tile, config_.atlasResolution, ideal))
            ideal.push_back(tile);
    }

    std::unordered_set<SceneEntityUuid, SceneEntityUuidHash> accepted;
    for (const SpotShadowTile& tile : ideal) accepted.insert(tile.owner);
    for (const SpotShadowTile& old : allocations_)
        if (!accepted.contains(old.owner)) ++stats.evicted;

    // Preserve compatible locations where possible, then place the rest by rank.
    std::vector<SpotShadowTile> next;
    next.reserve(ideal.size());
    for (const RankedRequest& candidate : ranked) {
        if (!accepted.contains(candidate.request.owner)) continue;
        const auto old = std::find_if(allocations_.begin(), allocations_.end(),
            [&](const SpotShadowTile& tile) {
                return tile.owner == candidate.request.owner &&
                    tile.size == candidate.resolution;
            });
        if (old == allocations_.end()) continue;
        SpotShadowTile retained = *old;
        retained.lightSlot = candidate.request.lightSlot;
        bool collision = false;
        for (const SpotShadowTile& tile : next)
            collision = collision || overlaps(retained, tile);
        if (!collision) {
            next.push_back(retained);
            ++stats.reused;
        }
    }
    for (const RankedRequest& candidate : ranked) {
        if (!accepted.contains(candidate.request.owner)) continue;
        if (std::any_of(next.begin(), next.end(), [&](const SpotShadowTile& tile) {
                return tile.owner == candidate.request.owner;
            })) continue;
        SpotShadowTile tile{ .owner = candidate.request.owner,
            .lightSlot = candidate.request.lightSlot,
            .size = candidate.resolution,
            .guardTexels = config_.guardTexels };
        if (!placeSpot(tile, config_.atlasResolution, next)) {
            // Retention produced fragmentation. Canonical repacking is deterministic
            // and only occurs when it is necessary to preserve the accepted set.
            next = ideal;
            stats.relocated = 0;
            stats.reused = 0;
            for (SpotShadowTile& assigned : next) {
                const auto request = std::find_if(ranked.begin(), ranked.end(),
                    [&](const RankedRequest& value) {
                        return value.request.owner == assigned.owner;
                    });
                assigned.lightSlot = request->request.lightSlot;
                assigned.guardTexels = config_.guardTexels;
                const auto previous = std::find_if(allocations_.begin(),
                    allocations_.end(), [&](const SpotShadowTile& value) {
                        return value.owner == assigned.owner;
                    });
                if (previous != allocations_.end() &&
                    (previous->x != assigned.x || previous->y != assigned.y ||
                        previous->size != assigned.size))
                    ++stats.relocated;
                else if (previous != allocations_.end()) ++stats.reused;
            }
            break;
        }
        const auto previous = std::find_if(allocations_.begin(),
            allocations_.end(), [&](const SpotShadowTile& value) {
                return value.owner == tile.owner;
            });
        if (previous != allocations_.end()) ++stats.relocated;
        next.push_back(tile);
    }
    std::sort(next.begin(), next.end(), [](const SpotShadowTile& left,
        const SpotShadowTile& right) { return left.owner < right.owner; });
    allocations_ = std::move(next);
    stats.allocated = static_cast<uint32_t>(allocations_.size());
    stats.omitted = stats.requested - stats.allocated;
    return stats;
}

StablePointShadowPools::StablePointShadowPools(PointShadowPoolConfig config)
    : config_(config) {
    if (std::ranges::any_of(config_.cubeCapacity,
            [](uint32_t value) { return value == 0u; }))
        throw std::invalid_argument("Point shadow pool capacity is invalid.");
}

LocalShadowAllocationStats StablePointShadowPools::reconcile(
    std::span<const LocalShadowRequest> requests) {
    const std::vector<RankedRequest> ranked = validateAndRank(
        requests, LocalShadowKind::Point);
    LocalShadowAllocationStats stats;
    stats.requested = static_cast<uint32_t>(ranked.size());
    std::array<uint32_t, 3> acceptedPerPool{};
    std::unordered_set<SceneEntityUuid, SceneEntityUuidHash> accepted;
    for (const RankedRequest& candidate : ranked) {
        const uint32_t pool = pointPoolIndex(candidate.resolution);
        if (acceptedPerPool[pool] >= config_.cubeCapacity[pool]) continue;
        ++acceptedPerPool[pool];
        accepted.insert(candidate.request.owner);
    }
    for (const PointShadowSlot& old : allocations_)
        if (!accepted.contains(old.owner)) ++stats.evicted;

    std::vector<PointShadowSlot> next;
    next.reserve(accepted.size());
    for (const RankedRequest& candidate : ranked) {
        if (!accepted.contains(candidate.request.owner)) continue;
        const auto old = std::find_if(allocations_.begin(), allocations_.end(),
            [&](const PointShadowSlot& slot) {
                return slot.owner == candidate.request.owner &&
                    slot.resolution == candidate.resolution;
            });
        if (old == allocations_.end()) continue;
        PointShadowSlot retained = *old;
        retained.lightSlot = candidate.request.lightSlot;
        next.push_back(retained);
        ++stats.reused;
    }
    for (const RankedRequest& candidate : ranked) {
        if (!accepted.contains(candidate.request.owner) ||
            std::any_of(next.begin(), next.end(), [&](const PointShadowSlot& slot) {
                return slot.owner == candidate.request.owner;
            })) continue;
        const uint32_t pool = pointPoolIndex(candidate.resolution);
        uint32_t index = 0;
        while (std::any_of(next.begin(), next.end(), [&](const PointShadowSlot& slot) {
            return slot.resolution == candidate.resolution &&
                slot.cubeIndex == index;
        })) ++index;
        if (index >= config_.cubeCapacity[pool])
            throw std::logic_error("Point shadow accepted set exceeded its pool.");
        if (std::any_of(allocations_.begin(), allocations_.end(),
            [&](const PointShadowSlot& slot) {
                return slot.owner == candidate.request.owner;
            })) ++stats.relocated;
        next.push_back({ candidate.request.owner, candidate.request.lightSlot,
            candidate.resolution, index });
    }
    std::sort(next.begin(), next.end(), [](const PointShadowSlot& left,
        const PointShadowSlot& right) { return left.owner < right.owner; });
    allocations_ = std::move(next);
    stats.allocated = static_cast<uint32_t>(allocations_.size());
    stats.omitted = stats.requested - stats.allocated;
    return stats;
}

std::array<PointShadowFace, 6> buildPointShadowFaces(
    glm::vec3 lightPosition, float nearPlane, float farPlane) {
    if (!std::isfinite(lightPosition.x) || !std::isfinite(lightPosition.y) ||
        !std::isfinite(lightPosition.z) || !std::isfinite(nearPlane) ||
        !std::isfinite(farPlane) || nearPlane <= 0.0f ||
        farPlane <= nearPlane)
        throw std::invalid_argument("Point shadow projection is invalid.");
    constexpr std::array<glm::vec3, 6> Directions{
        glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0),
        glm::vec3(0, 1, 0), glm::vec3(0, -1, 0),
        glm::vec3(0, 0, 1), glm::vec3(0, 0, -1),
    };
    constexpr std::array<glm::vec3, 6> Up{
        glm::vec3(0, -1, 0), glm::vec3(0, -1, 0),
        glm::vec3(0, 0, 1), glm::vec3(0, 0, -1),
        glm::vec3(0, -1, 0), glm::vec3(0, -1, 0),
    };
    glm::mat4 projection = glm::perspectiveRH_ZO(
        glm::radians(90.0f), 1.0f, nearPlane, farPlane);
    std::array<PointShadowFace, 6> result{};
    for (size_t index = 0; index < result.size(); ++index) {
        result[index].direction = Directions[index];
        result[index].up = Up[index];
        result[index].worldToShadowClip = projection * glm::lookAtRH(
            lightPosition, lightPosition + Directions[index], Up[index]);
    }
    return result;
}

SpotShadowProjection buildSpotShadowProjection(glm::vec3 lightPosition,
    glm::vec3 emissionDirection, float outerConeCosine,
    float nearPlane, float farPlane) {
    const auto finite3 = [](glm::vec3 value) {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
            std::isfinite(value.z);
    };
    if (!finite3(lightPosition) || !finite3(emissionDirection) ||
        glm::dot(emissionDirection, emissionDirection) < 1.0e-8f ||
        !std::isfinite(outerConeCosine) || outerConeCosine < 0.0f ||
        outerConeCosine >= 1.0f || !std::isfinite(nearPlane) ||
        !std::isfinite(farPlane) || nearPlane <= 0.0f ||
        farPlane <= nearPlane)
        throw std::invalid_argument("Spot shadow projection is invalid.");
    SpotShadowProjection result;
    result.lightForward = glm::normalize(emissionDirection);
    result.outerConeRadians = std::acos(outerConeCosine);
    const glm::vec3 upReference = std::abs(result.lightForward.y) < 0.99f
        ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);
    result.lightRight = glm::normalize(glm::cross(
        result.lightForward, upReference));
    glm::mat4 projection = glm::perspectiveRH_ZO(
        result.outerConeRadians * 2.0f, 1.0f, nearPlane, farPlane);
    projection[1][1] *= -1.0f;
    result.worldToShadowClip = projection * glm::lookAtRH(lightPosition,
        lightPosition + result.lightForward, upReference);
    return result;
}

LocalShadowCacheScheduler::LocalShadowCacheScheduler(
    LocalShadowScheduleConfig config) : config_(config) {
    if (config_.maximumRenderedTexels == 0u)
        throw std::invalid_argument("Local shadow update budget is invalid.");
}

void LocalShadowCacheScheduler::configure(LocalShadowScheduleConfig config) {
    if (currentSchedule_)
        throw std::logic_error(
            "Local shadow cache policy cannot change during a schedule.");
    if (config.maximumRenderedTexels == 0u)
        throw std::invalid_argument("Local shadow update budget is invalid.");
    config_ = config;
}

const LocalShadowSchedule& LocalShadowCacheScheduler::schedule(
    std::span<const LocalShadowCacheInput> inputs) {
    if (currentSchedule_)
        throw std::logic_error(
            "Local shadow schedule must be completed before rescheduling.");
    std::vector<LocalShadowCacheInput> ranked(inputs.begin(), inputs.end());
    std::unordered_set<SceneEntityUuid, SceneEntityUuidHash> owners;
    for (LocalShadowCacheInput& input : ranked) {
        if (input.request.owner.isNil() || input.request.quality > 3u ||
            input.resolution != localShadowResolution(
                input.request.kind, input.request.quality) ||
            input.allocationRevision == 0u || input.lightRevision == 0u ||
            input.projectionRevision == 0u || input.pipelineRevision == 0u ||
            !owners.insert(input.request.owner).second)
            throw std::invalid_argument("Local shadow cache input is invalid.");
    }
    std::sort(ranked.begin(), ranked.end(),
        [](const LocalShadowCacheInput& left,
            const LocalShadowCacheInput& right) {
            return localShadowRequestPrecedes(left.request, right.request);
        });

    // Allocator eviction/removal also retires the corresponding history.
    for (size_t index = stateOwners_.size(); index-- > 0;) {
        if (owners.contains(stateOwners_[index])) continue;
        stateOwners_.erase(stateOwners_.begin() + index);
        states_.erase(states_.begin() + index);
    }

    currentSchedule_.emplace();
    LocalShadowSchedule& result = *currentSchedule_;
    result.entries.reserve(ranked.size());
    result.stats.requests = static_cast<uint32_t>(ranked.size());
    uint64_t remaining = config_.maximumRenderedTexels;
    for (const LocalShadowCacheInput& input : ranked) {
        const auto ownerIt = std::find(stateOwners_.begin(), stateOwners_.end(),
            input.request.owner);
        State* state = ownerIt == stateOwners_.end() ? nullptr :
            &states_[static_cast<size_t>(ownerIt - stateOwners_.begin())];
        LocalShadowDirtyReason reason = LocalShadowDirtyReason::None;
        if (state == nullptr || !state->valid)
            reason = LocalShadowDirtyReason::NewAllocation;
        else if (state->published.allocationRevision != input.allocationRevision ||
            state->published.resolution != input.resolution ||
            state->published.request.kind != input.request.kind)
            reason = LocalShadowDirtyReason::AllocationChanged;
        else if (state->published.lightRevision != input.lightRevision)
            reason = LocalShadowDirtyReason::LightChanged;
        else if (state->published.projectionRevision != input.projectionRevision)
            reason = LocalShadowDirtyReason::ProjectionChanged;
        else if (state->published.pipelineRevision != input.pipelineRevision)
            reason = LocalShadowDirtyReason::PipelineChanged;
        else if (state->published.casterRevision != input.casterRevision)
            reason = LocalShadowDirtyReason::CasterChanged;

        const uint32_t faces = input.request.kind == LocalShadowKind::Point ? 6u : 1u;
        const uint64_t texels = static_cast<uint64_t>(input.resolution) *
            input.resolution * faces;
        LocalShadowScheduleEntry entry{ .owner = input.request.owner,
            .kind = input.request.kind, .dirtyReason = reason,
            .renderTexels = texels, .faceCount = faces };
        if (reason == LocalShadowDirtyReason::None) {
            entry.sampleable = true;
            ++result.stats.cacheHits;
            if (state != nullptr) state->staleAgeFrames = 0;
        }
        else {
            ++result.stats.dirty;
            if (texels <= remaining) {
                entry.update = true;
                entry.sampleable = true;
                remaining -= texels;
                result.stats.renderedTexels += texels;
                ++result.stats.updates;
                pendingUpdates_.push_back({ input.request.owner, input });
            }
            else if (reason == LocalShadowDirtyReason::CasterChanged &&
                state != nullptr && state->valid &&
                state->staleAgeFrames < config_.maximumCompatibleStaleFrames) {
                ++state->staleAgeFrames;
                entry.sampleable = true;
                entry.stale = true;
                entry.staleAgeFrames = state->staleAgeFrames;
                ++result.stats.staleSampled;
            }
            else {
                entry.sampleable = false;
                ++result.stats.unshadowed;
            }
        }
        result.entries.push_back(entry);
    }
    return result;
}

void LocalShadowCacheScheduler::markScheduledRendered() {
    if (!currentSchedule_)
        throw std::logic_error("No local shadow schedule is pending.");
    for (const PendingUpdate& update : pendingUpdates_) {
        auto ownerIt = std::find(stateOwners_.begin(), stateOwners_.end(),
            update.owner);
        if (ownerIt == stateOwners_.end()) {
            stateOwners_.push_back(update.owner);
            states_.push_back({});
            ownerIt = stateOwners_.end() - 1;
        }
        State& state = states_[static_cast<size_t>(ownerIt - stateOwners_.begin())];
        state.published = update.input;
        state.staleAgeFrames = 0;
        state.valid = true;
    }
    pendingUpdates_.clear();
    currentSchedule_.reset();
}

void LocalShadowCacheScheduler::reset() noexcept {
    states_.clear();
    stateOwners_.clear();
    pendingUpdates_.clear();
    currentSchedule_.reset();
}

} // namespace Iridium
