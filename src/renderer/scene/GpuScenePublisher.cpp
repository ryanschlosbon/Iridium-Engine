#include "GpuScenePublisher.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace Iridium {
namespace {

    template<typename T>
    bool bitsEqual(const T& lhs, const T& rhs) noexcept {
        static_assert(std::is_trivially_copyable_v<T>);
        return std::memcmp(&lhs, &rhs, sizeof(T)) == 0;
    }

    bool geometryEqual(const GpuSceneObservedPrimitive& lhs,
        const GpuSceneObservedPrimitive& rhs) noexcept {
        return lhs.legacyGeometry == rhs.legacyGeometry &&
            lhs.firstIndex == rhs.firstIndex && lhs.indexCount == rhs.indexCount &&
            lhs.vertexOffset == rhs.vertexOffset &&
            lhs.indexType == rhs.indexType && lhs.vertexLayout == rhs.vertexLayout &&
            lhs.geometryProductRevision == rhs.geometryProductRevision &&
            bitsEqual(lhs.localBoundsSphere, rhs.localBoundsSphere) &&
            bitsEqual(lhs.localBoundsMin, rhs.localBoundsMin) &&
            bitsEqual(lhs.localBoundsMax, rhs.localBoundsMax);
    }

    bool primitiveEqual(const GpuSceneObservedPrimitive& lhs,
        const GpuSceneObservedPrimitive& rhs) noexcept {
        return lhs.identity == rhs.identity &&
            lhs.geometryIdentity == rhs.geometryIdentity &&
            lhs.material == rhs.material && lhs.pipeline == rhs.pipeline &&
            lhs.primitiveFlags == rhs.primitiveFlags &&
            lhs.consumerMask == rhs.consumerMask &&
            lhs.geometryProductRevision == rhs.geometryProductRevision &&
            lhs.materialRevision == rhs.materialRevision;
    }

    bool observationRevisionsEqual(
        std::span<const GpuSceneObservedInstance> lhs,
        std::span<const GpuSceneObservedInstance> rhs) noexcept {
        if (lhs.size() != rhs.size()) return false;
        for (size_t index = 0; index < lhs.size(); ++index) {
            if (lhs[index].observationRevision == 0u ||
                lhs[index].observationRevision !=
                    rhs[index].observationRevision ||
                lhs[index].identity.owner != rhs[index].identity.owner)
                return false;
        }
        return true;
    }

    GpuSceneFloat4 pack4(const glm::vec4& value) noexcept {
        return { value.x, value.y, value.z, value.w };
    }

    bool validLodChildren(const GpuSceneObservedPrimitive& base) noexcept {
        if (base.lodChildren.size() >= MaximumGpuSceneLodLevels) return false;
        float previousError = 0.0f;
        uint32_t previousCount = base.indexCount;
        for (size_t index = 0; index < base.lodChildren.size(); ++index) {
            const auto& child = base.lodChildren[index];
            if (child.identity.modelAssetGuid != base.geometryIdentity.modelAssetGuid ||
                child.identity.sourcePrimitiveGuid != base.geometryIdentity.sourcePrimitiveGuid ||
                child.identity.primitiveGuid.isNil() ||
                child.identity.primitiveGuid == base.geometryIdentity.primitiveGuid ||
                !child.geometry.isValid() || child.indexCount == 0 ||
                child.indexCount % 3 != 0 || child.indexCount >= previousCount ||
                child.indexType > 1 || child.vertexLayout != base.vertexLayout ||
                !std::isfinite(child.geometricError) || child.geometricError < previousError ||
                !std::isfinite(child.localBoundsSphere.w) || child.localBoundsSphere.w < 0.0f)
                return false;
            for (size_t earlier = 0; earlier < index; ++earlier)
                if (base.lodChildren[earlier].identity == child.identity) return false;
            for (glm::length_t axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(child.localBoundsSphere[axis]) ||
                    !std::isfinite(child.localBoundsMin[axis]) ||
                    !std::isfinite(child.localBoundsMax[axis]) ||
                    child.localBoundsMin[axis] > child.localBoundsMax[axis] ||
                    child.localBoundsMin[axis] < base.localBoundsMin[axis] ||
                    child.localBoundsMax[axis] > base.localBoundsMax[axis]) return false;
            }
            previousError = child.geometricError;
            previousCount = child.indexCount;
        }
        return true;
    }

} // namespace

    GpuScenePublisher::GpuScenePublisher(GpuSceneCapacity capacity)
        : capacity_(capacity),
          instanceSlots_(capacity.maximumInstances),
          primitiveSlots_(capacity.maximumPrimitives),
          geometrySlots_(capacity.maximumGeometries) {
        geometrySources_.reserve((std::min)(capacity.maximumGeometries, 4096u));
        instanceSources_.reserve((std::min)(capacity.maximumInstances, 4096u));
        fallbackOwners_.reserve((std::min)(capacity.maximumInstances, 4096u));
        observationOrderScratch_.reserve(
            (std::min)(capacity.maximumInstances, 4096u));
        primitiveOrderScratch_.reserve(1024);
        preflightGeometryScratch_.reserve(1024);
        geometryLookupScratch_.reserve(
            (std::min)(capacity.maximumGeometries, 4096u));
    }

    const GpuScenePackedTables& GpuScenePublisher::synchronize(
        uint64_t sceneEpoch,
        std::span<const GpuSceneObservedInstance> observations,
        uint64_t retireAfterSerial, uint64_t completedSerial) {
        stats_ = {};
        (void)instanceSlots_.collect(completedSerial);
        (void)primitiveSlots_.collect(completedSerial);
        (void)geometrySlots_.collect(completedSerial);

        if (cachedObservationsValid_ && sceneEpoch == sceneEpoch_ &&
            cachedCapacityFallbackInstances_ == 0u &&
            packed_.directFallbackInstances.empty() &&
            observationRevisionsEqual(observations, cachedObservations_)) {
            stats_.activeInstances = static_cast<uint32_t>(
                packed_.instances.size());
            stats_.activePrimitives = static_cast<uint32_t>(
                packed_.primitives.size());
            stats_.activeGeometries = static_cast<uint32_t>(
                packed_.geometries.size());
            stats_.unchangedFastPath = 1u;
            return packed_;
        }
        fallbackOwners_.clear();

        if (sceneEpoch_ != 0 && sceneEpoch != sceneEpoch_) {
            for (const auto& [key, state] : instances_) {
                (void)key;
                if (instanceSlots_.retire(state.handle, retireAfterSerial))
                    ++stats_.retiredInstances;
            }
            for (const auto& [key, state] : primitives_) {
                (void)key;
                if (primitiveSlots_.retire(state.handle, retireAfterSerial))
                    ++stats_.retiredPrimitives;
            }
            for (const auto& [key, state] : geometries_) {
                (void)key;
                if (geometrySlots_.retire(state.handle, retireAfterSerial))
                    ++stats_.retiredGeometries;
            }
            instances_.clear();
            primitives_.clear();
            geometries_.clear();
        }
        sceneEpoch_ = sceneEpoch;
        for (auto& [key, state] : instances_) { (void)key; state.seen = false; }
        for (auto& [key, state] : primitives_) { (void)key; state.seen = false; }
        for (auto& [key, state] : geometries_) { (void)key; state.seen = false; }

        observationOrderScratch_.clear();
        observationOrderScratch_.reserve(observations.size());
        for (const auto& observation : observations)
            observationOrderScratch_.push_back(&observation);
        std::ranges::sort(observationOrderScratch_, {}, [](const auto* value) {
            return value->identity.owner;
        });
        SceneEntityUuid previousOwner;
        bool hasPreviousOwner = false;
        for (const GpuSceneObservedInstance* observation :
                observationOrderScratch_) {
            const SceneEntityUuid owner = observation->identity.owner;
            const bool duplicateOwner = hasPreviousOwner && owner == previousOwner;
            previousOwner = owner;
            hasPreviousOwner = true;
            if (owner.isNil() || duplicateOwner ||
                observation->primitives.empty()) {
                fallbackOwners_.push_back(owner);
                ++stats_.capacityFallbackInstances;
                continue;
            }

            primitiveOrderScratch_.clear();
            primitiveOrderScratch_.reserve(observation->primitives.size());
            for (const auto& primitive : observation->primitives)
                primitiveOrderScratch_.push_back(&primitive);
            std::ranges::sort(primitiveOrderScratch_, {}, [](const auto* value) {
                return value->identity.primitiveGuid;
            });
            preflightGeometryScratch_.clear();
            uint32_t requiredPrimitiveSlots = 0;
            bool duplicatePrimitive = false;
            AssetGuid previousPrimitive;
            bool hasPreviousPrimitive = false;
            for (const GpuSceneObservedPrimitive* primitive :
                    primitiveOrderScratch_) {
                duplicatePrimitive = duplicatePrimitive ||
                    (hasPreviousPrimitive &&
                        primitive->identity.primitiveGuid == previousPrimitive);
                previousPrimitive = primitive->identity.primitiveGuid;
                hasPreviousPrimitive = true;
                const PrimitiveKey key{ owner,
                    primitive->identity.primitiveGuid };
                if (!primitives_.contains(key)) ++requiredPrimitiveSlots;
                if (!geometries_.contains(primitive->geometryIdentity))
                    preflightGeometryScratch_.push_back(
                        primitive->geometryIdentity);
                duplicatePrimitive = duplicatePrimitive || !validLodChildren(*primitive);
                for (const auto& child : primitive->lodChildren) {
                    if (!geometries_.contains(child.identity))
                        preflightGeometryScratch_.push_back(child.identity);
                }
            }
            std::ranges::sort(preflightGeometryScratch_);
            const auto uniqueGeometryEnd = std::ranges::unique(
                preflightGeometryScratch_).begin();
            preflightGeometryScratch_.erase(uniqueGeometryEnd,
                preflightGeometryScratch_.end());
            auto instanceIt = instances_.find(owner);
            const uint32_t requiredInstanceSlots =
                instanceIt == instances_.end() ? 1u : 0u;
            if (duplicatePrimitive ||
                requiredInstanceSlots > instanceSlots_.availableCount() ||
                requiredPrimitiveSlots > primitiveSlots_.availableCount() ||
                preflightGeometryScratch_.size() >
                    geometrySlots_.availableCount()) {
                fallbackOwners_.push_back(owner);
                ++stats_.capacityFallbackInstances;
                continue;
            }
            if (instanceIt == instances_.end()) {
                const auto handle = instanceSlots_.allocate();
                if (!handle) throw std::logic_error(
                    "GPU-scene instance preflight disagreed with allocator");
                InstanceState state;
                state.handle = *handle;
                state.identity = observation->identity;
                state.mobility = observation->mobility;
                state.flags = observation->flags | GpuSceneInstanceHistoryReset;
                state.maximumLod = (std::min)(observation->maximumLod,
                    MaximumGpuSceneLodLevels - 1u);
                state.consumerMask = observation->consumerMask;
                state.currentWorld = observation->worldTransform;
                state.previousWorld = observation->worldTransform;
                state.sphere = observation->worldBoundsSphere;
                state.minimum = observation->worldBoundsMin;
                state.maximum = observation->worldBoundsMax;
                state.instanceRevision = ++recordRevision_;
                state.currentTransformRevision = ++recordRevision_;
                state.previousTransformRevision = ++recordRevision_;
                instanceIt = instances_.emplace(owner, std::move(state)).first;
                ++stats_.changedInstances;
                stats_.changedTransforms += 2;
            }
            InstanceState& instance = instanceIt->second;
            instance.seen = true;
            const bool transformChanged = !bitsEqual(instance.currentWorld,
                observation->worldTransform);
            if (transformChanged) {
                instance.previousWorld = instance.currentWorld;
                instance.currentWorld = observation->worldTransform;
                instance.previousTransformRevision = ++recordRevision_;
                instance.currentTransformRevision = ++recordRevision_;
                instance.flags &= ~GpuSceneInstanceHistoryReset;
                instance.instanceRevision = ++recordRevision_;
                stats_.changedTransforms += 2;
                ++stats_.changedInstances;
            }
            const bool instanceDataChanged =
                instance.identity != observation->identity ||
                instance.mobility != observation->mobility ||
                (instance.flags & ~GpuSceneInstanceHistoryReset) !=
                    observation->flags ||
                instance.maximumLod != (std::min)(observation->maximumLod,
                    MaximumGpuSceneLodLevels - 1u) ||
                instance.consumerMask != observation->consumerMask ||
                !bitsEqual(instance.sphere, observation->worldBoundsSphere) ||
                !bitsEqual(instance.minimum, observation->worldBoundsMin) ||
                !bitsEqual(instance.maximum, observation->worldBoundsMax);
            if (instanceDataChanged) {
                instance.identity = observation->identity;
                instance.mobility = observation->mobility;
                instance.flags = observation->flags |
                    (instance.flags & GpuSceneInstanceHistoryReset);
                instance.maximumLod = (std::min)(observation->maximumLod,
                    MaximumGpuSceneLodLevels - 1u);
                instance.consumerMask = observation->consumerMask;
                instance.sphere = observation->worldBoundsSphere;
                instance.minimum = observation->worldBoundsMin;
                instance.maximum = observation->worldBoundsMax;
                instance.instanceRevision = ++recordRevision_;
                if (!transformChanged) ++stats_.changedInstances;
            }

            instance.activePrimitives.clear();
            for (const GpuSceneObservedPrimitive* observedPrimitive :
                    primitiveOrderScratch_) {
                const PrimitiveKey primitiveKey{
                    owner, observedPrimitive->identity.primitiveGuid };
                const auto& children = observedPrimitive->lodChildren;
                observeGeometry(*observedPrimitive, children.empty() ? std::nullopt :
                    std::optional(children.front().identity), 0.0f, 0u);
                for (size_t childIndex = 0; childIndex < children.size(); ++childIndex) {
                    const auto& child = children[childIndex];
                    GpuSceneObservedPrimitive childGeometry{};
                    childGeometry.geometryIdentity = child.identity;
                    childGeometry.legacyGeometry = child.geometry;
                    childGeometry.firstIndex = child.firstIndex;
                    childGeometry.indexCount = child.indexCount;
                    childGeometry.vertexOffset = child.vertexOffset;
                    childGeometry.indexType = child.indexType;
                    childGeometry.vertexLayout = child.vertexLayout;
                    childGeometry.localBoundsSphere = child.localBoundsSphere;
                    childGeometry.localBoundsMin = child.localBoundsMin;
                    childGeometry.localBoundsMax = child.localBoundsMax;
                    childGeometry.geometryProductRevision = observedPrimitive->geometryProductRevision;
                    observeGeometry(childGeometry, childIndex + 1 == children.size()
                        ? std::nullopt : std::optional(children[childIndex + 1].identity),
                        child.geometricError, static_cast<uint32_t>(childIndex + 1));
                }

                auto primitiveIt = primitives_.find(primitiveKey);
                if (primitiveIt == primitives_.end()) {
                    const auto handle = primitiveSlots_.allocate();
                    if (!handle) throw std::logic_error(
                        "GPU-scene primitive preflight disagreed with allocator");
                    PrimitiveState state;
                    state.handle = *handle;
                    state.value = *observedPrimitive;
                    state.revision = ++recordRevision_;
                    primitiveIt = primitives_.emplace(
                        primitiveKey, std::move(state)).first;
                    ++stats_.changedPrimitives;
                } else if (!primitiveEqual(primitiveIt->second.value,
                        *observedPrimitive)) {
                    primitiveIt->second.value = *observedPrimitive;
                    primitiveIt->second.revision = ++recordRevision_;
                    ++stats_.changedPrimitives;
                }
                primitiveIt->second.seen = true;
                instance.activePrimitives.push_back(primitiveKey);
            }
        }

        for (auto it = instances_.begin(); it != instances_.end();) {
            if (it->second.seen) { ++it; continue; }
            if (instanceSlots_.retire(it->second.handle, retireAfterSerial))
                ++stats_.retiredInstances;
            it = instances_.erase(it);
        }
        for (auto it = primitives_.begin(); it != primitives_.end();) {
            if (it->second.seen) { ++it; continue; }
            if (primitiveSlots_.retire(it->second.handle, retireAfterSerial))
                ++stats_.retiredPrimitives;
            it = primitives_.erase(it);
        }
        for (auto it = geometries_.begin(); it != geometries_.end();) {
            if (it->second.seen) { ++it; continue; }
            if (geometrySlots_.retire(it->second.handle, retireAfterSerial))
                ++stats_.retiredGeometries;
            it = geometries_.erase(it);
        }

        geometrySources_.resize(geometries_.size());
        size_t geometrySourceIndex = 0;
        for (const auto& [identity, state] : geometries_) {
            const auto& value = state.value;
            geometrySources_[geometrySourceIndex++] = {
                .handle = state.handle,
                .localBoundsSphere = value.localBoundsSphere,
                .localBoundsMin = value.localBoundsMin,
                .localBoundsMax = value.localBoundsMax,
                .firstIndex = value.firstIndex,
                .indexCount = value.indexCount,
                .vertexOffset = value.vertexOffset,
                .indexType = value.indexType,
                .vertexArena = value.legacyGeometry.id,
                .indexArena = value.legacyGeometry.id,
                .vertexLayout = value.vertexLayout,
                .flags = GpuSceneGeometryLegacyRhiHandle,
                .productRevision = value.geometryProductRevision,
                .recordRevision = state.revision,
                .identity = identity,
                .coarserLod = state.coarserIdentity
                    ? geometries_.at(*state.coarserIdentity).handle : GpuSceneGeometryHandle{},
                .geometricError = state.geometricError,
                .lodLevel = state.lodLevel,
            };
        }
        instanceSources_.resize(instances_.size());
        size_t instanceSourceIndex = 0;
        for (const auto& [owner, state] : instances_) {
            (void)owner;
            GpuSceneInstanceSource& source =
                instanceSources_[instanceSourceIndex++];
            source.handle = state.handle;
            source.mobility = state.mobility;
            source.flags = state.flags;
            source.maximumLod = state.maximumLod;
            source.consumerMask = state.consumerMask;
            source.instanceRevision = state.instanceRevision;
            source.currentTransformRevision = state.currentTransformRevision;
            source.previousTransformRevision = state.previousTransformRevision;
            source.currentWorld = state.currentWorld;
            source.previousWorld = state.previousWorld;
            source.worldBoundsSphere = state.sphere;
            source.worldBoundsMin = state.minimum;
            source.worldBoundsMax = state.maximum;
            source.identity = state.identity;
            source.primitives.clear();
            source.primitives.reserve(state.activePrimitives.size());
            for (const PrimitiveKey& key : state.activePrimitives) {
                const auto primitive = primitives_.find(key);
                if (primitive == primitives_.end()) continue;
                const auto geometry = geometries_.find(
                    primitive->second.value.geometryIdentity);
                if (geometry == geometries_.end()) continue;
                const auto& value = primitive->second.value;
                source.primitives.push_back({
                    .handle = primitive->second.handle,
                    .geometry = geometry->second.handle,
                    .material = value.material,
                    .pipeline = value.pipeline,
                    .flags = value.primitiveFlags,
                    .consumerMask = value.consumerMask,
                    .geometryProductRevision = value.geometryProductRevision,
                    .instanceBindingRevision = static_cast<uint32_t>(
                        primitive->second.revision),
                    .materialRevision = value.materialRevision,
                    .recordRevision = primitive->second.revision,
                    .identity = value.identity,
                });
            }
        }

        ++publicationRevision_;
        packActiveSources();
        stats_.activeInstances = static_cast<uint32_t>(packed_.instances.size());
        stats_.activePrimitives = static_cast<uint32_t>(packed_.primitives.size());
        stats_.activeGeometries = static_cast<uint32_t>(packed_.geometries.size());
        stats_.changedInstanceBytes = static_cast<uint64_t>(
            stats_.changedInstances) * sizeof(GpuSceneInstanceRecord);
        stats_.changedTransformBytes = static_cast<uint64_t>(
            stats_.changedTransforms) * sizeof(GpuSceneAffineTransform);
        stats_.changedPrimitiveBytes = static_cast<uint64_t>(
            stats_.changedPrimitives) * sizeof(GpuScenePrimitiveRecord);
        stats_.changedGeometryBytes = static_cast<uint64_t>(
            stats_.changedGeometries) * sizeof(GpuSceneGeometryRecord);
        cachedObservations_.assign(observations.begin(), observations.end());
        cachedCapacityFallbackInstances_ = stats_.capacityFallbackInstances;
        cachedObservationsValid_ = true;
        return packed_;
    }

    void GpuScenePublisher::observeGeometry(const GpuSceneObservedPrimitive& value,
        std::optional<GpuSceneGeometryIdentity> coarserIdentity,
        float geometricError, uint32_t lodLevel) {
        auto found = geometries_.find(value.geometryIdentity);
        const bool inserted = found == geometries_.end();
        if (inserted) {
            const auto handle = geometrySlots_.allocate();
            if (!handle) throw std::logic_error(
                "GPU-scene geometry preflight disagreed with allocator");
            GeometryState state;
            state.handle = *handle;
            found = geometries_.emplace(value.geometryIdentity, std::move(state)).first;
        }
        auto& state = found->second;
        if (inserted || !geometryEqual(state.value, value) ||
            state.coarserIdentity != coarserIdentity ||
            state.geometricError != geometricError || state.lodLevel != lodLevel) {
            state.value = value;
            state.value.lodChildren.clear(); // Stored once as separate geometry states.
            state.coarserIdentity = coarserIdentity;
            state.geometricError = geometricError;
            state.lodLevel = lodLevel;
            state.revision = ++recordRevision_;
            ++stats_.changedGeometries;
        }
        state.seen = true;
    }

    void GpuScenePublisher::packActiveSources() {
        const uint64_t previousSceneEpoch = packed_.sceneEpoch;
        std::swap(packed_.transforms, packedHistoryScratch_.transforms);
        std::swap(packed_.instances, packedHistoryScratch_.instances);
        std::swap(packed_.primitives, packedHistoryScratch_.primitives);
        std::swap(packed_.geometries, packedHistoryScratch_.geometries);
        std::swap(packed_.transformRevisions,
            packedHistoryScratch_.transformRevisions);
        std::swap(packed_.instanceRevisions,
            packedHistoryScratch_.instanceRevisions);
        std::swap(packed_.primitiveRevisions,
            packedHistoryScratch_.primitiveRevisions);
        std::swap(packed_.geometryRevisions,
            packedHistoryScratch_.geometryRevisions);
        packed_.abiVersion = GpuSceneAbiVersion;
        packed_.sceneEpoch = sceneEpoch_;
        packed_.publicationRevision = publicationRevision_;
        packed_.transforms.clear();
        packed_.instances.clear();
        packed_.primitives.clear();
        packed_.geometries.clear();
        packed_.transformRevisions.clear();
        packed_.instanceRevisions.clear();
        packed_.primitiveRevisions.clear();
        packed_.geometryRevisions.clear();
        packed_.primitiveIdentities.clear();
        packed_.geometryIdentities.clear();
        packed_.denseInstanceHandles.clear();
        packed_.densePrimitiveHandles.clear();
        packed_.denseGeometryHandles.clear();
        packed_.directFallbackInstances.clear();
        packed_.invalidSourceCount = 0;
        packed_.capacityOmittedInstanceCount = 0;

        packed_.geometries.reserve(geometrySources_.size());
        packed_.geometryRevisions.reserve(geometrySources_.size());
        packed_.geometryIdentities.reserve(geometrySources_.size());
        packed_.denseGeometryHandles.reserve(geometrySources_.size());
        geometryLookupScratch_.clear();
        geometryLookupScratch_.reserve(geometrySources_.size());
        for (const GpuSceneGeometrySource& source : geometrySources_) {
            const uint32_t denseSlot = static_cast<uint32_t>(
                packed_.geometries.size());
            packed_.geometries.push_back({
                .localBoundsSphere = pack4(source.localBoundsSphere),
                .localBoundsMin = pack4(source.localBoundsMin),
                .localBoundsMax = pack4(source.localBoundsMax),
                .draw = { source.firstIndex, source.indexCount,
                    std::bit_cast<uint32_t>(source.vertexOffset),
                    source.indexType },
                .storage = { source.vertexArena, source.indexArena,
                    source.vertexLayout, source.flags },
                .state = { source.handle.generation,
                    static_cast<uint32_t>(packed_.geometryIdentities.size()),
                    InvalidGpuSceneIndex, source.productRevision },
            });
            packed_.geometries.back().localBoundsMin.w = source.geometricError;
            packed_.geometries.back().localBoundsMax.w = static_cast<float>(source.lodLevel);
            packed_.geometryRevisions.push_back(source.recordRevision);
            packed_.geometryIdentities.push_back(source.identity);
            packed_.denseGeometryHandles.push_back(source.handle);
            geometryLookupScratch_.push_back({ source.handle, denseSlot });
        }
        std::ranges::sort(geometryLookupScratch_, {}, &std::pair<
            GpuSceneGeometryHandle, uint32_t>::first);
        for (size_t index = 0; index < geometrySources_.size(); ++index) {
            const auto next = geometrySources_[index].coarserLod;
            if (!next.isValid()) continue;
            const auto found = std::ranges::lower_bound(geometryLookupScratch_, next, {},
                &std::pair<GpuSceneGeometryHandle, uint32_t>::first);
            if (found == geometryLookupScratch_.end() || found->first != next)
                throw std::logic_error("GPU-scene LOD link lost during atomic publication");
            packed_.geometries[index].state.z = found->second;
        }

        size_t totalPrimitives = 0;
        for (const auto& source : instanceSources_)
            totalPrimitives += source.primitives.size();
        packed_.instances.reserve(instanceSources_.size());
        packed_.instanceRevisions.reserve(instanceSources_.size());
        packed_.instanceIdentities.resize(instanceSources_.size());
        packed_.denseInstanceHandles.reserve(instanceSources_.size());
        packed_.transforms.reserve(instanceSources_.size() * 2u);
        packed_.transformRevisions.reserve(instanceSources_.size() * 2u);
        packed_.primitives.reserve(totalPrimitives);
        packed_.primitiveRevisions.reserve(totalPrimitives);
        packed_.primitiveIdentities.reserve(totalPrimitives);
        packed_.densePrimitiveHandles.reserve(totalPrimitives);
        for (const GpuSceneInstanceSource& source : instanceSources_) {
            const uint32_t denseInstance = static_cast<uint32_t>(
                packed_.instances.size());
            const uint32_t firstTransform = static_cast<uint32_t>(
                packed_.transforms.size());
            const uint32_t firstPrimitive = static_cast<uint32_t>(
                packed_.primitives.size());
            packed_.transforms.push_back(packGpuSceneAffine(source.currentWorld));
            packed_.transforms.push_back(packGpuSceneAffine(source.previousWorld));
            packed_.transformRevisions.push_back(
                source.currentTransformRevision);
            packed_.transformRevisions.push_back(
                source.previousTransformRevision);
            packed_.instances.push_back({
                .worldBoundsSphere = pack4(source.worldBoundsSphere),
                .worldBoundsMin = pack4(source.worldBoundsMin),
                .worldBoundsMax = pack4(source.worldBoundsMax),
                .references = { firstTransform, firstTransform + 1u,
                    firstPrimitive,
                    static_cast<uint32_t>(source.primitives.size()) },
                .state = { source.handle.generation,
                    static_cast<uint32_t>(source.mobility),
                    packGpuSceneInstanceFlags(source.flags, source.maximumLod),
                    source.consumerMask },
            });
            packed_.instanceRevisions.push_back(source.instanceRevision);
            packed_.instanceIdentities[denseInstance] = source.identity;
            packed_.denseInstanceHandles.push_back(source.handle);
            for (const GpuScenePrimitiveSource& primitive : source.primitives) {
                const auto geometry = std::ranges::lower_bound(
                    geometryLookupScratch_, primitive.geometry, {},
                    &std::pair<GpuSceneGeometryHandle, uint32_t>::first);
                if (geometry == geometryLookupScratch_.end() ||
                    geometry->first != primitive.geometry) {
                    packed_.directFallbackInstances.push_back(source.handle);
                    ++packed_.invalidSourceCount;
                    continue;
                }
                packed_.primitives.push_back({
                    .binding = { denseInstance, geometry->second,
                        primitive.material.id, primitive.pipeline.id },
                    .state = { primitive.handle.generation, primitive.flags,
                        static_cast<uint32_t>(packed_.primitiveIdentities.size()),
                        primitive.consumerMask },
                    .revisions = { primitive.primitiveLayoutRevision,
                        primitive.geometryProductRevision,
                        primitive.instanceBindingRevision,
                        primitive.materialRevision },
                });
                packed_.primitiveRevisions.push_back(primitive.recordRevision);
                packed_.primitiveIdentities.push_back(primitive.identity);
                packed_.densePrimitiveHandles.push_back(primitive.handle);
            }
        }

        // Revision tracking belongs to the exact packed bytes, not only to the
        // logical source objects. Insertion/removal can move dense geometry,
        // primitive ranges, and instance/transform references independently.
        const auto revise = [this, previousSceneEpoch](const auto& records,
            auto& revisions, const auto& previous, const auto& oldRevisions) {
            for (size_t index = 0; index < records.size(); ++index) {
                if (previousSceneEpoch == sceneEpoch_ &&
                    index < previous.size() &&
                    bitsEqual(records[index], previous[index])) {
                    revisions[index] = oldRevisions[index];
                }
                else {
                    revisions[index] = ++recordRevision_;
                }
            }
        };
        revise(packed_.transforms, packed_.transformRevisions,
            packedHistoryScratch_.transforms,
            packedHistoryScratch_.transformRevisions);
        revise(packed_.instances, packed_.instanceRevisions,
            packedHistoryScratch_.instances,
            packedHistoryScratch_.instanceRevisions);
        revise(packed_.primitives, packed_.primitiveRevisions,
            packedHistoryScratch_.primitives,
            packedHistoryScratch_.primitiveRevisions);
        revise(packed_.geometries, packed_.geometryRevisions,
            packedHistoryScratch_.geometries,
            packedHistoryScratch_.geometryRevisions);
        publishGpuSceneConsumerMembership(packed_);
    }

} // namespace Iridium
