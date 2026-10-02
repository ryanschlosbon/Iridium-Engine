// Per-frame qualification validators: scene-extent resize sequences (between
// frames), the deep layered tier lifecycle, texture residency churn (inside the
// open frame) and the live output-transport switch sequence (after endFrame).

#include "qualification/harness/QualificationHarness.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "profiling/CpuProfiler.h"
#include "renderer/rhi/Mesh.h"
#include "scene/SceneWorld.h"
#include "scene/components/MeshComponent.h"

namespace Iridium {

    void QualificationHarness::updateOrdinary2ResizeValidation(
        AppFrameContext& context, uint64_t measuredFrameIndex) {
        CpuProfiler& cpuProfiler = context.profiler;
        if (measuredFrameIndex >= 3u) {
            cpuProfiler.recordCounter(
                "ordinary2.lifecycle.resize.requests", 0u);
            cpuProfiler.recordCounter(
                "ordinary2.lifecycle.resize.successes", 0u);
            cpuProfiler.recordCounter(
                "ordinary2.lifecycle.resize.failures", 0u);
            return;
        }

        const std::array<RenderExtent, 3> sequence{{
            { 960u, 540u },
            { 1600u, 900u },
            ordinary2ResizeValidation_.originalExtent,
        }};
        const RenderExtent requested = sequence[measuredFrameIndex];
        ++ordinary2ResizeValidation_.requests;

        std::string diagnostic;
        bool resized = false;
        {
            CpuScope resizeScope(
                cpuProfiler, "cpu.ordinary2.lifecycle.resize");
            resized = context.control.resizeSceneExtent(
                requested, diagnostic);
        }
        if (resized) {
            ++ordinary2ResizeValidation_.successes;
            ordinary2ResizeValidation_.lastDiagnostic.clear();
        }
        else {
            ++ordinary2ResizeValidation_.failures;
            ordinary2ResizeValidation_.lastDiagnostic = diagnostic.empty()
                ? "scene target resize failed without a diagnostic"
                : std::move(diagnostic);
        }
        cpuProfiler.recordCounter(
            "ordinary2.lifecycle.resize.requests", 1u);
        cpuProfiler.recordCounter(
            "ordinary2.lifecycle.resize.successes", resized ? 1u : 0u);
        cpuProfiler.recordCounter(
            "ordinary2.lifecycle.resize.failures", resized ? 0u : 1u);
        const RenderExtent effective = context.control.renderExtent();
        std::cout << "IRIDIUM_ORDINARY2_RESIZE_EVENT {\"measured_frame\":"
            << measuredFrameIndex << ",\"requested\":["
            << requested.width << ',' << requested.height
            << "],\"effective\":[" << effective.width << ','
            << effective.height << "],\"success\":"
            << (resized ? "true" : "false") << "}\n" << std::flush;
    }

    void QualificationHarness::updateWeightedOitResizeValidation(
        AppFrameContext& context, uint64_t measuredFrameIndex) {
        CpuProfiler& cpuProfiler = context.profiler;
        if (measuredFrameIndex >= 3u) {
            cpuProfiler.recordCounter(
                "weighted_oit.lifecycle.resize.requests", 0u);
            cpuProfiler.recordCounter(
                "weighted_oit.lifecycle.resize.successes", 0u);
            cpuProfiler.recordCounter(
                "weighted_oit.lifecycle.resize.failures", 0u);
            return;
        }

        const std::array<RenderExtent, 3> sequence{{
            { 960u, 540u },
            { 1600u, 900u },
            weightedOitResizeValidation_.originalExtent,
        }};
        const RenderExtent requested = sequence[measuredFrameIndex];
        ++weightedOitResizeValidation_.requests;

        std::string diagnostic;
        bool resized = false;
        {
            CpuScope resizeScope(
                cpuProfiler, "cpu.weighted_oit.lifecycle.resize");
            resized = context.control.resizeSceneExtent(
                requested, diagnostic);
        }
        if (resized) {
            ++weightedOitResizeValidation_.successes;
            weightedOitResizeValidation_.lastDiagnostic.clear();
        }
        else {
            ++weightedOitResizeValidation_.failures;
            weightedOitResizeValidation_.lastDiagnostic = diagnostic.empty()
                ? "scene target resize failed without a diagnostic"
                : std::move(diagnostic);
        }
        cpuProfiler.recordCounter(
            "weighted_oit.lifecycle.resize.requests", 1u);
        cpuProfiler.recordCounter(
            "weighted_oit.lifecycle.resize.successes", resized ? 1u : 0u);
        cpuProfiler.recordCounter(
            "weighted_oit.lifecycle.resize.failures", resized ? 0u : 1u);
        const RenderExtent effective = context.control.renderExtent();
        std::cout << "IRIDIUM_WEIGHTED_OIT_RESIZE_EVENT {\"measured_frame\":"
            << measuredFrameIndex << ",\"requested\":["
            << requested.width << ',' << requested.height
            << "],\"effective\":[" << effective.width << ','
            << effective.height << "],\"success\":"
            << (resized ? "true" : "false") << "}\n" << std::flush;
    }

    void QualificationHarness::updateDepthPyramidResizeValidation(
        AppFrameContext& context, uint64_t measuredFrameIndex) {
        CpuProfiler& cpuProfiler = context.profiler;
        std::optional<RenderExtent> requested;
        if (measuredFrameIndex == 0u) requested = RenderExtent{ 960u, 540u };
        else if (measuredFrameIndex == 3u)
            requested = RenderExtent{ 1600u, 900u };
        else if (measuredFrameIndex == 6u)
            requested = depthPyramidResizeValidation_.originalExtent;

        if (!requested) {
            cpuProfiler.recordCounter(
                "depth.occlusion.lifecycle.resize.requests", 0u);
            cpuProfiler.recordCounter(
                "depth.occlusion.lifecycle.resize.successes", 0u);
            cpuProfiler.recordCounter(
                "depth.occlusion.lifecycle.resize.failures", 0u);
            return;
        }

        ++depthPyramidResizeValidation_.requests;
        std::string diagnostic;
        bool resized = false;
        {
            CpuScope resizeScope(
                cpuProfiler, "cpu.depth.occlusion.lifecycle.resize");
            resized = context.control.resizeSceneExtent(
                *requested, diagnostic);
        }
        if (resized) {
            ++depthPyramidResizeValidation_.successes;
            depthPyramidResizeValidation_.lastDiagnostic.clear();
        }
        else {
            ++depthPyramidResizeValidation_.failures;
            depthPyramidResizeValidation_.lastDiagnostic = diagnostic.empty()
                ? "scene target resize failed without a diagnostic"
                : std::move(diagnostic);
        }
        cpuProfiler.recordCounter(
            "depth.occlusion.lifecycle.resize.requests", 1u);
        cpuProfiler.recordCounter(
            "depth.occlusion.lifecycle.resize.successes", resized ? 1u : 0u);
        cpuProfiler.recordCounter(
            "depth.occlusion.lifecycle.resize.failures", resized ? 0u : 1u);
        const RenderExtent effective = context.control.renderExtent();
        std::cout << "IRIDIUM_DEPTH_PYRAMID_RESIZE_EVENT {\"measured_frame\":"
            << measuredFrameIndex << ",\"requested\":["
            << requested->width << ',' << requested->height
            << "],\"effective\":[" << effective.width << ','
            << effective.height << "],\"success\":"
            << (resized ? "true" : "false") << "}\n" << std::flush;
    }

    bool QualificationHarness::updateDeepLayeredLifecycleValidation(
        AppFrameContext& context, uint64_t measuredFrameIndex) {
        using State = DeepLayeredLifecycleValidationState;
        const RenderBackendRuntimeInfo runtimeInfo =
            context.backend.getRuntimeInfo();
        const bool selectedTierResident =
            options_.deepLayeredCaptureQuality == TransparencyQuality::Hero4
                ? runtimeInfo.hero4AtlasResident
                : runtimeInfo.cinematic8AtlasResident;
        const bool topologyResident = selectedTierResident &&
            runtimeInfo.refractionPyramidsResident;

        const auto setBenchmarkModelEnabled = [&](bool enabled) {
            auto* meshes = context.scene.registry().getPool<MeshComponent>();
            uint32_t changed = 0u;
            if (meshes != nullptr) {
                for (const BenchmarkInstanceState& instance :
                        benchmarkInstances_) {
                    if (!meshes->has(instance.entity)) continue;
                    MeshComponent& mesh = meshes->get(instance.entity);
                    if (!mesh.model || mesh.model != context.mainModel ||
                        mesh.enabled == enabled) {
                        continue;
                    }
                    mesh.enabled = enabled;
                    ++changed;
                }
            }
            if (changed == 0u) {
                throw std::runtime_error(
                    "Deep layered lifecycle validation could not change the benchmark model visibility.");
            }
            ++deepLayeredLifecycleValidation_.visibilityChanges;
        };
        const auto logEvent = [&](std::string_view event) {
            std::cout
                << "IRIDIUM_DEEP_LAYERED_LIFECYCLE_EVENT {\"measured_frame\":"
                << measuredFrameIndex << ",\"event\":\"" << event
                << "\",\"quality\":\""
                << transparencyQualityName(
                    options_.deepLayeredCaptureQuality)
                << "\",\"tier_resident\":"
                << (selectedTierResident ? "true" : "false")
                << ",\"refraction_pyramids_resident\":"
                << (runtimeInfo.refractionPyramidsResident
                    ? "true" : "false")
                << "}\n" << std::flush;
        };

        switch (deepLayeredLifecycleValidation_.phase) {
        case State::Phase::Initial:
            if (!topologyResident) {
                throw std::runtime_error(
                    "Deep layered lifecycle validation did not start with a resident selected tier.");
            }
            deepLayeredLifecycleValidation_.firstMeasuredFrame =
                measuredFrameIndex;
            setBenchmarkModelEnabled(false);
            deepLayeredLifecycleValidation_.phase =
                State::Phase::WaitingForRetirement;
            logEvent("hide_for_retirement");
            return false;
        case State::Phase::WaitingForRetirement:
            if (selectedTierResident ||
                runtimeInfo.refractionPyramidsResident) {
                return false;
            }
            ++deepLayeredLifecycleValidation_.retirements;
            setBenchmarkModelEnabled(true);
            deepLayeredLifecycleValidation_.phase =
                State::Phase::WaitingForReactivation;
            logEvent("retired_reveal_for_reactivation");
            return false;
        case State::Phase::WaitingForReactivation:
            if (!topologyResident) return false;
            ++deepLayeredLifecycleValidation_.reactivations;
            if (deepLayeredLifecycleValidation_.reactivations < 2u) {
                setBenchmarkModelEnabled(false);
                deepLayeredLifecycleValidation_.phase =
                    State::Phase::WaitingForRetirement;
                logEvent("reactivated_hide_for_retirement");
                return false;
            }
            deepLayeredLifecycleValidation_.completionMeasuredFrame =
                measuredFrameIndex;
            deepLayeredLifecycleValidation_.phase = State::Phase::Complete;
            logEvent("reactivated_complete");
            return true;
        case State::Phase::Complete:
            return false;
        }
        return false;
    }

    void QualificationHarness::updateTextureResidencyChurn(
        AppFrameContext& context) {
        if (!options_.validateTextureResidencyChurn) return;
        IRenderBackend& backend = context.backend;
        const uint64_t frameIndex = context.applicationFrameIndex;

        const TextureDesc probeDesc{
            .width = 512,
            .height = 512,
            .format = TextureFormat::RGBA8_UNorm,
            .usageClass = TextureUsageClass::Sampled2D,
        };

        if (frameIndex == 0) {
            if (!residencyProbeTexture_.isValid()) {
                throw std::runtime_error(
                    "Texture residency churn probe was not initialized");
            }
            residencyRetiredIndex_ = residencyProbeTexture_.getIndex();
            backend.freeTexture(residencyProbeTexture_);
            residencyProbeTexture_ = {};
            residencyReplacementTexture_ = backend.allocateTexture(
                probeDesc, residencyProbePixels_);
            if (residencyReplacementTexture_.getIndex() ==
                residencyRetiredIndex_) {
                throw std::runtime_error(
                    "Texture descriptor index was reused before fence retirement");
            }
        }
        else if (frameIndex == 2) {
            TextureHandle collected = backend.allocateTexture(
                probeDesc, residencyProbePixels_);
            if (collected.getIndex() != residencyRetiredIndex_) {
                backend.freeTexture(collected);
                throw std::runtime_error(
                    "Retired texture descriptor index was not reclaimed");
            }
            if (residencyReplacementTexture_.isValid()) {
                backend.freeTexture(residencyReplacementTexture_);
                residencyReplacementTexture_ = {};
            }
            backend.freeTexture(collected);
            std::cout << "IRIDIUM_TEXTURE_RESIDENCY_CHURN "
                "{\"fallback_before_destroy\":true,"
                "\"immediate_reuse\":false,"
                "\"reuse_after_fence\":true,"
                "\"retired_index\":" << residencyRetiredIndex_ << "}\n";
            residencyRetiredIndex_ = UINT32_MAX;
        }
    }

    void QualificationHarness::updateOutputTransportValidation(
        AppFrameContext& context) {
        if (!options_.validateOutputTransportSwitch ||
            outputTransportValidationStep_ >= 3u ||
            context.outputTransportPending) {
            return;
        }
        constexpr std::array sequence{
            Color::OutputTransport::ScRgb,
            Color::OutputTransport::Hdr10Pq,
            Color::OutputTransport::SdrSrgb,
        };
        (void)context.control.applyOutputTransport(
            sequence[outputTransportValidationStep_++]);
    }

} // namespace Iridium
