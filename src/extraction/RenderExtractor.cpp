// M7R R5a.3: code moved from Application.cpp (design section 3.4); see
// RenderExtractor.h.
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "extraction/RenderExtractor.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <vector>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>

#include "assets/AssetManager.h"
#include "profiling/CpuProfiler.h"
#include "renderer/rhi/IRenderBackend.h"
#include "renderer/rhi/Mesh.h"
#include "scene/components/MeshComponent.h"
#include "scene/components/RenderInstanceBatchComponent.h"
#include "scene/components/TransformComponent.h"

namespace Iridium {

    RenderExtractor::RenderExtractor(CpuProfiler& profiler, SceneWorld& scene)
        : cpuProfiler_(profiler),
          sceneWorld_(scene),
          registry(scene.registry()) {}

    RenderExtractor::~RenderExtractor() = default;

    void RenderExtractor::attachBackend(IRenderBackend& backend) {
        renderBackend = &backend;
        gpuScenePublisher_ = std::make_unique<GpuScenePublisher>(
            GpuSceneCapacity{
                .maximumInstances = 262'144u,
                .maximumPrimitives = 1'048'576u,
                .maximumGeometries = 1'048'576u,
                .maximumTransforms = 524'288u,
            });
    }

    void RenderExtractor::attachAssets(AssetManager& assets) {
        assetManager_ = &assets;
    }


    void RenderExtractor::prepareGpuScenePublication(Entity selectedEntity) {
        gpuSceneFrame_ = nullptr;
        gpuSceneDirectFallbackCount_ = 0;
        if (!gpuScenePublisher_) return;
        CpuScope publicationScope(cpuProfiler_, "cpu.gpu_scene.publish");
        size_t observationCount = 0;

        {
        CpuScope observationScope(cpuProfiler_, "cpu.gpu_scene.observe");
        auto* transformPool = registry.getPool<TransformComponent>();
        auto* meshPool = registry.getPool<MeshComponent>();
        auto* instanceBatchPool = registry.findPool<
            RenderInstanceBatchComponent>();
        if (transformPool && meshPool) {
            gpuSceneObservations_.reserve(meshPool->entities.size());
            gpuSceneObservationMetadata_.reserve(meshPool->entities.size());
            for (Entity entity : meshPool->entities) {
                const MeshComponent& mesh = meshPool->get(entity);
                if (!mesh.enabled || !mesh.model ||
                    !mesh.model->geometry.isValid() ||
                    !transformPool->has(entity)) continue;
                if (instanceBatchPool && instanceBatchPool->has(entity)) {
                    ++gpuSceneDirectFallbackCount_;
                    continue;
                }
                const auto owner = sceneWorld_.identities().persistentId(entity);
                if (!owner || owner->isNil()) {
                    ++gpuSceneDirectFallbackCount_;
                    continue;
                }
                const ModelAsset& model = *mesh.model;
                if (observationCount == gpuSceneObservations_.size()) {
                    gpuSceneObservations_.emplace_back();
                    gpuSceneObservationMetadata_.emplace_back();
                }
                GpuSceneObservedInstance& observation =
                    gpuSceneObservations_[observationCount];
                GpuSceneObservationMetadata& metadata =
                    gpuSceneObservationMetadata_[observationCount];
                const glm::mat4 previousWorld = observation.worldTransform;
                const uint32_t previousFlags = observation.flags;
                const uint32_t previousMaximumLod = observation.maximumLod;
                const uint32_t previousConsumerMask =
                    observation.consumerMask;
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
                            assetManager_->findCookedMaterialRuntime(
                                value.materialGuid)) {
                        mix(resolved->binding.material.id);
                        mix(resolved->binding.pipeline.id);
                        mix(static_cast<uint32_t>(
                            resolved->binding.renderQueue));
                    }
                }
                const bool rebuildPrimitives = metadata.owner != *owner ||
                    metadata.model != &model ||
                    metadata.geometry != model.geometry ||
                    metadata.cookKey != model.artifactCookKey ||
                    metadata.materialOverrideSignature != overrideSignature;
                observation.identity.owner = *owner;
                observation.identity.modelAssetGuid = model.assetGuid;
                observation.identity.publishedRevision = 1;
                observation.identity.artifactCookKey = model.artifactCookKey;
                observation.mobility = GpuSceneMobility::Movable;
                observation.flags = GpuSceneInstanceEnabled |
                    (entity == selectedEntity
                        ? GpuSceneInstanceSelected : 0u);
                observation.maximumLod = static_cast<uint32_t>(
                    std::clamp(mesh.maximumLodLevel, 0,
                        MeshComponent::MaximumLodLevel));
                observation.consumerMask = metadata.baseConsumerMask |
                    (entity == selectedEntity
                        ? GpuSceneConsumerSelection : 0u);
                observation.worldTransform =
                    transformPool->get(entity).worldMatrix;
                if (rebuildPrimitives) {
                    metadata.owner = *owner;
                    metadata.model = &model;
                    metadata.geometry = model.geometry;
                    metadata.cookKey = model.artifactCookKey;
                    metadata.materialOverrideSignature = overrideSignature;
                    metadata.localMinimum = glm::vec3(
                        (std::numeric_limits<float>::max)());
                    metadata.localMaximum = glm::vec3(
                        (std::numeric_limits<float>::lowest)());
                    metadata.baseConsumerMask = 0;
                    metadata.valid = true;
                    observation.primitives.clear();
                    for (const SubMesh& subMesh : model.subMeshes) {
                    if (subMesh.materialIndex < 0 ||
                        static_cast<size_t>(subMesh.materialIndex) >=
                            model.materials.size()) continue;
                    const MaterialBinding* binding = &model.materials[
                        static_cast<size_t>(subMesh.materialIndex)];
                    AssetGuid effectiveMaterialGuid = subMesh.materialGuid;
                    const auto materialOverride = std::ranges::find_if(
                        mesh.materialOverrides,
                        [&subMesh](const MeshComponent::MaterialOverride& value) {
                            return value.sourceMaterialGuid == subMesh.materialGuid;
                        });
                    std::optional<CookedMaterialRuntimeBinding> overrideBinding;
                    if (materialOverride != mesh.materialOverrides.end()) {
                        overrideBinding = assetManager_->findCookedMaterialRuntime(
                            materialOverride->materialGuid);
                        if (overrideBinding) {
                            binding = &overrideBinding->binding;
                            effectiveMaterialGuid = materialOverride->materialGuid;
                        }
                    }
                    if (binding->renderQueue == RenderQueue::Transparent) continue;
                    if (!binding->material.isValid() ||
                        !binding->pipeline.isValid() ||
                        subMesh.sourcePrimitiveGuid.isNil() ||
                        subMesh.primitiveGuid.isNil() ||
                        effectiveMaterialGuid.isNil() ||
                        subMesh.indexCount == 0) {
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
                        for (size_t levelIndex = 1; levelIndex < lodChain->levels.size(); ++levelIndex) {
                            const auto& level = lodChain->levels[levelIndex];
                            const SubMesh& child = level.subMesh;
                            children.push_back({
                                .identity = { model.assetGuid, child.sourcePrimitiveGuid, child.primitiveGuid },
                                .geometry = child.geometry,
                                .firstIndex = child.indexStart, .indexCount = child.indexCount,
                                .vertexOffset = child.vertexOffset,
                                .indexType = child.indexFormat, .vertexLayout = child.attributeMask,
                                .localBoundsSphere = glm::vec4(child.boundsSphereCenter, child.boundsSphereRadius),
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
                    (entity == selectedEntity
                        ? GpuSceneConsumerSelection : 0u);
                if (!metadata.valid) {
                    observation.primitives.clear();
                    ++gpuSceneDirectFallbackCount_;
                    continue;
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
                observation.observationRevision =
                    metadata.observationRevision;
                ++observationCount;
            }
        }
        gpuSceneObservations_.resize(observationCount);
        gpuSceneObservationMetadata_.resize(observationCount);
        }

        const GpuSceneFrameSerials serials =
            renderBackend->getGpuSceneFrameSerials();
        {
            CpuScope synchronizeScope(
                cpuProfiler_, "cpu.gpu_scene.synchronize");
            gpuSceneFrame_ = &gpuScenePublisher_->synchronize(
                sceneWorld_.stateEpoch(), gpuSceneObservations_,
                serials.lastSubmitted, serials.completed);
        }
        {
            CpuScope capacityScope(cpuProfiler_, "cpu.gpu_scene.prepare");
            renderBackend->prepareGpuScene({
                static_cast<uint32_t>(gpuSceneFrame_->transforms.size()),
                static_cast<uint32_t>(gpuSceneFrame_->instances.size()),
                static_cast<uint32_t>(gpuSceneFrame_->primitives.size()),
                static_cast<uint32_t>(gpuSceneFrame_->geometries.size()),
            });
        }
        const GpuScenePublisherStats& stats = gpuScenePublisher_->stats();
        cpuProfiler_.recordCounter("gpu_scene.instance.active",
            stats.activeInstances);
        cpuProfiler_.recordCounter("gpu_scene.primitive.active",
            stats.activePrimitives);
        cpuProfiler_.recordCounter("gpu_scene.geometry.active",
            stats.activeGeometries);
        cpuProfiler_.recordCounter("gpu_scene.transform.changed",
            stats.changedTransforms);
        cpuProfiler_.recordCounter("gpu_scene.instance.changed",
            stats.changedInstances);
        cpuProfiler_.recordCounter("gpu_scene.primitive.changed",
            stats.changedPrimitives);
        cpuProfiler_.recordCounter("gpu_scene.geometry.changed",
            stats.changedGeometries);
        cpuProfiler_.recordCounter("gpu_scene.publication.unchanged_fast_path",
            stats.unchangedFastPath);
        cpuProfiler_.recordCounter("gpu_scene.direct_fallback",
            gpuSceneDirectFallbackCount_ + stats.capacityFallbackInstances);
    }

    void RenderExtractor::prepareLightsAndProbes(SceneWorld& lightingWorld,
        const std::map<AssetGuid, LoadedEnvironmentAsset>& loadedEnvironments) {
        loadedEnvironments_ = &loadedEnvironments;
        LightingFramePacket& lightingFrame = lightingFrame_;
        {
            CpuScope lightScope(cpuProfiler_, "cpu.light.extract");
            lightingFrame = lightExtractor_.extract(lightingWorld);
        }
        {
            CpuScope lightScope(cpuProfiler_, "cpu.light.prepare");
            renderBackend->prepareLighting(lightingFrame.requiredCapacity);
        }
        ReflectionProbeFramePacket& extractedProbes = extractedProbes_;
        {
            CpuScope probeScope(cpuProfiler_, "cpu.probe.extract");
            extractedProbes = extractReflectionProbes(sceneWorld_,
                [&loadedEnvironments](AssetGuid environment) {
                    return loadedEnvironments.contains(environment);
                });
            std::vector<SceneEntityUuid> runtimeCaptureOwners;
            runtimeCaptureOwners.reserve(extractedProbes.candidates.size());
            for (const ReflectionProbeCandidate& candidate :
                    extractedProbes.candidates)
                if (candidate.probe.environmentAssetGuid.isNil())
                    runtimeCaptureOwners.push_back(candidate.owner);
            renderBackend->synchronizeReflectionProbeCaptureOwners(
                runtimeCaptureOwners);
            for (ReflectionProbeCandidate& candidate :
                    extractedProbes.candidates) {
                if (!candidate.probe.environmentAssetGuid.isNil()) continue;
                candidate.runtimeEnvironmentSlot = renderBackend->
                    capturedReflectionProbeEnvironmentSlot(candidate.owner);
                if (candidate.runtimeEnvironmentSlot)
                    candidate.resident = true;
            }
        }
        ReflectionProbeGpuFramePacket& publishedProbes = publishedProbes_;
        {
            CpuScope probeScope(cpuProfiler_, "cpu.probe.publish");
            reflectionProbeEnvironments_.clear();
            reflectionProbeEnvironments_.reserve(
                kMaximumGpuReflectionProbeEnvironments);
            for (const auto& [guid, environment] : loadedEnvironments) {
                (void)guid;
                if (reflectionProbeEnvironments_.size() >=
                    kMaximumGpuReflectionProbeEnvironments) break;
                reflectionProbeEnvironments_.push_back(environment.lighting);
            }
            publishedProbes = reflectionProbePublisher_.publish(
                extractedProbes.candidates,
                [this](AssetGuid environment) -> std::optional<uint32_t> {
                    const auto& environments = *loadedEnvironments_;
                    const auto found = environments.find(environment);
                    if (found == environments.end()) return std::nullopt;
                    const size_t index = static_cast<size_t>(std::distance(
                        environments.begin(), found));
                    if (index >= reflectionProbeEnvironments_.size())
                        return std::nullopt;
                    return static_cast<uint32_t>(index);
                });
        }
        {
            CpuScope probeScope(cpuProfiler_, "cpu.probe.prepare");
            renderBackend->prepareReflectionProbes(
                publishedProbes.requiredCapacity, reflectionProbeEnvironments_);
        }
        cpuProfiler_.recordCounter("probe.extracted",
            publishedProbes.stats.extractedCandidateCount);
        cpuProfiler_.recordCounter("probe.active",
            publishedProbes.stats.activeProbeCount);
        cpuProfiler_.recordCounter("probe.nonresident",
            publishedProbes.stats.nonresidentProbeCount);
        cpuProfiler_.recordCounter("probe.environment_unresolved",
            publishedProbes.stats.unresolvedEnvironmentCount);
        cpuProfiler_.recordCounter("probe.capacity_omitted",
            publishedProbes.stats.capacityOmittedCount);
        cpuProfiler_.recordCounter("probe.publish.changed_bytes",
            publishedProbes.stats.changedRecordBytes);
    }

    void RenderExtractor::releaseFrame() {
        lightingFrame_ = {};
        extractedProbes_ = {};
        publishedProbes_ = {};
    }

} // namespace Iridium
