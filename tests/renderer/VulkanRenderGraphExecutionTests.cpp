// M7R R3b executor tests: index addressing and the barrier sink. All
// recording goes through a VulkanBarrierSink against fake resources, so no
// device is needed.

#include "renderer/vulkan/VulkanProductionRenderGraph.h"
#include "renderer/vulkan/VulkanRenderGraphExecutor.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

    using namespace Iridium;
    using RenderGraph::Access;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    const VkCommandBuffer FakeCommandBuffer =
        reinterpret_cast<VkCommandBuffer>(uintptr_t{ 0xC0 });

    // Distinct deterministic handles with the aspect, mip and layer counts the
    // allocator factory would give each slot.
    class FakeResourceFactory final : public VulkanGraphResourceFactory {
    public:
        VulkanGraphPhysicalResource create(
            const RenderGraph::PhysicalResourceSlot& slot) override {
            ++createCount;
            const uintptr_t base = 0x10000 + createCount * 8;
            VulkanGraphPhysicalResource result{};
            result.type = slot.type;
            if (slot.type == RenderGraph::ResourceType::Image) {
                result.image.image = reinterpret_cast<VkImage>(base);
                result.image.memory = reinterpret_cast<VkDeviceMemory>(base + 1);
                result.image.view = reinterpret_cast<VkImageView>(base + 2);
                result.image.format = toVkFormat(slot.image.format);
                result.image.extent = { slot.image.extent.width, slot.image.extent.height };
                result.image.aspect = result.image.format == VK_FORMAT_D32_SFLOAT
                    ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
                result.image.mipLevels = slot.image.mipLevels;
                result.image.arrayLayers = slot.image.arrayLayers;
                result.image.allocation.requestedBytes = 4096;
                result.image.allocation.committedBytes = 4096;
            }
            else {
                result.buffer.buffer = reinterpret_cast<VkBuffer>(base);
                result.buffer.memory = reinterpret_cast<VkDeviceMemory>(base + 1);
                result.buffer.size = slot.buffer.size;
                result.buffer.allocation.requestedBytes = 1024;
                result.buffer.allocation.committedBytes = 1024;
            }
            return result;
        }

        void destroy(VulkanGraphPhysicalResource& resource) noexcept override {
            if (resource.isValid()) ++destroyCount;
            resource = {};
        }

        size_t createCount = 0;
        size_t destroyCount = 0;
    };

    // One recorded barrier, normalized so sync1 and sync2 recordings compare:
    // sync1 TOP_OF_PIPE sources are recorded as NONE (their sync2 equivalent).
    struct RecordedBarrier {
        uint32_t call = 0;           // index of the vkCmdPipelineBarrier* call
        uint64_t handle = 0;         // VkImage or VkBuffer
        bool image = false;
        VkPipelineStageFlags2 srcStages = 0;
        VkAccessFlags2 srcAccess = 0;
        VkPipelineStageFlags2 dstStages = 0;
        VkAccessFlags2 dstAccess = 0;
        VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageSubresourceRange range{};
        VkDeviceSize offset = 0;
        VkDeviceSize size = 0;

        [[nodiscard]] bool sameDependency(const RecordedBarrier& other) const noexcept {
            return handle == other.handle && image == other.image &&
                srcStages == other.srcStages && srcAccess == other.srcAccess &&
                dstStages == other.dstStages && dstAccess == other.dstAccess &&
                oldLayout == other.oldLayout && newLayout == other.newLayout &&
                range.aspectMask == other.range.aspectMask &&
                range.baseMipLevel == other.range.baseMipLevel &&
                range.levelCount == other.range.levelCount &&
                range.baseArrayLayer == other.range.baseArrayLayer &&
                range.layerCount == other.range.layerCount &&
                offset == other.offset && size == other.size;
        }
    };

    VkPipelineStageFlags2 normalizedSource(VkPipelineStageFlags stages) noexcept {
        return stages == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
            ? VK_PIPELINE_STAGE_2_NONE
            : static_cast<VkPipelineStageFlags2>(stages);
    }

    class RecordingBarrierSink final : public VulkanBarrierSink {
    public:
        void pipelineBarrier(VkCommandBuffer commandBuffer,
            VkPipelineStageFlags sourceStages, VkPipelineStageFlags destinationStages,
            std::span<const VkBufferMemoryBarrier> buffers,
            std::span<const VkImageMemoryBarrier> images) override {
            if (commandBuffer != FakeCommandBuffer) ++wrongCommandBuffer;
            ++sync1Calls;
            for (const VkBufferMemoryBarrier& value : buffers) {
                RecordedBarrier barrier{};
                barrier.call = calls;
                barrier.handle = reinterpret_cast<uint64_t>(value.buffer);
                barrier.srcStages = normalizedSource(sourceStages);
                barrier.srcAccess = value.srcAccessMask;
                barrier.dstStages = destinationStages;
                barrier.dstAccess = value.dstAccessMask;
                barrier.offset = value.offset;
                barrier.size = value.size;
                record(barrier);
            }
            for (const VkImageMemoryBarrier& value : images) {
                RecordedBarrier barrier{};
                barrier.call = calls;
                barrier.image = true;
                barrier.handle = reinterpret_cast<uint64_t>(value.image);
                barrier.srcStages = normalizedSource(sourceStages);
                barrier.srcAccess = value.srcAccessMask;
                barrier.dstStages = destinationStages;
                barrier.dstAccess = value.dstAccessMask;
                barrier.oldLayout = value.oldLayout;
                barrier.newLayout = value.newLayout;
                barrier.range = value.subresourceRange;
                record(barrier);
            }
            ++calls;
        }

        void pipelineBarrier2(VkCommandBuffer commandBuffer,
            const VkDependencyInfo& dependency) override {
            if (commandBuffer != FakeCommandBuffer) ++wrongCommandBuffer;
            ++sync2Calls;
            for (uint32_t index = 0; index < dependency.bufferMemoryBarrierCount; ++index) {
                const VkBufferMemoryBarrier2& value = dependency.pBufferMemoryBarriers[index];
                RecordedBarrier barrier{};
                barrier.call = calls;
                barrier.handle = reinterpret_cast<uint64_t>(value.buffer);
                barrier.srcStages = value.srcStageMask;
                barrier.srcAccess = value.srcAccessMask;
                barrier.dstStages = value.dstStageMask;
                barrier.dstAccess = value.dstAccessMask;
                barrier.offset = value.offset;
                barrier.size = value.size;
                record(barrier);
            }
            for (uint32_t index = 0; index < dependency.imageMemoryBarrierCount; ++index) {
                const VkImageMemoryBarrier2& value = dependency.pImageMemoryBarriers[index];
                RecordedBarrier barrier{};
                barrier.call = calls;
                barrier.image = true;
                barrier.handle = reinterpret_cast<uint64_t>(value.image);
                barrier.srcStages = value.srcStageMask;
                barrier.srcAccess = value.srcAccessMask;
                barrier.dstStages = value.dstStageMask;
                barrier.dstAccess = value.dstAccessMask;
                barrier.oldLayout = value.oldLayout;
                barrier.newLayout = value.newLayout;
                barrier.range = value.subresourceRange;
                record(barrier);
            }
            if (dependency.memoryBarrierCount != 0 || dependency.dependencyFlags != 0)
                ++unexpectedDependencyContent;
            ++calls;
        }

        void clear() noexcept {
            count = 0;
            calls = sync1Calls = sync2Calls = 0;
        }

        [[nodiscard]] std::span<const RecordedBarrier> recorded() const noexcept {
            return std::span(barriers.data(), count);
        }

        // Fixed storage so a steady frame recorded here allocates nothing.
        std::array<RecordedBarrier, 4096> barriers{};
        size_t count = 0;
        uint32_t calls = 0;
        uint32_t sync1Calls = 0;
        uint32_t sync2Calls = 0;
        uint32_t wrongCommandBuffer = 0;
        uint32_t unexpectedDependencyContent = 0;
        uint32_t overflow = 0;

    private:
        void record(const RecordedBarrier& barrier) noexcept {
            if (count < barriers.size()) barriers[count++] = barrier;
            else ++overflow;
        }
    };

    RenderGraph::ResourceDesc imageDesc(RenderGraph::Format format =
        RenderGraph::Format::Rgba16Float) {
        RenderGraph::ResourceDesc desc{};
        desc.type = RenderGraph::ResourceType::Image;
        desc.image.format = format;
        desc.image.extent = { 64, 32, 1 };
        return desc;
    }

    RenderGraph::ResourceDesc bufferDesc(uint64_t size = 256) {
        RenderGraph::ResourceDesc desc{};
        desc.type = RenderGraph::ResourceType::Buffer;
        desc.buffer.size = size;
        desc.buffer.alignment = 16;
        return desc;
    }

    template <typename Function>
    bool throws(Function&& function) {
        try { function(); }
        catch (const std::exception&) { return true; }
        return false;
    }

    // produce (color + storage buffer write) -> consume (sampled + storage
    // read-write) -> again (storage read-write, re-barriered).
    RenderGraph::CompiledGraph smallGraph() {
        RenderGraph::RenderGraphBuilder builder;
        auto color = builder.createResource("color", imageDesc());
        auto depth = builder.createResource("depth", imageDesc(RenderGraph::Format::D32Float));
        auto data = builder.createResource("data", bufferDesc());
        const auto produce = builder.addPass("produce");
        const auto consume = builder.addPass("consume");
        const auto again = builder.addPass("again", RenderGraph::QueueClass::Compute);
        color = builder.write(produce, color, Access::ColorAttachment,
            RenderGraph::LoadOp::Clear);
        depth = builder.write(produce, depth, Access::DepthAttachmentWrite,
            RenderGraph::LoadOp::Clear);
        data = builder.write(produce, data, Access::StorageWrite);
        builder.read(consume, color, Access::SampledRead);
        builder.read(consume, depth, Access::SampledRead);
        data = builder.write(consume, data, Access::StorageReadWrite);
        data = builder.write(again, data, Access::StorageReadWrite);
        builder.exportResource(data, Access::StorageReadWrite);
        builder.exportResource(color, Access::SampledRead);
        builder.exportResource(depth, Access::SampledRead);
        RenderGraph::CompileResult result = builder.compile();
        if (!result.succeeded()) throw std::runtime_error("small graph failed to compile");
        return std::move(*result.graph);
    }

    bool testIdResolution() {
        const RenderGraph::CompiledGraph reference = buildVulkanProductionRenderGraph(
            { 320, 180 }, VK_FORMAT_B8G8R8A8_SRGB);
        FakeResourceFactory factory;
        VulkanRenderGraphExecutor executor;
        executor.init(factory, 2);
        CHECK(!executor.findPass("gbuffer").isValid());
        CHECK(throws([&] { (void)executor.passId("gbuffer"); }));
        executor.rebuild(reference);
        const RenderGraph::CompiledGraph* graph = executor.compiledGraph();
        CHECK(graph != nullptr);
        CHECK(graph->topologyHash() == reference.topologyHash());
        for (uint32_t order = 0; order < graph->passes().size(); ++order) {
            CHECK(executor.passId(graph->passes()[order].name).order == order);
        }
        for (uint32_t index = 0; index < graph->resources().size(); ++index) {
            CHECK(executor.resourceId(graph->resources()[index].name).logical == index);
        }
        CHECK(!executor.findPass("no-such-pass").isValid());
        CHECK(!executor.findResource("no-such-resource").isValid());
        CHECK(throws([&] { (void)executor.resourceId("no-such-resource"); }));
        // Lookups accept std::string and string literals without temporaries.
        const std::string gbuffer = "gbuffer";
        CHECK(executor.findPass(gbuffer) == executor.passId("gbuffer"));
        // Id and name forms address the same physical resources.
        const auto scene = executor.resourceId("scene.color");
        CHECK(&executor.image(1, scene) == &executor.imageResource(1, "scene.color"));
        const auto headers = executor.resourceId(kClusterHeaderResourceName);
        CHECK(&executor.buffer(0, headers) ==
            &executor.bufferResource(0, kClusterHeaderResourceName));
        CHECK(throws([&] { (void)executor.image(0, headers); }));
        CHECK(throws([&] { (void)executor.buffer(0, scene); }));
        CHECK(throws([&] { (void)executor.image(0, RenderGraph::GraphResourceId{ 9999 }); }));
        // Imported resources have no executor-owned physical image.
        CHECK(throws([&] { (void)executor.image(0, executor.resourceId("swapchain")); }));

        // A rebuild re-resolves names against the new plan.
        const RenderGraph::PassId gbufferBefore = executor.passId("gbuffer");
        executor.rebuild(buildVulkanProductionRenderGraph({ 320, 180 },
            VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, false,
            GBufferLayout::CanonicalReference, {}, 4096, 8192, true, {},
            { .virtualShadowWorkingSetBytes = 8'192 }));
        CHECK(executor.passId("shadow.virtual.clip-upload").order == 0);
        CHECK(executor.passId("gbuffer").order == gbufferBefore.order + 1);
        executor.cleanupAfterDeviceIdle();
        CHECK(!executor.findPass("gbuffer").isValid());
        CHECK(executor.compiledGraph() == nullptr);
        return true;
    }

    bool testIdAndStringOrderChecks() {
        FakeResourceFactory factory;
        VulkanRenderGraphExecutor executor;
        RecordingBarrierSink sink;
        executor.setBarrierSink(&sink);
        executor.init(factory, 2);
        executor.rebuild(smallGraph());
        const auto produce = executor.passId("produce");
        const auto consume = executor.passId("consume");
        const auto again = executor.passId("again");

        CHECK(throws([&] { executor.beginPass(FakeCommandBuffer, produce); }));
        executor.beginFrameExecution(0);
        CHECK(throws([&] { executor.beginPass(FakeCommandBuffer, consume); }));
        CHECK(throws([&] { executor.skipPass(again); }));
        CHECK(throws([&] { executor.beginPass(VK_NULL_HANDLE, produce); }));
        CHECK(throws([&] { executor.beginPass(FakeCommandBuffer, "consume"); }));
        executor.beginPass(FakeCommandBuffer, produce);
        CHECK(throws([&] { executor.beginPass(FakeCommandBuffer, produce); }));
        executor.skipPass("consume");
        CHECK(throws([&] { executor.finishFrameExecution(); }));
        executor.beginPass(FakeCommandBuffer, "again");
        CHECK(throws([&] { executor.skipPass(RenderGraph::PassId{ 3 }); }));
        executor.finishFrameExecution();

        // Mixed forms address the same cursor.
        executor.onFrameFenceCompleted(1);
        executor.beginFrameExecution(1);
        executor.skipPass(produce);
        executor.beginPass(FakeCommandBuffer, "consume");
        executor.skipPass("again");
        executor.finishFrameExecution();
        CHECK(sink.wrongCommandBuffer == 0);
        executor.cleanupAfterDeviceIdle();
        return true;
    }

    // The sink sees exactly the barriers the executor would have recorded:
    // one barrier per state change and storage writes re-barriered.
    bool testBarrierSinkRecordsGraphBarriers() {
        FakeResourceFactory factory;
        VulkanRenderGraphExecutor executor;
        RecordingBarrierSink sink;
        executor.setBarrierSink(&sink);
        executor.init(factory, 2);
        executor.rebuild(smallGraph());
        const auto& graph = *executor.compiledGraph();
        const auto color = executor.resourceId("color");
        const auto depth = executor.resourceId("depth");
        const auto data = executor.resourceId("data");
        const uint64_t colorImage = reinterpret_cast<uint64_t>(executor.image(0, color).image);
        const uint64_t depthImage = reinterpret_cast<uint64_t>(executor.image(0, depth).image);
        const uint64_t dataBuffer = reinterpret_cast<uint64_t>(executor.buffer(0, data).buffer);
        CHECK(graph.physicalSlots().size() == 3);

        executor.beginFrameExecution(0);
        for (const auto& pass : graph.passes())
            executor.beginPass(FakeCommandBuffer, executor.passId(pass.name));
        executor.finishFrameExecution();
        const auto first = sink.recorded();
        // produce: 3 (from undefined), consume: 2 images + 1 buffer, again: 1 buffer.
        CHECK(first.size() == 7);
        CHECK(first[0].handle == colorImage && first[0].oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
        CHECK(first[0].srcStages == VK_PIPELINE_STAGE_2_NONE);
        CHECK(first[0].newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        CHECK(first[1].handle == depthImage &&
            first[1].range.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT);
        CHECK(first[2].handle == dataBuffer && !first[2].image && first[2].size == 256);
        CHECK(first[3].newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        CHECK(first[4].newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        CHECK(first[5].srcAccess == VK_ACCESS_2_SHADER_WRITE_BIT);
        CHECK(first[6].srcAccess == (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT));
        CHECK(sink.wrongCommandBuffer == 0);

        // The second use of slot 0 starts from the state the first frame left.
        sink.clear();
        executor.onFrameFenceCompleted(0);
        executor.beginFrameExecution(0);
        executor.beginPass(FakeCommandBuffer, executor.passId("produce"));
        executor.skipPass(executor.passId("consume"));
        executor.skipPass(executor.passId("again"));
        executor.finishFrameExecution();
        const auto second = sink.recorded();
        CHECK(second.size() == 3);
        CHECK(second[0].oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        CHECK(second[1].oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        CHECK(second[2].srcAccess == (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT));

        // Out-of-plan transitions go through the sink as well.
        sink.clear();
        executor.onFrameFenceCompleted(1);
        executor.beginFrameExecution(1);
        executor.transitionImage(FakeCommandBuffer, color, Access::TransferSource);
        executor.transitionImage(FakeCommandBuffer, "color", Access::TransferSource);
        CHECK(throws([&] { executor.transitionImage(FakeCommandBuffer, "nothing", Access::TransferSource); }));
        CHECK(sink.recorded().size() == 1);
        CHECK(sink.recorded()[0].newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        for (const auto& pass : graph.passes())
            executor.skipPass(executor.passId(pass.name));
        executor.finishFrameExecution();
        executor.cleanupAfterDeviceIdle();
        return true;
    }

    bool testExternalBufferBindingById() {
        RenderGraph::RenderGraphBuilder builder;
        RenderGraph::ResourceDesc desc = bufferDesc();
        desc.lifetime = RenderGraph::ResourceLifetime::External;
        desc.imported = true;
        desc.initialAccess = Access::TransferDestination;
        auto buffer = builder.createResource("external", desc);
        buffer = builder.write(builder.addPass("upload"), buffer, Access::TransferDestination);
        buffer = builder.write(builder.addPass("again"), buffer, Access::TransferDestination);
        const auto compiled = builder.compile();
        CHECK(compiled.succeeded());
        FakeResourceFactory factory;
        RecordingBarrierSink sink;
        VulkanRenderGraphExecutor executor;
        executor.setBarrierSink(&sink);
        executor.init(factory, 2);
        executor.rebuild(*compiled.graph);
        const auto id = executor.resourceId("external");
        const auto handle = reinterpret_cast<VkBuffer>(uintptr_t{ 123 });
        CHECK(throws([&] { executor.bindExternalBuffer(0, RenderGraph::GraphResourceId{ 7 }, handle, 256); }));
        CHECK(throws([&] { executor.bindExternalBuffer(0, "unknown", handle, 256); }));
        executor.bindExternalBuffer(0, id, handle, 512, Access::TransferDestination);
        executor.beginFrameExecution(0);
        executor.beginPass(FakeCommandBuffer, executor.passId("upload"));
        executor.beginPass(FakeCommandBuffer, executor.passId("again"));
        executor.finishFrameExecution();
        // Transfer writes re-barrier even at the same access; the barrier
        // covers the declared size.
        CHECK(sink.recorded().size() == 2);
        CHECK(sink.recorded()[0].handle == 123 && sink.recorded()[0].size == 256);
        CHECK(sink.recorded()[0].srcAccess == VK_ACCESS_2_TRANSFER_WRITE_BIT);
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
        { "id resolution", testIdResolution },
        { "id and string order checks", testIdAndStringOrderChecks },
        { "barrier sink records graph barriers", testBarrierSinkRecordsGraphBarriers },
        { "external buffer binding by id", testExternalBufferBindingById },
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
