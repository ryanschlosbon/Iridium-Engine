// M7R R5a.3: code moved from Application.cpp (design section 3.4); see
// RenderExtractor.h.
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "extraction/RenderExtractor.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <span>
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

    namespace {
        template <typename T>
        uint64_t shadowRevision(const T& value) noexcept {
            uint64_t hash = 1469598103934665603ull;
            const auto bytes = std::as_bytes(std::span{ &value, size_t{ 1 } });
            for (const std::byte byte : bytes) {
                hash ^= std::to_integer<uint8_t>(byte);
                hash *= 1099511628211ull;
            }
            return hash == 0u ? 1u : hash;
        }

        template <typename T>
        void appendCaptureRevision(uint64_t& hash, const T& value) noexcept {
            const auto bytes = std::as_bytes(std::span{ &value, size_t{ 1 } });
            for (const std::byte byte : bytes) {
                hash ^= std::to_integer<uint8_t>(byte);
                hash *= 1099511628211ull;
            }
        }

        uint64_t reflectionProbeSettingsRevision(
            const ReflectionProbeCandidate& candidate) noexcept {
            uint64_t hash = 1469598103934665603ull;
            appendCaptureRevision(hash, candidate.probeToWorld);
            appendCaptureRevision(hash, candidate.probe.captureResolution);
            appendCaptureRevision(hash, candidate.probe.captureNearMeters);
            appendCaptureRevision(hash, candidate.probe.captureFarMeters);
            appendCaptureRevision(hash, candidate.probe.captureSky);
            appendCaptureRevision(hash, candidate.probe.updateMode);
            return hash == 0u ? 1u : hash;
        }

        uint64_t reflectionProbeLightingRevision(
            const LightingFramePacket& lights) noexcept {
            uint64_t hash = 1469598103934665603ull;
            for (uint64_t revision : lights.recordRevisions)
                appendCaptureRevision(hash, revision);
            appendCaptureRevision(hash, lights.activeListRevision);
            return hash == 0u ? 1u : hash;
        }

    }

    RenderExtractor::RenderExtractor(CpuProfiler& profiler, SceneWorld& scene,
        const ProjectShadowSettings& shadowSettings,
        const ProjectReflectionProbeSettings& probeSettings)
        : cpuProfiler_(profiler),
          sceneWorld_(scene),
          registry(scene.registry()),
          shadowSettings_(shadowSettings),
          reflectionProbeCaptureScheduler_({
              .maximumRenderedTexels = probeSettings.
                  maximumRenderedTexelsPerFrame,
              .maximumFacesPerProbePerFrame = probeSettings.
                  maximumFacesPerProbePerFrame,
              .maximumCapturesInFlight = probeSettings.
                  maximumCapturesInFlight,
              .minimumRealtimeFramesBetweenCaptures =
                  probeSettings.
                      minimumRealtimeFramesBetweenCaptures }),
          spotShadowAtlas_({
              .atlasResolution = shadowSettings.spotAtlasResolution,
              .minimumTileResolution = 512,
              .guardTexels = 4 }),
          spotShadowCache_({
              .maximumRenderedTexels = shadowSettings.
                  maximumSpotRenderedTexelsPerFrame,
              .maximumCompatibleStaleFrames = shadowSettings.
                  maximumCompatibleSpotStaleFrames }),
          pointShadowPools_({ .cubeCapacity = {
              shadowSettings.pointPool256Capacity,
              shadowSettings.pointPool512Capacity,
              shadowSettings.pointPool1024Capacity } }),
          pointShadowCache_({
              .maximumRenderedTexels = shadowSettings.
                  maximumPointRenderedTexelsPerFrame,
              .maximumCompatibleStaleFrames = shadowSettings.
                  maximumCompatiblePointStaleFrames }) {}

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

    void RenderExtractor::configureProbeCaptures(
        const ProjectReflectionProbeSettings& probeSettings) {
        reflectionProbeCaptureScheduler_.configure({
            .maximumRenderedTexels = probeSettings.
                maximumRenderedTexelsPerFrame,
            .maximumFacesPerProbePerFrame = probeSettings.
                maximumFacesPerProbePerFrame,
            .maximumCapturesInFlight = probeSettings.
                maximumCapturesInFlight,
            .minimumRealtimeFramesBetweenCaptures = probeSettings.
                minimumRealtimeFramesBetweenCaptures,
        });
    }

    void RenderExtractor::markCapturePublished(
        const ReflectionProbeCaptureCompletion& completion) {
        reflectionProbeCaptureScheduler_.markPublished(
            completion.owner, completion.captureTicket);
    }

    void RenderExtractor::scheduleShadowsAndCaptures(
        const ShadowCasterSubmission& shadowCasters,
        const ReflectionProbeCasterSubmission& probeCasters,
        const glm::mat4& viewMatrix, const glm::vec3& renderCameraPosition,
        float renderVerticalFovDegrees, float aspect,
        float renderCameraNearPlane, float renderCameraFarPlane,
        bool assetPreviewActive, const std::string& environmentCookKey,
        uint64_t applicationFrameIndex, RenderFrame& renderFrame) {
        const LightingFramePacket& lightingFrame = lightingFrame_;
        const ReflectionProbeFramePacket& extractedProbes = extractedProbes_;
        std::vector<DirectionalShadowFramePacket>& directionalShadows =
            directionalShadows_;
        {
        CpuScope directionalScope(cpuProfiler_, "cpu.shadow.directional.schedule");
        const std::vector<DirectionalShadowSelection> shadowSelections =
            selectDirectionalShadowLights(lightingFrame,
                shadowSettings_.maximumDirectionalLights);
        if (!shadowSelections.empty()) {
            const glm::mat4 inverseView = glm::inverse(viewMatrix);
            DirectionalShadowCamera shadowCamera{};
            shadowCamera.position = renderCameraPosition;
            shadowCamera.forward = glm::normalize(-glm::vec3(inverseView[2]));
            shadowCamera.up = glm::normalize(glm::vec3(inverseView[1]));
            shadowCamera.verticalFovRadians = glm::radians(
                renderVerticalFovDegrees);
            shadowCamera.aspectRatio = aspect;
            shadowCamera.nearPlane = renderCameraNearPlane;
            shadowCamera.farPlane = (std::min)(renderCameraFarPlane,
                (std::max)(shadowSettings_.
                    directionalMaximumDistanceMeters,
                    renderCameraNearPlane + 0.001f));
            DirectionalShadowConfig shadowConfig{
                .resolution = shadowSettings_.directionalResolution,
                .splitLambda = shadowSettings_.directionalSplitLambda,
                .guardBandFraction =
                    shadowSettings_.directionalGuardBandFraction,
                .depthPaddingMeters =
                    shadowSettings_.directionalDepthPaddingMeters,
            };
            directionalShadows.reserve(shadowSelections.size());
            uint32_t dirtyCascades = 0;
            uint32_t casterInvalidatedCascades = 0;
            uint32_t updatedCascades = 0;
            uint32_t cachedCascades = 0;
            for (uint32_t shadowIndex = 0;
                shadowIndex < shadowSelections.size(); ++shadowIndex) {
                const DirectionalShadowSelection& selection =
                    shadowSelections[shadowIndex];
                const DirectionalShadowCascadePlan plan =
                    buildDirectionalShadowCascades(shadowCamera,
                        selection.lightForward, shadowConfig);
                const uint64_t lightRevision = selection.lightSlot <
                    lightingFrame.recordRevisions.size()
                    ? lightingFrame.recordRevisions[selection.lightSlot] : 0;
                std::array<uint64_t, kDirectionalShadowCascadeCount>
                    casterRevisions{};
                {
                    CpuScope revisionScope(cpuProfiler_,
                        "cpu.shadow.caster_revision.directional");
                    casterRevisions = renderBackend->
                        getDirectionalShadowCasterRevisions(
                            shadowCasters, plan);
                }
                const DirectionalShadowSchedule schedule =
                    directionalShadowCaches_[shadowIndex].schedule({
                        .selection = selection,
                        .plan = plan,
                        .lightRevision = lightRevision,
                        .casterRevisions = casterRevisions,
                        .pipelineRevision = 1,
                    },
                        shadowSettings_.maximumCascadeUpdatesPerLight);
                directionalShadows.push_back(DirectionalShadowFramePacket{
                    .selection = selection,
                    .plan = plan,
                    .shadowIndex = shadowIndex,
                    .updateMask = schedule.updateMask,
                    .sampleableMask = schedule.sampleableMask,
                    .resolution = shadowConfig.resolution,
                    .sourceAngularDiameterDegrees = shadowSettings_.
                        directionalSourceAngularDiameterDegrees,
                    .receiverDepthBiasTexels = shadowSettings_.
                        directionalReceiverDepthBiasTexels,
                    .receiverPlaneClampTexels = shadowSettings_.
                        directionalReceiverPlaneClampTexels,
                    .normalOffsetTexels = shadowSettings_.
                        directionalNormalOffsetTexels,
                    .filterProfile = effectiveShadowFilterProfile(
                        shadowSettings_, selection.quality),
                });
                dirtyCascades += schedule.invalidatedCount;
                casterInvalidatedCascades +=
                    schedule.casterInvalidatedCount;
                updatedCascades += std::popcount(schedule.updateMask);
                cachedCascades += schedule.cacheHitCount;
            }
            for (uint32_t shadowIndex = static_cast<uint32_t>(
                    shadowSelections.size());
                shadowIndex < directionalShadowCaches_.size(); ++shadowIndex)
                directionalShadowCaches_[shadowIndex].reset();
            activeDirectionalShadowSelection_ = shadowSelections.front();
            activeDirectionalShadowSampleableMask_ =
                directionalShadows.front().sampleableMask;
            activeDirectionalShadowOwnerCount_ = static_cast<uint32_t>(
                shadowSelections.size());
            cpuProfiler_.recordCounter("shadow.directional.requested",
                shadowSelections.size());
            cpuProfiler_.recordCounter("shadow.directional.omitted",
                shadowSelections.front().omittedShadowDirectionalLights);
            cpuProfiler_.recordCounter("shadow.directional.cascades.dirty",
                dirtyCascades);
            cpuProfiler_.recordCounter(
                "shadow.directional.cascades.caster_invalidated",
                casterInvalidatedCascades);
            cpuProfiler_.recordCounter("shadow.directional.cascades.updated",
                updatedCascades);
            cpuProfiler_.recordCounter("shadow.directional.cascades.cached",
                cachedCascades);
            const auto fixedMillionths = [](float value) {
                return static_cast<uint64_t>(std::llround(
                    static_cast<double>(value) * 1'000'000.0));
            };
            cpuProfiler_.recordCounter("shadow.directional.coverage_distance_m",
                fixedMillionths(shadowCamera.farPlane),
                ProfileCounterStatus::Exact, ProfileCounterUnit::Millionths);
            cpuProfiler_.recordCounter(
                "shadow.directional.receiver_depth_bias_texels",
                fixedMillionths(shadowSettings_.
                    directionalReceiverDepthBiasTexels),
                ProfileCounterStatus::Exact, ProfileCounterUnit::Millionths);
            cpuProfiler_.recordCounter(
                "shadow.directional.receiver_plane_clamp_texels",
                fixedMillionths(shadowSettings_.
                    directionalReceiverPlaneClampTexels),
                ProfileCounterStatus::Exact, ProfileCounterUnit::Millionths);
            cpuProfiler_.recordCounter(
                "shadow.directional.normal_offset_texels",
                fixedMillionths(shadowSettings_.
                    directionalNormalOffsetTexels),
                ProfileCounterStatus::Exact, ProfileCounterUnit::Millionths);
            constexpr std::array<const char*, 4> splitCounterNames{
                "shadow.directional.cascade0.split_far_m",
                "shadow.directional.cascade1.split_far_m",
                "shadow.directional.cascade2.split_far_m",
                "shadow.directional.cascade3.split_far_m" };
            constexpr std::array<const char*, 4> densityCounterNames{
                "shadow.directional.cascade0.world_units_per_texel_m",
                "shadow.directional.cascade1.world_units_per_texel_m",
                "shadow.directional.cascade2.world_units_per_texel_m",
                "shadow.directional.cascade3.world_units_per_texel_m" };
            const DirectionalShadowCascadePlan& diagnosticPlan =
                directionalShadows.front().plan;
            for (uint32_t cascade = 0;
                    cascade < kDirectionalShadowCascadeCount; ++cascade) {
                cpuProfiler_.recordCounter(splitCounterNames[cascade],
                    fixedMillionths(diagnosticPlan.cascades[cascade].splitFar),
                    ProfileCounterStatus::Exact,
                    ProfileCounterUnit::Millionths);
                cpuProfiler_.recordCounter(densityCounterNames[cascade],
                    fixedMillionths(diagnosticPlan.cascades[cascade].
                        worldUnitsPerTexel), ProfileCounterStatus::Exact,
                    ProfileCounterUnit::Millionths);
            }
        }
        else {
            for (DirectionalShadowCache& cache : directionalShadowCaches_)
                cache.reset();
            activeDirectionalShadowSelection_.reset();
            activeDirectionalShadowSampleableMask_ = 0;
            activeDirectionalShadowOwnerCount_ = 0;
            cpuProfiler_.recordCounter("shadow.directional.requested", 0);
        }
        }
        renderFrame.directionalShadows = { shadowCasters, directionalShadows };
        frameStage_ = { .directionalShadows = directionalShadows };

        // Spot shadows share the same extracted light slots and caster revision
        // as clustered lighting. Stable atlas allocation is reconciled before
        // cache scheduling so compatible tiles remain sampleable across frames.
        uint64_t localCasterRevision = 0;
        std::vector<SpotShadowFramePacket>& spotShadows = spotShadows_;
        std::vector<PointShadowFramePacket>& pointShadows = pointShadows_;
        {
        CpuScope localScope(cpuProfiler_, "cpu.shadow.local.schedule");
        const std::vector<LocalShadowRequest> localShadowRequests =
            buildLocalShadowRequests(lightingFrame, renderCameraPosition);
        const LocalShadowAllocationStats spotAllocation =
            spotShadowAtlas_.reconcile(localShadowRequests);
        spotShadowCache_.configure({
            .maximumRenderedTexels = shadowSettings_.
                maximumSpotRenderedTexelsPerFrame,
            .maximumCompatibleStaleFrames = shadowSettings_.
                maximumCompatibleSpotStaleFrames,
        });
        {
            CpuScope revisionScope(cpuProfiler_,
                "cpu.shadow.caster_revision.local");
            localCasterRevision =
                renderBackend->getShadowCasterRevision(shadowCasters);
        }
        std::vector<LocalShadowCacheInput> spotCacheInputs;
        spotCacheInputs.reserve(spotShadowAtlas_.allocations().size());
        for (const SpotShadowTile& tile : spotShadowAtlas_.allocations()) {
            const auto request = std::ranges::find_if(localShadowRequests,
                [&](const LocalShadowRequest& candidate) {
                    return candidate.kind == LocalShadowKind::Spot &&
                        candidate.owner == tile.owner;
                });
            if (request == localShadowRequests.end() ||
                tile.lightSlot >= lightingFrame.records.size()) continue;
            const PackedGpuLight& light = lightingFrame.records[tile.lightSlot];
            const float farPlane = light.positionRange.w;
            const float nearPlane = (std::max)(0.001f,
                (std::min)(0.05f, farPlane * 0.01f));
            if (!(farPlane > nearPlane)) continue;
            const SpotShadowProjection projection = buildSpotShadowProjection(
                glm::vec3(light.positionRange),
                glm::vec3(light.directionOuterCos),
                light.directionOuterCos.w, nearPlane, farPlane);
            const std::array<uint32_t, 5> allocationIdentity{
                tile.x, tile.y, tile.size, tile.guardTexels,
                shadowSettings_.spotAtlasResolution };
            spotCacheInputs.push_back({
                .request = *request,
                .resolution = tile.size,
                .allocationRevision = shadowRevision(allocationIdentity),
                .lightRevision = lightingFrame.recordRevisions[tile.lightSlot],
                .casterRevision = localCasterRevision,
                .projectionRevision = shadowRevision(
                    projection.worldToShadowClip),
                .pipelineRevision = 1,
            });
        }
        const LocalShadowSchedule& spotSchedule =
            spotShadowCache_.schedule(spotCacheInputs);
        spotShadows.reserve(spotSchedule.entries.size());
        for (const LocalShadowScheduleEntry& entry : spotSchedule.entries) {
            const auto tile = std::ranges::find_if(
                spotShadowAtlas_.allocations(),
                [&](const SpotShadowTile& candidate) {
                    return candidate.owner == entry.owner;
                });
            if (tile == spotShadowAtlas_.allocations().end() ||
                tile->lightSlot >= lightingFrame.records.size()) continue;
            const PackedGpuLight& light = lightingFrame.records[tile->lightSlot];
            const float farPlane = light.positionRange.w;
            const float nearPlane = (std::max)(0.001f,
                (std::min)(0.05f, farPlane * 0.01f));
            const SpotShadowProjection projection = buildSpotShadowProjection(
                glm::vec3(light.positionRange),
                glm::vec3(light.directionOuterCos),
                light.directionOuterCos.w, nearPlane, farPlane);
            const uint32_t shadowDataSlot = static_cast<uint32_t>(
                std::distance(spotShadowAtlas_.allocations().begin(), tile));
            if (shadowDataSlot >= kSpotShadowEntryCapacity) continue;
            spotShadows.push_back({
                .owner = entry.owner,
                .worldToShadowClip = projection.worldToShadowClip,
                .lightSlot = tile->lightSlot,
                .shadowDataSlot = shadowDataSlot,
                .atlasX = tile->x,
                .atlasY = tile->y,
                .tileSize = tile->size,
                .guardTexels = tile->guardTexels,
                .update = entry.update,
                .sampleable = entry.sampleable,
                .stale = entry.stale,
                .staleAgeFrames = entry.staleAgeFrames,
                .nearPlane = nearPlane,
                .farPlane = farPlane,
                .sourceRadiusMeters = light.shapeMetadata.x,
                .filterProfile = effectiveShadowFilterProfile(
                    shadowSettings_,
                    (std::bit_cast<uint32_t>(light.shapeMetadata.z) &
                        PackedGpuLightShadowQualityMask) >>
                        PackedGpuLightShadowQualityShift),
            });
        }
        renderFrame.spotShadows = { shadowCasters, spotShadows };
        frameStage_.spotAllocation = spotAllocation;
        frameStage_.spotSchedule = &spotSchedule;

        // Point lights use stable tiered cube slots. Cache publication is
        // all-or-nothing across the frozen six-face orientation so lighting can
        // never sample a partially refreshed cube.
        const LocalShadowAllocationStats pointAllocation =
            pointShadowPools_.reconcile(localShadowRequests);
        pointShadowCache_.configure({
            .maximumRenderedTexels = shadowSettings_.
                maximumPointRenderedTexelsPerFrame,
            .maximumCompatibleStaleFrames = shadowSettings_.
                maximumCompatiblePointStaleFrames,
        });
        std::vector<LocalShadowCacheInput> pointCacheInputs;
        pointCacheInputs.reserve(pointShadowPools_.allocations().size());
        for (const PointShadowSlot& slot : pointShadowPools_.allocations()) {
            const auto request = std::ranges::find_if(localShadowRequests,
                [&](const LocalShadowRequest& candidate) {
                    return candidate.kind == LocalShadowKind::Point &&
                        candidate.owner == slot.owner;
                });
            if (request == localShadowRequests.end() ||
                slot.lightSlot >= lightingFrame.records.size()) continue;
            const PackedGpuLight& light = lightingFrame.records[slot.lightSlot];
            const float farPlane = light.positionRange.w;
            const float nearPlane = (std::max)(0.001f,
                (std::min)(0.05f, farPlane * 0.01f));
            if (!(farPlane > nearPlane)) continue;
            const auto faces = buildPointShadowFaces(
                glm::vec3(light.positionRange), nearPlane, farPlane);
            std::array<glm::mat4, 6> matrices{};
            for (uint32_t face = 0; face < matrices.size(); ++face)
                matrices[face] = faces[face].worldToShadowClip;
            const std::array<uint32_t, 2> allocationIdentity{
                slot.resolution, slot.cubeIndex };
            pointCacheInputs.push_back({
                .request = *request,
                .resolution = slot.resolution,
                .allocationRevision = shadowRevision(allocationIdentity),
                .lightRevision = lightingFrame.recordRevisions[slot.lightSlot],
                .casterRevision = localCasterRevision,
                .projectionRevision = shadowRevision(matrices),
                .pipelineRevision = 1,
            });
        }
        const LocalShadowSchedule& pointSchedule =
            pointShadowCache_.schedule(pointCacheInputs);
        pointShadows.reserve(pointSchedule.entries.size());
        for (const LocalShadowScheduleEntry& entry : pointSchedule.entries) {
            const auto slot = std::ranges::find_if(
                pointShadowPools_.allocations(),
                [&](const PointShadowSlot& candidate) {
                    return candidate.owner == entry.owner;
                });
            if (slot == pointShadowPools_.allocations().end() ||
                slot->lightSlot >= lightingFrame.records.size()) continue;
            const PackedGpuLight& light = lightingFrame.records[slot->lightSlot];
            const float farPlane = light.positionRange.w;
            const float nearPlane = (std::max)(0.001f,
                (std::min)(0.05f, farPlane * 0.01f));
            const auto faces = buildPointShadowFaces(
                glm::vec3(light.positionRange), nearPlane, farPlane);
            const uint32_t shadowDataSlot = static_cast<uint32_t>(
                std::distance(pointShadowPools_.allocations().begin(), slot));
            if (shadowDataSlot >= kPointShadowEntryCapacity) continue;
            PointShadowFramePacket packet{
                .owner = entry.owner,
                .lightPosition = glm::vec3(light.positionRange),
                .nearPlane = nearPlane,
                .farPlane = farPlane,
                .lightSlot = slot->lightSlot,
                .shadowDataSlot = shadowDataSlot,
                .resolution = slot->resolution,
                .cubeIndex = slot->cubeIndex,
                .update = entry.update,
                .sampleable = entry.sampleable,
                .stale = entry.stale,
                .staleAgeFrames = entry.staleAgeFrames,
                .sourceRadiusMeters = light.shapeMetadata.x,
                .filterProfile = effectiveShadowFilterProfile(
                    shadowSettings_,
                    (std::bit_cast<uint32_t>(light.shapeMetadata.z) &
                        PackedGpuLightShadowQualityMask) >>
                        PackedGpuLightShadowQualityShift),
            };
            for (uint32_t face = 0; face < packet.worldToShadowClip.size();
                ++face)
                packet.worldToShadowClip[face] =
                    faces[face].worldToShadowClip;
            pointShadows.push_back(packet);
        }
        renderFrame.pointShadows = { shadowCasters, pointShadows };
        frameStage_.pointAllocation = pointAllocation;
        frameStage_.pointSchedule = &pointSchedule;
        }

        // Scene probes must never capture the isolated model or its preview sun.
        if (!assetPreviewActive) {
        CpuScope captureScope(cpuProfiler_, "cpu.probe.capture.schedule");
        std::vector<ReflectionProbeCaptureRequest> probeCaptureRequests;
        probeCaptureRequests.reserve(extractedProbes.candidates.size());
        uint64_t environmentRevision = 1469598103934665603ull;
        for (char character : environmentCookKey) {
            environmentRevision ^= static_cast<uint8_t>(character);
            environmentRevision *= 1099511628211ull;
        }
        if (environmentRevision == 0u) environmentRevision = 1u;
        const uint64_t lightingRevision =
            reflectionProbeLightingRevision(lightingFrame);
        for (const ReflectionProbeCandidate& candidate :
                extractedProbes.candidates) {
            if (!candidate.probe.environmentAssetGuid.isNil()) continue;
            probeCaptureRequests.push_back({
                .owner = candidate.owner,
                .updateMode = candidate.probe.updateMode,
                .position = glm::vec3(candidate.probeToWorld[3]),
                .resolution = static_cast<uint32_t>(
                    candidate.probe.captureResolution),
                .nearPlane = candidate.probe.captureNearMeters,
                .farPlane = candidate.probe.captureFarMeters,
                .priority = candidate.probe.priority,
                .captureSky = candidate.probe.captureSky,
                .settingsRevision = reflectionProbeSettingsRevision(candidate),
                .explicitRequestRevision =
                    candidate.probe.explicitCaptureRevision,
                .sceneRevision = localCasterRevision,
                .lightingRevision = lightingRevision,
                .environmentRevision = environmentRevision,
                .pipelineRevision = 1,
                .frameIndex = applicationFrameIndex,
            });
        }
        const ReflectionProbeCaptureSchedule& probeCaptureSchedule =
            reflectionProbeCaptureScheduler_.schedule(probeCaptureRequests);
        renderFrame.submitReflectionProbeCaptures = true;
        renderFrame.probeCasters = probeCasters;
        renderFrame.probeCaptureSchedule = probeCaptureSchedule.entries;
        frameStage_.probeCaptureSchedule = &probeCaptureSchedule;
        }    }

    void RenderExtractor::onRenderFrameStage(RenderFrameStage stage) {
        switch (stage) {
        case RenderFrameStage::DirectionalShadows:
            for (const DirectionalShadowFramePacket& shadow : frameStage_.directionalShadows)
                directionalShadowCaches_[shadow.shadowIndex].markRendered(
                    shadow.updateMask);
            break;
        case RenderFrameStage::SpotShadows: {
            // Completing the schedule retires it; its stats are read first.
            const auto stats = frameStage_.spotSchedule->stats;
            spotShadowCache_.markScheduledRendered();
            const LocalShadowAllocationStats& allocation = frameStage_.spotAllocation;
            cpuProfiler_.recordCounter("shadow.spot.requested", allocation.requested);
            cpuProfiler_.recordCounter("shadow.spot.allocated", allocation.allocated);
            cpuProfiler_.recordCounter("shadow.spot.omitted", allocation.omitted);
            cpuProfiler_.recordCounter("shadow.spot.cache_hits", stats.cacheHits);
            cpuProfiler_.recordCounter("shadow.spot.updates", stats.updates);
            cpuProfiler_.recordCounter("shadow.spot.stale_sampled",
                stats.staleSampled);
            cpuProfiler_.recordCounter("shadow.spot.unshadowed", stats.unshadowed);
            cpuProfiler_.recordCounter("shadow.spot.rendered_texels",
                stats.renderedTexels);
            break;
        }
        case RenderFrameStage::PointShadows: {
            // Completing the schedule retires it; its stats are read first.
            const auto stats = frameStage_.pointSchedule->stats;
            pointShadowCache_.markScheduledRendered();
            const LocalShadowAllocationStats& allocation = frameStage_.pointAllocation;
            cpuProfiler_.recordCounter("shadow.point.requested", allocation.requested);
            cpuProfiler_.recordCounter("shadow.point.allocated", allocation.allocated);
            cpuProfiler_.recordCounter("shadow.point.omitted", allocation.omitted);
            cpuProfiler_.recordCounter("shadow.point.cache_hits", stats.cacheHits);
            cpuProfiler_.recordCounter("shadow.point.updates", stats.updates);
            cpuProfiler_.recordCounter("shadow.point.stale_sampled",
                stats.staleSampled);
            cpuProfiler_.recordCounter("shadow.point.unshadowed", stats.unshadowed);
            cpuProfiler_.recordCounter("shadow.point.rendered_texels",
                stats.renderedTexels);
            break;
        }
        case RenderFrameStage::ReflectionProbeCaptures: {
            // Completing the schedule retires it; its stats are read first.
            const auto stats = frameStage_.probeCaptureSchedule->stats;
            reflectionProbeCaptureScheduler_.markScheduledFacesRendered();
            const ReflectionProbeCaptureTelemetry telemetry =
                renderBackend->frameTelemetry().probeCaptures;
            cpuProfiler_.recordCounter("probe.capture.faces_scheduled",
                stats.facesScheduled);
            cpuProfiler_.recordCounter("probe.capture.budget_deferred",
                stats.budgetDeferred);
            cpuProfiler_.recordCounter("probe.capture.capacity_deferred",
                stats.capacityDeferred);
            cpuProfiler_.recordCounter("probe.capture.cadence_deferred",
                stats.cadenceDeferred);
            cpuProfiler_.recordCounter("probe.capture.faces_rendered",
                telemetry.facesRendered);
            cpuProfiler_.recordCounter("probe.capture.filtered",
                telemetry.capturesFiltered);
            cpuProfiler_.recordCounter("probe.capture.published",
                telemetry.capturesPublished);
            cpuProfiler_.recordCounter("probe.capture.staging_bytes",
                telemetry.stagingLogicalBytes,
                ProfileCounterStatus::Exact, ProfileCounterUnit::Bytes);
            cpuProfiler_.recordCounter("probe.capture.published_bytes",
                telemetry.publishedLogicalBytes,
                ProfileCounterStatus::Exact, ProfileCounterUnit::Bytes);
            break;
        }
        case RenderFrameStage::Lighting: {
            const RenderFrameTelemetry telemetry = renderBackend->frameTelemetry();
            const LightingUploadTelemetry& lightUpload = telemetry.lightUploads;
            cpuProfiler_.recordCounter("light.gpu_upload_bytes", lightUpload.bytes,
                ProfileCounterStatus::Exact, ProfileCounterUnit::Bytes);
            cpuProfiler_.recordCounter("light.gpu_upload_ranges", lightUpload.ranges);
            const ClusteredLightingTelemetry& clusters = telemetry.clusters;
            const ProfileCounterStatus clusterStatus = clusters.available
                ? ProfileCounterStatus::Exact : ProfileCounterStatus::Unavailable;
            cpuProfiler_.recordCounter("cluster.buffer_bytes_per_frame",
                clusters.bufferBytesPerFrame, clusterStatus, ProfileCounterUnit::Bytes);
            cpuProfiler_.recordCounter("cluster.count", clusters.clusterCount, clusterStatus);
            cpuProfiler_.recordCounter("cluster.lights.active", clusters.activeLights,
                clusterStatus);
            cpuProfiler_.recordCounter("cluster.lights.directional",
                clusters.directionalLights, clusterStatus);
            cpuProfiler_.recordCounter("cluster.lights.local", clusters.localLights,
                clusterStatus);
            cpuProfiler_.recordCounter("cluster.references.requested",
                clusters.requestedReferences, clusterStatus);
            cpuProfiler_.recordCounter("cluster.references.published",
                clusters.publishedReferences, clusterStatus);
            cpuProfiler_.recordCounter("cluster.used", clusters.clustersUsed, clusterStatus);
            cpuProfiler_.recordCounter("cluster.occupancy.maximum",
                clusters.maximumOccupancy, clusterStatus);
            cpuProfiler_.recordCounter("cluster.fallback_lights",
                clusters.fallbackLights, clusterStatus);
            cpuProfiler_.recordCounter("cluster.dropped_lights",
                clusters.droppedLights, clusterStatus);
            cpuProfiler_.recordCounter("cluster.overflow_code", clusters.overflowCode,
                clusterStatus);
            break;
        }        case RenderFrameStage::SceneLinearComplete:
        case RenderFrameStage::OutputComplete:
            break;
        }
    }

    void RenderExtractor::releaseFrame() {
        frameStage_ = {};
        lightingFrame_ = {};
        extractedProbes_ = {};
        publishedProbes_ = {};
        directionalShadows_ = {};
        spotShadows_ = {};
        pointShadows_ = {};
    }

} // namespace Iridium
