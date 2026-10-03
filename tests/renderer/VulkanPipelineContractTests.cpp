// Headless production pipeline creation under the Khronos validation layer.
// Each production pass is initialised exactly as the backend does (same layouts,
// same SPIR-V); validation then checks shader/pipeline-layout compatibility,
// push-constant ranges, vertex-input coverage, rendering-format/fragment-output
// compatibility (M7R R4a: the main-scene pipelines chain
// VkPipelineRenderingCreateInfo instead of naming a render pass) and the
// enabled device features. Replaces the Vulkan pass source-text checks of
// Stage3ArchitectureTests groups 2 and 6.
//
// The lighting descriptor-set layout is owned by VkLightingPipeline, which needs
// a windowed VkContext; it is rebuilt here from the union of the deferred and
// forward consumers' reflected set (ShaderAbiContractTests proves that union is
// one consistent representation).

#include "HeadlessVulkanDevice.h"
#include "SpirvInspector.h"
#include "TestHarness.h"

#include "renderer/rhi/ShadowTypes.h"
#include "renderer/vulkan/DescriptorAllocator.h"
#include "renderer/vulkan/VulkanClusteredLightingPipeline.h"
#include "renderer/vulkan/VulkanDepthPyramid.h"
#include "renderer/vulkan/VulkanDirectionalShadowMap.h"
#include "renderer/vulkan/VulkanFrameTargets.h"
#include "renderer/vulkan/VulkanGBufferLayout.h"
#include "renderer/vulkan/VulkanIndexedTextureTable.h"
#include "renderer/vulkan/VulkanLayeredInterfaceCapturePass.h"
#include "renderer/vulkan/VulkanLayeredLocalCompositionPass.h"
#include "renderer/vulkan/VulkanLayeredSceneResolvePass.h"
#include "renderer/vulkan/VulkanMeshLayouts.h"
#include "renderer/vulkan/VulkanPipelineLibrary.h"
#include "renderer/vulkan/VulkanPointShadowPools.h"
#include "renderer/vulkan/VulkanReflectionProbeCapturePass.h"
#include "renderer/vulkan/VulkanReflectionProbePipeline.h"
#include "renderer/vulkan/VulkanResourceAllocator.h"
#include "renderer/vulkan/VulkanSpotShadowAtlas.h"
#include "renderer/vulkan/VulkanTransparencyPyramid.h"
#include "renderer/vulkan/VulkanUploadContext.h"
#include "renderer/vulkan/VulkanWeightedOitPass.h"

#include <array>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

    using namespace Iridium;
    using Iridium::Test::HeadlessVulkanDevice;
    using Iridium::Test::SpirvDescriptorKind;
    using Iridium::Test::SpirvModule;

    VkDescriptorType descriptorType(SpirvDescriptorKind kind) {
        switch (kind) {
        case SpirvDescriptorKind::UniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case SpirvDescriptorKind::StorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case SpirvDescriptorKind::CombinedImageSampler:
            return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case SpirvDescriptorKind::SampledImage: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case SpirvDescriptorKind::StorageImage: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case SpirvDescriptorKind::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
        default: throw std::runtime_error("unsupported reflected descriptor kind");
        }
    }

    // Lighting set layout reconstructed from every lit consumer's reflection.
    VkDescriptorSetLayout createReflectedLightingLayout(VkDevice device) {
        std::map<uint32_t, VkDescriptorSetLayoutBinding> bindings;
        const auto merge = [&](const char* spv, uint32_t set) {
            const SpirvModule module = SpirvModule::load(Iridium::Test::shaderBinary(spv));
            for (const auto& binding : module.descriptorBindings()) {
                if (binding.set != set) continue;
                VkDescriptorSetLayoutBinding value{};
                value.binding = binding.binding;
                value.descriptorType = descriptorType(binding.kind);
                value.descriptorCount = binding.count == 0 ? 1u : binding.count;
                value.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
                bindings.try_emplace(binding.binding, value);
            }
        };
        merge("canonical_reference_lighting_frag.spv", 0);
        for (const char* spv : { "complex_material_indexed_frag.spv",
                "complex_opaque_material_indexed_frag.spv",
                "layered_ordinary2_material_indexed_frag.spv",
                "layered_deep_material_indexed_frag.spv",
                "layered_deep_residual_material_indexed_frag.spv",
                "weighted_oit_material_indexed_frag.spv",
                "reflection_probe_capture_frag.spv" })
            merge(spv, 3);
        std::vector<VkDescriptorSetLayoutBinding> list;
        for (const auto& [index, binding] : bindings) list.push_back(binding);
        VkDescriptorSetLayoutCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        info.bindingCount = static_cast<uint32_t>(list.size());
        info.pBindings = list.data();
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        if (vkCreateDescriptorSetLayout(device, &info, nullptr, &layout) != VK_SUCCESS)
            throw std::runtime_error("reflected lighting layout");
        return layout;
    }

    // The shared production layouts every material pass is created against.
    struct ProductionLayouts {
        explicit ProductionLayouts(const HeadlessVulkanDevice& gpu) : device(gpu.device()) {
            descriptors.init(device);
            const uint32_t maximum = (std::min)(4096u,
                gpu.maxUpdateAfterBindDescriptors() /
                    ((VulkanIndexedTextureTable::FrameSetCount + 1u) * 2u));
            textures.init(device, 64, maximum);
            lighting = createReflectedLightingLayout(device);
            meshes.init(device, lighting, textures.materialViewLayout(),
                textures.samplerLayout());
        }
        ~ProductionLayouts() {
            meshes.cleanup();
            vkDestroyDescriptorSetLayout(device, lighting, nullptr);
            textures.cleanup();
            descriptors.cleanup();
        }
        VkDevice device;
        ::DescriptorAllocator descriptors;
        VulkanIndexedTextureTable textures;
        VkDescriptorSetLayout lighting = VK_NULL_HANDLE;
        VulkanMeshLayouts meshes;
    };

    std::unique_ptr<HeadlessVulkanDevice> sharedDevice;

    bool noValidationErrors(const char* stage) {
        IRIDIUM_CHECK_MSG(sharedDevice->validationErrors() == 0u, stage << ": "
            << sharedDevice->validationErrors() << " validation errors");
        return true;
    }

    bool testLayeredTransparencyPipelines() {
        HeadlessVulkanDevice& gpu = *sharedDevice;
        gpu.resetValidationErrors();
        {
            ProductionLayouts layouts(gpu);
            IRIDIUM_CHECK(gpu.hasDynamicRendering());

            VulkanLayeredInterfaceCapturePass capture;
            capture.init(gpu.device(), layouts.descriptors,
                layouts.meshes.getGlobalSetLayout(), layouts.textures.materialViewLayout(),
                layouts.textures.samplerLayout());
            IRIDIUM_CHECK(capture.pipeline() != VK_NULL_HANDLE);
            IRIDIUM_CHECK(capture.tileTerminationPipeline() != VK_NULL_HANDLE);
            IRIDIUM_CHECK(noValidationErrors("layered interface capture"));

            VulkanLayeredLocalCompositionPass composition;
            composition.init(gpu.device(), layouts.descriptors,
                layouts.meshes.getGlobalSetLayout(), layouts.textures.materialViewLayout(),
                layouts.textures.samplerLayout(), layouts.lighting);
            IRIDIUM_CHECK(composition.renderPass() != VK_NULL_HANDLE);
            IRIDIUM_CHECK(composition.pipeline() != VK_NULL_HANDLE);
            IRIDIUM_CHECK(composition.deepPipeline() != VK_NULL_HANDLE);
            IRIDIUM_CHECK(composition.deepResidualPipeline() != VK_NULL_HANDLE);
            IRIDIUM_CHECK(noValidationErrors("layered local composition"));

            VulkanLayeredSceneResolvePass resolve;
            resolve.init(gpu.device(), layouts.descriptors,
                layouts.meshes.getGlobalSetLayout());
            IRIDIUM_CHECK(resolve.pipeline() != VK_NULL_HANDLE);
            IRIDIUM_CHECK(noValidationErrors("layered scene resolve"));

            VulkanWeightedOitPass weighted;
            weighted.init(gpu.device(), layouts.descriptors,
                layouts.meshes.getForwardPipelineLayout());
            IRIDIUM_CHECK(weighted.accumulationRenderPass() != VK_NULL_HANDLE);
            IRIDIUM_CHECK(weighted.resolveRenderPass() != VK_NULL_HANDLE);
            IRIDIUM_CHECK(weighted.accumulationPipeline() != VK_NULL_HANDLE);
            IRIDIUM_CHECK(weighted.resolvePipeline() != VK_NULL_HANDLE);
            IRIDIUM_CHECK(noValidationErrors("weighted OIT"));

            VulkanTransparencyPyramid pyramid;
            pyramid.init(gpu.device(), layouts.descriptors,
                layouts.meshes.getGlobalSetLayout());
            IRIDIUM_CHECK(noValidationErrors("transparency pyramid"));

            pyramid.cleanup();
            weighted.cleanup();
            resolve.cleanup();
            composition.cleanup();
            capture.cleanup();
        }
        return noValidationErrors("layered transparency teardown");
    }

    // The material pipeline library with the backend's rendering-format
    // targets (the G-buffer formats of each layout + D32; scene colour + D32
    // for forward and transparent), one pipeline per program/pass class.
    bool testMaterialPipelineLibrary() {
        HeadlessVulkanDevice& gpu = *sharedDevice;
        gpu.resetValidationErrors();
        for (const GBufferLayout layout : { GBufferLayout::CanonicalReference,
                GBufferLayout::CanonicalQuality, GBufferLayout::CanonicalCompact }) {
            ProductionLayouts layouts(gpu);
            VulkanPipelineLibrary library;
            library.init(gpu.device(),
                { vulkanGBufferColorAttachmentFormats(layout),
                    vulkanGBufferFormats(layout).colorAttachmentCount,
                    VK_FORMAT_D32_SFLOAT, layouts.meshes.getGBufferPipelineLayout() },
                { { VulkanSceneColorFormat }, 1, VK_FORMAT_D32_SFLOAT,
                    layouts.meshes.getForwardPipelineLayout() },
                { { VulkanSceneColorFormat }, 1, VK_FORMAT_D32_SFLOAT,
                    layouts.meshes.getForwardPipelineLayout() },
                layout);
            PipelineStateDesc gbuffer{};
            PipelineStateDesc forward{};
            forward.shaderProgram = ShaderProgram::CanonicalComplexOpaqueForward;
            forward.renderPass = RenderPassClass::Forward;
            PipelineStateDesc transparent{};
            transparent.shaderProgram = ShaderProgram::CanonicalComplexForward;
            transparent.renderPass = RenderPassClass::Transparent;
            transparent.blendMode = BlendMode::PremultipliedAlpha;
            transparent.depthWrite = false;
            for (const PipelineStateDesc& desc : { gbuffer, forward, transparent }) {
                const VulkanPipelineRecord* record =
                    library.get(library.getOrCreatePipeline(desc));
                IRIDIUM_CHECK(record != nullptr && record->pipeline != VK_NULL_HANDLE);
                IRIDIUM_CHECK((desc.renderPass == RenderPassClass::GBuffer) ==
                    (record->gpuSceneIndirectPipeline != VK_NULL_HANDLE));
            }
            IRIDIUM_CHECK(library.pipelineCount() == 3u);
            IRIDIUM_CHECK(noValidationErrors("material pipeline library"));
            library.cleanup();
        }
        return noValidationErrors("material pipeline library teardown");
    }

    bool testShadowAndProbePipelines() {
        HeadlessVulkanDevice& gpu = *sharedDevice;
        gpu.resetValidationErrors();
        {
            ProductionLayouts layouts(gpu);
            VulkanResourceAllocator allocator;
            allocator.init(gpu.instance(), gpu.physicalDevice(), gpu.device(),
                gpu.hasMemoryBudget());
            VulkanUploadContext uploads;
            uploads.init(gpu.device(), gpu.queue(), gpu.queueFamily(), allocator, nullptr);

            // Opaque variants bind position-only vertex input; validation fails
            // pipeline creation if the shader consumed an unprovided location.
            VulkanDirectionalShadowMap directional;
            directional.init(gpu.device(), allocator, uploads, layouts.descriptors,
                layouts.textures.materialViewLayout(), layouts.textures.samplerLayout(),
                layouts.meshes.getGpuSceneSetLayout(), 256);
            VulkanSpotShadowAtlas spot;
            spot.init(gpu.device(), allocator, uploads, layouts.descriptors,
                layouts.textures.materialViewLayout(), layouts.textures.samplerLayout(),
                layouts.meshes.getGpuSceneSetLayout(), 512);
            VulkanPointShadowPools point;
            point.init(gpu.device(), allocator, uploads, layouts.descriptors,
                layouts.textures.materialViewLayout(), layouts.textures.samplerLayout(),
                layouts.meshes.getGpuSceneSetLayout(), { 1u, 1u, 1u });
            // The backend flushes initial target transitions after init.
            uploads.flush();
            for (const bool masked : { false, true })
                for (const bool twoSided : { false, true })
                    for (const bool gpuScene : { false, true }) {
                        IRIDIUM_CHECK(directional.pipeline(masked, twoSided, gpuScene) !=
                            VK_NULL_HANDLE);
                        IRIDIUM_CHECK(spot.pipeline(masked, twoSided, gpuScene) !=
                            VK_NULL_HANDLE);
                        IRIDIUM_CHECK(point.pipeline(masked, twoSided, gpuScene) !=
                            VK_NULL_HANDLE);
                    }
            IRIDIUM_CHECK(noValidationErrors("shadow pipelines"));

            VulkanReflectionProbeCapturePass probeCapture;
            probeCapture.init(gpu.device(), gpu.physicalDevice(), allocator,
                layouts.descriptors, layouts.textures.materialViewLayout(),
                layouts.textures.samplerLayout(), layouts.lighting,
                layouts.meshes.getGpuSceneSetLayout());
            for (const bool masked : { false, true })
                for (const bool gpuScene : { false, true })
                    IRIDIUM_CHECK(probeCapture.pipeline(masked, false, gpuScene) !=
                        VK_NULL_HANDLE);
            IRIDIUM_CHECK(noValidationErrors("reflection-probe capture"));

            VulkanClusteredLightingPipeline clusters;
            clusters.init(gpu.device(), layouts.descriptors);
            VulkanReflectionProbePipeline probeClusters;
            probeClusters.init(gpu.device(), layouts.descriptors);
            VulkanDepthPyramid depthPyramid;
            depthPyramid.init(gpu.device(), layouts.descriptors, allocator,
                layouts.meshes.getGlobalSetLayout(),
                layouts.meshes.getGpuSceneSetLayout());
            IRIDIUM_CHECK(noValidationErrors("compute pipelines"));

            uploads.flush();
            vkDeviceWaitIdle(gpu.device());
            depthPyramid.cleanup();
            probeClusters.cleanup();
            clusters.cleanup();
            probeCapture.cleanup();
            point.cleanup();
            spot.cleanup();
            directional.cleanup();
            uploads.cleanup();
            allocator.cleanup();
        }
        return noValidationErrors("shadow and probe teardown");
    }

} // namespace

int main() {
    try {
        sharedDevice = std::make_unique<HeadlessVulkanDevice>(
            HeadlessVulkanDevice::Options{ "Iridium pipeline contracts" });
    } catch (const std::exception& exception) {
        std::cerr << "headless Vulkan device unavailable: " << exception.what() << '\n';
        return 1;
    }
    constexpr Iridium::Test::TestCase tests[] = {
        { "layered transparency pipelines validate", testLayeredTransparencyPipelines },
        { "material pipeline library validates", testMaterialPipelineLibrary },
        { "shadow, probe and compute pipelines validate", testShadowAndProbePipelines },
    };
    const int result = Iridium::Test::runTests(tests);
    sharedDevice.reset();
    return result;
}
