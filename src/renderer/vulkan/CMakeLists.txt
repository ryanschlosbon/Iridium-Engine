# iridium_vulkan: the Vulkan backend (device, frame lifecycle, passes, graph
# executor) and RenderBackendFactory, which implements the rhi-declared factory.
# It has no editor-UI dependency (M7R R3c.10): the ImGui editor bridge is
# iridium_vulkan_imgui (renderer/vulkan_imgui).
add_library(iridium_vulkan STATIC
        DescriptorAllocator.cpp
        RenderBackendFactory.cpp
        VkContext.cpp
        VkGraphicsPipeline.cpp
        VkLightingPipeline.cpp
        VkSwapchain.cpp
        VulkanClusterLightingFeature.cpp
        VulkanClusteredLightingPipeline.cpp
        VulkanDeferredLightingFeature.cpp
        VulkanDeletionQueue.cpp
        VulkanDepthPyramid.cpp
        VulkanDirectionalShadowMap.cpp
        VulkanExtensionHooks.cpp
        VulkanForwardFeature.cpp
        VulkanFrameScheduler.cpp
        VulkanFrameTargets.cpp
        VulkanFrameTelemetry.cpp
        VulkanGpuSceneState.cpp
        VulkanHdrEncodePass.cpp
        VulkanHookPasses.cpp
        VulkanIndexedTextureTable.cpp
        VulkanIndirectCullerShared.cpp
        VulkanIndirectViewCuller.cpp
        VulkanOpaqueFeature.cpp
        VulkanOpaqueIndirectCuller.cpp
        VulkanLayeredInterfaceCapturePass.cpp
        VulkanLayeredLocalCompositionPass.cpp
        VulkanLayeredSceneResolvePass.cpp
        VulkanLayeredTransparencyFeature.cpp
        VulkanLocalShadowFeature.cpp
        VulkanMemoryAllocatorImpl.cpp
        VulkanMeshLayouts.cpp
        VulkanOutputFeature.cpp
        VulkanOutputPass.cpp
        VulkanOutputTransport.cpp
        VulkanPipelineCache.cpp
        VulkanPipelineLibrary.cpp
        VulkanPointShadowPools.cpp
        VulkanProductionRenderGraph.cpp
        VulkanReflectionProbeCapturePass.cpp
        VulkanReflectionProbeCaptureTargets.cpp
        VulkanReflectionProbeFeature.cpp
        VulkanReflectionProbePipeline.cpp
        VulkanRenderGraphExecutor.cpp
        VulkanResourceAllocator.cpp
        VulkanResourceRegistry.cpp
        VulkanResourceState.cpp
        VulkanSceneDescriptors.cpp
        VulkanShadowCasters.cpp
        VulkanShadowFeature.cpp
        VulkanSpotShadowAtlas.cpp
        VulkanTransparencyPyramid.cpp
        VulkanUiFeature.cpp
        VulkanUploadContext.cpp
        VulkanVertexBackend.cpp
        VulkanViewUniforms.cpp
        VulkanVirtualShadowDepthReceiverPass.cpp
        VulkanVirtualShadowFullViewPass.cpp
        VulkanVirtualShadowMarkingPass.cpp
        VulkanVirtualShadowResources.cpp
        VulkanWeightedOitFeature.cpp
        VulkanWeightedOitPass.cpp
)
target_link_libraries(iridium_vulkan PUBLIC
        iridium_lighting
        iridium_transparency
        iridium_rhi
        iridium_graph
        iridium_profiling
        iridium_color
        Vulkan::Vulkan
        glfw)
# VMA stays private: VulkanResourceAllocator.h only forward-declares its handles.
target_link_libraries(iridium_vulkan PRIVATE GPUOpen::VulkanMemoryAllocator)
iridium_target_pch(iridium_vulkan ENGINE VULKAN)
# The VMA implementation TU (M7R R4b) is third-party code: no PCH, and its
# warnings are suppressed inside the file.
set_source_files_properties(VulkanMemoryAllocatorImpl.cpp PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON)
