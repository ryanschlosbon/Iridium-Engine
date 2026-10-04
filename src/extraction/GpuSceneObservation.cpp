// M7R R5c.5: see GpuSceneObservation.h. The per-entity step is the
// observation code moved from RenderExtractor::prepareGpuScenePublication
// (formerly Application.cpp), unchanged apart from const access.
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include "extraction/GpuSceneObservation.h"

#include "assets/AssetManager.h"
#include "scene/SceneWorld.h"
#include "scene/components/MeshComponent.h"
#include "scene/components/RenderInstanceBatchComponent.h"
#include "scene/components/TransformComponent.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace Iridium {

    struct GpuSceneObservation::Pools {
        ComponentPool<TransformComponent>* transforms = nullptr;
        ComponentPool<MeshComponent>* meshes = nullptr;
        ComponentPool<RenderInstanceBatchComponent>* batches = nullptr;
    };

    namespace {
        // One submesh as the observation resolves it: skipped (no material
        // slot, or transparent after overrides), invalid for the GPU scene, or
        // an opaque primitive with its binding and effective material.
        enum class SubMeshResolution : uint8_t { Skip, Invalid, Primitive };

        SubMeshResolution resolveSubMesh(const ModelAsset& model,
            const SubMesh& subMesh, const MeshComponent& mesh,
            const AssetManager* assets, const MaterialBinding*& binding,
            AssetGuid& effectiveMaterialGuid,
            std::optional<CookedMaterialRuntimeBinding>& overrideBinding) {
            if (subMesh.materialIndex < 0 ||
                static_cast<size_t>(subMesh.materialIndex) >=
                    model.materials.size()) return SubMeshResolution::Skip;
            binding = &model.materials[
                static_cast<size_t>(subMesh.materialIndex)];
            effectiveMaterialGuid = subMesh.materialGuid;
            const auto materialOverride = std::ranges::find_if(
                mesh.materialOverrides,
                [&subMesh](const MeshComponent::MaterialOverride& value) {
                    return value.sourceMaterialGuid == subMesh.materialGuid;
                });
            overrideBinding.reset();
            if (materialOverride != mesh.materialOverrides.end()) {
                overrideBinding = assets->findCookedMaterialRuntime(
                    materialOverride->materialGuid);
                if (overrideBinding) {
                    binding = &overrideBinding->binding;
                    effectiveMaterialGuid = materialOverride->materialGuid;
                }
            }
            if (binding->renderQueue == RenderQueue::Transparent)
                return SubMeshResolution::Skip;
            if (!binding->material.isValid() ||
                !binding->pipeline.isValid() ||
                subMesh.sourcePrimitiveGuid.isNil() ||
                subMesh.primitiveGuid.isNil() ||
                effectiveMaterialGuid.isNil() ||
                subMesh.indexCount == 0)
                return SubMeshResolution::Invalid;
            return SubMeshResolution::Primitive;
        }

        // The metadata validity a rebuild computes (no observation written).
        bool rebuildValidity(const ModelAsset& model, const MeshComponent& mesh,
            const AssetManager* assets) {
            const MaterialBinding* binding = nullptr;
            AssetGuid effectiveMaterialGuid;
            std::optional<CookedMaterialRuntimeBinding> overrideBinding;
            bool anyPrimitive = false;
            for (const SubMesh& subMesh : model.subMeshes) {
                switch (resolveSubMesh(model, subMesh, mesh, assets, binding,
                        effectiveMaterialGuid, overrideBinding)) {
                case SubMeshResolution::Skip: continue;
                case SubMeshResolution::Invalid: return false;
                case SubMeshResolution::Primitive: anyPrimitive = true; break;
                }
            }
            return anyPrimitive;
        }

        template<typename T>
        bool bitsEqual(const T& lhs, const T& rhs) noexcept {
            return std::memcmp(&lhs, &rhs, sizeof(T)) == 0;
        }
    }

    GpuSceneObservation::GpuSceneObservation(SceneWorld& scene,
        Mode mode) noexcept
        : scene_(scene), mode_(mode) {}

    uint64_t GpuSceneObservation::overrideSignature(
        const MeshComponent& mesh) const {
        uint64_t overrideSignature = 1469598103934665603ull;
        const auto mix = [&overrideSignature](uint64_t value) {
            overrideSignature ^= value;
            overrideSignature *= 1099511628211ull;
        };
        for (const MeshComponent::MaterialOverride& value :
                mesh.materialOverrides) {
            for (uint8_t byte : value.sourceMaterialGuid.bytes()) mix(byte);
            for (uint8_t byte : value.materialGuid.bytes()) mix(byte);
            if (const auto resolved =
                    assets_->findCookedMaterialRuntime(value.materialGuid)) {
                mix(resolved->binding.material.id);
                mix(resolved->binding.pipeline.id);
                mix(static_cast<uint32_t>(resolved->binding.renderQueue));
            }
        }
        return overrideSignature;
    }

    GpuSceneObservation::Kind GpuSceneObservation::processEntity(
        const Pools& pools, Entity entity, Entity selectedEntity,
        size_t& observationCount) {
        const MeshComponent& mesh = std::as_const(*pools.meshes).get(entity);
        if (!mesh.enabled || !mesh.model ||
            !mesh.model->geometry.isValid() ||
            !pools.transforms->has(entity)) return Kind::EarlySkip;
        if (pools.batches && pools.batches->has(entity)) {
            ++directFallbackCount_;
            return Kind::BatchFallback;
        }
        const auto owner = scene_.identities().persistentId(entity);
        if (!owner || owner->isNil()) {
            ++directFallbackCount_;
            return Kind::OwnerFallback;
        }
        const ModelAsset& model = *mesh.model;
        if (observationCount == observations_.size()) {
            observations_.emplace_back();
            metadata_.emplace_back();
        }
        GpuSceneObservedInstance& observation = observations_[observationCount];
        Metadata& metadata = metadata_[observationCount];
        const glm::mat4 previousWorld = observation.worldTransform;
        const uint32_t previousFlags = observation.flags;
        const uint32_t previousMaximumLod = observation.maximumLod;
        const uint32_t previousConsumerMask = observation.consumerMask;
        const uint64_t signature = overrideSignature(mesh);
        const bool rebuildPrimitives = metadata.owner != *owner ||
            metadata.model != &model ||
            metadata.geometry != model.geometry ||
            metadata.cookKey != model.artifactCookKey ||
            metadata.materialOverrideSignature != signature;
        observation.identity.owner = *owner;
        observation.identity.modelAssetGuid = model.assetGuid;
        observation.identity.publishedRevision = 1;
        observation.identity.artifactCookKey = model.artifactCookKey;
        observation.mobility = GpuSceneMobility::Movable;
        observation.flags = GpuSceneInstanceEnabled |
            (entity == selectedEntity ? GpuSceneInstanceSelected : 0u);
        observation.maximumLod = static_cast<uint32_t>(
            std::clamp(mesh.maximumLodLevel, 0,
                MeshComponent::MaximumLodLevel));
        observation.consumerMask = metadata.baseConsumerMask |
            (entity == selectedEntity ? GpuSceneConsumerSelection : 0u);
        observation.worldTransform =
            std::as_const(*pools.transforms).get(entity).worldMatrix;
        if (rebuildPrimitives) {
            metadata.owner = *owner;
            metadata.model = &model;
            metadata.geometry = model.geometry;
            metadata.cookKey = model.artifactCookKey;
            metadata.materialOverrideSignature = signature;
            metadata.localMinimum = glm::vec3(
                (std::numeric_limits<float>::max)());
            metadata.localMaximum = glm::vec3(
                (std::numeric_limits<float>::lowest)());
            metadata.baseConsumerMask = 0;
            metadata.valid = true;
            observation.primitives.clear();
            const MaterialBinding* binding = nullptr;
            AssetGuid effectiveMaterialGuid;
            std::optional<CookedMaterialRuntimeBinding> overrideBinding;
            for (const SubMesh& subMesh : model.subMeshes) {
                const SubMeshResolution resolution = resolveSubMesh(model,
                    subMesh, mesh, assets_, binding, effectiveMaterialGuid,
                    overrideBinding);
                if (resolution == SubMeshResolution::Skip) continue;
                if (resolution == SubMeshResolution::Invalid) {
                    metadata.valid = false;
                    break;
                }
                uint32_t primitiveFlags = GpuScenePrimitiveOpaque;
                if (subMesh.coverage == static_cast<uint8_t>(
                        ModelCoverage::Masked))
                    primitiveFlags |= GpuScenePrimitiveAlphaMask;
                if ((subMesh.flags & ModelPrimitiveDoubleSided) != 0)
                    primitiveFlags |= GpuScenePrimitiveTwoSided;
                metadata.baseConsumerMask |= binding->renderQueue ==
                    RenderQueue::ForwardOpaque
                    ? GpuSceneConsumerForwardOpaque
                    : GpuSceneConsumerMainOpaque;
                metadata.baseConsumerMask |= GpuSceneConsumerShadow |
                    GpuSceneConsumerProbe;
                observation.primitives.push_back({
                    .identity = { *owner, subMesh.sourcePrimitiveGuid,
                        subMesh.primitiveGuid, effectiveMaterialGuid },
                    .geometryIdentity = { model.assetGuid,
                        subMesh.sourcePrimitiveGuid,
                        subMesh.primitiveGuid },
                    .legacyGeometry = subMesh.geometry.isValid()
                        ? subMesh.geometry : model.geometry,
                    .material = binding->material,
                    .pipeline = binding->pipeline,
                    .firstIndex = subMesh.indexStart,
                    .indexCount = subMesh.indexCount,
                    .vertexOffset = subMesh.vertexOffset,
                    .indexType = subMesh.indexFormat,
                    .vertexLayout = subMesh.attributeMask,
                    .primitiveFlags = primitiveFlags,
                    .consumerMask = binding->renderQueue ==
                        RenderQueue::ForwardOpaque
                        ? GpuSceneConsumerForwardOpaque |
                            GpuSceneConsumerShadow |
                            GpuSceneConsumerProbe
                        : GpuSceneConsumerMainOpaque |
                            GpuSceneConsumerShadow |
                            GpuSceneConsumerProbe,
                    .localBoundsSphere = glm::vec4(
                        subMesh.boundsSphereCenter,
                        subMesh.boundsSphereRadius),
                    .localBoundsMin = glm::vec4(subMesh.boundsMin, 0.0f),
                    .localBoundsMax = glm::vec4(subMesh.boundsMax, 0.0f),
                    .geometryProductRevision = static_cast<uint32_t>(
                        observation.identity.publishedRevision),
                    .materialRevision = binding->material.id,
                });
                const auto lodChain = std::ranges::find(model.lodChains,
                    subMesh.primitiveGuid, &ModelLodChain::basePrimitiveGuid);
                if (lodChain != model.lodChains.end()) {
                    auto& children = observation.primitives.back().lodChildren;
                    for (size_t levelIndex = 1;
                            levelIndex < lodChain->levels.size(); ++levelIndex) {
                        const auto& level = lodChain->levels[levelIndex];
                        const SubMesh& child = level.subMesh;
                        children.push_back({
                            .identity = { model.assetGuid,
                                child.sourcePrimitiveGuid, child.primitiveGuid },
                            .geometry = child.geometry,
                            .firstIndex = child.indexStart,
                            .indexCount = child.indexCount,
                            .vertexOffset = child.vertexOffset,
                            .indexType = child.indexFormat,
                            .vertexLayout = child.attributeMask,
                            .localBoundsSphere = glm::vec4(
                                child.boundsSphereCenter,
                                child.boundsSphereRadius),
                            .localBoundsMin = glm::vec4(child.boundsMin, 0.0f),
                            .localBoundsMax = glm::vec4(child.boundsMax, 0.0f),
                            .geometricError = level.geometricError,
                        });
                    }
                }
                metadata.localMinimum = glm::min(
                    metadata.localMinimum, subMesh.boundsMin);
                metadata.localMaximum = glm::max(
                    metadata.localMaximum, subMesh.boundsMax);
            }
            metadata.valid = metadata.valid &&
                !observation.primitives.empty();
        }
        observation.consumerMask = metadata.baseConsumerMask |
            (entity == selectedEntity ? GpuSceneConsumerSelection : 0u);
        if (!metadata.valid) {
            observation.primitives.clear();
            ++directFallbackCount_;
            return Kind::InvalidMetadata;
        }
        glm::vec3 worldMinimum((std::numeric_limits<float>::max)());
        glm::vec3 worldMaximum((std::numeric_limits<float>::lowest)());
        for (uint32_t corner = 0; corner < 8u; ++corner) {
            const glm::vec3 local{
                (corner & 1u) ? metadata.localMaximum.x
                    : metadata.localMinimum.x,
                (corner & 2u) ? metadata.localMaximum.y
                    : metadata.localMinimum.y,
                (corner & 4u) ? metadata.localMaximum.z
                    : metadata.localMinimum.z,
            };
            const glm::vec3 world = glm::vec3(
                observation.worldTransform * glm::vec4(local, 1.0f));
            worldMinimum = glm::min(worldMinimum, world);
            worldMaximum = glm::max(worldMaximum, world);
        }
        const glm::vec3 center = (worldMinimum + worldMaximum) * 0.5f;
        observation.worldBoundsSphere = glm::vec4(center,
            glm::length(worldMaximum - center));
        observation.worldBoundsMin = glm::vec4(worldMinimum, 0.0f);
        observation.worldBoundsMax = glm::vec4(worldMaximum, 0.0f);
        const bool observationChanged = rebuildPrimitives ||
            std::memcmp(&previousWorld, &observation.worldTransform,
                sizeof(previousWorld)) != 0 ||
            previousFlags != observation.flags ||
            previousMaximumLod != observation.maximumLod ||
            previousConsumerMask != observation.consumerMask;
        if (observationChanged) {
            if (metadata.observationRevision ==
                    (std::numeric_limits<uint64_t>::max)()) {
                throw std::overflow_error(
                    "GPU-scene observation revision exhausted");
            }
            ++metadata.observationRevision;
        }
        observation.observationRevision = metadata.observationRevision;
        ++observationCount;
        return Kind::Accepted;
    }

    bool GpuSceneObservation::modelsUnchanged() const {
        for (const ModelRecord& record : models_) {
            // An expired model was replaced by a component write, which the
            // journal reports; walk everything anyway.
            const std::shared_ptr<ModelAsset> model = record.model.lock();
            if (!model || model.get() != record.address ||
                model->geometry != record.geometry ||
                model->artifactCookKey != record.cookKey ||
                model->assetGuid != record.assetGuid)
                return false;
        }
        return true;
    }

    void GpuSceneObservation::fullWalk(const Pools& pools,
        Entity selectedEntity) {
        ++stats_.fullWalks;
        directFallbackCount_ = 0;
        size_t observationCount = 0;
        records_.clear();
        runFirst_.clear();
        volatileRuns_.clear();
        overrideEntities_.clear();
        // Keep the model records' storage; rebuild the list in place.
        size_t modelCount = 0;
        if (pools.transforms && pools.meshes) {
            observations_.reserve(pools.meshes->entities.size());
            metadata_.reserve(pools.meshes->entities.size());
            records_.reserve(pools.meshes->entities.size());
            uint32_t runStart = 0;
            bool runVolatile = false;
            const auto& entities = pools.meshes->entities;
            for (uint32_t dense = 0; dense < entities.size(); ++dense) {
                const Entity entity = entities[dense];
                const MeshComponent& mesh =
                    std::as_const(*pools.meshes).get(entity);
                const uint32_t slot = static_cast<uint32_t>(observationCount);
                const Kind kind = processEntity(pools, entity, selectedEntity,
                    observationCount);
                records_.push_back({
                    .entity = entity,
                    .model = mesh.model.get(),
                    .slot = slot,
                    .kind = kind,
                    .hasOverrides = !mesh.materialOverrides.empty(),
                });
                if (mesh.model) {
                    const ModelAsset* address = mesh.model.get();
                    bool known = false;
                    for (size_t index = 0; index < modelCount; ++index)
                        known = known || models_[index].address == address;
                    if (!known) {
                        if (modelCount == models_.size()) models_.emplace_back();
                        ModelRecord& record = models_[modelCount++];
                        record.model = mesh.model;
                        record.address = address;
                        record.geometry = mesh.model->geometry;
                        record.cookKey = mesh.model->artifactCookKey;
                        record.assetGuid = mesh.model->assetGuid;
                    }
                }
                if (!mesh.materialOverrides.empty())
                    overrideEntities_.push_back(dense);
                if (kind == Kind::InvalidMetadata) runVolatile = true;
                if (kind == Kind::Accepted) {
                    runFirst_.push_back(runStart);
                    if (runVolatile) volatileRuns_.push_back(slot);
                    runStart = dense + 1u;
                    runVolatile = false;
                }
            }
            // The tail run (no accepted entity) and the end sentinel.
            runFirst_.push_back(runStart);
            if (runVolatile)
                volatileRuns_.push_back(static_cast<uint32_t>(observationCount));
            runFirst_.push_back(static_cast<uint32_t>(entities.size()));
        }
        models_.resize(modelCount);
        observations_.resize(observationCount);
        metadata_.resize(observationCount);
    }

    bool GpuSceneObservation::runKindsUnchanged(const Pools& pools,
        uint32_t run) const {
        // The slot's metadata as the run's metadata-stage entities see it,
        // in turn: the previous state, then each such entity's own key.
        struct Key {
            SceneEntityUuid owner;
            const ModelAsset* model = nullptr;
            GeometryHandle geometry;
            const std::string* cookKey = nullptr;
            uint64_t signature = 0;
            bool valid = false;
        };
        static const std::string EmptyCookKey;
        Key key{ .cookKey = &EmptyCookKey };
        if (run < metadata_.size()) {
            const Metadata& previous = metadata_[run];
            key = { previous.owner, previous.model, previous.geometry,
                &previous.cookKey, previous.materialOverrideSignature,
                previous.valid };
        }
        for (uint32_t dense = runFirst_[run]; dense < runFirst_[run + 1u];
                ++dense) {
            const EntityRecord& record = records_[dense];
            const Entity entity = record.entity;
            const MeshComponent& mesh = std::as_const(*pools.meshes).get(entity);
            Kind kind = Kind::EarlySkip;
            if (!mesh.enabled || !mesh.model ||
                !mesh.model->geometry.isValid() ||
                !pools.transforms->has(entity))
                kind = Kind::EarlySkip;
            else if (pools.batches && pools.batches->has(entity))
                kind = Kind::BatchFallback;
            else {
                const auto owner = scene_.identities().persistentId(entity);
                if (!owner || owner->isNil()) kind = Kind::OwnerFallback;
                else {
                    const ModelAsset& model = *mesh.model;
                    const uint64_t signature = overrideSignature(mesh);
                    const bool rebuild = key.owner != *owner ||
                        key.model != &model || key.geometry != model.geometry ||
                        *key.cookKey != model.artifactCookKey ||
                        key.signature != signature;
                    if (rebuild) {
                        key = { *owner, &model, model.geometry,
                            &model.artifactCookKey, signature,
                            rebuildValidity(model, mesh, assets_) };
                    }
                    kind = key.valid ? Kind::Accepted : Kind::InvalidMetadata;
                }
            }
            if (kind != record.kind) return false;
        }
        return true;
    }

    bool GpuSceneObservation::changeDrivenWalk(const Pools& pools,
        const GpuSceneObservationInputs& inputs) {
        auto& journal = pools.meshes->journal;
        if (journal.overflowed || !modelsUnchanged()) return false;
        const uint32_t slots = static_cast<uint32_t>(observations_.size());
        if (runFirst_.size() != static_cast<size_t>(slots) + 2u ||
            records_.size() != pools.meshes->entities.size())
            return false;

        // Collect the runs to re-run.
        runStamp_.resize(runFirst_.size());
        if (++stamp_ == 0u) {
            std::ranges::fill(runStamp_, 0u);
            stamp_ = 1u;
        }
        runs_.clear();
        bool structural = false;
        const auto markRun = [&](uint32_t run) {
            if (runStamp_[run] == stamp_) return;
            runStamp_[run] = stamp_;
            runs_.push_back(run);
        };
        const auto visit = [&](Entity entity) {
            if (structural || entity.isNull() || !pools.meshes->has(entity))
                return;
            const uint32_t dense = pools.meshes->sparseIndex.get(entity.index());
            const EntityRecord& record = records_[dense];
            const MeshComponent& mesh = std::as_const(*pools.meshes).get(entity);
            if (record.entity != entity || record.model != mesh.model.get() ||
                record.hasOverrides != !mesh.materialOverrides.empty()) {
                structural = true;
                return;
            }
            markRun(record.slot);
        };
        for (const Entity entity : journal.writtenEntities) visit(entity);
        for (const Entity entity : inputs.changedTransforms) visit(entity);
        visit(selected_);
        visit(inputs.selectedEntity);
        for (const uint32_t dense : overrideEntities_)
            visit(records_[dense].entity);
        if (structural) return false;
        for (const uint32_t run : volatileRuns_) markRun(run);
        for (const uint32_t run : runs_)
            if (!runKindsUnchanged(pools, run)) return false;

        // Commit: re-run each run with the full walk's per-entity step.
        std::ranges::sort(runs_);
        ++stats_.changeDrivenFrames;
        stats_.visitedRuns += runs_.size();
        for (const uint32_t run : runs_) {
            size_t observationCount = run;
            const uint32_t fallbackBefore = directFallbackCount_;
            for (uint32_t dense = runFirst_[run]; dense < runFirst_[run + 1u];
                    ++dense) {
                const Kind kind = processEntity(pools, records_[dense].entity,
                    inputs.selectedEntity, observationCount);
                if (kind != records_[dense].kind)
                    throw std::logic_error(
                        "GPU-scene observation run changed after validation");
            }
            // The count of the run's fallbacks is unchanged (same kinds).
            directFallbackCount_ = fallbackBefore;
        }
        // A tail run may have appended a scratch slot, as the full walk does.
        observations_.resize(slots);
        metadata_.resize(slots);
        return true;
    }

    void GpuSceneObservation::observe(const GpuSceneObservationInputs& inputs) {
        assets_ = inputs.assets;
        Registry& registry = scene_.registry();
        const Pools pools{
            .transforms = registry.getPool<TransformComponent>(),
            .meshes = registry.getPool<MeshComponent>(),
            .batches = registry.findPool<RenderInstanceBatchComponent>(),
        };
        if (mode_ == Mode::ChangeDriven) pools.meshes->journal.enabled = true;
        const bool sameStructure = walked_ &&
            meshPool_ == pools.meshes && transformPool_ == pools.transforms &&
            batchPool_ == pools.batches &&
            meshStructure_ == pools.meshes->journal.structureRevision &&
            transformStructure_ == pools.transforms->journal.structureRevision &&
            batchStructure_ == (pools.batches
                ? pools.batches->journal.structureRevision : 0u) &&
            sceneEpoch_ == scene_.stateEpoch();
        if (mode_ == Mode::FullWalk || !sameStructure ||
            !changeDrivenWalk(pools, inputs))
            fullWalk(pools, inputs.selectedEntity);
        walked_ = true;
        meshPool_ = pools.meshes;
        transformPool_ = pools.transforms;
        batchPool_ = pools.batches;
        meshStructure_ = pools.meshes->journal.structureRevision;
        transformStructure_ = pools.transforms->journal.structureRevision;
        batchStructure_ = pools.batches
            ? pools.batches->journal.structureRevision : 0u;
        sceneEpoch_ = scene_.stateEpoch();
        selected_ = inputs.selectedEntity;
        if (mode_ == Mode::ChangeDriven) {
            pools.meshes->journal.writtenEntities.clear();
            pools.meshes->journal.overflowed = false;
        }
    }

    std::string GpuSceneObservation::compare(const GpuSceneObservation& expected,
        const GpuSceneObservation& actual) {
        if (expected.directFallbackCount_ != actual.directFallbackCount_)
            return "direct fallback count";
        if (expected.observations_.size() != actual.observations_.size())
            return "observation count";
        if (expected.metadata_.size() != actual.metadata_.size())
            return "metadata count";
        for (size_t index = 0; index < expected.observations_.size(); ++index) {
            const std::string at = " at " + std::to_string(index);
            const GpuSceneObservedInstance& lhs = expected.observations_[index];
            const GpuSceneObservedInstance& rhs = actual.observations_[index];
            if (lhs.identity != rhs.identity) return "identity" + at;
            if (lhs.observationRevision != rhs.observationRevision)
                return "observation revision" + at;
            if (lhs.mobility != rhs.mobility || lhs.flags != rhs.flags ||
                lhs.maximumLod != rhs.maximumLod ||
                lhs.consumerMask != rhs.consumerMask)
                return "instance state" + at;
            if (!bitsEqual(lhs.worldTransform, rhs.worldTransform) ||
                !bitsEqual(lhs.worldBoundsSphere, rhs.worldBoundsSphere) ||
                !bitsEqual(lhs.worldBoundsMin, rhs.worldBoundsMin) ||
                !bitsEqual(lhs.worldBoundsMax, rhs.worldBoundsMax))
                return "transform or bounds" + at;
            if (lhs.primitives.size() != rhs.primitives.size())
                return "primitive count" + at;
            for (size_t primitive = 0; primitive < lhs.primitives.size();
                    ++primitive) {
                const GpuSceneObservedPrimitive& a = lhs.primitives[primitive];
                const GpuSceneObservedPrimitive& b = rhs.primitives[primitive];
                if (a.identity != b.identity ||
                    a.geometryIdentity != b.geometryIdentity ||
                    a.legacyGeometry != b.legacyGeometry ||
                    a.material != b.material || a.pipeline != b.pipeline ||
                    a.firstIndex != b.firstIndex || a.indexCount != b.indexCount ||
                    a.vertexOffset != b.vertexOffset ||
                    a.indexType != b.indexType ||
                    a.vertexLayout != b.vertexLayout ||
                    a.primitiveFlags != b.primitiveFlags ||
                    a.consumerMask != b.consumerMask ||
                    !bitsEqual(a.localBoundsSphere, b.localBoundsSphere) ||
                    !bitsEqual(a.localBoundsMin, b.localBoundsMin) ||
                    !bitsEqual(a.localBoundsMax, b.localBoundsMax) ||
                    a.geometryProductRevision != b.geometryProductRevision ||
                    a.materialRevision != b.materialRevision ||
                    a.lodChildren.size() != b.lodChildren.size())
                    return "primitive" + at;
                for (size_t child = 0; child < a.lodChildren.size(); ++child) {
                    const GpuSceneObservedLodGeometry& c = a.lodChildren[child];
                    const GpuSceneObservedLodGeometry& d = b.lodChildren[child];
                    if (c.identity != d.identity || c.geometry != d.geometry ||
                        c.firstIndex != d.firstIndex ||
                        c.indexCount != d.indexCount ||
                        c.vertexOffset != d.vertexOffset ||
                        c.indexType != d.indexType ||
                        c.vertexLayout != d.vertexLayout ||
                        !bitsEqual(c.localBoundsSphere, d.localBoundsSphere) ||
                        !bitsEqual(c.localBoundsMin, d.localBoundsMin) ||
                        !bitsEqual(c.localBoundsMax, d.localBoundsMax) ||
                        !bitsEqual(c.geometricError, d.geometricError))
                        return "LOD child" + at;
                }
            }
            const Metadata& m = expected.metadata_[index];
            const Metadata& n = actual.metadata_[index];
            if (m.owner != n.owner || m.model != n.model ||
                m.geometry != n.geometry || m.cookKey != n.cookKey ||
                m.materialOverrideSignature != n.materialOverrideSignature ||
                m.observationRevision != n.observationRevision ||
                !bitsEqual(m.localMinimum, n.localMinimum) ||
                !bitsEqual(m.localMaximum, n.localMaximum) ||
                m.baseConsumerMask != n.baseConsumerMask || m.valid != n.valid)
                return "metadata" + at;
        }
        return {};
    }

} // namespace Iridium
