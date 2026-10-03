#pragma once

// M7R R3c: the shared services a Vulkan feature owner works with, and the
// owner lifecycle. Feature owners (clustered lighting, output, OIT, hooks,
// later shadows/probes/opaque/lighting/forward) own their pipelines and
// per-slot resources and register execute callbacks for their graph passes
// (ADR-0016); the backend keeps the frame lifecycle.
//
// Per-frame staging. A callback reads only what its owner holds. Per-frame
// inputs it needs (view and lighting packets, exposure, queue spans, counts)
// are staged into the owner by the backend's submitFrame stage, before that
// pass's drain point, and stay valid until the callback ran. Owners stage
// into fixed members (spans and PODs, never containers that grow), so a
// steady frame allocates nothing.
//
// Drain points. submitFrame (R3c.11) sequences the frame through the stages
// the former IRenderBackend::submit* calls had, in the same order. Each owner
// runs its registered passes at the point where they used to be recorded: it
// stages, then calls VulkanRenderGraphExecutor::drainRegisteredThrough(lastPass)
// inside the same CPU scope as before. Explicit drains keep work from moving
// across the transparent pipeline-statistics bracket, other passes' GPU
// ranges and CPU scopes. The executor still decides activity, emits the
// barriers and GPU ranges, and runs the callbacks in compiled order, so the
// command stream is unchanged.

#include "renderer/vulkan/VulkanProductionGraphIds.h"

#include <vulkan/vulkan.h>

#include <cstdint>

class DescriptorAllocator;
class VkContext;

namespace Iridium {

    class CpuProfiler;
    class VulkanExtensionHooks;
    class VulkanFrameScheduler;
    class VulkanFrameTargets;
    class VulkanFrameTelemetry;
    class VulkanGpuSceneState;
    class VulkanMeshLayouts;
    class VulkanPipelineLibrary;
    class VulkanRenderGraphExecutor;
    class VulkanResourceAllocator;
    class VulkanResourceRegistry;
    class VulkanUploadContext;

    // Built once the device exists (backend init); valid until cleanup.
    struct VulkanFeatureContext {
        ::VkContext& vk;
        VkDevice device = VK_NULL_HANDLE;
        VulkanResourceAllocator& allocator;
        VulkanUploadContext& uploads;
        ::DescriptorAllocator& descriptors;
        VulkanFrameScheduler& scheduler;
        VulkanRenderGraphExecutor& graph;
        VulkanFrameTargets& frameTargets;
        VulkanMeshLayouts& meshLayouts;
        VulkanPipelineLibrary& pipelines;
        VulkanResourceRegistry& resources;
        VulkanGpuSceneState& gpuScene;
        // Null when profiling is unavailable; enabled state is fixed per process.
        CpuProfiler* profiler = nullptr;
        // Draw/dispatch/bind counters of the frame being recorded.
        VulkanFrameTelemetry& telemetry;
        VulkanExtensionHooks& extensions;
        // The backend's open-frame flag (capacity changes are frame-boundary only).
        const bool& frameOpen;
    };

    // Owner lifecycle, driven by the backend:
    //   create            once, after the context's services exist;
    //   onGraphRebuilt    after every production-graph rebuild, with the new
    //                     plan's ids (owners re-query graph images and buffers
    //                     here, which keeps R4b aliasing safe);
    //   registerPasses    right after onGraphRebuilt (a rebuild clears every
    //                     registration);
    //   onGraphReleased   before the graph's resources are destroyed (resize,
    //                     transport and topology changes);
    //   onFrameSlotRetired when a frame slot's fence has completed (beginFrame);
    //   destroy           after device idle, before the device is destroyed.
    class IVulkanFeature {
    public:
        virtual ~IVulkanFeature() = default;
        virtual void create(const VulkanFeatureContext& context) = 0;
        virtual void onGraphRebuilt(const VulkanProductionGraphIds& ids) = 0;
        virtual void registerPasses(VulkanRenderGraphExecutor& graph) = 0;
        virtual void onGraphReleased() {}
        virtual void onFrameSlotRetired(uint32_t frameIndex) { (void)frameIndex; }
        virtual void destroy() noexcept = 0;
    };

} // namespace Iridium
