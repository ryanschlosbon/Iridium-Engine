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
#include "renderer/vulkan/VulkanPipelineCache.h"
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
#include <chrono>
#include <filesystem>
#include <fstream>
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

    // M7R R4c.4: every production pass creates its pipelines through one
    // persisted cache, as the backend does; the last test saves and reloads it.
    std::filesystem::path sharedCacheDirectory;
    std::unique_ptr<VulkanPipelineCache> sharedCache;

    VkPipelineCache pipelineCache() { return sharedCache->handle(); }

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
            capture.init(gpu.device(), pipelineCache(), layouts.descriptors,
                layouts.meshes.getGlobalSetLayout(), layouts.textures.materialViewLayout(),
                layouts.textures.samplerLayout());
            IRIDIUM_CHECK(capture.pipeline() != VK_NULL_HANDLE);
            IRIDIUM_CHECK(capture.tileTerminationPipeline() != VK_NULL_HANDLE);
            IRIDIUM_CHECK(noValidationErrors("layered interface capture"));

            VulkanLayeredLocalCompositionPass composition;
            composition.init(gpu.device(), pipelineCache(), layouts.descriptors,
                layouts.meshes.getGlobalSetLayout(), layouts.textures.materialViewLayout(),
                layouts.textures.samplerLayout(), layouts.lighting);
            IRIDIUM_CHECK(composition.pipeline() != VK_NULL_HANDLE);
            IRIDIUM_CHECK(composition.deepPipeline() != VK_NULL_HANDLE);
            IRIDIUM_CHECK(composition.deepResidualPipeline() != VK_NULL_HANDLE);
            IRIDIUM_CHECK(noValidationErrors("layered local composition"));

            VulkanLayeredSceneResolvePass resolve;
            resolve.init(gpu.device(), pipelineCache(), layouts.descriptors,
                layouts.meshes.getGlobalSetLayout());
            IRIDIUM_CHECK(resolve.pipeline() != VK_NULL_HANDLE);
            IRIDIUM_CHECK(noValidationErrors("layered scene resolve"));

            VulkanWeightedOitPass weighted;
            weighted.init(gpu.device(), pipelineCache(), layouts.descriptors,
                layouts.meshes.getForwardPipelineLayout());
            IRIDIUM_CHECK(weighted.accumulationPipeline() != VK_NULL_HANDLE);
            IRIDIUM_CHECK(weighted.resolvePipeline() != VK_NULL_HANDLE);
            IRIDIUM_CHECK(noValidationErrors("weighted OIT"));

            VulkanTransparencyPyramid pyramid;
            pyramid.init(gpu.device(), pipelineCache(), layouts.descriptors,
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
            std::array<VkFormat, VulkanPipelineMaxColorTargets> gbufferFormats{};
            const auto passFormats = vulkanGBufferPassColorAttachmentFormats(layout);
            std::copy(passFormats.begin(), passFormats.end(), gbufferFormats.begin());
            library.init(gpu.device(), pipelineCache(),
                { gbufferFormats, VulkanGBufferPassColorAttachmentCount,
                    VK_FORMAT_D32_SFLOAT, layouts.meshes.getGBufferPipelineLayout() },
                { { VulkanSceneColorFormat }, 1, VK_FORMAT_D32_SFLOAT,
                    layouts.meshes.getForwardPipelineLayout() },
                // M9.1: forward-opaque writes scene colour and velocity.
                { { VulkanSceneColorFormat, VulkanVelocityFormat }, 2, VK_FORMAT_D32_SFLOAT,
                    layouts.meshes.getForwardPipelineLayout() },
                { { VulkanSceneColorFormat }, 1, VK_FORMAT_D32_SFLOAT,
                    layouts.meshes.getForwardPipelineLayout() },
                layout);
            PipelineStateDesc gbuffer{};
            PipelineStateDesc forward{};
            forward.shaderProgram = ShaderProgram::CanonicalComplexOpaqueForward;
            forward.renderPass = RenderPassClass::Forward;
            forward.depthWrite = true;
            PipelineStateDesc compatibility{};
            compatibility.shaderProgram = ShaderProgram::CanonicalComplexForward;
            compatibility.renderPass = RenderPassClass::Forward;
            compatibility.blendMode = BlendMode::AlphaBlend;
            compatibility.depthWrite = false;
            PipelineStateDesc transparent{};
            transparent.shaderProgram = ShaderProgram::CanonicalComplexForward;
            transparent.renderPass = RenderPassClass::Transparent;
            transparent.blendMode = BlendMode::PremultipliedAlpha;
            transparent.depthWrite = false;
            for (const PipelineStateDesc& desc : { gbuffer, forward, compatibility, transparent }) {
                const VulkanPipelineRecord* record =
                    library.get(library.getOrCreatePipeline(desc));
                IRIDIUM_CHECK(record != nullptr && record->pipeline != VK_NULL_HANDLE);
                IRIDIUM_CHECK((desc.renderPass == RenderPassClass::GBuffer) ==
                    (record->gpuSceneIndirectPipeline != VK_NULL_HANDLE));
                // M9.1: G-buffer 6 targets (velocity last), forward-opaque 2,
                // compatibility and sorted transparency 1.
                const uint32_t expected = desc.renderPass == RenderPassClass::GBuffer
                    ? VulkanGBufferPassColorAttachmentCount
                    : (desc.renderPass == RenderPassClass::Forward && desc.depthWrite) ? 2u : 1u;
                IRIDIUM_CHECK(record->colorAttachmentCount == expected);
            }
            IRIDIUM_CHECK(library.pipelineCount() == 4u);
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
            directional.init(gpu.device(), pipelineCache(), allocator, uploads, layouts.descriptors,
                layouts.textures.materialViewLayout(), layouts.textures.samplerLayout(),
                layouts.meshes.getGpuSceneSetLayout(), 256);
            VulkanSpotShadowAtlas spot;
            spot.init(gpu.device(), pipelineCache(), allocator, uploads, layouts.descriptors,
                layouts.textures.materialViewLayout(), layouts.textures.samplerLayout(),
                layouts.meshes.getGpuSceneSetLayout(), 512);
            VulkanPointShadowPools point;
            point.init(gpu.device(), pipelineCache(), allocator, uploads, layouts.descriptors,
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
            probeCapture.init(gpu.device(), pipelineCache(), gpu.physicalDevice(), allocator,
                layouts.descriptors, layouts.textures.materialViewLayout(),
                layouts.textures.samplerLayout(), layouts.lighting,
                layouts.meshes.getGpuSceneSetLayout());
            for (const bool masked : { false, true })
                for (const bool gpuScene : { false, true })
                    IRIDIUM_CHECK(probeCapture.pipeline(masked, false, gpuScene) !=
                        VK_NULL_HANDLE);
            IRIDIUM_CHECK(noValidationErrors("reflection-probe capture"));

            VulkanClusteredLightingPipeline clusters;
            clusters.init(gpu.device(), pipelineCache(), layouts.descriptors);
            VulkanReflectionProbePipeline probeClusters;
            probeClusters.init(gpu.device(), pipelineCache(), layouts.descriptors);
            VulkanDepthPyramid depthPyramid;
            depthPyramid.init(gpu.device(), pipelineCache(), layouts.descriptors, allocator,
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

    // The cache the tests above filled is saved atomically, reloaded warm
    // (non-empty, byte-identical payload) and used again; a corrupted file is
    // discarded and the cache starts empty.
    bool testPersistedPipelineCache() {
        HeadlessVulkanDevice& gpu = *sharedDevice;
        gpu.resetValidationErrors();
        const VulkanPipelineCacheIdentity identity =
            VulkanPipelineCacheIdentity::of(gpu.physicalDevice());
        IRIDIUM_CHECK(sharedCache->stats().state == PipelineCacheState::Cold);
        IRIDIUM_CHECK(sharedCache->handle() != VK_NULL_HANDLE);
        IRIDIUM_CHECK(sharedCache->save());
        const uint64_t savedBytes = sharedCache->stats().savedBytes;
        IRIDIUM_CHECK(savedBytes > kPipelineCacheFileHeaderBytes + 32u);
        const std::filesystem::path file = sharedCache->stats().file;
        IRIDIUM_CHECK(file.filename() == pipelineCacheFileName(identity));
        IRIDIUM_CHECK(std::filesystem::file_size(file) == savedBytes);
        size_t liveSize = 0;
        IRIDIUM_CHECK(vkGetPipelineCacheData(gpu.device(), sharedCache->handle(),
            &liveSize, nullptr) == VK_SUCCESS);
        sharedCache->destroy();

        VulkanPipelineCache warm;
        warm.init(gpu.device(), identity, sharedCacheDirectory);
        IRIDIUM_CHECK(warm.stats().state == PipelineCacheState::Warm);
        IRIDIUM_CHECK(warm.handle() != VK_NULL_HANDLE);
        IRIDIUM_CHECK(warm.stats().loadedBytes == savedBytes - kPipelineCacheFileHeaderBytes);
        IRIDIUM_CHECK(warm.stats().loadedBytes == liveSize);
        size_t warmSize = 0;
        IRIDIUM_CHECK(vkGetPipelineCacheData(gpu.device(), warm.handle(), &warmSize,
            nullptr) == VK_SUCCESS);
        IRIDIUM_CHECK(warmSize > 32u);
        {
            // Pipelines created from the warm cache still validate.
            ProductionLayouts layouts(gpu);
            VulkanClusteredLightingPipeline clusters;
            clusters.init(gpu.device(), warm.handle(), layouts.descriptors);
            IRIDIUM_CHECK(noValidationErrors("warm-cache compute pipelines"));
            clusters.cleanup();
        }
        IRIDIUM_CHECK(warm.save());
        IRIDIUM_CHECK(warm.stats().saveSkippedUnchanged || warm.stats().savedBytes > 0u);
        warm.destroy();

        // Flip one payload byte: the FNV-64 check discards the file.
        {
            std::fstream stream(file, std::ios::in | std::ios::out | std::ios::binary);
            stream.seekg(static_cast<std::streamoff>(kPipelineCacheFileHeaderBytes + 40u));
            char value = 0;
            stream.read(&value, 1);
            value = static_cast<char>(value ^ 0x5a);
            stream.seekp(static_cast<std::streamoff>(kPipelineCacheFileHeaderBytes + 40u));
            stream.write(&value, 1);
        }
        VulkanPipelineCache discarded;
        discarded.init(gpu.device(), identity, sharedCacheDirectory);
        IRIDIUM_CHECK(discarded.stats().state == PipelineCacheState::Discarded);
        IRIDIUM_CHECK(discarded.stats().discardReason == PipelineCacheFileStatus::HashMismatch);
        IRIDIUM_CHECK(discarded.handle() != VK_NULL_HANDLE);
        IRIDIUM_CHECK(discarded.stats().loadedBytes == 0u);
        // The next save replaces the corrupt file with a valid one.
        IRIDIUM_CHECK(discarded.save());
        discarded.destroy();
        VulkanPipelineCache repaired;
        repaired.init(gpu.device(), identity, sharedCacheDirectory);
        IRIDIUM_CHECK(repaired.stats().state == PipelineCacheState::Warm);
        repaired.destroy();

        // No directory: no cache object at all (pre-R4c.4 behavior).
        VulkanPipelineCache off;
        off.init(gpu.device(), identity, {});
        IRIDIUM_CHECK(off.stats().state == PipelineCacheState::Off);
        IRIDIUM_CHECK(off.handle() == VK_NULL_HANDLE);
        IRIDIUM_CHECK(off.save());
        return noValidationErrors("persisted pipeline cache");
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
    std::error_code error;
    sharedCacheDirectory = std::filesystem::temp_directory_path(error) /
        ("iridium-pipeline-contract-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(sharedCacheDirectory, error);
    sharedCache = std::make_unique<VulkanPipelineCache>();
    sharedCache->init(sharedDevice->device(),
        VulkanPipelineCacheIdentity::of(sharedDevice->physicalDevice()),
        sharedCacheDirectory);
    constexpr Iridium::Test::TestCase tests[] = {
        { "layered transparency pipelines validate", testLayeredTransparencyPipelines },
        { "material pipeline library validates", testMaterialPipelineLibrary },
        { "shadow, probe and compute pipelines validate", testShadowAndProbePipelines },
        { "persisted pipeline cache saves, reloads warm and discards corruption",
            testPersistedPipelineCache },
    };
    const int result = Iridium::Test::runTests(tests);
    sharedCache.reset();
    sharedDevice.reset();
    std::filesystem::remove_all(sharedCacheDirectory, error);
    return result;
}
