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
        allocator.init(gpu.instance(), gpu.physicalDevice(), gpu.device(),
            gpu.hasMemoryBudget());
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

    // History pairs on real images: alternating slots, an invalid previous
    // discarded from UNDEFINED (reset revision), and a skipped writer.
    bool testHistoryPairBarriers() {
        HeadlessVulkanDevice& gpu = *sharedDevice;
        gpu.resetValidationErrors();
        RenderGraph::RenderGraphBuilder builder;
        RenderGraph::ResourceDesc desc{};
        desc.image.format = RenderGraph::Format::Rgba16Float;
        desc.image.extent = { 256, 128, 1 };
        const auto history = builder.createHistory("taa", desc);
        auto scene = builder.createResource("scene", desc);
        auto output = builder.createResource("output", desc);
        const auto draw = builder.addPass("scene");
        const auto resolve = builder.addPass("resolve");
        const auto post = builder.addPass("post");
        scene = builder.write(draw, scene, RenderGraph::Access::ColorAttachment,
            RenderGraph::LoadOp::Clear);
        builder.read(resolve, scene, RenderGraph::Access::SampledRead);
        builder.read(resolve, history.previous, RenderGraph::Access::SampledRead);
        const auto current = builder.write(resolve, history.current,
            RenderGraph::Access::ColorAttachment, RenderGraph::LoadOp::Clear);
        builder.read(post, current, RenderGraph::Access::TransferSource);
        output = builder.write(post, output, RenderGraph::Access::TransferDestination);
        // An image view needs a view-compatible usage besides TRANSFER_DST.
        builder.read(builder.addPass("display"), output, RenderGraph::Access::SampledRead);
        builder.exportResource(output, RenderGraph::Access::SampledRead);
        auto compiled = builder.compile();
        IRIDIUM_CHECK(compiled.succeeded());

        VulkanResourceAllocator allocator;
        allocator.init(gpu.instance(), gpu.physicalDevice(), gpu.device(),
            gpu.hasMemoryBudget());
        {
            VulkanRenderGraphExecutor executor;
            executor.init(allocator, 2);
            executor.rebuild(std::move(*compiled.graph));
            const RenderGraph::ViewHistoryContext views[] = {
                { 1, 0 }, { 1, 0 }, { 1, 1 }, { 1, 1 }, { 1, 1 }, { 1, 1 }, { 2, 1 }, { 2, 1 } };
            for (uint32_t frame = 0; frame < std::size(views); ++frame) {
                const uint32_t slot = frame % 2;
                executor.onFrameFenceCompleted(slot);
                executor.beginFrameExecution(slot, views[frame]);
                gpu.submitAndWait([&](VkCommandBuffer commandBuffer) {
                    executor.beginPass(commandBuffer, executor.passId("scene"));
                    if (frame == 4) {
                        executor.skipPass(executor.passId("resolve"));
                        executor.skipPass(executor.passId("post"));
                    }
                    else {
                        executor.beginPass(commandBuffer, executor.passId("resolve"));
                        executor.beginPass(commandBuffer, executor.passId("post"));
                    }
                    executor.beginPass(commandBuffer, executor.passId("display"));
                });
                executor.finishFrameExecution();
            }
            executor.cleanupAfterDeviceIdle();
        }
        allocator.cleanup();
        IRIDIUM_CHECK_MSG(gpu.validationErrors() == 0,
            gpu.validationErrors() << " validation errors");
        return true;
    }

    // M7R R4a.0: passes flagged for dynamic rendering begin and end rendering
    // through their plans on real images: clear, then loads of the same
    // colour and depth attachments (ordered only by the same-access
    // re-barrier), read-only depth with STORE_OP_NONE, a discard-on-first-use
    // import written from UNDEFINED, and its frame-end export transition.
    struct DynamicRenderingOwner {
        uint32_t scopes = 0;
    };

    bool runDynamicRenderingFrames(bool forceSynchronization1) {
        HeadlessVulkanDevice& gpu = *sharedDevice;
        gpu.resetValidationErrors();
        using RenderGraph::Access;
        using RenderGraph::LoadOp;
        using RenderGraph::StoreOp;
        using RenderGraph::ClearValue;
        RenderGraph::RenderGraphBuilder builder;
        RenderGraph::ResourceDesc colorDesc{};
        colorDesc.image.format = RenderGraph::Format::Rgba16Float;
        colorDesc.image.extent = { 256, 128, 1 };
        RenderGraph::ResourceDesc depthDesc = colorDesc;
        depthDesc.image.format = RenderGraph::Format::D32Float;
        RenderGraph::ResourceDesc targetDesc = colorDesc;
        targetDesc.image.format = RenderGraph::Format::Rgba8Unorm;
        targetDesc.lifetime = RenderGraph::ResourceLifetime::External;
        targetDesc.imported = true;
        targetDesc.initialAccess = Access::SampledRead;
        auto color = builder.createResource("color", colorDesc);
        auto depth = builder.createResource("depth", depthDesc);
        auto target = builder.createResource("target", targetDesc);
        const auto clear = builder.addPass("clear");
        const auto overdraw = builder.addPass("overdraw");
        const auto readOnly = builder.addPass("read-only-depth");
        const auto sample = builder.addPass("sample");
        const auto present = builder.addPass("present");
        color = builder.write(clear, color, Access::ColorAttachment, LoadOp::Clear,
            StoreOp::Store, ClearValue::color(0.25f, 0.5f, 0.75f, 1.0f));
        depth = builder.write(clear, depth, Access::DepthAttachmentWrite, LoadOp::Clear,
            StoreOp::Store, ClearValue::depthStencil(1.0f));
        color = builder.write(overdraw, color, Access::ColorAttachment, LoadOp::Load);
        depth = builder.write(overdraw, depth, Access::DepthAttachmentWrite, LoadOp::Load);
        builder.read(readOnly, depth, Access::DepthAttachmentRead);
        color = builder.write(readOnly, color, Access::ColorAttachment, LoadOp::Load);
        builder.read(sample, color, Access::SampledRead);
        builder.read(sample, depth, Access::SampledRead);
        target = builder.write(present, target, Access::ColorAttachment, LoadOp::Clear,
            StoreOp::Store, ClearValue::color(0.0f, 0.0f, 0.0f, 1.0f));
        builder.exportResource(target, Access::SampledRead);
        auto compiled = builder.compile();
        IRIDIUM_CHECK(compiled.succeeded());

        VulkanResourceAllocator allocator;
        allocator.init(gpu.physicalDevice(), gpu.device(), gpu.hasMemoryBudget());
        VulkanImageResource targetImage = allocator.createImage2D({ 256, 128 },
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
        DynamicRenderingOwner owner;
        {
            VulkanRenderGraphExecutor executor;
            executor.init(allocator, 2);
            if (forceSynchronization1) executor.setBarrierApi(VulkanBarrierApi::Synchronization1);
            executor.rebuild(std::move(*compiled.graph));
            executor.bindExternalImage(VulkanGlobalBinding, executor.resourceId("target"),
                targetImage, Access::SampledRead, ExternalSyncPolicy::discardOnFirstUse());
            VulkanPassCallbacks callbacks{};
            callbacks.owner = &owner;
            callbacks.dynamicRendering = true;
            callbacks.execute = [](void* self, VulkanPassContext& context) {
                context.beginRendering();
                context.endRendering();
                ++static_cast<DynamicRenderingOwner*>(self)->scopes;
            };
            for (const char* pass : { "clear", "overdraw", "read-only-depth", "present" })
                executor.registerPass(executor.passId(pass), callbacks);
            for (uint32_t frame = 0; frame < 4; ++frame) {
                const uint32_t slot = frame % 2;
                executor.onFrameFenceCompleted(slot);
                IRIDIUM_CHECK(executor.validateFrame(slot));
                executor.beginFrameExecution(slot);
                gpu.submitAndWait([&](VkCommandBuffer commandBuffer) {
                    executor.beginPass(commandBuffer, executor.passId("sample"));
                    // Drains "present", then the export to SampledRead.
                    executor.finishFrameExecution();
                });
            }
            IRIDIUM_CHECK(executor.externalImageAccess(VulkanGlobalBinding,
                executor.resourceId("target")) == Access::SampledRead);
            executor.cleanupAfterDeviceIdle();
        }
        allocator.destroy(targetImage);
        allocator.cleanup();
        IRIDIUM_CHECK(owner.scopes == 16);
        IRIDIUM_CHECK_MSG(gpu.validationErrors() == 0,
            gpu.validationErrors() << " validation errors");
        return true;
    }

    bool testDynamicRenderingThroughPlans() {
        if (!sharedDevice->hasDynamicRendering()) {
            std::cout << "  dynamic rendering unsupported; skipped\n";
            return true;
        }
        return runDynamicRenderingFrames(false) && runDynamicRenderingFrames(true);
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
        << ", dynamic rendering: " << (sharedDevice->hasDynamicRendering() ? "yes" : "no")
        << '\n';
    constexpr Iridium::Test::TestCase tests[] = {
        { "base topology barriers validate", testBaseTopology },
        { "all-features topology barriers validate", testAllFeaturesTopology },
        { "History pair barriers validate", testHistoryPairBarriers },
        { "dynamic rendering through plans validates", testDynamicRenderingThroughPlans },
    };
    const int result = Iridium::Test::runTests(tests);
    sharedDevice.reset();
    return result;
}
