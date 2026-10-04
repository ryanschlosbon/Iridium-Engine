#include "GpuScenePublisher.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>

// M7R R5c.5: incremental publication.
//
// Byte identity with the full-scan publisher (tests/renderer/support/
// ReferenceGpuScenePublisher) rests on four facts:
//  1. The full scan visits observations in owner order and advances the
//     record-revision counter only when a state changes. An observation whose
//     (index, owner, nonzero revision) equals the previous call's has the
//     content it was accepted with, so re-reading it changes nothing, provided
//     no geometry it shares was changed earlier in the same scan. Re-reading
//     only the other observations, in the same order, therefore reproduces
//     every counter increment, slot allocation and statistic.
//  2. That proviso holds while the scene is coherent (every accepted
//     observation of a geometry carries the geometry's stored value) and no
//     re-read observation would change a geometry that a skipped observation
//     also references. Both are checked before anything is mutated; otherwise
//     the pass re-reads every observation, exactly as the full scan did.
//  3. Dense order is owner order for instances, primitive-GUID order inside an
//     instance and identity order for geometries, as the old std::map order.
//  4. Revisions are reassigned per record by comparing packed bytes at the same
//     dense index with the previous publication, in table order. Without an
//     added or removed instance, primitive or geometry, the dense layout is
//     unchanged and only records of re-read instances and changed geometries
//     can differ, so revising just those, in ascending order, assigns the
//     same numbers as revising every record.
// Slot retirement order cannot change a handle: retire() and collect() act
// per slot and allocate() scans from a cursor. It is kept in the old order.

namespace Iridium {
namespace {

    template<typename T>
    bool bitsEqual(const T& lhs, const T& rhs) noexcept {
        static_assert(std::is_trivially_copyable_v<T>);
        return std::memcmp(&lhs, &rhs, sizeof(T)) == 0;
    }

    bool observationRevisionsEqual(
        std::span<const GpuSceneObservedInstance> observations,
        std::span<const SceneEntityUuid> owners,
        std::span<const uint64_t> revisions) noexcept {
        if (observations.size() != owners.size()) return false;
        for (size_t index = 0; index < observations.size(); ++index) {
            if (observations[index].observationRevision == 0u ||
                observations[index].observationRevision != revisions[index] ||
                observations[index].identity.owner != owners[index])
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

    uint64_t loadBigEndian64(const uint8_t* bytes) noexcept {
        uint64_t value = 0;
        for (uint32_t index = 0; index < 8u; ++index)
            value = (value << 8u) | bytes[index];
        return value;
    }

    // The same strict order as the defaulted operator< of a 16-byte array
    // (lexicographic over unsigned bytes), as two big-endian words. Every
    // comparison has the same outcome, so std::ranges::sort produces the same
    // permutation as with the default comparator, ties included.
    struct Bytes16Less {
        template<typename Value>
        bool operator()(const Value& lhs, const Value& rhs) const noexcept {
            const auto& a = lhs.bytes();
            const auto& b = rhs.bytes();
            static_assert(sizeof(a) == 16u);
            const uint64_t lhsHigh = loadBigEndian64(a.data());
            const uint64_t rhsHigh = loadBigEndian64(b.data());
            if (lhsHigh != rhsHigh) return lhsHigh < rhsHigh;
            return loadBigEndian64(a.data() + 8) < loadBigEndian64(b.data() + 8);
        }
    };

    uint64_t geometryIdentityHash(const GpuSceneGeometryIdentity& identity) noexcept {
        static_assert(sizeof(GpuSceneGeometryIdentity) == 48);
        uint64_t words[6];
        std::memcpy(words, &identity, sizeof(words));
        uint64_t hash = 0x9e3779b97f4a7c15ull;
        for (const uint64_t word : words) {
            hash ^= word;
            hash *= 0xbf58476d1ce4e5b9ull;
            hash ^= hash >> 31u;
        }
        return hash;
    }

    template<typename Value>
    bool geometryValueEqual(const Value& lhs, const Value& rhs) noexcept {
        return lhs.legacyGeometry == rhs.legacyGeometry &&
            lhs.firstIndex == rhs.firstIndex && lhs.indexCount == rhs.indexCount &&
            lhs.vertexOffset == rhs.vertexOffset &&
            lhs.indexType == rhs.indexType && lhs.vertexLayout == rhs.vertexLayout &&
            lhs.geometryProductRevision == rhs.geometryProductRevision &&
            bitsEqual(lhs.localBoundsSphere, rhs.localBoundsSphere) &&
            bitsEqual(lhs.localBoundsMin, rhs.localBoundsMin) &&
            bitsEqual(lhs.localBoundsMax, rhs.localBoundsMax);
    }

} // namespace

    namespace {
        template<typename Value>
        Value baseGeometryValue(const GpuSceneObservedPrimitive& primitive) noexcept {
            Value value;
            value.legacyGeometry = primitive.legacyGeometry;
            value.firstIndex = primitive.firstIndex;
            value.indexCount = primitive.indexCount;
            value.vertexOffset = primitive.vertexOffset;
            value.indexType = primitive.indexType;
            value.vertexLayout = primitive.vertexLayout;
            value.geometryProductRevision = primitive.geometryProductRevision;
            value.localBoundsSphere = primitive.localBoundsSphere;
            value.localBoundsMin = primitive.localBoundsMin;
            value.localBoundsMax = primitive.localBoundsMax;
            return value;
        }

        template<typename Value>
        Value childGeometryValue(const GpuSceneObservedLodGeometry& child,
            uint32_t geometryProductRevision) noexcept {
            Value value;
            value.legacyGeometry = child.geometry;
            value.firstIndex = child.firstIndex;
            value.indexCount = child.indexCount;
            value.vertexOffset = child.vertexOffset;
            value.indexType = child.indexType;
            value.vertexLayout = child.vertexLayout;
            value.geometryProductRevision = geometryProductRevision;
            value.localBoundsSphere = child.localBoundsSphere;
            value.localBoundsMin = child.localBoundsMin;
            value.localBoundsMax = child.localBoundsMax;
            return value;
        }

        template<typename State, typename Value>
        bool geometryChanges(const State& state, const Value& value,
            const GpuSceneGeometryIdentity* coarser, float geometricError,
            uint32_t lodLevel) noexcept {
            return !geometryValueEqual(state.value, value) ||
                state.hasCoarser != (coarser != nullptr) ||
                (coarser != nullptr && state.coarserIdentity != *coarser) ||
                state.geometricError != geometricError ||
                state.lodLevel != lodLevel;
        }

        template<typename State>
        bool primitiveEqual(const State& state,
            const GpuSceneObservedPrimitive& primitive) noexcept {
            return state.identity == primitive.identity &&
                state.geometryIdentity == primitive.geometryIdentity &&
                state.material == primitive.material &&
                state.pipeline == primitive.pipeline &&
                state.primitiveFlags == primitive.primitiveFlags &&
                state.consumerMask == primitive.consumerMask &&
                state.geometryProductRevision ==
                    primitive.geometryProductRevision &&
                state.materialRevision == primitive.materialRevision;
        }

        template<typename State>
        void assignPrimitive(State& state,
            const GpuSceneObservedPrimitive& primitive) noexcept {
            state.identity = primitive.identity;
            state.geometryIdentity = primitive.geometryIdentity;
            state.material = primitive.material;
            state.pipeline = primitive.pipeline;
            state.primitiveFlags = primitive.primitiveFlags;
            state.consumerMask = primitive.consumerMask;
            state.geometryProductRevision = primitive.geometryProductRevision;
            state.materialRevision = primitive.materialRevision;
        }
    } // namespace

    GpuScenePublisher::GpuScenePublisher(GpuSceneCapacity capacity)
        : capacity_(capacity),
          instanceSlots_(capacity.maximumInstances),
          primitiveSlots_(capacity.maximumPrimitives),
          geometrySlots_(capacity.maximumGeometries) {
        fallbackOwners_.reserve((std::min)(capacity.maximumInstances, 4096u));
        observationOrderScratch_.reserve(
            (std::min)(capacity.maximumInstances, 4096u));
        observationEntries_.reserve(
            (std::min)(capacity.maximumInstances, 4096u));
        primitiveOrderScratch_.reserve(1024);
        preflightGeometryScratch_.reserve(1024);
        primitiveListScratch_.reserve(1024);
        geometryTable_.assign(1024u, InvalidPoolIndex);
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
            observationRevisionsEqual(observations, cachedOwners_,
                cachedRevisions_)) {
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
        ++pass_;
        passConflict_ = false;
        topologyChanged_ = false;
        bool fullScan = !coherent_;

        if (sceneEpoch_ != 0 && sceneEpoch != sceneEpoch_) {
            retireAllForEpochChange(retireAfterSerial);
            fullScan = true;
        }
        sceneEpoch_ = sceneEpoch;

        observationOrderScratch_.clear();
        observationOrderScratch_.reserve(observations.size());
        for (const auto& observation : observations)
            observationOrderScratch_.push_back(&observation);
        // The full scan's sort with an outcome-identical comparator, so
        // duplicate owners resolve alike.
        std::ranges::sort(observationOrderScratch_, Bytes16Less{},
            [](const auto* value) -> const SceneEntityUuid& {
                return value->identity.owner;
            });
        classifyObservations(observations, fullScan);
        if (!fullScan && incrementalWouldDiverge()) {
            fullScan = true;
            for (ObservationEntry& entry : observationEntries_)
                if (entry.kind == ObservationKind::Clean)
                    entry.kind = ObservationKind::Dirty;
        }

        // Grow the state pools at most once for the new instances of a pass
        // rather than repeatedly during the walk.
        size_t newInstances = 0, newPrimitives = 0;
        for (const ObservationEntry& entry : observationEntries_) {
            if (entry.kind != ObservationKind::Dirty ||
                entry.existing != InvalidPoolIndex) continue;
            ++newInstances;
            newPrimitives += entry.observation->primitives.size();
        }
        const auto reservePool = [](auto& pool, size_t freeCount, size_t added) {
            if (added <= freeCount) return;
            const size_t required = pool.size() + (added - freeCount);
            if (required > pool.capacity())
                pool.reserve((std::max)(required, pool.capacity() * 3u / 2u));
        };
        reservePool(primitivePool_, primitiveFree_.size(), newPrimitives);
        reservePool(instancePool_, instanceFree_.size(), newInstances);
        instanceOrderScratch_.clear();
        instanceOrderScratch_.reserve(instanceOrder_.size() +
            observationEntries_.size());
        retiredInstanceScratch_.clear();
        retiredPrimitiveScratch_.clear();
        deadGeometryCandidateScratch_.clear();
        touchedInstanceScratch_.clear();
        changedGeometryScratch_.clear();
        size_t oldCursor = 0;
        const auto releaseUnmatchedBefore = [&](const SceneEntityUuid* owner) {
            while (oldCursor < instanceOrder_.size()) {
                const uint32_t old = instanceOrder_[oldCursor];
                if (instancePool_[old].matchedPass != pass_) {
                    if (owner != nullptr && !(instancePool_[old].owner < *owner))
                        break;
                    releaseInstance(old);
                }
                ++oldCursor;
            }
        };
        for (const ObservationEntry& entry : observationEntries_) {
            const SceneEntityUuid owner = entry.observation->identity.owner;
            releaseUnmatchedBefore(&owner);
            switch (entry.kind) {
            case ObservationKind::Fallback:
                fallbackOwners_.push_back(owner);
                ++stats_.capacityFallbackInstances;
                if (entry.existing != InvalidPoolIndex)
                    releaseInstance(entry.existing);
                break;
            case ObservationKind::Clean:
                instanceOrderScratch_.push_back(entry.existing);
                break;
            case ObservationKind::Dirty: {
                const size_t index = static_cast<size_t>(
                    entry.observation - observations.data());
                const uint32_t accepted = processObservation(entry, index);
                if (accepted == InvalidPoolIndex) {
                    if (entry.existing != InvalidPoolIndex)
                        releaseInstance(entry.existing);
                    break;
                }
                instanceOrderScratch_.push_back(accepted);
                if (accepted == entry.existing)
                    touchedInstanceScratch_.push_back(accepted);
                break;
            }
            }
        }
        releaseUnmatchedBefore(nullptr);

        for (const uint32_t instance : retiredInstanceScratch_) {
            if (instanceSlots_.retire(instancePool_[instance].handle,
                    retireAfterSerial))
                ++stats_.retiredInstances;
            instanceFree_.push_back(instance);
        }
        for (const uint32_t primitive : retiredPrimitiveScratch_) {
            if (primitiveSlots_.retire(primitivePool_[primitive].handle,
                    retireAfterSerial))
                ++stats_.retiredPrimitives;
            primitiveFree_.push_back(primitive);
        }
        finishGeometries(retireAfterSerial);
        instanceOrder_.swap(instanceOrderScratch_);

        ++publicationRevision_;
        if (topologyChanged_ || fullScan || publicationRevision_ == 1u ||
            packed_.sceneEpoch != sceneEpoch_)
            packAll();
        else
            packTouched();
        coherent_ = !passConflict_;

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
        cachedOwners_.resize(observations.size());
        cachedRevisions_.resize(observations.size());
        for (size_t index = 0; index < observations.size(); ++index) {
            cachedOwners_[index] = observations[index].identity.owner;
            cachedRevisions_[index] = observations[index].observationRevision;
        }
        cachedCapacityFallbackInstances_ = stats_.capacityFallbackInstances;
        cachedObservationsValid_ = true;
        return packed_;
    }

    void GpuScenePublisher::retireAllForEpochChange(uint64_t retireAfterSerial) {
        for (const uint32_t instance : instanceOrder_) {
            if (instanceSlots_.retire(instancePool_[instance].handle,
                    retireAfterSerial))
                ++stats_.retiredInstances;
        }
        for (const uint32_t instance : instanceOrder_) {
            for (const uint32_t primitive : instancePool_[instance].primitives) {
                if (primitiveSlots_.retire(primitivePool_[primitive].handle,
                        retireAfterSerial))
                    ++stats_.retiredPrimitives;
            }
        }
        for (const uint32_t geometry : geometryOrder_) {
            if (geometrySlots_.retire(geometryPool_[geometry].handle,
                    retireAfterSerial))
                ++stats_.retiredGeometries;
        }
        instancePool_.clear();
        primitivePool_.clear();
        geometryPool_.clear();
        instanceFree_.clear();
        primitiveFree_.clear();
        geometryFree_.clear();
        instanceOrder_.clear();
        geometryOrder_.clear();
        geometryPending_.clear();
        std::ranges::fill(geometryTable_, InvalidPoolIndex);
        geometryTableCount_ = 0;
        topologyChanged_ = true;
    }

    void GpuScenePublisher::classifyObservations(
        std::span<const GpuSceneObservedInstance> observations, bool fullScan) {
        observationEntries_.clear();
        observationEntries_.reserve(observationOrderScratch_.size());
        size_t cursor = 0;
        SceneEntityUuid previousOwner;
        bool hasPreviousOwner = false;
        for (const GpuSceneObservedInstance* observation :
                observationOrderScratch_) {
            const SceneEntityUuid owner = observation->identity.owner;
            const bool duplicateOwner = hasPreviousOwner && owner == previousOwner;
            previousOwner = owner;
            hasPreviousOwner = true;
            ObservationEntry entry{ .observation = observation };
            if (!duplicateOwner) {
                while (cursor < instanceOrder_.size() &&
                    instancePool_[instanceOrder_[cursor]].owner < owner)
                    ++cursor;
                if (cursor < instanceOrder_.size() &&
                    instancePool_[instanceOrder_[cursor]].owner == owner) {
                    entry.existing = instanceOrder_[cursor++];
                    instancePool_[entry.existing].matchedPass = pass_;
                }
            }
            if (owner.isNil() || duplicateOwner ||
                observation->primitives.empty()) {
                entry.kind = ObservationKind::Fallback;
            }
            else if (!fullScan && entry.existing != InvalidPoolIndex) {
                const InstanceState& instance = instancePool_[entry.existing];
                const size_t index = static_cast<size_t>(
                    observation - observations.data());
                // An observation carrying the history-reset bit never compares
                // equal to its state, so the full scan revises it every time.
                const bool unchanged = observation->observationRevision != 0u &&
                    instance.observationIndex == index &&
                    instance.observationRevision ==
                        observation->observationRevision &&
                    (observation->flags & GpuSceneInstanceHistoryReset) == 0u;
                entry.kind = unchanged ? ObservationKind::Clean
                    : ObservationKind::Dirty;
            }
            else {
                entry.kind = ObservationKind::Dirty;
            }
            observationEntries_.push_back(entry);
        }
    }

    bool GpuScenePublisher::incrementalWouldDiverge() {
        // References released this pass: instances re-read or removed. The
        // rest belong to skipped (clean) observations.
        releasedGeometryScratch_.clear();
        const auto countReleased = [this](uint32_t instance) {
            for (const uint32_t geometry :
                    instancePool_[instance].geometryReferences) {
                if (geometryPool_[geometry].releasedScratch++ == 0u)
                    releasedGeometryScratch_.push_back(geometry);
            }
        };
        for (const ObservationEntry& entry : observationEntries_) {
            if (entry.existing != InvalidPoolIndex &&
                entry.kind != ObservationKind::Clean)
                countReleased(entry.existing);
        }
        for (const uint32_t instance : instanceOrder_)
            if (instancePool_[instance].matchedPass != pass_)
                countReleased(instance);

        bool diverges = false;
        checkedLookupScratch_.clear();
        const auto check = [&](const GpuSceneGeometryIdentity& identity,
            const GeometryValue& value, const GpuSceneGeometryIdentity* coarser,
            float geometricError, uint32_t lodLevel) {
            const uint32_t geometry = findGeometry(identity);
            checkedLookupScratch_.push_back(geometry);
            if (geometry == InvalidPoolIndex) return;
            const GeometryState& state = geometryPool_[geometry];
            if (state.references > state.releasedScratch &&
                geometryChanges(state, value, coarser, geometricError, lodLevel))
                diverges = true;
        };
        for (ObservationEntry& entry : observationEntries_) {
            if (diverges) break;
            if (entry.kind != ObservationKind::Dirty) continue;
            // Nothing is mutated before processing, so these lookups stay
            // valid; a miss may be created by an earlier observation.
            entry.lookupOffset = static_cast<uint32_t>(
                checkedLookupScratch_.size());
            for (const GpuSceneObservedPrimitive& primitive :
                    entry.observation->primitives) {
                const auto& children = primitive.lodChildren;
                check(primitive.geometryIdentity,
                    baseGeometryValue<GeometryValue>(primitive),
                    children.empty() ? nullptr : &children.front().identity,
                    0.0f, 0u);
                for (size_t child = 0; child < children.size(); ++child) {
                    check(children[child].identity,
                        childGeometryValue<GeometryValue>(children[child],
                            primitive.geometryProductRevision),
                        child + 1 == children.size()
                            ? nullptr : &children[child + 1].identity,
                        children[child].geometricError,
                        static_cast<uint32_t>(child + 1));
                }
            }
        }
        for (const uint32_t geometry : releasedGeometryScratch_)
            geometryPool_[geometry].releasedScratch = 0;
        releasedGeometryScratch_.clear();
        return diverges;
    }

    uint32_t GpuScenePublisher::processObservation(
        const ObservationEntry& entry, size_t observationIndex) {
        const GpuSceneObservedInstance& observation = *entry.observation;
        const uint32_t existing = entry.existing;
        const SceneEntityUuid owner = observation.identity.owner;
        primitiveOrderScratch_.clear();
        primitiveOrderScratch_.reserve(observation.primitives.size());
        for (const auto& primitive : observation.primitives)
            primitiveOrderScratch_.push_back(&primitive);
        std::ranges::sort(primitiveOrderScratch_, Bytes16Less{},
            [](const auto* value) -> const AssetGuid& {
                return value->identity.primitiveGuid;
            });
        // Geometry lookups per chain entry (base, then LOD levels) in
        // observation order. Geometries are only created during the walk, so
        // a hit stays valid; a miss is looked up again.
        chainOffsetScratch_.resize(observation.primitives.size() + 1u);
        uint32_t chainEntries = 0;
        for (size_t index = 0; index < observation.primitives.size(); ++index) {
            chainOffsetScratch_[index] = chainEntries;
            chainEntries += 1u + static_cast<uint32_t>(
                observation.primitives[index].lodChildren.size());
        }
        chainOffsetScratch_.back() = chainEntries;
        chainLookupScratch_.resize(chainEntries);
        if (entry.lookupOffset != InvalidPoolIndex)
            std::copy_n(checkedLookupScratch_.begin() + entry.lookupOffset,
                chainEntries, chainLookupScratch_.begin());
        else
            std::ranges::fill(chainLookupScratch_, InvalidPoolIndex);
        const auto lookup = [this](uint32_t& cached,
            const GpuSceneGeometryIdentity& identity) {
            if (cached == InvalidPoolIndex) cached = findGeometry(identity);
            return cached;
        };
        preflightGeometryScratch_.clear();
        uint32_t requiredPrimitiveSlots = 0;
        bool duplicatePrimitive = false;
        AssetGuid previousPrimitive;
        bool hasPreviousPrimitive = false;
        size_t existingCursor = 0;
        for (const GpuSceneObservedPrimitive* primitive : primitiveOrderScratch_) {
            const AssetGuid& primitiveGuid = primitive->identity.primitiveGuid;
            duplicatePrimitive = duplicatePrimitive ||
                (hasPreviousPrimitive && primitiveGuid == previousPrimitive);
            previousPrimitive = primitiveGuid;
            hasPreviousPrimitive = true;
            bool primitiveExists = false;
            if (existing != InvalidPoolIndex) {
                const auto& current = instancePool_[existing].primitives;
                while (existingCursor < current.size() &&
                    primitivePool_[current[existingCursor]].identity.primitiveGuid <
                        primitiveGuid)
                    ++existingCursor;
                primitiveExists = existingCursor < current.size() &&
                    primitivePool_[current[existingCursor]].identity.primitiveGuid ==
                        primitiveGuid;
            }
            if (!primitiveExists) ++requiredPrimitiveSlots;
            uint32_t* chain = chainLookupScratch_.data() + chainOffsetScratch_[
                static_cast<size_t>(primitive - observation.primitives.data())];
            if (lookup(chain[0], primitive->geometryIdentity) == InvalidPoolIndex)
                preflightGeometryScratch_.push_back(primitive->geometryIdentity);
            duplicatePrimitive = duplicatePrimitive || !validLodChildren(*primitive);
            for (size_t child = 0; child < primitive->lodChildren.size(); ++child) {
                const auto& identity = primitive->lodChildren[child].identity;
                if (lookup(chain[child + 1u], identity) == InvalidPoolIndex)
                    preflightGeometryScratch_.push_back(identity);
            }
        }
        std::ranges::sort(preflightGeometryScratch_);
        const auto uniqueGeometryEnd = std::ranges::unique(
            preflightGeometryScratch_).begin();
        preflightGeometryScratch_.erase(uniqueGeometryEnd,
            preflightGeometryScratch_.end());
        const uint32_t requiredInstanceSlots =
            existing == InvalidPoolIndex ? 1u : 0u;
        if (duplicatePrimitive ||
            requiredInstanceSlots > instanceSlots_.availableCount() ||
            requiredPrimitiveSlots > primitiveSlots_.availableCount() ||
            preflightGeometryScratch_.size() > geometrySlots_.availableCount()) {
            fallbackOwners_.push_back(owner);
            ++stats_.capacityFallbackInstances;
            return InvalidPoolIndex;
        }

        uint32_t instanceIndex = existing;
        if (instanceIndex == InvalidPoolIndex) {
            const auto handle = instanceSlots_.allocate();
            if (!handle) throw std::logic_error(
                "GPU-scene instance preflight disagreed with allocator");
            instanceIndex = allocateInstanceState();
            InstanceState& state = instancePool_[instanceIndex];
            state.owner = owner;
            state.handle = *handle;
            state.identity = observation.identity;
            state.mobility = observation.mobility;
            state.flags = observation.flags | GpuSceneInstanceHistoryReset;
            state.maximumLod = (std::min)(observation.maximumLod,
                MaximumGpuSceneLodLevels - 1u);
            state.consumerMask = observation.consumerMask;
            state.currentWorld = observation.worldTransform;
            state.previousWorld = observation.worldTransform;
            state.sphere = observation.worldBoundsSphere;
            state.minimum = observation.worldBoundsMin;
            state.maximum = observation.worldBoundsMax;
            state.instanceRevision = ++recordRevision_;
            state.currentTransformRevision = ++recordRevision_;
            state.previousTransformRevision = ++recordRevision_;
            state.primitives.clear();
            state.geometryReferences.clear();
            state.matchedPass = pass_;
            state.denseIndex = InvalidGpuSceneIndex;
            state.firstPrimitive = 0;
            ++stats_.changedInstances;
            stats_.changedTransforms += 2;
            topologyChanged_ = true;
        }
        {
            InstanceState& instance = instancePool_[instanceIndex];
            const bool transformChanged = !bitsEqual(instance.currentWorld,
                observation.worldTransform);
            if (transformChanged) {
                instance.previousWorld = instance.currentWorld;
                instance.currentWorld = observation.worldTransform;
                instance.previousTransformRevision = ++recordRevision_;
                instance.currentTransformRevision = ++recordRevision_;
                instance.flags &= ~GpuSceneInstanceHistoryReset;
                instance.instanceRevision = ++recordRevision_;
                stats_.changedTransforms += 2;
                ++stats_.changedInstances;
            }
            const bool instanceDataChanged =
                instance.identity != observation.identity ||
                instance.mobility != observation.mobility ||
                (instance.flags & ~GpuSceneInstanceHistoryReset) !=
                    observation.flags ||
                instance.maximumLod != (std::min)(observation.maximumLod,
                    MaximumGpuSceneLodLevels - 1u) ||
                instance.consumerMask != observation.consumerMask ||
                !bitsEqual(instance.sphere, observation.worldBoundsSphere) ||
                !bitsEqual(instance.minimum, observation.worldBoundsMin) ||
                !bitsEqual(instance.maximum, observation.worldBoundsMax);
            if (instanceDataChanged) {
                instance.identity = observation.identity;
                instance.mobility = observation.mobility;
                instance.flags = observation.flags |
                    (instance.flags & GpuSceneInstanceHistoryReset);
                instance.maximumLod = (std::min)(observation.maximumLod,
                    MaximumGpuSceneLodLevels - 1u);
                instance.consumerMask = observation.consumerMask;
                instance.sphere = observation.worldBoundsSphere;
                instance.minimum = observation.worldBoundsMin;
                instance.maximum = observation.worldBoundsMax;
                instance.instanceRevision = ++recordRevision_;
                if (!transformChanged) ++stats_.changedInstances;
            }
            instance.observationIndex = observationIndex;
            instance.observationRevision = observation.observationRevision;
            for (const uint32_t geometry : instance.geometryReferences)
                releaseGeometry(geometry);
            instance.geometryReferences.clear();
        }

        primitiveListScratch_.clear();
        size_t oldCursor = 0;
        const auto retireOldBefore = [&](const AssetGuid* primitiveGuid) {
            const auto& current = instancePool_[instanceIndex].primitives;
            while (oldCursor < current.size() && (primitiveGuid == nullptr ||
                primitivePool_[current[oldCursor]].identity.primitiveGuid <
                    *primitiveGuid)) {
                retiredPrimitiveScratch_.push_back(current[oldCursor++]);
                topologyChanged_ = true;
            }
        };
        for (const GpuSceneObservedPrimitive* observedPrimitive :
                primitiveOrderScratch_) {
            const auto& children = observedPrimitive->lodChildren;
            const uint32_t* chain = chainLookupScratch_.data() + chainOffsetScratch_[
                static_cast<size_t>(observedPrimitive - observation.primitives.data())];
            const uint32_t baseGeometry = observeGeometry(
                observedPrimitive->geometryIdentity,
                baseGeometryValue<GeometryValue>(*observedPrimitive),
                children.empty() ? nullptr : &children.front().identity,
                0.0f, 0u, chain[0]);
            instancePool_[instanceIndex].geometryReferences.push_back(baseGeometry);
            for (size_t childIndex = 0; childIndex < children.size(); ++childIndex) {
                const auto& child = children[childIndex];
                const uint32_t childGeometry = observeGeometry(child.identity,
                    childGeometryValue<GeometryValue>(child,
                        observedPrimitive->geometryProductRevision),
                    childIndex + 1 == children.size()
                        ? nullptr : &children[childIndex + 1].identity,
                    child.geometricError, static_cast<uint32_t>(childIndex + 1),
                    chain[childIndex + 1u]);
                instancePool_[instanceIndex].geometryReferences.push_back(
                    childGeometry);
            }

            const AssetGuid& primitiveGuid =
                observedPrimitive->identity.primitiveGuid;
            retireOldBefore(&primitiveGuid);
            const auto& current = instancePool_[instanceIndex].primitives;
            uint32_t primitiveIndex = InvalidPoolIndex;
            if (oldCursor < current.size() &&
                primitivePool_[current[oldCursor]].identity.primitiveGuid ==
                    primitiveGuid) {
                primitiveIndex = current[oldCursor++];
                PrimitiveState& state = primitivePool_[primitiveIndex];
                if (!primitiveEqual(state, *observedPrimitive)) {
                    assignPrimitive(state, *observedPrimitive);
                    state.revision = ++recordRevision_;
                    ++stats_.changedPrimitives;
                }
            }
            else {
                const auto handle = primitiveSlots_.allocate();
                if (!handle) throw std::logic_error(
                    "GPU-scene primitive preflight disagreed with allocator");
                primitiveIndex = allocatePrimitiveState();
                PrimitiveState& state = primitivePool_[primitiveIndex];
                state.handle = *handle;
                assignPrimitive(state, *observedPrimitive);
                state.revision = ++recordRevision_;
                ++stats_.changedPrimitives;
                topologyChanged_ = true;
            }
            primitivePool_[primitiveIndex].geometry = baseGeometry;
            primitiveListScratch_.push_back(primitiveIndex);
        }
        retireOldBefore(nullptr);
        InstanceState& instance = instancePool_[instanceIndex];
        instance.primitives.swap(primitiveListScratch_);
        for (const uint32_t geometry : instance.geometryReferences)
            ++geometryPool_[geometry].references;
        return instanceIndex;
    }

    void GpuScenePublisher::releaseInstance(uint32_t instanceIndex) {
        InstanceState& instance = instancePool_[instanceIndex];
        for (const uint32_t geometry : instance.geometryReferences)
            releaseGeometry(geometry);
        instance.geometryReferences.clear();
        for (const uint32_t primitive : instance.primitives)
            retiredPrimitiveScratch_.push_back(primitive);
        instance.primitives.clear();
        retiredInstanceScratch_.push_back(instanceIndex);
        topologyChanged_ = true;
    }

    void GpuScenePublisher::releaseGeometry(uint32_t geometry) {
        if (--geometryPool_[geometry].references == 0u)
            deadGeometryCandidateScratch_.push_back(geometry);
    }

    uint32_t GpuScenePublisher::observeGeometry(
        const GpuSceneGeometryIdentity& identity, const GeometryValue& value,
        const GpuSceneGeometryIdentity* coarser, float geometricError,
        uint32_t lodLevel, uint32_t knownGeometry) {
        uint32_t geometry = knownGeometry != InvalidPoolIndex
            ? knownGeometry : findGeometry(identity);
        const bool inserted = geometry == InvalidPoolIndex;
        if (inserted) {
            const auto handle = geometrySlots_.allocate();
            if (!handle) throw std::logic_error(
                "GPU-scene geometry preflight disagreed with allocator");
            geometry = allocateGeometryState();
            GeometryState& state = geometryPool_[geometry];
            state = {};
            state.identity = identity;
            state.handle = *handle;
            state.hash = geometryIdentityHash(identity);
            insertGeometryKey(geometry);
            geometryPending_.push_back(geometry);
            topologyChanged_ = true;
        }
        GeometryState& state = geometryPool_[geometry];
        const bool changed = inserted ||
            geometryChanges(state, value, coarser, geometricError, lodLevel);
        if (changed && !inserted && state.observedPass == pass_)
            passConflict_ = true;
        if (changed) {
            state.value = value;
            state.hasCoarser = coarser != nullptr;
            state.coarserIdentity = coarser != nullptr
                ? *coarser : GpuSceneGeometryIdentity{};
            state.geometricError = geometricError;
            state.lodLevel = lodLevel;
            state.revision = ++recordRevision_;
            ++stats_.changedGeometries;
            if (!inserted && state.changedPass != pass_) {
                state.changedPass = pass_;
                changedGeometryScratch_.push_back(geometry);
            }
        }
        state.observedPass = pass_;
        return geometry;
    }

    uint32_t GpuScenePublisher::findGeometry(
        const GpuSceneGeometryIdentity& identity) const noexcept {
        const uint64_t hash = geometryIdentityHash(identity);
        const size_t mask = geometryTable_.size() - 1u;
        for (size_t slot = static_cast<size_t>(hash) & mask;;
            slot = (slot + 1u) & mask) {
            const uint32_t geometry = geometryTable_[slot];
            if (geometry == InvalidPoolIndex) return InvalidPoolIndex;
            const GeometryState& state = geometryPool_[geometry];
            if (state.hash == hash && state.identity == identity) return geometry;
        }
    }

    void GpuScenePublisher::insertGeometryKey(uint32_t geometry) {
        if ((geometryTableCount_ + 1u) * 2u > geometryTable_.size()) {
            std::vector<uint32_t> previous(geometryTable_.size() * 2u,
                InvalidPoolIndex);
            previous.swap(geometryTable_);
            geometryTableCount_ = 0;
            for (const uint32_t existing : previous)
                if (existing != InvalidPoolIndex) insertGeometryKey(existing);
        }
        const size_t mask = geometryTable_.size() - 1u;
        size_t slot = static_cast<size_t>(geometryPool_[geometry].hash) & mask;
        while (geometryTable_[slot] != InvalidPoolIndex) slot = (slot + 1u) & mask;
        geometryTable_[slot] = geometry;
        ++geometryTableCount_;
    }

    void GpuScenePublisher::eraseGeometryKey(uint32_t geometry) noexcept {
        const size_t mask = geometryTable_.size() - 1u;
        size_t slot = static_cast<size_t>(geometryPool_[geometry].hash) & mask;
        while (geometryTable_[slot] != geometry) slot = (slot + 1u) & mask;
        // Backward-shift deletion keeps every probe sequence unbroken.
        size_t next = slot;
        for (;;) {
            next = (next + 1u) & mask;
            const uint32_t candidate = geometryTable_[next];
            if (candidate == InvalidPoolIndex) break;
            const size_t home = static_cast<size_t>(
                geometryPool_[candidate].hash) & mask;
            const bool movable = slot <= next
                ? (home <= slot || home > next)
                : (home <= slot && home > next);
            if (movable) {
                geometryTable_[slot] = candidate;
                slot = next;
            }
        }
        geometryTable_[slot] = InvalidPoolIndex;
        --geometryTableCount_;
    }

    void GpuScenePublisher::finishGeometries(uint64_t retireAfterSerial) {
        bool anyRetired = false;
        for (const uint32_t geometry : deadGeometryCandidateScratch_) {
            if (geometryPool_[geometry].references == 0u) {
                anyRetired = true;
                break;
            }
        }
        deadGeometryCandidateScratch_.clear();
        if (!anyRetired && geometryPending_.empty()) return;
        topologyChanged_ = true;
        const auto identityLess = [this](uint32_t lhs, uint32_t rhs) {
            return geometryPool_[lhs].identity < geometryPool_[rhs].identity;
        };
        std::ranges::sort(geometryPending_, identityLess);
        geometryOrderScratch_.clear();
        geometryOrderScratch_.reserve(geometryOrder_.size() +
            geometryPending_.size());
        size_t pending = 0;
        for (const uint32_t geometry : geometryOrder_) {
            GeometryState& state = geometryPool_[geometry];
            if (state.references == 0u) {
                // Retired in identity order, as the full scan's map order.
                if (geometrySlots_.retire(state.handle, retireAfterSerial))
                    ++stats_.retiredGeometries;
                eraseGeometryKey(geometry);
                geometryFree_.push_back(geometry);
                continue;
            }
            while (pending < geometryPending_.size() &&
                identityLess(geometryPending_[pending], geometry))
                geometryOrderScratch_.push_back(geometryPending_[pending++]);
            geometryOrderScratch_.push_back(geometry);
        }
        while (pending < geometryPending_.size())
            geometryOrderScratch_.push_back(geometryPending_[pending++]);
        geometryPending_.clear();
        geometryOrder_.swap(geometryOrderScratch_);
    }

    GpuSceneInstanceRecord GpuScenePublisher::instanceRecord(
        const InstanceState& instance) const noexcept {
        const uint32_t firstTransform = instance.denseIndex * 2u;
        return {
            .worldBoundsSphere = pack4(instance.sphere),
            .worldBoundsMin = pack4(instance.minimum),
            .worldBoundsMax = pack4(instance.maximum),
            .references = { firstTransform, firstTransform + 1u,
                instance.firstPrimitive,
                static_cast<uint32_t>(instance.primitives.size()) },
            .state = { instance.handle.generation,
                static_cast<uint32_t>(instance.mobility),
                packGpuSceneInstanceFlags(instance.flags, instance.maximumLod),
                instance.consumerMask },
        };
    }

    GpuScenePrimitiveRecord GpuScenePublisher::primitiveRecord(
        const PrimitiveState& primitive, uint32_t denseInstance,
        uint32_t densePrimitive) const noexcept {
        return {
            .binding = { denseInstance,
                geometryPool_[primitive.geometry].denseIndex,
                primitive.material.id, primitive.pipeline.id },
            .state = { primitive.handle.generation, primitive.primitiveFlags,
                densePrimitive, primitive.consumerMask },
            // Layout revision 0, as the full scan never set it.
            .revisions = { 0u, primitive.geometryProductRevision,
                static_cast<uint32_t>(primitive.revision),
                primitive.materialRevision },
        };
    }

    GpuSceneGeometryRecord GpuScenePublisher::geometryRecord(
        const GeometryState& geometry) const {
        uint32_t coarser = InvalidGpuSceneIndex;
        if (geometry.hasCoarser) {
            const uint32_t found = findGeometry(geometry.coarserIdentity);
            if (found == InvalidPoolIndex ||
                geometryPool_[found].denseIndex == InvalidGpuSceneIndex)
                throw std::logic_error(
                    "GPU-scene LOD link lost during atomic publication");
            coarser = geometryPool_[found].denseIndex;
        }
        const GeometryValue& value = geometry.value;
        GpuSceneGeometryRecord record{
            .localBoundsSphere = pack4(value.localBoundsSphere),
            .localBoundsMin = pack4(value.localBoundsMin),
            .localBoundsMax = pack4(value.localBoundsMax),
            .draw = { value.firstIndex, value.indexCount,
                std::bit_cast<uint32_t>(value.vertexOffset), value.indexType },
            .storage = { value.legacyGeometry.id, value.legacyGeometry.id,
                value.vertexLayout, GpuSceneGeometryLegacyRhiHandle },
            .state = { geometry.handle.generation, geometry.denseIndex,
                coarser, value.geometryProductRevision },
        };
        record.localBoundsMin.w = geometry.geometricError;
        record.localBoundsMax.w = static_cast<float>(geometry.lodLevel);
        return record;
    }

    void GpuScenePublisher::packAll() {
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

        const size_t geometryCount = geometryOrder_.size();
        for (size_t index = 0; index < geometryCount; ++index)
            geometryPool_[geometryOrder_[index]].denseIndex =
                static_cast<uint32_t>(index);
        packed_.geometries.reserve(geometryCount);
        packed_.geometryRevisions.reserve(geometryCount);
        packed_.geometryIdentities.reserve(geometryCount);
        packed_.denseGeometryHandles.reserve(geometryCount);
        for (const uint32_t geometry : geometryOrder_) {
            const GeometryState& state = geometryPool_[geometry];
            packed_.geometries.push_back(geometryRecord(state));
            packed_.geometryRevisions.push_back(state.revision);
            packed_.geometryIdentities.push_back(state.identity);
            packed_.denseGeometryHandles.push_back(state.handle);
        }

        size_t totalPrimitives = 0;
        for (const uint32_t instance : instanceOrder_)
            totalPrimitives += instancePool_[instance].primitives.size();
        const size_t instanceCount = instanceOrder_.size();
        packed_.instances.reserve(instanceCount);
        packed_.instanceRevisions.reserve(instanceCount);
        packed_.instanceIdentities.resize(instanceCount);
        packed_.denseInstanceHandles.reserve(instanceCount);
        packed_.transforms.reserve(instanceCount * 2u);
        packed_.transformRevisions.reserve(instanceCount * 2u);
        packed_.primitives.reserve(totalPrimitives);
        packed_.primitiveRevisions.reserve(totalPrimitives);
        packed_.primitiveIdentities.reserve(totalPrimitives);
        packed_.densePrimitiveHandles.reserve(totalPrimitives);
        for (size_t dense = 0; dense < instanceCount; ++dense) {
            InstanceState& instance = instancePool_[instanceOrder_[dense]];
            instance.denseIndex = static_cast<uint32_t>(dense);
            instance.firstPrimitive = static_cast<uint32_t>(
                packed_.primitives.size());
            packed_.transforms.push_back(packGpuSceneAffine(instance.currentWorld));
            packed_.transforms.push_back(packGpuSceneAffine(instance.previousWorld));
            packed_.transformRevisions.push_back(
                instance.currentTransformRevision);
            packed_.transformRevisions.push_back(
                instance.previousTransformRevision);
            packed_.instances.push_back(instanceRecord(instance));
            packed_.instanceRevisions.push_back(instance.instanceRevision);
            packed_.instanceIdentities[dense] = instance.identity;
            packed_.denseInstanceHandles.push_back(instance.handle);
            for (const uint32_t primitiveIndex : instance.primitives) {
                const PrimitiveState& primitive = primitivePool_[primitiveIndex];
                packed_.primitives.push_back(primitiveRecord(primitive,
                    instance.denseIndex, static_cast<uint32_t>(
                        packed_.primitiveIdentities.size())));
                packed_.primitiveRevisions.push_back(primitive.revision);
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

    void GpuScenePublisher::packTouched() {
        // Same epoch, same dense layout: only re-read instances and changed
        // geometries can differ from the previous publication. Revise them
        // table by table in ascending dense order, as the full revise would.
        packed_.publicationRevision = publicationRevision_;
        bool membershipInputsChanged = false;
        const auto update = [this](auto& records, auto& revisions, size_t index,
            const auto& record) {
            if (bitsEqual(records[index], record)) return false;
            records[index] = record;
            revisions[index] = ++recordRevision_;
            return true;
        };
        for (const uint32_t instanceIndex : touchedInstanceScratch_) {
            const InstanceState& instance = instancePool_[instanceIndex];
            const size_t first = static_cast<size_t>(instance.denseIndex) * 2u;
            (void)update(packed_.transforms, packed_.transformRevisions, first,
                packGpuSceneAffine(instance.currentWorld));
            (void)update(packed_.transforms, packed_.transformRevisions,
                first + 1u, packGpuSceneAffine(instance.previousWorld));
        }
        for (const uint32_t instanceIndex : touchedInstanceScratch_) {
            const InstanceState& instance = instancePool_[instanceIndex];
            const GpuSceneInstanceRecord record = instanceRecord(instance);
            const size_t dense = instance.denseIndex;
            const bool stateChanged =
                !bitsEqual(packed_.instances[dense].state, record.state);
            if (update(packed_.instances, packed_.instanceRevisions, dense, record))
                membershipInputsChanged = membershipInputsChanged || stateChanged;
            if (packed_.instanceIdentities[dense] != instance.identity)
                packed_.instanceIdentities[dense] = instance.identity;
        }
        for (const uint32_t instanceIndex : touchedInstanceScratch_) {
            const InstanceState& instance = instancePool_[instanceIndex];
            for (size_t offset = 0; offset < instance.primitives.size(); ++offset) {
                const PrimitiveState& primitive =
                    primitivePool_[instance.primitives[offset]];
                const size_t dense = instance.firstPrimitive + offset;
                if (update(packed_.primitives, packed_.primitiveRevisions, dense,
                        primitiveRecord(primitive, instance.denseIndex,
                            static_cast<uint32_t>(dense))))
                    membershipInputsChanged = true;
                if (packed_.primitiveIdentities[dense] != primitive.identity)
                    packed_.primitiveIdentities[dense] = primitive.identity;
            }
        }
        std::ranges::sort(changedGeometryScratch_, {}, [this](uint32_t geometry) {
            return geometryPool_[geometry].denseIndex;
        });
        for (const uint32_t geometryIndex : changedGeometryScratch_) {
            const GeometryState& geometry = geometryPool_[geometryIndex];
            if (update(packed_.geometries, packed_.geometryRevisions,
                    geometry.denseIndex, geometryRecord(geometry)))
                membershipInputsChanged = true;
        }
        // Membership lists and revisions are a pure function of the epoch,
        // primitive records, instance state words, geometry records and
        // geometry revisions; transforms and bounds do not participate. The
        // content watermarks do include transform revisions, so they are
        // refreshed whenever membership is not republished.
        if (membershipInputsChanged) publishGpuSceneConsumerMembership(packed_);
        else refreshGpuSceneConsumerContentWatermarks(packed_);
    }

    uint32_t GpuScenePublisher::allocateInstanceState() {
        if (!instanceFree_.empty()) {
            const uint32_t index = instanceFree_.back();
            instanceFree_.pop_back();
            return index;
        }
        instancePool_.emplace_back();
        return static_cast<uint32_t>(instancePool_.size() - 1u);
    }

    uint32_t GpuScenePublisher::allocatePrimitiveState() {
        if (!primitiveFree_.empty()) {
            const uint32_t index = primitiveFree_.back();
            primitiveFree_.pop_back();
            return index;
        }
        primitivePool_.emplace_back();
        return static_cast<uint32_t>(primitivePool_.size() - 1u);
    }

    uint32_t GpuScenePublisher::allocateGeometryState() {
        if (!geometryFree_.empty()) {
            const uint32_t index = geometryFree_.back();
            geometryFree_.pop_back();
            return index;
        }
        geometryPool_.emplace_back();
        return static_cast<uint32_t>(geometryPool_.size() - 1u);
    }

} // namespace Iridium
