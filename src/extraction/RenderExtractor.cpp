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
#include <iostream>
#include <string>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

#include "assets/AssetManager.h"
#include "core/BuildFeatures.h"
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
        const ProjectReflectionProbeSettings& probeSettings)
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
                  maximumCompatiblePointStaleFrames }) {}

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

    void RenderExtractor::clearQueues() {
        opaqueQueue.clear();
        forwardOpaqueQueue.clear();
        transparentQueue.clear();
        sortedSurfaceQueue.clear();
        selectionQueue.clear();
        shadowCasterQueue.clear();
        probeCasterQueue_.clear();
        forwardInstanceTransforms_.clear();
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
        const ViewTransportRecord viewTransport = makeViewTransportRecord(
            viewMatrix_, projMatrix_, renderCameraPosition_,
            renderCameraNearPlane_, renderCameraFarPlane_,
            { inputs.renderExtent.width, inputs.renderExtent.height });
        const RenderDebugView debugView = view.debugView;
        renderBackend->setEnvironmentLightingSettings(assetPreviewActive
            ? view.previewEnvironmentSettings : inputs.sceneEnvironmentSettings);
        const float viewExposure = assetPreviewActive ? view.previewExposureEv : inputs.manualExposureEv;
        // M7R R3c.11: the frame is assembled from spans over this frame's
        // queues and packets and submitted once, after extraction.
        renderFrame_ = RenderFrame{
            .view = viewTransport,
            .history = {
                .identity = assetPreviewActive ? view.previewSessionSerial + 2u : 1u,
                .resetRevision = assetPreviewActive ? view.previewFramingRevision :
                    inputs.viewHistoryResetRevision.value_or(0u),
            },
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

        // --- 3. THE EXTRACTION PHASE (Data-Oriented Design) ---
        uint64_t requestedModelRecords = 0;
        uint64_t requestedInstances = 0;
        uint64_t requestedSubmeshes = 0;
        uint64_t requestedSourceTriangles = 0;
        uint64_t requestedSourceIndexBytes = 0;
        uint64_t requestedArenaIndexBytes = 0;
        uint64_t requestedArenaSavedIndexBytes = 0;
        uint64_t requestedUInt16Indices = 0;
        uint64_t requestedUInt32Indices = 0;
        uint64_t requestedLodFallbackChains = 0;
        uint64_t requestedLodWithheldRanges = 0;
        uint64_t requestedLodWithheldIndexBytes = 0;
        uint32_t maximumRequestedLodResidentBase = 0;
        GpuSceneVisibilityStats gpuSceneVisibilityStats{};
        uint64_t gpuSceneDeferredCandidateCount = 0;
        uint64_t gpuSceneDeferredCandidateTriangles = 0;
        uint64_t gpuSceneForwardVisibleCount = 0;
        uint64_t gpuSceneForwardVisibleTriangles = 0;
        if (!assetPreviewActive && gpuSceneFrame_) {
            CpuScope classifyScope(cpuProfiler_, "cpu.render.classify");
            classifyGpuSceneFrustum(*gpuSceneFrame_,
                makeGpuSceneFrustum(projMatrix * viewMatrix),
                GpuSceneConsumerMainOpaque |
                    GpuSceneConsumerForwardOpaque,
                gpuSceneVisibility_);
            gpuSceneVisibilityStats = gpuSceneVisibility_.stats;
        }
        uint64_t transparentCulled = 0;
        {
            CpuScope extractionScope(cpuProfiler_, "cpu.render.extract");
            const auto appendModel = [&](const ModelAsset& model,
                    const glm::mat4& worldTransform,
                    const MeshComponent* meshComponent,
                    const RenderInstanceBatchComponent* instanceBatch,
                    const CookedMaterialRuntimeBinding* forcedMaterial,
                    bool selected, SceneEntityUuid owner,
                    bool persistentOpaque) {
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
                ++requestedModelRecords;
                requestedSourceIndexBytes += model.sourceIndexBytes;
                requestedArenaIndexBytes += model.arenaIndexBytes;
                requestedArenaSavedIndexBytes += model.arenaSavedIndexBytes;
                requestedUInt16Indices += model.arenaUInt16IndexCount;
                requestedUInt32Indices += model.arenaUInt32IndexCount;
                requestedLodFallbackChains += model.lodFallbackChainCount;
                requestedLodWithheldRanges +=
                    model.lodWithheldPrimitiveRangeCount;
                requestedLodWithheldIndexBytes +=
                    model.lodWithheldIndexBytes;
                maximumRequestedLodResidentBase = (std::max)(
                    maximumRequestedLodResidentBase,
                    model.lodResidentBaseLevel);
                if (instanceBatch && selected) {
                    throw std::logic_error(
                        "Render instance batch selection is not implemented");
                }
                const uint32_t firstInstanceTransform = static_cast<uint32_t>(
                    forwardInstanceTransforms_.size());
                if (instanceBatch) {
                    forwardInstanceTransforms_.reserve(
                        forwardInstanceTransforms_.size() + instanceCount);
                    const glm::mat4 identity(1.0f);
                    if (std::memcmp(&worldTransform, &identity,
                            sizeof(glm::mat4)) == 0) {
                        forwardInstanceTransforms_.insert(
                            forwardInstanceTransforms_.end(),
                            instanceBatch->localTransforms.begin(),
                            instanceBatch->localTransforms.end());
                    }
                    else {
                        const size_t first =
                            forwardInstanceTransforms_.size();
                        forwardInstanceTransforms_.resize(first +
                            instanceCount);
                        for (uint32_t index = 0u;
                            index < instanceCount; ++index) {
                            forwardInstanceTransforms_[first + index] =
                                worldTransform *
                                instanceBatch->localTransforms[index];
                        }
                    }
                }
                requestedInstances += instanceCount;
                const float distanceToCamera = glm::distance(
                    renderCameraPosition, glm::vec3(worldTransform[3]));
                for (size_t subMeshIndex = 0u;
                    subMeshIndex < model.subMeshes.size(); ++subMeshIndex) {
                    const SubMesh& subMesh = model.subMeshes[subMeshIndex];
                    const bool previewPartSelected = assetPreviewActive &&
                        view.previewSelectedPart &&
                        *view.previewSelectedPart == (view.previewSelectedPartIsMaterial
                            ? subMesh.materialGuid : subMesh.sourcePrimitiveGuid);
                    if (assetPreviewActive &&
                        view.previewIsolateSelectedPart && !previewPartSelected) continue;
                    requestedSubmeshes += instanceCount;
                    requestedSourceTriangles +=
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
                    if (!binding->material.isValid() ||
                        !binding->pipeline.isValid()) {
                        continue;
                    }
                    if (persistentOpaque &&
                        binding->renderQueue != RenderQueue::Transparent) {
                        continue;
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
                            ++transparentCulled;
                            continue;
                        }
                        if (effectiveExecutionMode ==
                                TransparencyExecutionMode::Classified &&
                            (packet.transparency.resolvedClass ==
                                    TransparencyClass::SortedSurface ||
                                isWeightedOitPacket(packet))) {
                            sortedSurfaceQueue.push_back(packet);
                        }
                        else {
                            transparentQueue.push_back(packet);
                        }
                    }
                    else if (binding->renderQueue == RenderQueue::ForwardOpaque) {
                        if (instanceBatch) {
                            throw std::logic_error(
                                "Forward-opaque instance batches are not implemented");
                        }
                        forwardOpaqueQueue.push_back(packet);
                    }
                    else {
                        if (instanceBatch) {
                            throw std::logic_error(
                                "Opaque instance batches are not implemented");
                        }
                        opaqueQueue.push_back(packet);
                    }
                    const bool previewHovered = assetPreviewActive &&
                        !view.previewIsolateSelectedPart && !view.previewHoveredPart.isNil() &&
                        view.previewHoveredPart == (view.previewHoveredPartIsMaterial
                            ? subMesh.materialGuid : subMesh.sourcePrimitiveGuid);
                    if (selected || previewHovered || (previewPartSelected && !view.previewIsolateSelectedPart)) {
                        packet.selectionFeedback = static_cast<uint8_t>(
                            ((selected || previewPartSelected) ? 1u : 0u) |
                            (previewHovered ? 2u : 0u));
                        selectionQueue.push_back(packet);
                    }
                }
            };

            if (assetPreviewActive) {
                if (previewModel) {
                    appendModel(*previewModel, glm::mat4(1.0f), nullptr,
                        nullptr,
                        nullptr, false,
                        {}, false);
                }
            }
            else {
                auto* transformPool = registry.getPool<TransformComponent>();
                auto* meshPool = registry.getPool<MeshComponent>();
                auto* instanceBatchPool = registry.findPool<
                    RenderInstanceBatchComponent>();
                if (transformPool && meshPool) {
                    for (Entity entity : meshPool->entities) {
                        const MeshComponent& meshComponent =
                            std::as_const(*meshPool).get(entity);
                        if (!meshComponent.enabled || !meshComponent.model ||
                            !transformPool->has(entity)) {
                            continue;
                        }
                        const SceneEntityUuid owner = sceneWorld_.identities()
                            .persistentId(entity).value_or(
                                SceneEntityUuid{});
                        bool persistentOpaque = false;
                        if (gpuSceneFrame_ && !owner.isNil()) {
                            const auto found = std::ranges::lower_bound(
                                gpuSceneFrame_->instanceIdentities, owner, {},
                                &GpuSceneInstanceIdentity::owner);
                            persistentOpaque = found !=
                                gpuSceneFrame_->instanceIdentities.end() &&
                                found->owner == owner;
                        }
                        appendModel(*meshComponent.model,
                            transformPool->get(entity).worldMatrix,
                            &meshComponent,
                            instanceBatchPool && instanceBatchPool->has(entity)
                                ? &instanceBatchPool->get(entity) : nullptr,
                            nullptr,
                            entity == selectedEntity, owner,
                            persistentOpaque);
                    }
                }
            }

            // M7.2 parity stage: ordinary scene opaque work is reconstructed
            // from the persistent publication. Transparent and explicit
            // fallback owners above continue to use the M6 packet path.
            if (!assetPreviewActive && gpuSceneFrame_) {
                CpuScope parityScope(cpuProfiler_, "cpu.render.extract.parity");
                for (uint32_t primitiveIndex = 0;
                        primitiveIndex < gpuSceneFrame_->primitives.size();
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
                    const glm::mat4 worldTransform = unpackGpuSceneAffine(
                        gpuSceneFrame_->transforms[instance.references.x]);
                    const SceneEntityUuid owner = gpuSceneFrame_->
                        instanceIdentities[instanceIndex].owner;
                    const bool selected = (instance.state.z &
                        GpuSceneInstanceSelected) != 0;
                    const GpuSceneGeometryRecord& geometry =
                        gpuSceneFrame_->geometries[primitive.binding.y];
                        DrawPacket packet{};
                        packet.geometry = GeometryHandle{ geometry.storage.x };
                        packet.material = MaterialHandle{ primitive.binding.z };
                        packet.pipeline = PipelineHandle{ primitive.binding.w };
                        packet.opaqueSortKey =
                            (static_cast<uint64_t>(primitive.binding.w) << 32u) |
                            primitive.binding.z;
                        packet.indexCount = geometry.draw.y;
                        packet.firstIndex = geometry.draw.x;
                        packet.executionFlags =
                            DrawPacketGpuScenePrimitive;
                        if (cpuVisible) {
                            packet.executionFlags |=
                                DrawPacketCpuVisibilityOracle;
                        }
                        packet.firstInstanceTransform = primitiveIndex;
                        packet.worldTransform = worldTransform;
                        packet.distanceToCamera = glm::distance(
                            renderCameraPosition, glm::vec3(worldTransform[3]));
                        packet.isSelected = selected ? 1 : 0;
                        packet.owner = owner;
                        const GpuScenePrimitiveIdentity& identity =
                            gpuSceneFrame_->primitiveIdentities[primitiveIndex];
                        packet.sourcePrimitiveGuid = identity.sourcePrimitiveGuid;
                        packet.primitiveGuid = identity.primitiveGuid;
                        packet.materialGuid = identity.effectiveMaterialGuid;
                        packet.coverage = (primitive.state.y &
                            GpuScenePrimitiveAlphaMask) != 0
                            ? static_cast<uint8_t>(ModelCoverage::Masked)
                            : static_cast<uint8_t>(ModelCoverage::Opaque);
                        packet.boundsSphereCenterWorld = {
                            instance.worldBoundsSphere.x,
                            instance.worldBoundsSphere.y,
                            instance.worldBoundsSphere.z,
                        };
                        packet.boundsSphereRadiusWorld =
                            instance.worldBoundsSphere.w;
                        if ((primitive.state.w &
                                GpuSceneConsumerForwardOpaque) != 0) {
                            if (cpuVisible) {
                                forwardOpaqueQueue.push_back(packet);
                                ++gpuSceneForwardVisibleCount;
                                gpuSceneForwardVisibleTriangles +=
                                    packet.indexCount / 3u;
                            }
                        }
                        else if ((primitive.state.w &
                                GpuSceneConsumerMainOpaque) != 0) {
                            opaqueQueue.push_back(packet);
                            ++gpuSceneDeferredCandidateCount;
                            gpuSceneDeferredCandidateTriangles +=
                                packet.indexCount / 3u;
                        }
                    if (selected && cpuVisible)
                        selectionQueue.push_back(packet);
                }
            }
        }

        cpuProfiler_.recordCounter("draw.requested.opaque", opaqueQueue.size());
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
        const uint64_t totalQueuedPrimitives =
            opaqueQueue.size() + forwardOpaqueQueue.size();
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
            submittedTriangles(opaqueQueue) +
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
            // M7R R5c.3: compact (key, index) sort; same permutation as the
            // packet sort (opaqueSortKey, geometry, firstIndex).
            sortOpaqueDrawPackets(opaqueQueue, drawSortScratch_);
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
        const auto appendDirectProbeFallbacks = [&](const auto& queue) {
            for (const DrawPacket& packet : queue) {
                if (policy_.forceDirectGBufferReference ||
                    policy_.forceDirectProbeCaptureReference ||
                    !hasGpuScenePrimitive(packet))
                    probeCasterQueue_.push_back(packet);
            }
        };
        appendDirectProbeFallbacks(opaqueQueue);
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
            sortTransparentCompatibilityDrawPackets(transparentQueue,
                drawSortScratch_);
            sortTransparentWorkDrawPackets(sortedSurfaceQueue,
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
            transparentIntervalEndpointScratch.resize(sortedSurfaceQueue.size());
            transparentIntervalNearScratch.resize(sortedSurfaceQueue.size());
            transparentIntervalFenwickScratch.resize(
                sortedSurfaceQueue.size() + 1u);
            cpuProfiler_.recordCounter("transparent.sort.ambiguous_intervals",
                sweepAmbiguousTransparentIntervals(sortedSurfaceQueue,
                    transparentIntervalEndpointScratch,
                    transparentIntervalNearScratch,
                    transparentIntervalFenwickScratch));
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
        renderFrame.opaqueQueue = opaqueQueue;
        renderFrame.selectionQueue = activeSelectionQueue;
        renderFrame.wireframe = isWireframe;
        renderFrame.forwardOpaqueQueue = forwardOpaqueQueue;
        renderFrame.sortedSurfaceQueue = sortedSurfaceQueue;
        renderFrame.compatibilityTransparentQueue = transparentQueue;
        renderFrame.instanceTransforms = forwardInstanceTransforms_;
        renderFrame.lights = &lightingFrame;
        renderFrame.reflectionProbes = &publishedProbes;
        renderFrame.stageObserver = inputs.stageObserver;
        return renderFrame_;
    }

    void RenderExtractor::releaseFrame() {
        frameStage_ = {};
        view_ = nullptr;
        renderFrame_ = {};
        lightingFrame_ = {};
        // extractedProbes_ keeps its storage: the next extraction rewrites it in
        // place (M7R R5c.6).
        publishedProbes_ = {};
        // `vector = {}` assigns an empty initializer list and keeps the
        // capacity; swapping with an empty vector frees it, as the former
        // drawFrame locals did, so the next frame allocates them again.
        std::vector<DirectionalShadowFramePacket>().swap(directionalShadows_);
        std::vector<SpotShadowFramePacket>().swap(spotShadows_);
        std::vector<PointShadowFramePacket>().swap(pointShadows_);
    }

} // namespace Iridium
