#include "renderer/vulkan/VulkanProductionRenderGraph.h"
#include "renderer/vulkan/VulkanRenderGraphExecutor.h"
#include "renderer/lighting/ClusteredLighting.h"
#include "renderer/rhi/ShadowTypes.h"

#include <cstdint>
#include <array>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <tuple>

namespace {

    using namespace Iridium;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    class FakeResourceFactory final : public VulkanGraphResourceFactory {
    public:
        VulkanGraphPhysicalResource create(
            const RenderGraph::PhysicalResourceSlot& slot) override {
            if (createCount == failureAtCreateCount) {
                throw std::runtime_error("injected allocation failure");
            }

            ++createCount;
            const uintptr_t base = createCount * 8 + 1;
            VulkanGraphPhysicalResource result{};
            result.type = slot.type;
            if (slot.type == RenderGraph::ResourceType::Image) {
                result.image.image = reinterpret_cast<VkImage>(base);
                result.image.memory = reinterpret_cast<VkDeviceMemory>(base + 1);
                result.image.view = reinterpret_cast<VkImageView>(base + 2);
                result.image.allocation.requestedBytes = 4096;
                result.image.allocation.committedBytes = 8192;
            }
            else {
                result.buffer.buffer = reinterpret_cast<VkBuffer>(base);
                result.buffer.memory = reinterpret_cast<VkDeviceMemory>(base + 1);
                result.buffer.allocation.requestedBytes = 4096;
                result.buffer.allocation.committedBytes = 8192;
            }
            return result;
        }

        void destroy(VulkanGraphPhysicalResource& resource) noexcept override {
            if (resource.isValid()) {
                ++destroyCount;
            }
            resource = {};
        }

        size_t createCount = 0;
        size_t destroyCount = 0;
        size_t failureAtCreateCount = std::numeric_limits<size_t>::max();
    };

    bool testAccessAndFormatMappings() {
        const VulkanGraphAccessInfo color = getVulkanGraphAccessInfo(
            RenderGraph::Access::ColorAttachment,
            RenderGraph::ResourceType::Image);
        CHECK(color.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        CHECK((color.stages & VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT) != 0);
        CHECK((color.access & VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) != 0);

        const VulkanGraphAccessInfo sampled = getVulkanGraphAccessInfo(
            RenderGraph::Access::SampledRead, RenderGraph::ResourceType::Image);
        CHECK(sampled.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        CHECK((sampled.access & VK_ACCESS_SHADER_READ_BIT) != 0);

        const VulkanGraphAccessInfo storage = getVulkanGraphAccessInfo(
            RenderGraph::Access::StorageWrite, RenderGraph::ResourceType::Image);
        CHECK(storage.layout == VK_IMAGE_LAYOUT_GENERAL);
        CHECK((storage.access & VK_ACCESS_SHADER_WRITE_BIT) != 0);

        const VulkanGraphAccessInfo present = getVulkanGraphAccessInfo(
            RenderGraph::Access::Present, RenderGraph::ResourceType::Image);
        CHECK(present.layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        const VulkanGraphAccessInfo buffer = getVulkanGraphAccessInfo(
            RenderGraph::Access::StorageRead, RenderGraph::ResourceType::Buffer);
        CHECK(buffer.layout == VK_IMAGE_LAYOUT_UNDEFINED);
        const VulkanGraphAccessInfo readWrite = getVulkanGraphAccessInfo(
            RenderGraph::Access::StorageReadWrite,
            RenderGraph::ResourceType::Buffer);
        CHECK((readWrite.access & VK_ACCESS_SHADER_READ_BIT) != 0);
        CHECK((readWrite.access & VK_ACCESS_SHADER_WRITE_BIT) != 0);

        constexpr RenderGraph::Format formats[] = {
            RenderGraph::Format::Rgba8Unorm,
            RenderGraph::Format::Bgra8Srgb,
            RenderGraph::Format::Rgb10A2Unorm,
            RenderGraph::Format::Rgba16Float,
            RenderGraph::Format::R32Float,
            RenderGraph::Format::D32Float,
        };
        for (const RenderGraph::Format format : formats) {
            CHECK(toGraphFormat(toVkFormat(format)) == format);
        }
        return true;
    }

    bool testProductionTopologyContract() {
        const RenderGraph::CompiledGraph graph = buildVulkanProductionRenderGraph(
            { 3840, 2160 }, VK_FORMAT_B8G8R8A8_SRGB);
        CHECK(graph.passes().size() == 22);
        CHECK(graph.resources().size() == 26);
        CHECK(graph.physicalSlots().size() == 18);
        CHECK(!graph.transitions().empty());
        CHECK(graph.passes().front().name == "shadow.directional");
        CHECK(graph.passes().back().name == "ui-present");
        CHECK(graph.passes()[1].name == "shadow.spot");
        CHECK(graph.passes()[2].name == "shadow.point");
        CHECK(graph.passes()[3].name == "gbuffer");
        CHECK(graph.passes()[4].name == "lighting.cluster.clear");
        CHECK(graph.passes()[8].name == "lighting.cluster.finalize");
        CHECK(graph.passes()[9].name == "lighting.cluster.readback");
        CHECK(graph.passes()[11].name == "forward-opaque");
        CHECK(graph.passes()[12].name == "transparent.background.copy");
        CHECK(graph.passes()[15].name == "transparent.foreground.copy");
        for (size_t passIndex = 4; passIndex <= 8; ++passIndex) {
            CHECK(graph.passes()[passIndex].queue ==
                RenderGraph::QueueClass::Compute);
        }

        const uint64_t clusterCount = 120ull * 68ull *
            kClusterDepthSlices;
        const auto findResource = [&](std::string_view name) {
            return std::find_if(graph.resources().begin(), graph.resources().end(),
                [name](const RenderGraph::CompiledResource& resource) {
                    return resource.name == name;
                });
        };
        const auto headers = findResource(kClusterHeaderResourceName);
        const auto indices = findResource(kClusterIndexResourceName);
        const auto shadow = findResource("shadow.directional");
        const auto spotShadow = findResource("shadow.spot");
        CHECK(shadow != graph.resources().end());
        CHECK(shadow->desc.lifetime ==
            RenderGraph::ResourceLifetime::External);
        CHECK(shadow->desc.imported);
        CHECK(shadow->desc.image.arrayLayers ==
            kDirectionalShadowLayerCount);
        CHECK(shadow->desc.image.extent.width == 4096);
        CHECK(shadow->desc.image.extent.height == 4096);
        CHECK(shadow->physicalSlot == RenderGraph::InvalidIndex);
        CHECK(spotShadow != graph.resources().end());
        CHECK(spotShadow->desc.lifetime ==
            RenderGraph::ResourceLifetime::External);
        CHECK(spotShadow->desc.imported);
        CHECK(spotShadow->desc.image.extent.width == 8192);
        CHECK(spotShadow->desc.image.extent.height == 8192);
        CHECK(spotShadow->physicalSlot == RenderGraph::InvalidIndex);
        for (const auto [name, resolution, capacity] : std::array{
                std::tuple{ "shadow.point.256", 256u,
                    kPointShadowPool256Capacity },
                std::tuple{ "shadow.point.512", 512u,
                    kPointShadowPool512Capacity },
                std::tuple{ "shadow.point.1024", 1024u,
                    kPointShadowPool1024Capacity } }) {
            const auto pointShadow = findResource(name);
            CHECK(pointShadow != graph.resources().end());
            CHECK(pointShadow->desc.lifetime ==
                RenderGraph::ResourceLifetime::External);
            CHECK(pointShadow->desc.imported);
            CHECK(pointShadow->desc.image.extent.width == resolution);
            CHECK(pointShadow->desc.image.arrayLayers == capacity * 6u);
            CHECK(pointShadow->physicalSlot == RenderGraph::InvalidIndex);
        }
        CHECK(headers != graph.resources().end());
        CHECK(headers->desc.type == RenderGraph::ResourceType::Buffer);
        CHECK(headers->desc.buffer.size == clusterCount *
            sizeof(ClusterLightHeader));
        CHECK(indices != graph.resources().end());
        CHECK(indices->desc.buffer.size ==
            static_cast<uint64_t>(kMaximumClusterLightReferences) *
                sizeof(uint32_t));
        CHECK((headers->usages & RenderGraph::usageBit(
            RenderGraph::Access::StorageRead)) != 0);
        CHECK((headers->usages & RenderGraph::usageBit(
            RenderGraph::Access::StorageReadWrite)) != 0);
        ClusterGridConfig coarseConfig{};
        coarseConfig.tileWidth = coarseConfig.tileHeight = 32;
        coarseConfig.depthSlices = 32;
        const RenderGraph::CompiledGraph coarse = buildVulkanProductionRenderGraph(
            { 3840, 2160 }, VK_FORMAT_B8G8R8A8_SRGB,
            VK_FORMAT_B8G8R8A8_SRGB, false,
            GBufferLayout::CanonicalReference, coarseConfig);
        const auto coarseHeaders = std::find_if(coarse.resources().begin(),
            coarse.resources().end(), [](const RenderGraph::CompiledResource& resource) {
                return resource.name == kClusterHeaderResourceName;
            });
        CHECK(coarseHeaders != coarse.resources().end());
        CHECK(coarseHeaders->desc.buffer.size ==
            120ull * 68ull * 32ull * sizeof(ClusterLightHeader));
        CHECK(coarse.topologyHash() != graph.topologyHash());

        const RenderGraph::CompiledResource& swapchain = graph.resources().back();
        CHECK(swapchain.desc.lifetime == RenderGraph::ResourceLifetime::External);
        CHECK(swapchain.desc.imported);
        CHECK(swapchain.physicalSlot == RenderGraph::InvalidIndex);
        CHECK(swapchain.exported);
        CHECK(swapchain.finalAccess == RenderGraph::Access::Present);
        uint32_t opaqueCopySlot = RenderGraph::InvalidIndex;
        bool reusesExpiredGBufferSlot = false;
        for (const RenderGraph::CompiledResource& resource : graph.resources()) {
            if (resource.name == "scene.opaque-copy") {
                opaqueCopySlot = resource.physicalSlot;
            }
        }
        for (const RenderGraph::CompiledResource& resource : graph.resources()) {
            if (resource.name.starts_with("gbuffer.") &&
                resource.physicalSlot == opaqueCopySlot) {
                reusesExpiredGBufferSlot = true;
            }
        }
        CHECK(opaqueCopySlot != RenderGraph::InvalidIndex);
        CHECK(reusesExpiredGBufferSlot);
        const auto output = std::find_if(graph.resources().begin(),
            graph.resources().end(), [](const RenderGraph::CompiledResource& resource) {
                return resource.name == "output.display";
            });
        CHECK(output != graph.resources().end());
        CHECK(output->desc.image.format == RenderGraph::Format::Bgra8Srgb);
        CHECK((output->usages & RenderGraph::usageBit(
            RenderGraph::Access::TransferSource)) != 0);
        return true;
    }

    bool testHdr10TopologyContract() {
        const RenderGraph::CompiledGraph graph = buildVulkanProductionRenderGraph(
            { 3840, 2160 }, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
            VK_FORMAT_R16G16B16A16_SFLOAT, true);
        CHECK(graph.passes().size() == 23);
        CHECK(graph.resources().size() == 27);
        CHECK(graph.passes()[21].name == "ui-compose");
        CHECK(graph.passes().back().name == "hdr10-encode-present");
        const auto composition = std::find_if(graph.resources().begin(),
            graph.resources().end(), [](const RenderGraph::CompiledResource& resource) {
                return resource.name == "output.ui-composition";
            });
        CHECK(composition != graph.resources().end());
        CHECK(composition->desc.image.format == RenderGraph::Format::Rgba16Float);
        CHECK((composition->usages & RenderGraph::usageBit(
            RenderGraph::Access::ColorAttachment)) != 0);
        CHECK((composition->usages & RenderGraph::usageBit(
            RenderGraph::Access::SampledRead)) != 0);
        CHECK(graph.resources().back().desc.image.format ==
            RenderGraph::Format::Rgb10A2Unorm);
        return true;
    }

    bool testSceneAndPresentationExtentSeparation() {
        constexpr VkExtent2D sceneExtent{ 1728, 972 };
        constexpr VkExtent2D presentationExtent{ 3840, 2160 };
        const auto findResource = [](const RenderGraph::CompiledGraph& graph,
                                     std::string_view name) {
            return std::find_if(graph.resources().begin(), graph.resources().end(),
                [name](const RenderGraph::CompiledResource& resource) {
                    return resource.name == name;
                });
        };
        const auto hasExtent = [](const RenderGraph::CompiledResource& resource,
                                  VkExtent2D extent) {
            return resource.desc.image.extent.width == extent.width &&
                resource.desc.image.extent.height == extent.height;
        };

        const RenderGraph::CompiledGraph sdr = buildVulkanProductionRenderGraph(
            sceneExtent, presentationExtent, VK_FORMAT_B8G8R8A8_SRGB);
        for (const std::string_view name : {
                 "gbuffer.albedo", "gbuffer.normal", "gbuffer.emissive",
                 "gbuffer.f0-roughness", "gbuffer.material-flags",
                 "depth.opaque", "scene.color", "scene.opaque-copy",
                 "depth.glass", "output.display" }) {
            const auto resource = findResource(sdr, name);
            CHECK(resource != sdr.resources().end());
            CHECK(hasExtent(*resource, sceneExtent));
        }
        const auto sdrSwapchain = findResource(sdr, "swapchain");
        CHECK(sdrSwapchain != sdr.resources().end());
        CHECK(hasExtent(*sdrSwapchain, presentationExtent));

        const RenderGraph::CompiledGraph hdr = buildVulkanProductionRenderGraph(
            sceneExtent, presentationExtent,
            VK_FORMAT_A2B10G10R10_UNORM_PACK32,
            VK_FORMAT_R16G16B16A16_SFLOAT, true);
        const auto hdrOutput = findResource(hdr, "output.display");
        const auto hdrComposition = findResource(hdr, "output.ui-composition");
        const auto hdrSwapchain = findResource(hdr, "swapchain");
        CHECK(hdrOutput != hdr.resources().end());
        CHECK(hdrComposition != hdr.resources().end());
        CHECK(hdrSwapchain != hdr.resources().end());
        CHECK(hasExtent(*hdrOutput, sceneExtent));
        CHECK(hasExtent(*hdrComposition, presentationExtent));
        CHECK(hasExtent(*hdrSwapchain, presentationExtent));
        return true;
    }

    bool testFenceScopedRetirementAndCleanup() {
        const RenderGraph::CompiledGraph graph = buildVulkanProductionRenderGraph(
            { 1920, 1080 }, VK_FORMAT_B8G8R8A8_SRGB);
        const size_t slotCount = graph.physicalSlots().size();
        FakeResourceFactory factory;
        VulkanGraphResourcePool pool;
        pool.init(factory, 2);
        pool.rebuild(graph);
        CHECK(factory.createCount == slotCount * 2);
        CHECK(pool.activeResourceCount(0) == slotCount);
        CHECK(pool.activeResourceCount(1) == slotCount);
        CHECK(pool.retiredResourceCount(0) == 0);
        CHECK(pool.requestedBytes() == slotCount * 2 * 4096);
        CHECK(pool.committedBytes() == slotCount * 2 * 8192);

        pool.rebuild(graph);
        CHECK(pool.retiredResourceCount(0) == slotCount);
        CHECK(pool.retiredResourceCount(1) == slotCount);
        pool.onFrameFenceCompleted(0);
        CHECK(factory.destroyCount == slotCount);
        CHECK(pool.retiredResourceCount(0) == 0);
        CHECK(pool.retiredResourceCount(1) == slotCount);

        pool.cleanupAfterDeviceIdle();
        CHECK(factory.destroyCount == slotCount * 4);
        CHECK(pool.frameCount() == 0);
        return true;
    }

    bool testAllocationFailurePreservesActiveSet() {
        const RenderGraph::CompiledGraph graph = buildVulkanProductionRenderGraph(
            { 1280, 720 }, VK_FORMAT_B8G8R8A8_SRGB);
        const size_t slotCount = graph.physicalSlots().size();
        FakeResourceFactory factory;
        VulkanGraphResourcePool pool;
        pool.init(factory, 2);
        pool.rebuild(graph);
        const size_t firstBuildCount = factory.createCount;
        factory.failureAtCreateCount = firstBuildCount + 3;

        bool threw = false;
        try {
            pool.rebuild(graph);
        }
        catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(factory.destroyCount == 3);
        CHECK(pool.activeResourceCount(0) == slotCount);
        CHECK(pool.activeResourceCount(1) == slotCount);
        CHECK(pool.retiredResourceCount(0) == 0);
        CHECK(pool.retiredResourceCount(1) == 0);

        pool.cleanupAfterDeviceIdle();
        CHECK(factory.destroyCount == 3 + slotCount * 2);
        return true;
    }

    bool testResizeAndBounds() {
        const RenderGraph::CompiledGraph small = buildVulkanProductionRenderGraph(
            { 1280, 720 }, VK_FORMAT_B8G8R8A8_SRGB);
        const RenderGraph::CompiledGraph large = buildVulkanProductionRenderGraph(
            { 3840, 2160 }, VK_FORMAT_B8G8R8A8_SRGB);
        CHECK(small.topologyHash() != large.topologyHash());
        CHECK(small.physicalSlots().front().image.extent.width == 1280);
        CHECK(large.physicalSlots().front().image.extent.width == 3840);

        FakeResourceFactory factory;
        VulkanGraphResourcePool pool;
        pool.init(factory, 2);
        pool.rebuild(small);
        pool.rebuild(large);
        CHECK(pool.activeResourceCount(0) == large.physicalSlots().size());

        bool threw = false;
        try {
            pool.onFrameFenceCompleted(2);
        }
        catch (const std::out_of_range&) {
            threw = true;
        }
        CHECK(threw);
        pool.cleanupAfterDeviceIdle();
        return true;
    }

    bool testExecutorCacheBarriersAndStats() {
        RenderGraph::CompiledGraph graph = buildVulkanProductionRenderGraph(
            { 1920, 1080 }, VK_FORMAT_B8G8R8A8_SRGB);
        const size_t transitionCount = graph.transitions().size();
        FakeResourceFactory factory;
        VulkanRenderGraphExecutor executor;
        executor.init(factory, 2);
        executor.rebuild(std::move(graph));

        const VulkanGraphStats stats = executor.stats();
        CHECK(stats.enabled);
        CHECK(stats.passCount == 22);
        CHECK(stats.logicalResourceCount == 26);
        CHECK(stats.physicalSlotCount == 18);
        CHECK(stats.barrierCount == transitionCount);
        CHECK(stats.frameCount == 2);
        CHECK(stats.rebuildCount == 1);
        CHECK(stats.cacheMissCount == 0);
        CHECK(executor.barriers().size() == transitionCount);
        bool foundDepthSampledRead = false;
        for (const VulkanGraphBarrierIntent& barrier : executor.barriers()) {
            if (barrier.after.layout ==
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) {
                CHECK((barrier.after.access & VK_ACCESS_SHADER_READ_BIT) != 0);
                foundDepthSampledRead = true;
            }
        }
        CHECK(foundDepthSampledRead);
        CHECK(executor.validateFrame(0));
        CHECK(executor.validateFrame(1));
        CHECK(!executor.validateFrame(2));
        CHECK(executor.stats().cacheMissCount == 0);

        constexpr std::string_view passNames[] = {
            "shadow.directional",
            "shadow.spot",
            "shadow.point",
            "gbuffer",
            "lighting.cluster.clear",
            "lighting.cluster.count",
            "lighting.cluster.scan",
            "lighting.cluster.fill",
            "lighting.cluster.finalize",
            "lighting.cluster.readback",
            "lighting",
            "forward-opaque",
            "transparent.background.copy",
            "transparent.background.depth",
            "transparent.background.forward",
            "transparent.foreground.copy",
            "transparent.foreground.depth",
            "transparent.foreground.forward",
            "bloom-hook",
            "output-transform",
            "final-capture-hook",
            "ui-present",
        };
        executor.beginFrameExecution(0);
        for (const std::string_view passName : passNames) {
            executor.skipPass(passName);
        }
        executor.finishFrameExecution();

        bool orderRejected = false;
        executor.beginFrameExecution(1);
        try {
            executor.skipPass("lighting");
        }
        catch (const std::logic_error&) {
            orderRejected = true;
        }
        CHECK(orderRejected);
        for (const std::string_view passName : passNames) {
            executor.skipPass(passName);
        }
        executor.finishFrameExecution();

        executor.cleanupAfterDeviceIdle();
        CHECK(factory.destroyCount == stats.physicalSlotCount * stats.frameCount);
        CHECK(!executor.stats().enabled);
        CHECK(executor.stats().rebuildCount == 1);
        return true;
    }

    bool testNamedBufferAccess() {
        RenderGraph::RenderGraphBuilder builder;
        RenderGraph::ResourceDesc desc{};
        desc.type = RenderGraph::ResourceType::Buffer;
        desc.buffer.size = 256;
        desc.buffer.alignment = 16;
        auto buffer = builder.createResource("lighting.cluster.headers", desc);
        const auto pass = builder.addPass("cluster-build",
            RenderGraph::QueueClass::Compute);
        buffer = builder.write(pass, buffer, RenderGraph::Access::StorageWrite);
        const auto compiled = builder.compile();
        CHECK(compiled.succeeded());
        FakeResourceFactory factory;
        VulkanRenderGraphExecutor executor;
        executor.init(factory, 2);
        executor.rebuild(*compiled.graph);
        CHECK(executor.bufferResource(0, "lighting.cluster.headers").isValid());
        CHECK(executor.bufferResource(1, "lighting.cluster.headers").isValid());
        bool rejectedImage = false;
        try {
            (void)executor.imageResource(0, "lighting.cluster.headers");
        }
        catch (const std::out_of_range&) {
            rejectedImage = true;
        }
        CHECK(rejectedImage);
        executor.cleanupAfterDeviceIdle();
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        std::string_view name;
        bool (*function)();
    };
    constexpr TestCase tests[] = {
        { "access and format mappings", testAccessAndFormatMappings },
        { "production topology contract", testProductionTopologyContract },
        { "HDR10 topology contract", testHdr10TopologyContract },
        { "scene and presentation extent separation",
            testSceneAndPresentationExtentSeparation },
        { "fence-scoped retirement and cleanup", testFenceScopedRetirementAndCleanup },
        { "allocation failure preserves active set", testAllocationFailurePreservesActiveSet },
        { "resize and bounds", testResizeAndBounds },
        { "executor cache barriers and stats", testExecutorCacheBarriersAndStats },
        { "named buffer access", testNamedBufferAccess },
    };

    size_t passed = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.function()) {
                ++passed;
                std::cout << "[pass] " << test.name << '\n';
            }
            else {
                std::cout << "[fail] " << test.name << '\n';
            }
        }
        catch (const std::exception& exception) {
            std::cout << "[fail] " << test.name << ": " << exception.what() << '\n';
        }
    }
    std::cout << passed << '/' << std::size(tests) << " tests passed\n";
    return passed == std::size(tests) ? 0 : 1;
}
