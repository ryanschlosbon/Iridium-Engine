// M7R R3b: the graph executor's recorded barriers on a real device under the
// Khronos validation layer with synchronization validation. Production
// topologies are executed with graph-owned images and buffers (no draws), over
// four frames and two frame slots with skipped passes, through both the
// batched vkCmdPipelineBarrier2 path and the sync1 fallback. Validation checks
// every recorded layout transition against the image's actual layout, the
// usage flags each layout needs, and the barriers' stage/access scopes.

#include "HeadlessVulkanDevice.h"
#include "TestHarness.h"

#include "renderer/vulkan/VulkanProductionRenderGraph.h"
#include "renderer/vulkan/VulkanRenderGraphExecutor.h"
#include "renderer/vulkan/VulkanResourceAllocator.h"

#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace {

    using namespace Iridium;
    using Iridium::Test::HeadlessVulkanDevice;

    std::unique_ptr<HeadlessVulkanDevice> sharedDevice;

    bool alwaysRecorded(std::string_view name) {
        return name == "gbuffer" || name == "lighting" || name == "forward-opaque" ||
            name == "output-transform" || name == "ui-present" || name == "ui-compose" ||
            name == "hdr10-encode-present" ||
            (name.starts_with("lighting.cluster.") && name != "lighting.cluster.readback") ||
            name.starts_with("shadow.virtual.");
    }

    bool runs(std::string_view name, uint32_t frame, uint32_t pass) {
        if (alwaysRecorded(name)) return true;
        switch (frame) {
        case 0: return true;
        case 1: return false;
        case 2: return pass % 2 == 0;
        default: return pass % 3 != 1;
        }
    }

    bool executeTopology(const char* label, RenderGraph::CompiledGraph graph,
        bool forceSynchronization1) {
        HeadlessVulkanDevice& gpu = *sharedDevice;
        gpu.resetValidationErrors();
        constexpr uint32_t FrameSlots = 2;
        VulkanResourceAllocator allocator;
        allocator.init(gpu.physicalDevice(), gpu.device(), gpu.hasMemoryBudget());
        Test::HeadlessVulkanBuffer workingSets[FrameSlots]{};
        {
            VulkanRenderGraphExecutor executor;
            executor.init(allocator, FrameSlots);
            IRIDIUM_CHECK(executor.barrierApi() == (gpu.hasSynchronization2()
                ? VulkanBarrierApi::Synchronization2 : VulkanBarrierApi::Synchronization1));
            if (forceSynchronization1) executor.setBarrierApi(VulkanBarrierApi::Synchronization1);
            executor.rebuild(std::move(graph));
            const auto vsm = executor.findResource("shadow.virtual.working-set");
            if (vsm.isValid()) {
                for (uint32_t frame = 0; frame < FrameSlots; ++frame) {
                    workingSets[frame] = gpu.createHostBuffer(8'192,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT);
                    executor.bindExternalBuffer(frame, vsm, workingSets[frame].buffer, 8'192);
                }
            }
            const RenderGraph::CompiledGraph& compiled = *executor.compiledGraph();
            for (uint32_t frame = 0; frame < 4; ++frame) {
                const uint32_t slot = frame % FrameSlots;
                executor.onFrameFenceCompleted(slot);
                IRIDIUM_CHECK(executor.validateFrame(slot));
                executor.beginFrameExecution(slot);
                gpu.submitAndWait([&](VkCommandBuffer commandBuffer) {
                    for (uint32_t pass = 0; pass < compiled.passes().size(); ++pass) {
                        if (runs(compiled.passes()[pass].name, frame, pass))
                            executor.beginPass(commandBuffer, RenderGraph::PassId{ pass });
                        else
                            executor.skipPass(RenderGraph::PassId{ pass });
                    }
                });
                executor.finishFrameExecution();
            }
            executor.cleanupAfterDeviceIdle();
        }
        for (auto& buffer : workingSets) gpu.destroy(buffer);
        allocator.cleanup();
        IRIDIUM_CHECK_MSG(gpu.validationErrors() == 0, label << ": "
            << gpu.validationErrors() << " validation errors");
        return true;
    }

    RenderGraph::CompiledGraph build(bool hdr10, VulkanLayeredGraphConfig layered,
        VulkanProductionGraphFeatures features) {
        constexpr VkExtent2D Scene{ 1920, 1080 };
        return buildVulkanProductionRenderGraph(Scene, Scene,
            hdr10 ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_B8G8R8A8_SRGB,
            hdr10 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_B8G8R8A8_SRGB,
            hdr10, GBufferLayout::CanonicalReference, {}, 4096, 8192, true,
            layered, features);
    }

    bool testBaseTopology() {
        return executeTopology("base SDR", build(false, {}, {}), false) &&
            executeTopology("base SDR sync1", build(false, {}, {}), true);
    }

    bool testAllFeaturesTopology() {
        const VulkanLayeredGraphConfig layered{ { 960, 528 }, { 1920, 528 },
            { 1920, 1072 }, true };
        const VulkanProductionGraphFeatures features{ .depthPyramid = true,
            .virtualShadowWorkingSetBytes = 8'192 };
        return executeTopology("all features HDR10", build(true, layered, features), false) &&
            executeTopology("all features sync1", build(false, layered, features), true);
    }

} // namespace

int main() {
    try {
        sharedDevice = std::make_unique<HeadlessVulkanDevice>(HeadlessVulkanDevice::Options{
            "Iridium render-graph barriers", true });
    } catch (const std::exception& exception) {
        std::cerr << "headless Vulkan device unavailable: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "synchronization2: " << (sharedDevice->hasSynchronization2() ? "yes" : "no")
        << '\n';
    constexpr Iridium::Test::TestCase tests[] = {
        { "base topology barriers validate", testBaseTopology },
        { "all-features topology barriers validate", testAllFeaturesTopology },
    };
    const int result = Iridium::Test::runTests(tests);
    sharedDevice.reset();
    return result;
}
