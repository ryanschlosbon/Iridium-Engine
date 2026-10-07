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
#include <exception>
#include <iostream>
#include <string>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

#include "assets/AssetManager.h"
#include "core/BuildFeatures.h"
#include "core/tasks/TaskSystem.h"
#include "renderer/lighting/ShadowCasterCulling.h"
#include "renderer/rhi/TransparencyQualityOverride.h"
#include "renderer/transparency/WeightedOit.h"
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
        const ProjectReflectionProbeSettings& probeSettings,
        Tasks::TaskSystem* tasks)
        : cpuProfiler_(profiler),
          sceneWorld_(scene),
          registry(scene.registry()),
          gpuSceneObservation_(scene, GpuSceneObservation::Mode::ChangeDriven),
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
                  maximumCompatiblePointStaleFrames }),
          tasks_(tasks),
          transparentSorter_(tasks) {}

    RenderExtractor::~RenderExtractor() {
        if constexpr (kQualificationBuild) {
            if (observationVerifier_) {
                const GpuSceneObservation::Stats& stats =
                    gpuSceneObservation_.stats();
                std::cout << "IRIDIUM_EXTRACTION_VERIFIER {\"passed\":true,"
                    "\"verified_frames\":" << verifiedObservationFrames_ <<
                    ",\"full_walks\":" << stats.fullWalks <<
                    ",\"change_driven_frames\":" << stats.changeDrivenFrames <<
                    ",\"visited_runs\":" << stats.visitedRuns << "}" << std::endl;
            }
        }
    }

    void RenderExtractor::attachBackend(IRenderBackend& backend,
        const RenderExtractorPolicy& policy) {
        renderBackend = &backend;
        policy_ = policy;
        if constexpr (kQualificationBuild) {
            if (policy.verifyGpuSceneObservations)
                observationVerifier_ = std::make_unique<GpuSceneObservation>(
                    sceneWorld_, GpuSceneObservation::Mode::FullWalk);
        }
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

    void RenderExtractor::verifyGpuSceneObservation(
        const GpuSceneObservationInputs& inputs) {
        // M7R R5c.5 ExtractionVerifier (qualification only): the full walk on
        // its own state, every frame, against the change-driven observation.
        observationVerifier_->observe(inputs);
        ++verifiedObservationFrames_;
        const std::string difference = GpuSceneObservation::compare(
            *observationVerifier_, gpuSceneObservation_);
        if (!difference.empty()) {
            std::cout << "IRIDIUM_EXTRACTION_VERIFIER {\"passed\":false,"
                "\"frame\":" << verifiedObservationFrames_ <<
                ",\"difference\":\"" << difference << "\"}" << std::endl;
            throw std::logic_error(
                "GPU-scene observation diverged from the full walk: " + difference);
        }
    }


    void RenderExtractor::prepareGpuScenePublication(Entity selectedEntity,
        std::span<const Entity> changedTransforms) {
        gpuSceneFrame_ = nullptr;
        gpuSceneDirectFallbackCount_ = 0;
        if (!gpuScenePublisher_) return;
        CpuScope publicationScope(cpuProfiler_, "cpu.gpu_scene.publish");

        const GpuSceneObservationInputs observationInputs{
            .assets = assetManager_,
            .selectedEntity = selectedEntity,
            .changedTransforms = changedTransforms,
        };
        {
            CpuScope observationScope(cpuProfiler_, "cpu.gpu_scene.observe");
            gpuSceneObservation_.observe(observationInputs);
        }
        if constexpr (kQualificationBuild) {
            if (observationVerifier_) verifyGpuSceneObservation(observationInputs);
        }
        gpuSceneDirectFallbackCount_ = gpuSceneObservation_.directFallbackCount();

        const GpuSceneFrameSerials serials =
            renderBackend->getGpuSceneFrameSerials();
        {
            CpuScope synchronizeScope(
                cpuProfiler_, "cpu.gpu_scene.synchronize");
            gpuSceneFrame_ = &gpuScenePublisher_->synchronize(
                sceneWorld_.stateEpoch(), gpuSceneObservation_.observations(),
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
        cpuProfiler_.recordCounter("gpu_scene.transform.settled",
            stats.settledTransforms);
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
            extractReflectionProbes(sceneWorld_,
                [&loadedEnvironments](AssetGuid environment) {
                    return loadedEnvironments.contains(environment);
                }, extractedProbes);
            // M7R R5c.8: member scratch (keeps its capacity).
            std::vector<SceneEntityUuid>& runtimeCaptureOwners =
                runtimeCaptureOwners_;
            runtimeCaptureOwners.clear();
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
        const std::vector<DirectionalShadowSelection>& shadowSelections =
            directionalShadowSelections_;
        selectDirectionalShadowLights(lightingFrame,
            shadowSettings_.maximumDirectionalLights,
            directionalShadowSelections_);
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
        const std::vector<LocalShadowRequest>& localShadowRequests =
            localShadowRequests_;
        buildLocalShadowRequests(lightingFrame, renderCameraPosition,
            localShadowRequests_);
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
        std::vector<LocalShadowCacheInput>& spotCacheInputs = spotCacheInputs_;
        spotCacheInputs.clear();
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
        std::vector<LocalShadowCacheInput>& pointCacheInputs = pointCacheInputs_;
        pointCacheInputs.clear();
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
        std::vector<ReflectionProbeCaptureRequest>& probeCaptureRequests =
            probeCaptureRequests_;
        probeCaptureRequests.clear();
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

    void RenderExtractor::clearQueues() {
        // M7R R5c.7: the opaque, forward-opaque, transparent, sorted-surface
        // and selection queues and the instance transforms are not cleared:
        // every extraction sizes them exactly in mergeExtractionChunks, and
        // keeping their sizes means a steady frame constructs no packets
        // before the parallel copy overwrites them.
        shadowCasterQueue.clear();
        probeCasterQueue_.clear();
    }

    void RenderExtractor::beginView(const EditorViewState& preBuildView,
        const RenderCamera& camera, float aspect) {
        aspect_ = aspect;
        const bool preBuildAssetPreviewActive =
            preBuildView.assetPreviewActive;
        shadowGpuScenePrimitiveIndices_ =
            !preBuildAssetPreviewActive && gpuSceneFrame_
                ? std::span<const uint32_t>(
                    gpuSceneFrame_->shadowConsumerPrimitiveIndices)
                : std::span<const uint32_t>{};
        probeGpuScenePrimitiveIndices_ =
            !preBuildAssetPreviewActive && gpuSceneFrame_ &&
                !policy_.forceDirectGBufferReference &&
                !policy_.forceDirectProbeCaptureReference
                ? std::span<const uint32_t>(
                    gpuSceneFrame_->probeConsumerPrimitiveIndices)
                : std::span<const uint32_t>{};

        renderCameraPosition_ = camera.position;
        renderCameraNearPlane_ = camera.nearPlane;
        renderCameraFarPlane_ = camera.farPlane;
        renderVerticalFovDegrees_ = camera.verticalFovDegrees;
        viewMatrix_ = glm::lookAt(
            camera.position, camera.position + camera.front, camera.up);
        projMatrix_ = glm::perspective(
            glm::radians(camera.verticalFovDegrees), aspect,
            camera.nearPlane, camera.farPlane);
        projMatrix_[1][1] *= -1.0f; // Vulkan inverted Y
        usePreviewCamera(preBuildView);
    }

    void RenderExtractor::usePreviewCamera(const EditorViewState& view) {
        if (!view.assetPreviewActive || !view.hasPreviewCamera) return;
        renderCameraPosition_ = view.previewCameraPosition;
        viewMatrix_ = view.previewView;
        projMatrix_ = view.previewProjection;
        renderCameraNearPlane_ = view.previewNearPlane;
        renderCameraFarPlane_ = view.previewFarPlane;
        renderVerticalFovDegrees_ = view.previewVerticalFovDegrees;
    }

    void RenderExtractor::finalizeView(const EditorViewState& view,
        const RenderViewInputs& inputs) {
        view_ = &view;
        usePreviewCamera(view);
        const bool assetPreviewActive = view.assetPreviewActive;
        if (assetPreviewActive) {
            projMatrix_[0][0] *= view.previewProjectionScale;
            projMatrix_[1][1] *= view.previewProjectionScale;
        }
        ViewTransportRecord viewTransport = makeViewTransportRecord(
            viewMatrix_, projMatrix_, renderCameraPosition_,
            renderCameraNearPlane_, renderCameraFarPlane_,
            { inputs.renderExtent.width, inputs.renderExtent.height });
        const RenderDebugView debugView = view.debugView;
        renderBackend->setEnvironmentLightingSettings(assetPreviewActive
            ? view.previewEnvironmentSettings : inputs.sceneEnvironmentSettings);
        const float viewExposure = assetPreviewActive ? view.previewExposureEv : inputs.manualExposureEv;
        // M9 G2: the retained view's History context. Set 0 is the scene view
        // (identity 1), set 1 the asset preview (identity sessionSerial + 2).
        lastViewMotion_ = viewMotion_.update({
            .historySet = assetPreviewActive ? 1u : 0u,
            .identity = assetPreviewActive ? view.previewSessionSerial + 2u : 1u,
            .requestedResetRevision = assetPreviewActive ? view.previewFramingRevision :
                inputs.viewHistoryResetRevision.value_or(0u),
            .view = viewMatrix_,
            .projection = projMatrix_,
            .projectionKind = viewTransport.renderInfo.z,
            .extent = { inputs.renderExtent.width, inputs.renderExtent.height },
            .metresPerWorldUnit = viewTransport.worldUnits.x,
            .jitter = inputs.temporalJitter,
            .jitterSequenceLength = inputs.temporalJitterSequenceLength,
            .timeSeconds = inputs.timeSeconds,
        });
        // M9 G5b: temporal fields. Without jitter the jittered pair stays the
        // unjittered pair (makeViewTransportRecord), bit for bit.
        viewTransport.previousViewProjection = lastViewMotion_.previousViewProjection;
        viewTransport.jitter = glm::vec4(lastViewMotion_.jitterNdc,
            lastViewMotion_.previousJitterNdc);
        uint32_t temporalFlags = 0;
        if (lastViewMotion_.cut != ViewCutReason::None) temporalFlags |= ViewTemporalHistoryReset;
        // Temporal health in profiles: a view cut and its reason (0 none, 1 first
        // turn, 2 requested, 3 explicit, 4 projection, 5 translation, 6 rotation).
        cpuProfiler_.recordCounter("view.cut", lastViewMotion_.cut != ViewCutReason::None ? 1 : 0);
        cpuProfiler_.recordCounter("view.cut_reason", static_cast<uint64_t>(lastViewMotion_.cut));
        if (lastViewMotion_.jitterNdc != glm::vec2(0.0f)) {
            temporalFlags |= ViewTemporalJitterActive;
            viewTransport.jitteredProjection =
                jitterProjection(viewTransport.projection, lastViewMotion_.jitterNdc);
            viewTransport.jitteredInverseProjection =
                glm::inverse(viewTransport.jitteredProjection);
        }
        viewTransport.temporalInfo = glm::uvec4(lastViewMotion_.jitterIndex,
            static_cast<uint32_t>(std::min<uint64_t>(lastViewMotion_.turnsSinceCut, UINT32_MAX)),
            temporalFlags, inputs.temporalJitterSequenceLength);
        // M7R R3c.11: the frame is assembled from spans over this frame's
        // queues and packets and submitted once, after extraction.
        renderFrame_ = RenderFrame{
            .view = viewTransport,
            .history = lastViewMotion_.history,
            .viewDeltaSeconds = lastViewMotion_.deltaSeconds,
            .debugView = debugView,
            .output = { viewExposure, static_cast<float>(inputs.paperWhiteNits),
                static_cast<float>(inputs.peakNits) },
        };
    }

    void RenderExtractor::setGridOverlay(const ViewportGridOverlay& overlay) {
        renderFrame_.gridOverlay = overlay;
    }

    const RenderFrame& RenderExtractor::extract(
        const RenderExtractionInputs& inputs) {
        const EditorViewState& view = *view_;
        const bool assetPreviewActive = view.assetPreviewActive;
        const std::shared_ptr<ModelAsset>& previewModel = view.previewModel;
        const Entity selectedEntity = view.selectedEntity;
        const RenderDebugView debugView = view.debugView;
        const float aspect = aspect_;
        const std::span<const uint32_t> shadowGpuScenePrimitiveIndices =
            shadowGpuScenePrimitiveIndices_;
        const std::span<const uint32_t> probeGpuScenePrimitiveIndices =
            probeGpuScenePrimitiveIndices_;
        const glm::vec3& renderCameraPosition = renderCameraPosition_;
        const float renderCameraNearPlane = renderCameraNearPlane_;
        const float renderCameraFarPlane = renderCameraFarPlane_;
        const float renderVerticalFovDegrees = renderVerticalFovDegrees_;
        const glm::mat4& viewMatrix = viewMatrix_;
        const glm::mat4& projMatrix = projMatrix_;
        const LightingFramePacket& lightingFrame = lightingFrame_;
        ReflectionProbeGpuFramePacket& publishedProbes = publishedProbes_;
        RenderFrame& renderFrame = renderFrame_;
        ++extractionFrame_;
        lastTransparentSubmeshModel_ = nullptr;
        lastTransparentSubmeshList_ = UINT32_MAX;

        // --- 3. THE EXTRACTION PHASE (Data-Oriented Design) ---
        GpuSceneVisibilityStats gpuSceneVisibilityStats{};
        uint64_t gpuSceneDeferredCandidateCount = 0;
        uint64_t gpuSceneDeferredCandidateTriangles = 0;
        uint64_t gpuSceneForwardVisibleCount = 0;
        uint64_t gpuSceneForwardVisibleTriangles = 0;
        if (!assetPreviewActive && gpuSceneFrame_) {
            CpuScope classifyScope(cpuProfiler_, "cpu.render.classify");
            classifyMainView(projMatrix * viewMatrix);
            gpuSceneVisibilityStats = gpuSceneVisibility_.stats;
        }
        ExtractionCounters extracted{};
        {
            CpuScope extractionScope(cpuProfiler_, "cpu.render.extract");
            // M7R R5c.7: appends one model's packets, transforms and counters
            // to `out`, a chunk's scratch (see ExtractionChunk). It reads only
            // frame-constant state, so chunks run concurrently.
            const auto appendModel = [&](ExtractionChunk& out,
                    const ModelAsset& model,
                    const glm::mat4& worldTransform,
                    const MeshComponent* meshComponent,
                    const RenderInstanceBatchComponent* instanceBatch,
                    const CookedMaterialRuntimeBinding* forcedMaterial,
                    bool selected, SceneEntityUuid owner,
                    bool persistentOpaque,
                    const TransparentSubmeshList* transparentList) {
                if (!model.geometry.isValid()) return;
                if (instanceBatch &&
                    instanceBatch->localTransforms.size() >
                        kWeightedOitMaximumInstanceCount) {
                    throw std::length_error(
                        "Render instance batch exceeds WeightedOIT capacity");
                }
                const uint32_t instanceCount = instanceBatch
                    ? static_cast<uint32_t>(
                        instanceBatch->localTransforms.size())
                    : 1u;
                if (instanceCount == 0u) return;
                ExtractionCounters& counters = out.counters;
                ++counters.modelRecords;
                counters.sourceIndexBytes += model.sourceIndexBytes;
                counters.arenaIndexBytes += model.arenaIndexBytes;
                counters.arenaSavedIndexBytes += model.arenaSavedIndexBytes;
                counters.uint16Indices += model.arenaUInt16IndexCount;
                counters.uint32Indices += model.arenaUInt32IndexCount;
                counters.lodFallbackChains += model.lodFallbackChainCount;
                counters.lodWithheldRanges +=
                    model.lodWithheldPrimitiveRangeCount;
                counters.lodWithheldIndexBytes +=
                    model.lodWithheldIndexBytes;
                counters.maximumLodResidentBase = (std::max)(
                    counters.maximumLodResidentBase,
                    model.lodResidentBaseLevel);
                if (instanceBatch && selected) {
                    throw std::logic_error(
                        "Render instance batch selection is not implemented");
                }
                // Chunk-local; the merge adds the chunk's offset.
                std::vector<glm::mat4>& instanceTransforms =
                    out.instanceTransforms;
                const uint32_t firstInstanceTransform = static_cast<uint32_t>(
                    instanceTransforms.size());
                if (instanceBatch) {
                    instanceTransforms.reserve(
                        instanceTransforms.size() + instanceCount);
                    const glm::mat4 identity(1.0f);
                    if (std::memcmp(&worldTransform, &identity,
                            sizeof(glm::mat4)) == 0) {
                        instanceTransforms.insert(
                            instanceTransforms.end(),
                            instanceBatch->localTransforms.begin(),
                            instanceBatch->localTransforms.end());
                    }
                    else {
                        const size_t first =
                            instanceTransforms.size();
                        instanceTransforms.resize(first +
                            instanceCount);
                        for (uint32_t index = 0u;
                            index < instanceCount; ++index) {
                            instanceTransforms[first + index] =
                                worldTransform *
                                instanceBatch->localTransforms[index];
                        }
                    }
                }
                counters.instances += instanceCount;
                const float distanceToCamera = glm::distance(
                    renderCameraPosition, glm::vec3(worldTransform[3]));
                // One submesh with its effective binding, after the
                // material-index check and the override lookup.
                const auto emitSubmesh = [&](size_t subMeshIndex,
                        const MaterialBinding* binding,
                        AssetGuid effectiveMaterialGuid,
                        CompiledTransparencyPolicy effectiveTransparency,
                        TransparencyExecutionMode effectiveExecutionMode,
                        bool previewPartSelected) {
                    const SubMesh& subMesh = model.subMeshes[subMeshIndex];
                    if (!binding->material.isValid() ||
                        !binding->pipeline.isValid()) {
                        return;
                    }
                    if (persistentOpaque &&
                        binding->renderQueue != RenderQueue::Transparent) {
                        return;
                    }
                    DrawPacket packet{};
                    packet.geometry = subMesh.geometry.isValid()
                        ? subMesh.geometry : model.geometry;
                    packet.material = binding->material;
                    packet.pipeline = binding->pipeline;
                    packet.opaqueSortKey = binding->opaqueSortKey;
                    packet.indexCount = subMesh.indexCount;
                    packet.firstIndex = subMesh.indexStart;
                    packet.worldTransform = worldTransform;
                    packet.distanceToCamera = distanceToCamera;
                    packet.isSelected = selected ? 1 : 0;
                    packet.owner = owner;
                    packet.sourcePrimitiveGuid =
                        subMesh.sourcePrimitiveGuid;
                    packet.primitiveGuid = subMesh.primitiveGuid;
                    packet.materialGuid = effectiveMaterialGuid;
                    packet.transparency = policy_.deterministicContent ? effectiveTransparency :
                        withLayeredInterfaceBudget(effectiveTransparency,
                            static_cast<unsigned>(view.layeredInterfaceOverride));
                    packet.transparencyExecutionMode =
                        effectiveExecutionMode;
                    packet.coverage = subMesh.coverage;
                    packet.firstInstanceTransform = instanceBatch
                        ? firstInstanceTransform : UINT32_MAX;
                    packet.instanceCount = instanceCount;
                    glm::vec3 packetBoundsMin = subMesh.boundsMin;
                    glm::vec3 packetBoundsMax = subMesh.boundsMax;
                    if (instanceBatch) {
                        if (subMeshIndex >=
                                instanceBatch->subMeshBounds.size() ||
                            !instanceBatch->subMeshBounds[subMeshIndex].valid) {
                            throw std::logic_error(
                                "Render instance batch bounds do not match model");
                        }
                        packetBoundsMin = instanceBatch->subMeshBounds[
                            subMeshIndex].minimum;
                        packetBoundsMax = instanceBatch->subMeshBounds[
                            subMeshIndex].maximum;
                    }
                    const glm::vec3 packetBoundsCenter =
                        (packetBoundsMin + packetBoundsMax) * 0.5f;
                    const float packetBoundsRadius = glm::length(
                        packetBoundsMax - packetBoundsCenter);
                    const ShadowCasterSphere shadowBounds =
                        transformShadowCasterSphere(
                            instanceBatch ? packetBoundsCenter :
                                subMesh.boundsSphereCenter,
                            instanceBatch ? packetBoundsRadius :
                                subMesh.boundsSphereRadius,
                            worldTransform);
                    packet.boundsSphereCenterWorld = shadowBounds.center;
                    packet.boundsSphereRadiusWorld = shadowBounds.radius;
                    if (binding->renderQueue == RenderQueue::Transparent) {
                        if (instanceBatch && !isWeightedOitPacket(packet)) {
                            throw std::logic_error(
                                "Render instance batches currently require "
                                "classified WeightedOIT materials");
                        }
                        const bool visible = prepareTransparentWorkInterval(
                            packet, packetBoundsMin, packetBoundsMax,
                            viewMatrix, renderCameraNearPlane,
                            renderCameraFarPlane);
                        if (!visible && effectiveExecutionMode ==
                                TransparencyExecutionMode::Classified) {
                            ++counters.transparentCulled;
                            return;
                        }
                        if (effectiveExecutionMode ==
                                TransparencyExecutionMode::Classified &&
                            (packet.transparency.resolvedClass ==
                                    TransparencyClass::SortedSurface ||
                                isWeightedOitPacket(packet))) {
                            out.queues[ExtractionSortedSurface].push_back(
                                packet);
                        }
                        else {
                            out.queues[ExtractionTransparent].push_back(packet);
                        }
                    }
                    else if (binding->renderQueue == RenderQueue::ForwardOpaque) {
                        if (instanceBatch) {
                            throw std::logic_error(
                                "Forward-opaque instance batches are not implemented");
                        }
                        out.queues[ExtractionForwardOpaque].push_back(packet);
                    }
                    else {
                        if (instanceBatch) {
                            throw std::logic_error(
                                "Opaque instance batches are not implemented");
                        }
                        out.queues[ExtractionOpaque].push_back(packet);
                    }
                    const bool previewHovered = assetPreviewActive &&
                        !view.previewIsolateSelectedPart && !view.previewHoveredPart.isNil() &&
                        view.previewHoveredPart == (view.previewHoveredPartIsMaterial
                            ? subMesh.materialGuid : subMesh.sourcePrimitiveGuid);
                    if (selected || previewHovered || (previewPartSelected && !view.previewIsolateSelectedPart)) {
                        packet.selectionFeedback = static_cast<uint8_t>(
                            ((selected || previewPartSelected) ? 1u : 0u) |
                            (previewHovered ? 2u : 0u));
                        out.selection.push_back(packet);
                    }
                };
                // M7R R5c.7: a GPU-scene owner without material overrides
                // draws only its model's transparent submeshes here; the
                // others reach only the two counters below and the
                // queue check in emitSubmesh. Visiting the cached list gives
                // the full visit's packets in the same order.
                if (transparentList && persistentOpaque && !forcedMaterial &&
                    !assetPreviewActive && meshComponent &&
                    meshComponent->materialOverrides.empty()) {
                    counters.submeshes +=
                        static_cast<uint64_t>(model.subMeshes.size()) *
                        instanceCount;
                    counters.sourceTriangles +=
                        transparentList->sourceTriangles * instanceCount;
                    for (const uint32_t subMeshIndex :
                            transparentList->submeshes) {
                        const SubMesh& subMesh = model.subMeshes[subMeshIndex];
                        emitSubmesh(subMeshIndex,
                            &model.materials[static_cast<size_t>(
                                subMesh.materialIndex)],
                            subMesh.materialGuid, subMesh.transparency,
                            model.transparencyExecutionMode, false);
                    }
                    return;
                }
                for (size_t subMeshIndex = 0u;
                    subMeshIndex < model.subMeshes.size(); ++subMeshIndex) {
                    const SubMesh& subMesh = model.subMeshes[subMeshIndex];
                    const bool previewPartSelected = assetPreviewActive &&
                        view.previewSelectedPart &&
                        *view.previewSelectedPart == (view.previewSelectedPartIsMaterial
                            ? subMesh.materialGuid : subMesh.sourcePrimitiveGuid);
                    if (assetPreviewActive &&
                        view.previewIsolateSelectedPart && !previewPartSelected) continue;
                    counters.submeshes += instanceCount;
                    counters.sourceTriangles +=
                        (static_cast<uint64_t>(subMesh.indexCount) / 3u) *
                        instanceCount;
                    const int materialIndex = subMesh.materialIndex;
                    if (materialIndex < 0 ||
                        static_cast<size_t>(materialIndex) >= model.materials.size()) {
                        continue;
                    }
                    const MaterialBinding* binding = forcedMaterial
                        ? &forcedMaterial->binding
                        : &model.materials[materialIndex];
                    std::optional<CookedMaterialRuntimeBinding>
                        overrideBinding;
                    AssetGuid effectiveMaterialGuid = subMesh.materialGuid;
                    CompiledTransparencyPolicy effectiveTransparency =
                        subMesh.transparency;
                    TransparencyExecutionMode effectiveExecutionMode =
                        model.transparencyExecutionMode;
                    if (forcedMaterial) {
                        effectiveMaterialGuid = forcedMaterial->materialGuid;
                        effectiveTransparency = forcedMaterial->transparency;
                        effectiveExecutionMode =
                            forcedMaterial->transparencyExecutionMode;
                    }
                    if (!forcedMaterial && meshComponent) {
                        const auto materialOverride = std::ranges::find_if(
                            meshComponent->materialOverrides,
                            [&subMesh](const MeshComponent::MaterialOverride& candidate) {
                                return candidate.sourceMaterialGuid ==
                                    subMesh.materialGuid;
                            });
                        if (materialOverride !=
                            meshComponent->materialOverrides.end()) {
                            overrideBinding =
                                assetManager_->findCookedMaterialRuntime(
                                    materialOverride->materialGuid);
                            if (overrideBinding) {
                                effectiveMaterialGuid =
                                    materialOverride->materialGuid;
                                binding = &overrideBinding->binding;
                                effectiveTransparency =
                                    overrideBinding->transparency;
                                effectiveExecutionMode = overrideBinding
                                    ->transparencyExecutionMode;
                            }
                        }
                    }
                    emitSubmesh(subMeshIndex, binding, effectiveMaterialGuid,
                        effectiveTransparency, effectiveExecutionMode,
                        previewPartSelected);
                }
            };

            const auto resetChunk = [](ExtractionChunk& chunk) {
                for (std::vector<DrawPacket>& queue : chunk.queues) queue.clear();
                chunk.selection.clear();
                chunk.instanceTransforms.clear();
                chunk.counters = {};
                chunk.failure = nullptr;
            };

            uint32_t extractionChunkCount = 0;
            uint32_t parityChunkCount = 0;
            if (assetPreviewActive) {
                if (extractionChunks_.empty()) extractionChunks_.resize(1);
                ExtractionChunk& chunk = extractionChunks_.front();
                resetChunk(chunk);
                if (previewModel) {
                    appendModel(chunk, *previewModel, glm::mat4(1.0f), nullptr,
                        nullptr,
                        nullptr, false,
                        {}, false, nullptr);
                }
                extractionChunkCount = 1;
            }
            else {
                auto* transformPool = registry.getPool<TransformComponent>();
                auto* meshPool = registry.getPool<MeshComponent>();
                auto* instanceBatchPool = registry.findPool<
                    RenderInstanceBatchComponent>();
                // Serial pre-pass in mesh-pool order: the entities the
                // former loop drew, their components and model lists, and
                // a visit estimate that balances the chunks.
                extractionItems_.clear();
                uint64_t totalVisits = 0;
                if (transformPool && meshPool) {
                    const ComponentPool<MeshComponent>& meshes =
                        std::as_const(*meshPool);
                    for (Entity entity : meshes.entities) {
                        const MeshComponent& meshComponent = meshes.get(entity);
                        if (!meshComponent.enabled || !meshComponent.model ||
                            !transformPool->has(entity)) {
                            continue;
                        }
                        const ModelAsset& model = *meshComponent.model;
                        const uint32_t list = transparentSubmeshes(model);
                        const size_t visits = meshComponent.materialOverrides.empty()
                            ? transparentSubmeshLists_[list].submeshes.size()
                            : model.subMeshes.size();
                        totalVisits += 1u + visits;
                        extractionItems_.push_back({
                            .entity = entity,
                            .mesh = &meshComponent,
                            .batch = instanceBatchPool &&
                                    instanceBatchPool->has(entity)
                                ? &std::as_const(*instanceBatchPool).get(entity)
                                : nullptr,
                            .transform = &std::as_const(*transformPool).get(entity),
                            .transparentList = list,
                            .visits = static_cast<uint32_t>(1u + visits),
                        });
                    }
                }
                const uint32_t chunkCount = chunkCountFor(totalVisits,
                    ExtractionVisitsPerChunk);
                if (extractionChunks_.size() < chunkCount)
                    extractionChunks_.resize(chunkCount);
                {
                    // Contiguous item ranges of about equal visits.
                    uint32_t chunk = 0;
                    uint64_t accumulated = 0;
                    extractionChunks_[0].firstItem = 0;
                    for (uint32_t item = 0; item < extractionItems_.size(); ++item) {
                        while (chunk + 1u < chunkCount && accumulated >=
                                totalVisits * (chunk + 1u) / chunkCount) {
                            extractionChunks_[chunk].endItem = item;
                            extractionChunks_[++chunk].firstItem = item;
                        }
                        accumulated += extractionItems_[item].visits;
                    }
                    const uint32_t itemCount =
                        static_cast<uint32_t>(extractionItems_.size());
                    extractionChunks_[chunk].endItem = itemCount;
                    while (++chunk < chunkCount) {
                        extractionChunks_[chunk].firstItem = itemCount;
                        extractionChunks_[chunk].endItem = itemCount;
                    }
                }
                const GpuScenePackedTables* const gpuSceneFrame = gpuSceneFrame_;
                runChunks(chunkCount, "cpu.render.extract.chunk",
                    [&](uint32_t chunkIndex) {
                    ExtractionChunk& chunk = extractionChunks_[chunkIndex];
                    resetChunk(chunk);
                    try {
                        for (uint32_t index = chunk.firstItem;
                                index < chunk.endItem; ++index) {
                            const ExtractionItem& item = extractionItems_[index];
                            const SceneEntityUuid owner = sceneWorld_.identities()
                                .persistentId(item.entity).value_or(
                                    SceneEntityUuid{});
                            bool persistentOpaque = false;
                            if (gpuSceneFrame && !owner.isNil()) {
                                const auto found = std::ranges::lower_bound(
                                    gpuSceneFrame->instanceIdentities, owner, {},
                                    &GpuSceneInstanceIdentity::owner);
                                persistentOpaque = found !=
                                    gpuSceneFrame->instanceIdentities.end() &&
                                    found->owner == owner;
                            }
                            appendModel(chunk, *item.mesh->model,
                                item.transform->worldMatrix,
                                item.mesh,
                                item.batch,
                                nullptr,
                                item.entity == selectedEntity, owner,
                                persistentOpaque,
                                persistentOpaque
                                    ? &transparentSubmeshLists_[item.transparentList]
                                    : nullptr);
                        }
                    }
                    catch (...) {
                        // The serial loop stopped at its first failure; the
                        // first failing chunk holds that one (rethrown below).
                        chunk.failure = std::current_exception();
                    }
                });
                for (uint32_t chunk = 0; chunk < chunkCount; ++chunk)
                    if (extractionChunks_[chunk].failure)
                        std::rethrow_exception(extractionChunks_[chunk].failure);
                extractionChunkCount = chunkCount;
                evictTransparentSubmeshLists();
            }

            // M7.2 parity stage, reduced by M7R R5c.4b/e: main-opaque work is
            // the published main-opaque list (see OpaqueSubmission); packets
            // are still built for visible forward-opaque primitives and, from
            // the instance selection flag, for the selected instance's visible
            // primitives. Transparent and explicit fallback owners above use
            // the M6 packet path. M7R R5c.7: primitive ranges in parallel,
            // appended in range order.
            if (!assetPreviewActive && gpuSceneFrame_) {
                CpuScope parityScope(cpuProfiler_, "cpu.render.extract.parity");
                const uint32_t primitiveCount = static_cast<uint32_t>(
                    gpuSceneFrame_->primitives.size());
                const uint32_t chunkCount = chunkCountFor(primitiveCount,
                    ParityPrimitivesPerChunk);
                if (parityChunks_.size() < chunkCount)
                    parityChunks_.resize(chunkCount);
                for (uint32_t chunk = 0; chunk < chunkCount; ++chunk) {
                    parityChunks_[chunk].firstPrimitive = static_cast<uint32_t>(
                        uint64_t{ primitiveCount } * chunk / chunkCount);
                    parityChunks_[chunk].endPrimitive = static_cast<uint32_t>(
                        uint64_t{ primitiveCount } * (chunk + 1u) / chunkCount);
                }
                runChunks(chunkCount, "cpu.render.extract.parity.chunk",
                    [&](uint32_t chunkIndex) {
                    ParityChunk& chunk = parityChunks_[chunkIndex];
                    chunk.forward.clear();
                    chunk.deferredCandidates = 0;
                    chunk.deferredCandidateTriangles = 0;
                    chunk.forwardVisible = 0;
                    chunk.forwardVisibleTriangles = 0;
                    chunk.failure = nullptr;
                    try {
                        for (uint32_t primitiveIndex = chunk.firstPrimitive;
                                primitiveIndex < chunk.endPrimitive;
                                ++primitiveIndex) {
                            const GpuScenePrimitiveRecord& primitive =
                                gpuSceneFrame_->primitives[primitiveIndex];
                            const bool cpuVisible = primitiveIndex <
                                    gpuSceneVisibility_.primitiveVisibility.size() &&
                                gpuSceneVisibility_.primitiveVisibility[primitiveIndex] != 0u;
                            const uint32_t instanceIndex = primitive.binding.x;
                            if (instanceIndex >= gpuSceneFrame_->instances.size() ||
                                instanceIndex >= gpuSceneFrame_->instanceIdentities.size() ||
                                primitive.binding.y >= gpuSceneFrame_->geometries.size() ||
                                primitiveIndex >= gpuSceneFrame_->primitiveIdentities.size()) {
                                throw std::logic_error(
                                    "Published visible GPU-scene references are invalid");
                            }
                            const GpuSceneInstanceRecord& instance =
                                gpuSceneFrame_->instances[instanceIndex];
                            if (instance.references.x >= gpuSceneFrame_->transforms.size()) {
                                throw std::logic_error(
                                    "Published visible GPU-scene transform is invalid");
                            }
                            const bool forward = (primitive.state.w &
                                GpuSceneConsumerForwardOpaque) != 0;
                            if (!forward && (primitive.state.w &
                                    GpuSceneConsumerMainOpaque) != 0) {
                                ++chunk.deferredCandidates;
                                chunk.deferredCandidateTriangles +=
                                    gpuSceneFrame_->geometries[primitive.binding.y].draw.y / 3u;
                            }
                            if (!forward || !cpuVisible) continue;
                            const DrawPacket packet = gpuSceneParityPacket(
                                primitiveIndex, cpuVisible);
                            chunk.forward.push_back(packet);
                            ++chunk.forwardVisible;
                            chunk.forwardVisibleTriangles += packet.indexCount / 3u;
                        }
                    }
                    catch (...) {
                        chunk.failure = std::current_exception();
                    }
                });
                for (uint32_t chunk = 0; chunk < chunkCount; ++chunk)
                    if (parityChunks_[chunk].failure)
                        std::rethrow_exception(parityChunks_[chunk].failure);
                for (uint32_t chunk = 0; chunk < chunkCount; ++chunk) {
                    const ParityChunk& part = parityChunks_[chunk];
                    gpuSceneDeferredCandidateCount += part.deferredCandidates;
                    gpuSceneDeferredCandidateTriangles +=
                        part.deferredCandidateTriangles;
                    gpuSceneForwardVisibleCount += part.forwardVisible;
                    gpuSceneForwardVisibleTriangles +=
                        part.forwardVisibleTriangles;
                }
                parityChunkCount = chunkCount;
            }

            // The chunks' packets, transforms and counters into the frame's
            // queues, in chunk order (forward-opaque parity packets after the
            // extraction chunks', as the serial stages appended them).
            mergeExtractionChunks(extractionChunkCount, parityChunkCount,
                extracted);

            if (!assetPreviewActive && gpuSceneFrame_) {
                // M7R R5c.4e: the selection outline from the instance flag:
                // the selected instances' visible primitives, in dense order
                // (as the parity loop appended them).
                for (uint32_t instanceIndex = 0;
                        instanceIndex < gpuSceneFrame_->instances.size();
                        ++instanceIndex) {
                    const GpuSceneInstanceRecord& instance =
                        gpuSceneFrame_->instances[instanceIndex];
                    if ((instance.state.z & GpuSceneInstanceSelected) == 0) continue;
                    for (uint32_t offset = 0; offset < instance.references.w;
                            ++offset) {
                        const uint32_t primitiveIndex =
                            instance.references.z + offset;
                        if (primitiveIndex >=
                                gpuSceneVisibility_.primitiveVisibility.size() ||
                            gpuSceneVisibility_.primitiveVisibility[
                                primitiveIndex] == 0u) continue;
                        selectionQueue.push_back(
                            gpuSceneParityPacket(primitiveIndex, true));
                    }
                }
            }
        }
        const uint64_t requestedModelRecords = extracted.modelRecords;
        const uint64_t requestedInstances = extracted.instances;
        const uint64_t requestedSubmeshes = extracted.submeshes;
        const uint64_t requestedSourceTriangles = extracted.sourceTriangles;
        const uint64_t requestedSourceIndexBytes = extracted.sourceIndexBytes;
        const uint64_t requestedArenaIndexBytes = extracted.arenaIndexBytes;
        const uint64_t requestedArenaSavedIndexBytes =
            extracted.arenaSavedIndexBytes;
        const uint64_t requestedUInt16Indices = extracted.uint16Indices;
        const uint64_t requestedUInt32Indices = extracted.uint32Indices;
        const uint64_t requestedLodFallbackChains = extracted.lodFallbackChains;
        const uint64_t requestedLodWithheldRanges = extracted.lodWithheldRanges;
        const uint64_t requestedLodWithheldIndexBytes =
            extracted.lodWithheldIndexBytes;
        const uint32_t maximumRequestedLodResidentBase =
            extracted.maximumLodResidentBase;
        const uint64_t transparentCulled = extracted.transparentCulled;

        // The main-opaque GPU-scene primitives of this view (ascending).
        const std::span<const uint32_t> gpuSceneOpaquePrimitives =
            !assetPreviewActive && gpuSceneFrame_
                ? std::span<const uint32_t>(
                    gpuSceneFrame_->mainOpaqueConsumerPrimitiveIndices)
                : std::span<const uint32_t>{};

        cpuProfiler_.recordCounter("draw.requested.opaque",
            opaqueQueue.size() + gpuSceneOpaquePrimitives.size());
        cpuProfiler_.recordCounter("draw.requested.forward_opaque",
            forwardOpaqueQueue.size());
        const uint64_t requestedTransparent =
            transparentQueue.size() + sortedSurfaceQueue.size();
        cpuProfiler_.recordCounter("draw.requested.transparent",
            requestedTransparent);
        cpuProfiler_.recordCounter("draw.requested.selection", selectionQueue.size());
        cpuProfiler_.recordCounter("geometry.model_record.requested",
            requestedModelRecords);
        cpuProfiler_.recordCounter("instance.requested", requestedInstances);
        cpuProfiler_.recordCounter("submesh.requested", requestedSubmeshes);
        cpuProfiler_.recordCounter("geometry.triangle.source_requested",
            requestedSourceTriangles);
        cpuProfiler_.recordCounter("geometry.index.source_requested_bytes",
            requestedSourceIndexBytes);
        cpuProfiler_.recordCounter("geometry.index.arena_requested_bytes",
            requestedArenaIndexBytes);
        cpuProfiler_.recordCounter("geometry.index.arena_saved_requested_bytes",
            requestedArenaSavedIndexBytes);
        cpuProfiler_.recordCounter("geometry.index.uint16_requested",
            requestedUInt16Indices);
        cpuProfiler_.recordCounter("geometry.index.uint32_requested",
            requestedUInt32Indices);
        cpuProfiler_.recordCounter("geometry.lod.resident_base_level.maximum_requested",
            maximumRequestedLodResidentBase);
        cpuProfiler_.recordCounter("geometry.lod.fallback_chains.requested",
            requestedLodFallbackChains);
        cpuProfiler_.recordCounter("geometry.lod.withheld_ranges.requested",
            requestedLodWithheldRanges);
        cpuProfiler_.recordCounter("geometry.lod.withheld_index_bytes.requested",
            requestedLodWithheldIndexBytes, ProfileCounterStatus::Exact,
            ProfileCounterUnit::Bytes);
        const uint64_t gpuSceneQueuedPrimitives =
            gpuSceneDeferredCandidateCount + gpuSceneForwardVisibleCount;
        const uint64_t totalQueuedPrimitives = opaqueQueue.size() +
            gpuSceneOpaquePrimitives.size() + forwardOpaqueQueue.size();
        const uint64_t directOpaquePrimitives = totalQueuedPrimitives >=
                gpuSceneQueuedPrimitives
            ? totalQueuedPrimitives - gpuSceneQueuedPrimitives : 0;
        const uint64_t visibleOpaquePrimitives = directOpaquePrimitives +
            gpuSceneVisibilityStats.visiblePrimitives;
        const uint64_t requestedOpaquePrimitives = directOpaquePrimitives +
            gpuSceneVisibilityStats.requestedPrimitives;
        const auto submittedTriangles = [](const auto& queue) noexcept {
            uint64_t triangles = 0;
            for (const DrawPacket& packet : queue) {
                triangles += (static_cast<uint64_t>(packet.indexCount) / 3u) *
                    packet.instanceCount;
            }
            return triangles;
        };
        const uint64_t totalQueuedTriangles =
            submittedTriangles(opaqueQueue) + gpuSceneDeferredCandidateTriangles +
            submittedTriangles(forwardOpaqueQueue);
        const uint64_t gpuSceneQueuedTriangles =
            gpuSceneDeferredCandidateTriangles +
            gpuSceneForwardVisibleTriangles;
        const uint64_t directOpaqueTriangles = totalQueuedTriangles >=
                gpuSceneQueuedTriangles
            ? totalQueuedTriangles - gpuSceneQueuedTriangles : 0;
        const uint64_t visibleOpaqueTriangles = directOpaqueTriangles +
            gpuSceneVisibilityStats.visibleTriangles;
        const uint64_t requestedOpaqueTriangles = directOpaqueTriangles +
            gpuSceneVisibilityStats.requestedTriangles;
        cpuProfiler_.recordCounter("opaque.primitive.requested",
            requestedOpaquePrimitives);
        cpuProfiler_.recordCounter("opaque.triangle.requested",
            requestedOpaqueTriangles);
        cpuProfiler_.recordCounter("opaque.primitive.visible",
            visibleOpaquePrimitives);
        cpuProfiler_.recordCounter("opaque.primitive.frustum_rejected",
            requestedOpaquePrimitives - visibleOpaquePrimitives);
        cpuProfiler_.recordCounter("gpu_scene.visibility.instance.requested",
            gpuSceneVisibilityStats.requestedInstances);
        cpuProfiler_.recordCounter("gpu_scene.visibility.instance.visible",
            gpuSceneVisibilityStats.visibleInstances);
        cpuProfiler_.recordCounter(
            "gpu_scene.visibility.instance.frustum_rejected",
            gpuSceneVisibilityStats.frustumRejectedInstances);
        cpuProfiler_.recordCounter("gpu_scene.visibility.instance.fail_visible",
            gpuSceneVisibilityStats.failVisibleInstances);
        cpuProfiler_.recordCounter("gpu_scene.visibility.primitive.fail_visible",
            gpuSceneVisibilityStats.failVisiblePrimitives);
        cpuProfiler_.recordCounter("opaque.primitive.occlusion_rejected", 0,
            ProfileCounterStatus::Unavailable);
        cpuProfiler_.recordCounter("opaque.triangle.visible",
            visibleOpaqueTriangles);
        const GpuScenePublisherStats gpuSceneStats = gpuScenePublisher_
            ? gpuScenePublisher_->stats() : GpuScenePublisherStats{};
        cpuProfiler_.recordCounter("gpu_scene.instance_upload_bytes",
            gpuSceneStats.changedInstanceBytes);
        cpuProfiler_.recordCounter("gpu_scene.transform_upload_bytes",
            gpuSceneStats.changedTransformBytes);
        cpuProfiler_.recordCounter("transparent.primitive.requested",
            requestedTransparent);
        const uint64_t sortedSurfacePackets = std::ranges::count_if(
            sortedSurfaceQueue, [](const DrawPacket& packet) {
                return packet.transparency.resolvedClass ==
                    TransparencyClass::SortedSurface;
            });
        const uint64_t weightedOitPackets = std::ranges::count_if(
            sortedSurfaceQueue, [](const DrawPacket& packet) {
                return isWeightedOitPacket(packet);
            });
        cpuProfiler_.recordCounter("transparent.class.sorted_surface",
            sortedSurfacePackets);
        cpuProfiler_.recordCounter("transparent.class.weighted_oit",
            weightedOitPackets);
        cpuProfiler_.recordCounter("transparent.class.compatibility_fallback",
            transparentQueue.size());
        uint64_t thinGlassPackets = 0;
        uint64_t layeredGlassPackets = 0;
        uint64_t zeroThicknessPackets = 0;
        uint64_t metricThicknessPackets = 0;
        const auto countTransportPackets = [&](const auto& queue) {
            for (const DrawPacket& packet : queue) {
                if (packet.transparency.resolvedClass ==
                        TransparencyClass::ThinGlass) {
                    ++thinGlassPackets;
                }
                else if (packet.transparency.resolvedClass ==
                        TransparencyClass::LayeredGlass) {
                    ++layeredGlassPackets;
                }
                else {
                    continue;
                }
                if (packet.transparency.thinSheetThicknessMeters > 0.0f)
                    ++metricThicknessPackets;
                else
                    ++zeroThicknessPackets;
            }
        };
        countTransportPackets(transparentQueue);
        countTransportPackets(sortedSurfaceQueue);
        cpuProfiler_.recordCounter("transparent.class.thin_glass",
            thinGlassPackets);
        cpuProfiler_.recordCounter("transparent.class.layered_glass",
            layeredGlassPackets);
        cpuProfiler_.recordCounter("transparent.transport.zero_sheet_thickness",
            zeroThicknessPackets);
        cpuProfiler_.recordCounter("transparent.transport.metric_sheet_thickness",
            metricThicknessPackets);
        cpuProfiler_.recordCounter("transparent.work.culled", transparentCulled);
        cpuProfiler_.recordCounter("light.scene",
            lightingFrame.stats.sceneLightCount);
        cpuProfiler_.recordCounter("light.active",
            lightingFrame.stats.activeLightCount);
        cpuProfiler_.recordCounter("light.omitted",
            lightingFrame.stats.omittedLightCount);
        cpuProfiler_.recordCounter("light.capacity",
            lightingFrame.stats.capacity);
        cpuProfiler_.recordCounter("light.changed_record_bytes",
            lightingFrame.stats.changedRecordBytes);
        cpuProfiler_.recordCounter("light.changed_record_ranges",
            lightingFrame.stats.changedRangeCount);
        cpuProfiler_.recordCounter("changed.transforms", inputs.changedTransforms);
        cpuProfiler_.recordCounter("changed.materials", 0,
            ProfileCounterStatus::Unavailable);
        cpuProfiler_.recordCounter("changed.lights",
            lightingFrame.stats.changedRecordCount);
        cpuProfiler_.recordCounter("changed.instances",
            gpuSceneStats.changedInstances);

        // --- 4. THE SORTING PHASE (CPU Cache Optimization) ---

        // Group opaque objects by the PSO/material identity carried by each binding.
        {
            CpuScope sortScope(cpuProfiler_, "cpu.render.sort.opaque");
            // M7R R5c.3: compact (key, index) sorts; M7R R5c.4b: the opaque
            // draw order over the direct packets and the main-opaque list.
            buildOpaqueOrder(gpuSceneOpaquePrimitives);
        }
        {
            CpuScope sortScope(cpuProfiler_, "cpu.render.sort.forward_opaque");
            sortOpaqueDrawPackets(forwardOpaqueQueue, drawSortScratch_);
        }

        shadowCasterQueue.reserve(
            opaqueQueue.size() + forwardOpaqueQueue.size());
        const auto appendDirectShadowFallbacks = [&](const auto& queue) {
            for (const DrawPacket& packet : queue) {
                if (!hasGpuScenePrimitive(packet))
                    shadowCasterQueue.push_back(packet);
            }
        };
        appendDirectShadowFallbacks(opaqueQueue);
        appendDirectShadowFallbacks(forwardOpaqueQueue);
        const bool directProbeReference =
            policy_.forceDirectGBufferReference ||
            policy_.forceDirectProbeCaptureReference;
        const auto appendDirectProbeFallbacks = [&](const auto& queue) {
            for (const DrawPacket& packet : queue) {
                if (directProbeReference || !hasGpuScenePrimitive(packet))
                    probeCasterQueue_.push_back(packet);
            }
        };
        if (directProbeReference) {
            // The direct reference routes capture every opaque packet the
            // parity queue held, in its draw order (M7R R5c.4b).
            for (const uint32_t entry : opaqueOrder_) {
                probeCasterQueue_.push_back(OpaqueSubmission::isDirect(entry)
                    ? opaqueQueue[OpaqueSubmission::indexOf(entry)]
                    : gpuSceneParityPacket(entry, gpuSceneVisibility_.
                        primitiveVisibility.size() > entry &&
                        gpuSceneVisibility_.primitiveVisibility[entry] != 0u));
            }
        }
        else appendDirectProbeFallbacks(opaqueQueue);
        appendDirectProbeFallbacks(forwardOpaqueQueue);
        const ShadowCasterSubmission shadowCasters{
            .gpuScenePrimitiveIndices = shadowGpuScenePrimitiveIndices,
            .directPackets = shadowCasterQueue,
            .membershipRevision = !assetPreviewActive && gpuSceneFrame_
                ? gpuSceneFrame_->shadowConsumerMembershipRevision : 0u,
        };
        cpuProfiler_.recordCounter("shadow.casters.gpu_scene",
            shadowGpuScenePrimitiveIndices.size());
        cpuProfiler_.recordCounter("shadow.casters.direct_fallback",
            shadowCasterQueue.size());
        cpuProfiler_.recordCounter("shadow.casters.submission_bytes",
            shadowGpuScenePrimitiveIndices.size() * sizeof(uint32_t) +
                shadowCasterQueue.size() * sizeof(DrawPacket),
            ProfileCounterStatus::Exact, ProfileCounterUnit::Bytes);
        const ReflectionProbeCasterSubmission probeCasters{
            .gpuScenePrimitiveIndices = probeGpuScenePrimitiveIndices,
            .directPackets = probeCasterQueue_,
            .membershipRevision = !assetPreviewActive && gpuSceneFrame_ &&
                !policy_.forceDirectGBufferReference &&
                !policy_.forceDirectProbeCaptureReference
                ? gpuSceneFrame_->probeConsumerMembershipRevision : 0u,
        };
        cpuProfiler_.recordCounter("probe.capture.casters.gpu_scene",
            probeGpuScenePrimitiveIndices.size());
        cpuProfiler_.recordCounter("probe.capture.casters.direct_fallback",
            probeCasterQueue_.size());
        cpuProfiler_.recordCounter("probe.capture.casters.submission_bytes",
            probeGpuScenePrimitiveIndices.size() * sizeof(uint32_t) +
                probeCasterQueue_.size() * sizeof(DrawPacket),
            ProfileCounterStatus::Exact, ProfileCounterUnit::Bytes);

        // Sort transparent objects Back-to-Front to ensure perfect alpha blending and refraction
        {
            CpuScope sortScope(cpuProfiler_, "cpu.render.sort.transparent");
            // M7R R5c.7: parallel above a threshold, same order (see
            // ParallelDrawSort.h); the sorted-surface interval count may run
            // on a worker until the intervals scope below joins it.
            transparentSorter_.sort(transparentQueue, sortedSurfaceQueue,
                drawSortScratch_);
        }
        cpuProfiler_.recordCounter("transparent.work.invalid_bounds",
            std::ranges::count_if(sortedSurfaceQueue,
                [](const DrawPacket& packet) {
                    return (packet.transparentWorkFlags &
                        TransparentWorkInvalidBoundsFallback) != 0;
                }));
        cpuProfiler_.recordCounter("transparent.work.near_clipped",
            std::ranges::count_if(sortedSurfaceQueue,
                [](const DrawPacket& packet) {
                    return (packet.transparentWorkFlags &
                        TransparentWorkNearClipped) != 0;
                }));
        {
            CpuScope intervalScope(cpuProfiler_, "cpu.render.transparent.intervals");
            cpuProfiler_.recordCounter("transparent.sort.ambiguous_intervals",
                transparentSorter_.ambiguousIntervals(sortedSurfaceQueue));
        }

        // --- 5. THE SUBMISSION PHASE (The Black Box) ---

        scheduleShadowsAndCaptures(shadowCasters, probeCasters,
            viewMatrix, renderCameraPosition, renderVerticalFovDegrees, aspect,
            renderCameraNearPlane, renderCameraFarPlane, assetPreviewActive,
            *inputs.environmentCookKey, inputs.applicationFrameIndex,
            renderFrame);

        // Keep scene-probe resources resident, but exclude their local influence
        // from the isolated preview. Tag the active-list identity across views.
        publishedProbes.activeListRevision = publishedProbes.activeListRevision * 2u + (assetPreviewActive ? 1u : 0u);
        if (assetPreviewActive) {
            publishedProbes.activeSlots = {};
            publishedProbes.stats.activeProbeCount = 0;
        }
        // Pass 1: Opaque G-Buffer
        bool isWireframe = inputs.forceWireframe || view.wireframe;
        const std::span<const DrawPacket> activeSelectionQueue =
            debugView == RenderDebugView::Final
            ? std::span<const DrawPacket>(selectionQueue.data(), selectionQueue.size())
            : std::span<const DrawPacket>{};
        // M9 G4: last frame's transform for every opaque direct and
        // forward-opaque packet.
        previousTransforms_.beginFrame();
        resolvePreviousTransforms(opaqueQueue, opaqueDirectPrevious_);
        resolvePreviousTransforms(forwardOpaqueQueue, forwardOpaquePrevious_);
        resolvePreviousTransforms(sortedSurfaceQueue, sortedSurfacePrevious_);
        resolvePreviousTransforms(transparentQueue, compatibilityPrevious_);
        previousTransforms_.endFrame();
        renderFrame.opaque = {
            .order = opaqueOrder_,
            .directPackets = opaqueQueue,
            .directPreviousTransforms = opaqueDirectPrevious_,
            .gpuScenePrimitiveCount =
                static_cast<uint32_t>(gpuSceneOpaquePrimitives.size()),
            .membershipRevision = !gpuSceneOpaquePrimitives.empty()
                ? gpuSceneFrame_->mainOpaqueConsumerMembershipRevision : 0u,
            .cpuVisibility = !gpuSceneOpaquePrimitives.empty()
                ? std::span<const uint8_t>(gpuSceneVisibility_.primitiveVisibility)
                : std::span<const uint8_t>{},
        };
        renderFrame.selectionQueue = activeSelectionQueue;
        renderFrame.wireframe = isWireframe;
        renderFrame.forwardOpaqueQueue = forwardOpaqueQueue;
        renderFrame.forwardOpaquePreviousTransforms = forwardOpaquePrevious_;
        renderFrame.sortedSurfaceQueue = sortedSurfaceQueue;
        renderFrame.compatibilityTransparentQueue = transparentQueue;
        renderFrame.sortedSurfacePreviousTransforms = sortedSurfacePrevious_;
        renderFrame.compatibilityPreviousTransforms = compatibilityPrevious_;
        renderFrame.instanceTransforms = forwardInstanceTransforms_;
        renderFrame.lights = &lightingFrame;
        renderFrame.reflectionProbes = &publishedProbes;
        renderFrame.stageObserver = inputs.stageObserver;
        return renderFrame_;
    }

    glm::mat4 RenderExtractor::previousWorldFor(const DrawPacket& packet) {
        // A GPU-scene primitive's previous transform is its instance's slot
        // 2d + 1 (settled the frame after it stops, M9 G3).
        if (hasGpuScenePrimitive(packet) && gpuSceneFrame_ != nullptr &&
            packet.firstInstanceTransform < gpuSceneFrame_->primitives.size()) {
            const GpuScenePackedTables& frame = *gpuSceneFrame_;
            const GpuScenePrimitiveRecord& primitive =
                frame.primitives[packet.firstInstanceTransform];
            if (primitive.binding.x < frame.instances.size()) {
                const uint32_t previous = frame.instances[primitive.binding.x].references.y;
                if (previous < frame.transforms.size())
                    return unpackGpuSceneAffine(frame.transforms[previous]);
            }
        }
        return previousTransforms_.resolve({ packet.owner, packet.primitiveGuid },
            packet.worldTransform);
    }

    void RenderExtractor::resolvePreviousTransforms(
        std::span<const DrawPacket> packets, std::vector<glm::mat4>& previous) {
        previous.resize(packets.size());
        for (size_t index = 0; index < packets.size(); ++index)
            previous[index] = previousWorldFor(packets[index]);
    }

    DrawPacket RenderExtractor::gpuSceneParityPacket(uint32_t primitiveIndex,
        bool cpuVisible) const {
        // The M7.2 parity packet of one published primitive (references were
        // validated by the parity stage).
        const GpuScenePackedTables& frame = *gpuSceneFrame_;
        const GpuScenePrimitiveRecord& primitive = frame.primitives[primitiveIndex];
        const GpuSceneInstanceRecord& instance = frame.instances[primitive.binding.x];
        const glm::mat4 worldTransform = unpackGpuSceneAffine(
            frame.transforms[instance.references.x]);
        const GpuSceneGeometryRecord& geometry = frame.geometries[primitive.binding.y];
        DrawPacket packet{};
        packet.geometry = GeometryHandle{ geometry.storage.x };
        packet.material = MaterialHandle{ primitive.binding.z };
        packet.pipeline = PipelineHandle{ primitive.binding.w };
        packet.opaqueSortKey =
            (static_cast<uint64_t>(primitive.binding.w) << 32u) |
            primitive.binding.z;
        packet.indexCount = geometry.draw.y;
        packet.firstIndex = geometry.draw.x;
        packet.executionFlags = DrawPacketGpuScenePrimitive;
        if (cpuVisible) packet.executionFlags |= DrawPacketCpuVisibilityOracle;
        packet.firstInstanceTransform = primitiveIndex;
        packet.worldTransform = worldTransform;
        packet.distanceToCamera = glm::distance(
            renderCameraPosition_, glm::vec3(worldTransform[3]));
        packet.isSelected = (instance.state.z & GpuSceneInstanceSelected) != 0
            ? 1 : 0;
        packet.owner = frame.instanceIdentities[primitive.binding.x].owner;
        const GpuScenePrimitiveIdentity& identity =
            frame.primitiveIdentities[primitiveIndex];
        packet.sourcePrimitiveGuid = identity.sourcePrimitiveGuid;
        packet.primitiveGuid = identity.primitiveGuid;
        packet.materialGuid = identity.effectiveMaterialGuid;
        packet.coverage = (primitive.state.y & GpuScenePrimitiveAlphaMask) != 0
            ? static_cast<uint8_t>(ModelCoverage::Masked)
            : static_cast<uint8_t>(ModelCoverage::Opaque);
        packet.boundsSphereCenterWorld = {
            instance.worldBoundsSphere.x,
            instance.worldBoundsSphere.y,
            instance.worldBoundsSphere.z,
        };
        packet.boundsSphereRadiusWorld = instance.worldBoundsSphere.w;
        return packet;
    }

    void RenderExtractor::buildOpaqueOrder(
        std::span<const uint32_t> gpuScenePrimitives) {
        // The parity queue held the direct packets (append order) followed by
        // one packet per main-opaque primitive (ascending) and was sorted with
        // the opaque comparator. The same std::sort over keys in that input
        // order gives the same permutation (CompactDrawSort.h).
        opaqueOrder_.clear();
        if (gpuScenePrimitives.empty()) {
            sortOpaqueDrawPackets(opaqueQueue, drawSortScratch_);
            for (uint32_t index = 0; index < opaqueQueue.size(); ++index)
                opaqueOrder_.push_back(OpaqueSubmissionDirectBit | index);
            return;
        }
        const GpuScenePackedTables& frame = *gpuSceneFrame_;
        const auto gpuKey = [&frame](uint32_t primitiveIndex) {
            const GpuScenePrimitiveRecord& primitive =
                frame.primitives[primitiveIndex];
            const GpuSceneGeometryRecord& geometry =
                frame.geometries[primitive.binding.y];
            return OpaqueDrawSortKey{
                .opaqueSortKey =
                    (static_cast<uint64_t>(primitive.binding.w) << 32u) |
                    primitive.binding.z,
                .geometry = GeometryHandle{ geometry.storage.x },
                .firstIndex = geometry.draw.x,
                .packet = primitiveIndex,
            };
        };
        if (opaqueQueue.empty()) {
            // Only GPU-scene work: the order is a function of the main-opaque
            // membership inputs, so it is rebuilt only when they change.
            const uint64_t revision = frame.mainOpaqueConsumerMembershipRevision;
            if (revision == 0u || revision != gpuSceneOpaqueOrderRevision_) {
                auto& keys = drawSortScratch_.opaqueKeys;
                keys.resize(gpuScenePrimitives.size());
                for (size_t index = 0; index < gpuScenePrimitives.size(); ++index)
                    keys[index] = gpuKey(gpuScenePrimitives[index]);
                sortOpaqueDrawKeys(keys);
                gpuSceneOpaqueOrder_.resize(keys.size());
                for (size_t index = 0; index < keys.size(); ++index)
                    gpuSceneOpaqueOrder_[index] = keys[index].packet;
                gpuSceneOpaqueOrderRevision_ = revision;
            }
            opaqueOrder_.assign(gpuSceneOpaqueOrder_.begin(),
                gpuSceneOpaqueOrder_.end());
            return;
        }
        // Mixed: one sort over both, then the direct packets are permuted
        // into their order within it.
        auto& keys = drawSortScratch_.opaqueKeys;
        keys.resize(opaqueQueue.size() + gpuScenePrimitives.size());
        for (uint32_t index = 0; index < opaqueQueue.size(); ++index) {
            const DrawPacket& packet = opaqueQueue[index];
            keys[index] = {
                .opaqueSortKey = packet.opaqueSortKey,
                .geometry = packet.geometry,
                .firstIndex = packet.firstIndex,
                .packet = OpaqueSubmissionDirectBit | index,
            };
        }
        for (size_t index = 0; index < gpuScenePrimitives.size(); ++index)
            keys[opaqueQueue.size() + index] = gpuKey(gpuScenePrimitives[index]);
        sortOpaqueDrawKeys(keys);
        opaqueDirectScratch_.clear();
        for (const OpaqueDrawSortKey& key : keys) {
            if (OpaqueSubmission::isDirect(key.packet)) {
                opaqueOrder_.push_back(OpaqueSubmissionDirectBit |
                    static_cast<uint32_t>(opaqueDirectScratch_.size()));
                opaqueDirectScratch_.push_back(
                    opaqueQueue[OpaqueSubmission::indexOf(key.packet)]);
            }
            else opaqueOrder_.push_back(key.packet);
        }
        opaqueQueue.swap(opaqueDirectScratch_);
    }

    template <class Fn>
    void RenderExtractor::runChunks(uint32_t count, const char* scope, Fn&& fn) {
        if (count == 0) return;
        if (count == 1 || !tasks_) {
            for (uint32_t chunk = 0; chunk < count; ++chunk) fn(chunk);
            return;
        }
        // Frame-critical: the main thread joins it (and runs chunks itself);
        // each chunk writes only its own scratch, so the schedule does not
        // affect the output.
        tasks_->parallelFor(Tasks::TaskPriority::FrameCritical, count, 1,
            [&fn](Tasks::TaskRange range, uint32_t) {
                for (uint32_t chunk = range.begin; chunk < range.end; ++chunk)
                    fn(chunk);
            }, scope);
    }

    uint32_t RenderExtractor::chunkCountFor(uint64_t work,
        uint64_t unitsPerChunk) const noexcept {
        if (!tasks_ || work <= unitsPerChunk) return 1;
        return static_cast<uint32_t>((std::min<uint64_t>)(MaximumChunks,
            (work + unitsPerChunk - 1u) / unitsPerChunk));
    }

    void RenderExtractor::classifyMainView(const glm::mat4& clipFromWorld) {
        const GpuScenePackedTables& scene = *gpuSceneFrame_;
        const GpuSceneFrustum frustum = makeGpuSceneFrustum(clipFromWorld);
        constexpr uint32_t consumerMask =
            GpuSceneConsumerMainOpaque | GpuSceneConsumerForwardOpaque;
        const uint32_t chunkCount = chunkCountFor(scene.primitives.size(),
            ClassifyPrimitivesPerChunk);
        if (chunkCount <= 1) {
            classifyGpuSceneFrustum(scene, frustum, consumerMask,
                gpuSceneVisibility_);
            return;
        }
        // M7R R5c.7: instance ranges in parallel. Each range sets the
        // visibility bytes of its own instances' primitives and appends to
        // its own lists; merging the lists and statistics in range order
        // gives classifyGpuSceneFrustum's result.
        beginGpuSceneVisibility(scene, gpuSceneVisibility_);
        if (classifyChunks_.size() < chunkCount)
            classifyChunks_.resize(chunkCount);
        const uint64_t instanceCount = scene.instances.size();
        for (uint32_t chunk = 0; chunk < chunkCount; ++chunk) {
            classifyChunks_[chunk].firstInstance = static_cast<uint32_t>(
                instanceCount * chunk / chunkCount);
            classifyChunks_[chunk].endInstance = static_cast<uint32_t>(
                instanceCount * (chunk + 1u) / chunkCount);
        }
        const std::span<uint8_t> visibility =
            gpuSceneVisibility_.primitiveVisibility;
        runChunks(chunkCount, "cpu.render.classify.chunk",
            [&](uint32_t chunkIndex) {
            ClassifyChunk& chunk = classifyChunks_[chunkIndex];
            chunk.part.visibleInstanceIndices.clear();
            chunk.part.visiblePrimitiveIndices.clear();
            chunk.part.stats = {};
            chunk.failure = nullptr;
            try {
                classifyGpuSceneFrustumRange(scene, frustum, consumerMask,
                    chunk.firstInstance, chunk.endInstance, visibility,
                    chunk.part);
            }
            catch (...) {
                chunk.failure = std::current_exception();
            }
        });
        for (uint32_t chunk = 0; chunk < chunkCount; ++chunk)
            if (classifyChunks_[chunk].failure)
                std::rethrow_exception(classifyChunks_[chunk].failure);
        for (uint32_t chunk = 0; chunk < chunkCount; ++chunk)
            mergeGpuSceneVisibility(classifyChunks_[chunk].part,
                gpuSceneVisibility_);
    }

    void RenderExtractor::mergeExtractionChunks(uint32_t extractionChunkCount,
        uint32_t parityChunkCount, ExtractionCounters& extracted) {
        // Offsets: every destination range is a prefix sum in chunk order.
        std::array<size_t, ExtractionQueueCount> totals{};
        size_t selectionTotal = 0;
        size_t transformTotal = 0;
        for (uint32_t index = 0; index < extractionChunkCount; ++index) {
            ExtractionChunk& chunk = extractionChunks_[index];
            for (uint32_t queue = 0; queue < ExtractionQueueCount; ++queue) {
                chunk.queueOffsets[queue] = totals[queue];
                totals[queue] += chunk.queues[queue].size();
            }
            chunk.selectionOffset = selectionTotal;
            selectionTotal += chunk.selection.size();
            chunk.transformOffset = transformTotal;
            transformTotal += chunk.instanceTransforms.size();
            const ExtractionCounters& counters = chunk.counters;
            extracted.modelRecords += counters.modelRecords;
            extracted.instances += counters.instances;
            extracted.submeshes += counters.submeshes;
            extracted.sourceTriangles += counters.sourceTriangles;
            extracted.sourceIndexBytes += counters.sourceIndexBytes;
            extracted.arenaIndexBytes += counters.arenaIndexBytes;
            extracted.arenaSavedIndexBytes += counters.arenaSavedIndexBytes;
            extracted.uint16Indices += counters.uint16Indices;
            extracted.uint32Indices += counters.uint32Indices;
            extracted.lodFallbackChains += counters.lodFallbackChains;
            extracted.lodWithheldRanges += counters.lodWithheldRanges;
            extracted.lodWithheldIndexBytes += counters.lodWithheldIndexBytes;
            extracted.transparentCulled += counters.transparentCulled;
            extracted.maximumLodResidentBase = (std::max)(
                extracted.maximumLodResidentBase,
                counters.maximumLodResidentBase);
        }
        for (uint32_t index = 0; index < parityChunkCount; ++index) {
            ParityChunk& chunk = parityChunks_[index];
            chunk.forwardOffset = totals[ExtractionForwardOpaque];
            totals[ExtractionForwardOpaque] += chunk.forward.size();
        }
        if (transformTotal > UINT32_MAX)
            throw std::length_error(
                "Instance transform stream exceeds 32-bit indices");
        // A single extraction chunk (every small frame) hands its storage to
        // the queues instead of being copied: its packets are their prefix
        // at offset 0. The two buffers alternate between frames and keep
        // their capacity.
        const bool swapSingleChunk = extractionChunkCount == 1;
        size_t swappedPackets = 0;
        if (swapSingleChunk) {
            ExtractionChunk& chunk = extractionChunks_.front();
            for (const std::vector<DrawPacket>& queue : chunk.queues)
                swappedPackets += queue.size();
            swappedPackets += chunk.selection.size();
            opaqueQueue.swap(chunk.queues[ExtractionOpaque]);
            forwardOpaqueQueue.swap(chunk.queues[ExtractionForwardOpaque]);
            transparentQueue.swap(chunk.queues[ExtractionTransparent]);
            sortedSurfaceQueue.swap(chunk.queues[ExtractionSortedSurface]);
            selectionQueue.swap(chunk.selection);
            forwardInstanceTransforms_.swap(chunk.instanceTransforms);
        }
        // Exact sizes. A steady frame has the previous frame's sizes, so
        // these construct nothing; every element is overwritten below.
        opaqueQueue.resize(totals[ExtractionOpaque]);
        forwardOpaqueQueue.resize(totals[ExtractionForwardOpaque]);
        transparentQueue.resize(totals[ExtractionTransparent]);
        sortedSurfaceQueue.resize(totals[ExtractionSortedSurface]);
        selectionQueue.resize(selectionTotal);
        forwardInstanceTransforms_.resize(transformTotal);
        const std::array<std::vector<DrawPacket>*, ExtractionQueueCount>
            destinations{ &opaqueQueue, &forwardOpaqueQueue, &transparentQueue,
                &sortedSurfaceQueue };
        // One copy task per chunk; each writes only its own ranges. A small
        // frame copies inline (a task round trip would cost more).
        const size_t copiedPackets = totals[ExtractionOpaque] +
            totals[ExtractionForwardOpaque] + totals[ExtractionTransparent] +
            totals[ExtractionSortedSurface] + selectionTotal -
            swappedPackets;
        const uint32_t copyTasks = extractionChunkCount + parityChunkCount;
        runChunks(copiedPackets >= MergeParallelMinimumPackets ? copyTasks : 1u,
            "cpu.render.extract.merge.chunk", [&](uint32_t task) {
            const auto copyItem = [&](uint32_t index) {
                if (index >= extractionChunkCount) {
                    const ParityChunk& chunk =
                        parityChunks_[index - extractionChunkCount];
                    std::copy(chunk.forward.begin(), chunk.forward.end(),
                        forwardOpaqueQueue.data() + chunk.forwardOffset);
                    return;
                }
                if (swapSingleChunk) return;   // already in place
                const ExtractionChunk& chunk = extractionChunks_[index];
                // Instance-batch packets index the chunk's transforms; rebase them
                // onto the frame's stream (non-batch packets hold UINT32_MAX).
                const uint32_t rebase = static_cast<uint32_t>(chunk.transformOffset);
                const auto copy = [rebase](const std::vector<DrawPacket>& source,
                    std::vector<DrawPacket>& destination, size_t offset) {
                    DrawPacket* const target = destination.data() + offset;
                    std::copy(source.begin(), source.end(), target);
                    if (rebase == 0u) return;
                    for (size_t packet = 0; packet < source.size(); ++packet)
                        if (target[packet].firstInstanceTransform != UINT32_MAX)
                            target[packet].firstInstanceTransform += rebase;
                };
                for (uint32_t queue = 0; queue < ExtractionQueueCount; ++queue)
                    copy(chunk.queues[queue], *destinations[queue],
                        chunk.queueOffsets[queue]);
                copy(chunk.selection, selectionQueue, chunk.selectionOffset);
                std::copy(chunk.instanceTransforms.begin(),
                    chunk.instanceTransforms.end(),
                    forwardInstanceTransforms_.data() + chunk.transformOffset);
            };
            if (copiedPackets >= MergeParallelMinimumPackets) {
                copyItem(task);
                return;
            }
            for (uint32_t index = 0; index < copyTasks; ++index) copyItem(index);
        });
    }

    uint32_t RenderExtractor::transparentSubmeshes(const ModelAsset& model) {
        // Consecutive mesh entities usually share a model.
        if (lastTransparentSubmeshModel_ == &model)
            return lastTransparentSubmeshList_;
        uint32_t listIndex = 0;
        const auto found = transparentSubmeshListIndex_.find(&model);
        if (found != transparentSubmeshListIndex_.end()) {
            listIndex = found->second;
        }
        else {
            listIndex = static_cast<uint32_t>(transparentSubmeshLists_.size());
            transparentSubmeshListIndex_.emplace(&model, listIndex);
            transparentSubmeshLists_.emplace_back().model = &model;
        }
        TransparentSubmeshList* const list =
            &transparentSubmeshLists_[listIndex];
        if (list->usedFrame != extractionFrame_) {
            // Exact check of every input the list is a function of; a model
            // reallocated at the same address is caught the same way.
            const auto drawsTransparent = [](const MaterialBinding& binding) {
                return binding.material.isValid() &&
                    binding.pipeline.isValid() &&
                    binding.renderQueue == RenderQueue::Transparent;
            };
            bool unchanged = list->usedFrame != 0 &&
                list->materialIndices.size() == model.subMeshes.size() &&
                list->materialDrawsTransparent.size() == model.materials.size();
            for (size_t index = 0; unchanged && index < model.materials.size();
                    ++index) {
                unchanged = list->materialDrawsTransparent[index] ==
                    (drawsTransparent(model.materials[index]) ? 1u : 0u);
            }
            for (size_t index = 0; unchanged && index < model.subMeshes.size();
                    ++index) {
                unchanged = list->materialIndices[index] ==
                        model.subMeshes[index].materialIndex &&
                    list->indexCounts[index] == model.subMeshes[index].indexCount;
            }
            if (!unchanged) {
                list->materialDrawsTransparent.resize(model.materials.size());
                for (size_t index = 0; index < model.materials.size(); ++index)
                    list->materialDrawsTransparent[index] =
                        drawsTransparent(model.materials[index]) ? 1u : 0u;
                list->materialIndices.resize(model.subMeshes.size());
                list->indexCounts.resize(model.subMeshes.size());
                list->submeshes.clear();
                list->sourceTriangles = 0;
                for (size_t index = 0; index < model.subMeshes.size(); ++index) {
                    const SubMesh& subMesh = model.subMeshes[index];
                    list->materialIndices[index] = subMesh.materialIndex;
                    list->indexCounts[index] = subMesh.indexCount;
                    list->sourceTriangles +=
                        static_cast<uint64_t>(subMesh.indexCount) / 3u;
                    if (subMesh.materialIndex >= 0 &&
                        static_cast<size_t>(subMesh.materialIndex) <
                            model.materials.size() &&
                        list->materialDrawsTransparent[static_cast<size_t>(
                            subMesh.materialIndex)] != 0u)
                        list->submeshes.push_back(static_cast<uint32_t>(index));
                }
            }
            list->usedFrame = extractionFrame_;
        }
        lastTransparentSubmeshModel_ = &model;
        lastTransparentSubmeshList_ = listIndex;
        return listIndex;
    }

    void RenderExtractor::evictTransparentSubmeshLists() {
        // Lists unused for a while are dropped (models that left the scene);
        // a short absence keeps the list and its storage.
        constexpr uint64_t RetainedFrames = 256;
        lastTransparentSubmeshModel_ = nullptr;
        lastTransparentSubmeshList_ = UINT32_MAX;
        for (size_t index = 0; index < transparentSubmeshLists_.size();) {
            TransparentSubmeshList& list = transparentSubmeshLists_[index];
            if (list.usedFrame + RetainedFrames >= extractionFrame_) {
                ++index;
                continue;
            }
            transparentSubmeshListIndex_.erase(list.model);
            if (index + 1 != transparentSubmeshLists_.size()) {
                list = std::move(transparentSubmeshLists_.back());
                transparentSubmeshListIndex_[list.model] =
                    static_cast<uint32_t>(index);
            }
            transparentSubmeshLists_.pop_back();
        }
    }

    void RenderExtractor::releaseFrame() {
        frameStage_ = {};
        view_ = nullptr;
        renderFrame_ = {};
        lightingFrame_ = {};
        // extractedProbes_ keeps its storage: the next extraction rewrites it in
        // place (M7R R5c.6).
        publishedProbes_ = {};
        // M7R R5c.8: the frame's shadow packets are released by size only.
        // They keep their capacity, so the next frame's schedule refills them
        // without allocating. (Until R5c.8 they were swapped with empty
        // vectors, which freed them as the former drawFrame locals did; R5a
        // kept that so its allocation counters stayed identical. The packets
        // are still empty after releaseFrame, and renderFrame_ above drops
        // the spans that referred to them.)
        directionalShadows_.clear();
        spotShadows_.clear();
        pointShadows_.clear();
    }

} // namespace Iridium
