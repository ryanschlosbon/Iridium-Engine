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

    // ---- R3b.2: batched synchronization2 equivalence ---------------------------

    struct NamedTopology {
        std::string name;
        RenderGraph::CompiledGraph graph;
    };

    // Every production topology the backend can build.
    std::vector<NamedTopology> productionTopologies() {
        constexpr VkExtent2D Scene{ 1920, 1080 };
        constexpr VkExtent2D Ordinary2{ 960, 528 };
        constexpr VkExtent2D Hero4{ 1920, 528 };
        constexpr VkExtent2D Cinematic8{ 1920, 1072 };
        const auto build = [&](bool hdr10, VulkanLayeredGraphConfig layered,
            VulkanProductionGraphFeatures features, bool pyramids = true) {
            return buildVulkanProductionRenderGraph(Scene, Scene,
                hdr10 ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_B8G8R8A8_SRGB,
                hdr10 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_B8G8R8A8_SRGB,
                hdr10, GBufferLayout::CanonicalReference, {}, 4096, 8192, pyramids,
                layered, features);
        };
        std::vector<NamedTopology> result;
        result.push_back({ "base SDR", build(false, {}, {}) });
        result.push_back({ "base SDR, no pyramids, no telemetry",
            build(false, {}, { .clusterTelemetryReadback = false }, false) });
        result.push_back({ "HDR10", build(true, {}, {}) });
        result.push_back({ "VSM", build(false, {}, { .virtualShadowWorkingSetBytes = 8'192 }) });
        result.push_back({ "Hi-Z", build(false, {}, { .depthPyramid = true }) });
        result.push_back({ "Ordinary2", build(false, { Ordinary2, {}, {} }, {}) });
        result.push_back({ "Hero4", build(false, { {}, Hero4, {} }, {}) });
        result.push_back({ "Cinematic8", build(false, { {}, {}, Cinematic8 }, {}) });
        result.push_back({ "OIT", build(false, { {}, {}, {}, true }, {}) });
        result.push_back({ "all features", build(true,
            { Ordinary2, Hero4, Cinematic8, true },
            { .depthPyramid = true, .virtualShadowWorkingSetBytes = 8'192 }) });
        result.push_back({ "all features, no hooks", build(false,
            { Ordinary2, Hero4, Cinematic8, true },
            { .depthPyramid = true, .virtualShadowWorkingSetBytes = 8'192,
              .hooks = VulkanGraphHooks::none() }) });
        return result;
    }

    // Passes the backend always records when their feature is declared; all
    // others have a skipPass path somewhere (shadows, transparency tiers,
    // hooks, readbacks, refraction pyramids, ...).
    bool alwaysRecorded(std::string_view name) {
        return name == "gbuffer" || name == "lighting" || name == "forward-opaque" ||
            name == "output-transform" || name == "ui-present" || name == "ui-compose" ||
            name == "hdr10-encode-present" ||
            (name.starts_with("lighting.cluster.") && name != "lighting.cluster.readback") ||
            name.starts_with("shadow.virtual.");
    }

    // Four frames over two frame slots: everything, skip every optional pass,
    // then two alternating patterns, so slot state carries over skipped work.
    bool scriptRuns(const RenderGraph::CompiledGraph& graph, uint32_t frame, uint32_t pass) {
        if (alwaysRecorded(graph.passes()[pass].name)) return true;
        switch (frame) {
        case 0: return true;
        case 1: return false;
        case 2: return pass % 2 == 0;
        default: return pass % 3 != 1;
        }
    }

    // Reference: the pre-R3b.2 emission, one sync1 barrier per resource state
    // change, kept here only to prove the batched recording equivalent.
    class LegacyEmissionReference {
    public:
        LegacyEmissionReference(const VulkanRenderGraphExecutor& executor,
            uint32_t frameCount)
            : executor_(executor), graph_(*executor.compiledGraph()),
              slotAccess_(frameCount, std::vector<Access>(
                  graph_.physicalSlots().size(), Access::Undefined)),
              external_(frameCount, std::vector<External>(graph_.resources().size())) {}

        void bindExternalBuffer(uint32_t frame, uint32_t logical, VkBuffer buffer,
            Access access) {
            external_[frame][logical] = { buffer, access, true };
        }

        // Ordered barriers the legacy executor records for one pass.
        std::vector<RecordedBarrier> beginPass(uint32_t frame, uint32_t order) {
            std::vector<RecordedBarrier> result;
            const RenderGraph::CompiledPass& pass = graph_.passes()[order];
            for (uint32_t index = 0; index < pass.usageCount; ++index) {
                const RenderGraph::CompiledUsage& usage =
                    graph_.usages()[pass.firstUsage + index];
                const RenderGraph::CompiledResource& resource =
                    graph_.resources()[usage.logicalResourceIndex];
                if (resource.physicalSlot == RenderGraph::InvalidIndex) {
                    External& binding = external_[frame][usage.logicalResourceIndex];
                    if (!binding.tracked) continue;
                    const auto before = getVulkanGraphAccessInfo(binding.access,
                        RenderGraph::ResourceType::Buffer);
                    const auto after = getVulkanGraphAccessInfo(usage.access,
                        RenderGraph::ResourceType::Buffer);
                    const VkAccessFlags writes = VK_ACCESS_SHADER_WRITE_BIT |
                        VK_ACCESS_TRANSFER_WRITE_BIT;
                    if (binding.access != usage.access || (before.access & writes)) {
                        RecordedBarrier barrier{};
                        barrier.handle = reinterpret_cast<uint64_t>(binding.buffer);
                        barrier.srcStages = normalizedSource(before.stages);
                        barrier.srcAccess = before.access;
                        barrier.dstStages = after.stages;
                        barrier.dstAccess = after.access;
                        barrier.size = resource.desc.buffer.size;
                        result.push_back(barrier);
                    }
                    binding.access = usage.access;
                    continue;
                }
                Access& current = slotAccess_[frame][resource.physicalSlot];
                if (current == usage.access && usage.access != Access::StorageWrite &&
                    usage.access != Access::StorageReadWrite) {
                    continue;
                }
                const RenderGraph::GraphResourceId id{ usage.logicalResourceIndex };
                RecordedBarrier barrier{};
                if (resource.desc.type == RenderGraph::ResourceType::Image) {
                    const VulkanImageResource& image = executor_.image(frame, id);
                    const auto before = legacyImageInfo(current, image.aspect);
                    const auto after = legacyImageInfo(usage.access, image.aspect);
                    barrier.image = true;
                    barrier.handle = reinterpret_cast<uint64_t>(image.image);
                    barrier.srcStages = normalizedSource(before.stages);
                    barrier.srcAccess = before.access;
                    barrier.dstStages = after.stages;
                    barrier.dstAccess = after.access;
                    barrier.oldLayout = before.layout;
                    barrier.newLayout = after.layout;
                    barrier.range = { image.aspect, 0, image.mipLevels, 0, image.arrayLayers };
                }
                else {
                    const VulkanBufferResource& buffer = executor_.buffer(frame, id);
                    const auto before = getVulkanGraphAccessInfo(current,
                        RenderGraph::ResourceType::Buffer);
                    const auto after = getVulkanGraphAccessInfo(usage.access,
                        RenderGraph::ResourceType::Buffer);
                    barrier.handle = reinterpret_cast<uint64_t>(buffer.buffer);
                    barrier.srcStages = normalizedSource(before.stages);
                    barrier.srcAccess = before.access;
                    barrier.dstStages = after.stages;
                    barrier.dstAccess = after.access;
                    barrier.size = buffer.size;
                }
                result.push_back(barrier);
                current = usage.access;
            }
            return result;
        }

    private:
        struct External {
            VkBuffer buffer = VK_NULL_HANDLE;
            Access access = Access::Undefined;
            bool tracked = false;
        };

        static VulkanGraphAccessInfo legacyImageInfo(Access access,
            VkImageAspectFlags aspect) {
            VulkanGraphAccessInfo info = getVulkanGraphAccessInfo(access,
                RenderGraph::ResourceType::Image);
            if (access == Access::SampledRead &&
                (aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0)
                info.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            return info;
        }

        const VulkanRenderGraphExecutor& executor_;
        const RenderGraph::CompiledGraph& graph_;
        std::vector<std::vector<Access>> slotAccess_;
        std::vector<std::vector<External>> external_;
    };

    // Same barriers per pass as an ordered set: images in usage order and
    // buffers in usage order (sync2 records the two arrays separately).
    bool samePassBarriers(std::span<const RecordedBarrier> expected,
        std::span<const RecordedBarrier> actual) {
        if (expected.size() != actual.size()) return false;
        for (const bool image : { true, false }) {
            std::vector<const RecordedBarrier*> lhs;
            std::vector<const RecordedBarrier*> rhs;
            for (const auto& value : expected) if (value.image == image) lhs.push_back(&value);
            for (const auto& value : actual) if (value.image == image) rhs.push_back(&value);
            if (lhs.size() != rhs.size()) return false;
            for (size_t index = 0; index < lhs.size(); ++index)
                if (!lhs[index]->sameDependency(*rhs[index])) return false;
        }
        return true;
    }

    struct EquivalenceTotals {
        size_t barriers = 0;
        size_t passesWithBarriers = 0;
        size_t sync2Calls = 0;
        size_t legacyCalls = 0;
    };

    bool runEquivalence(const NamedTopology& topology, VulkanBarrierApi api,
        EquivalenceTotals& totals) {
        constexpr uint32_t FrameSlots = 2;
        FakeResourceFactory factory;
        RecordingBarrierSink sink;
        VulkanRenderGraphExecutor executor;
        executor.setBarrierSink(&sink);
        executor.init(factory, FrameSlots);
        executor.setBarrierApi(api);
        executor.rebuild(topology.graph);
        LegacyEmissionReference reference(executor, FrameSlots);
        const RenderGraph::CompiledGraph& graph = *executor.compiledGraph();
        const auto vsm = executor.findResource("shadow.virtual.working-set");
        if (vsm.isValid()) {
            for (uint32_t frame = 0; frame < FrameSlots; ++frame) {
                const auto buffer = reinterpret_cast<VkBuffer>(uintptr_t{ 0x900 + frame });
                executor.bindExternalBuffer(frame, vsm, buffer, 8'192);
                reference.bindExternalBuffer(frame, vsm.logical, buffer, Access::Undefined);
            }
        }
        for (uint32_t frame = 0; frame < 4; ++frame) {
            const uint32_t slot = frame % FrameSlots;
            executor.onFrameFenceCompleted(slot);
            executor.beginFrameExecution(slot);
            for (uint32_t pass = 0; pass < graph.passes().size(); ++pass) {
                if (!scriptRuns(graph, frame, pass)) {
                    executor.skipPass(RenderGraph::PassId{ pass });
                    continue;
                }
                sink.clear();
                executor.beginPass(FakeCommandBuffer, RenderGraph::PassId{ pass });
                const std::vector<RecordedBarrier> expected = reference.beginPass(slot, pass);
                const auto actual = sink.recorded();
                const bool same = api == VulkanBarrierApi::Synchronization2
                    ? samePassBarriers(expected, actual) &&
                        sink.sync1Calls == 0 && sink.sync2Calls == (expected.empty() ? 0u : 1u)
                    : actual.size() == expected.size() && sink.sync2Calls == 0 &&
                        sink.sync1Calls == expected.size() &&
                        std::equal(expected.begin(), expected.end(), actual.begin(),
                            [](const auto& lhs, const auto& rhs) { return lhs.sameDependency(rhs); });
                if (!same) {
                    std::cerr << "  " << topology.name << ": frame " << frame << " pass '"
                        << graph.passes()[pass].name << "' expected " << expected.size()
                        << " barriers, recorded " << actual.size() << " in "
                        << sink.sync1Calls << "+" << sink.sync2Calls << " calls\n";
                    return false;
                }
                totals.barriers += expected.size();
                totals.passesWithBarriers += expected.empty() ? 0 : 1;
                totals.sync2Calls += sink.sync2Calls;
                totals.legacyCalls += expected.size();
            }
            executor.finishFrameExecution();
        }
        if (sink.wrongCommandBuffer != 0 || sink.unexpectedDependencyContent != 0 ||
            sink.overflow != 0) return false;
        executor.cleanupAfterDeviceIdle();
        return true;
    }

    bool testBatchedSynchronization2MatchesLegacyEmission() {
        EquivalenceTotals sync2{};
        EquivalenceTotals sync1{};
        for (const NamedTopology& topology : productionTopologies()) {
            CHECK(runEquivalence(topology, VulkanBarrierApi::Synchronization2, sync2));
            CHECK(runEquivalence(topology, VulkanBarrierApi::Synchronization1, sync1));
        }
        CHECK(sync2.barriers == sync1.barriers);
        CHECK(sync2.barriers > 0);
        CHECK(sync2.sync2Calls == sync2.passesWithBarriers);
        std::cout << "  " << productionTopologies().size() << " topologies x 4 frames: "
            << sync2.barriers << " barriers, " << sync2.legacyCalls
            << " legacy sync1 calls -> " << sync2.sync2Calls << " vkCmdPipelineBarrier2\n";
        return true;
    }

    // Collapse rule contract: no production pass uses one physical resource
    // twice, so batching never merges barriers in current topologies.
    bool testProductionPassesNeverCollapse() {
        for (const NamedTopology& topology : productionTopologies()) {
            const RenderGraph::CompiledGraph& graph = topology.graph;
            for (const RenderGraph::CompiledPass& pass : graph.passes()) {
                std::vector<uint64_t> keys;
                for (uint32_t index = 0; index < pass.usageCount; ++index) {
                    const auto& usage = graph.usages()[pass.firstUsage + index];
                    const auto& resource = graph.resources()[usage.logicalResourceIndex];
                    keys.push_back(resource.physicalSlot != RenderGraph::InvalidIndex
                        ? resource.physicalSlot
                        : (uint64_t{ 1 } << 32) | usage.logicalResourceIndex);
                }
                std::sort(keys.begin(), keys.end());
                if (std::adjacent_find(keys.begin(), keys.end()) != keys.end()) {
                    std::cerr << "  " << topology.name << ": pass '" << pass.name
                        << "' uses one physical resource twice\n";
                    return false;
                }
            }
        }
        return true;
    }

    // Two usages of one resource in a pass become one barrier from the first
    // "before" state to the last "after" state.
    bool testCollapseRule() {
        RenderGraph::RenderGraphBuilder builder;
        auto data = builder.createResource("data", bufferDesc());
        auto color = builder.createResource("color", imageDesc());
        const auto produce = builder.addPass("produce");
        const auto both = builder.addPass("both");
        data = builder.write(produce, data, Access::TransferDestination);
        color = builder.write(produce, color, Access::ColorAttachment, RenderGraph::LoadOp::Clear);
        builder.read(both, data, Access::StorageRead);
        data = builder.write(both, data, Access::StorageReadWrite);
        builder.read(both, color, Access::SampledRead);
        color = builder.write(both, color, Access::StorageWrite);
        builder.exportResource(data, Access::StorageReadWrite);
        builder.exportResource(color, Access::StorageWrite);
        const auto compiled = builder.compile();
        CHECK(compiled.succeeded());
        for (const VulkanBarrierApi api : { VulkanBarrierApi::Synchronization2,
                 VulkanBarrierApi::Synchronization1 }) {
            FakeResourceFactory factory;
            RecordingBarrierSink sink;
            VulkanRenderGraphExecutor executor;
            executor.setBarrierSink(&sink);
            executor.init(factory, 1);
            executor.setBarrierApi(api);
            executor.rebuild(*compiled.graph);
            executor.beginFrameExecution(0);
            executor.beginPass(FakeCommandBuffer, executor.passId("produce"));
            sink.clear();
            executor.beginPass(FakeCommandBuffer, executor.passId("both"));
            executor.finishFrameExecution();
            const auto recorded = sink.recorded();
            CHECK(recorded.size() == 2);
            CHECK(sink.calls == (api == VulkanBarrierApi::Synchronization2 ? 1u : 2u));
            const RecordedBarrier& buffer = recorded[recorded[0].image ? 1 : 0];
            const RecordedBarrier& image = recorded[recorded[0].image ? 0 : 1];
            CHECK(buffer.srcAccess == VK_ACCESS_2_TRANSFER_WRITE_BIT);
            CHECK(buffer.srcStages == VK_PIPELINE_STAGE_2_TRANSFER_BIT);
            CHECK(buffer.dstAccess == (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT));
            CHECK(image.oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            CHECK(image.newLayout == VK_IMAGE_LAYOUT_GENERAL);
            CHECK(image.dstAccess == VK_ACCESS_2_SHADER_WRITE_BIT);
            executor.cleanupAfterDeviceIdle();
        }
        return true;
    }

    bool testSynchronization2Mapping() {
        CHECK(toVulkanSourceStages2(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT) ==
            VK_PIPELINE_STAGE_2_NONE);
        CHECK(toVulkanSourceStages2(VK_PIPELINE_STAGE_TRANSFER_BIT) ==
            VK_PIPELINE_STAGE_2_TRANSFER_BIT);
        CHECK(toVulkanDestinationStages2(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT) ==
            VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
        for (uint8_t value = 0; value <= static_cast<uint8_t>(Access::Present); ++value) {
            for (const auto type : { RenderGraph::ResourceType::Image,
                     RenderGraph::ResourceType::Buffer }) {
                const auto info = getVulkanGraphAccessInfo(static_cast<Access>(value), type);
                // Every stage/access bit the graph uses is a legacy 32-bit bit.
                CHECK(static_cast<VkPipelineStageFlags2>(info.stages) ==
                    toVulkanDestinationStages2(info.stages));
                CHECK((info.stages & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) == 0);
            }
        }
        FakeResourceFactory factory;
        VulkanRenderGraphExecutor executor;
        executor.init(factory, 1);
        CHECK(executor.barrierApi() == VulkanBarrierApi::Synchronization1);
        CHECK(!vulkanDeviceSupportsSynchronization2(VK_NULL_HANDLE));
        executor.rebuild(smallGraph());
        executor.beginFrameExecution(0);
        CHECK(throws([&] { executor.setBarrierApi(VulkanBarrierApi::Synchronization2); }));
        for (uint32_t pass = 0; pass < 3; ++pass) executor.skipPass(RenderGraph::PassId{ pass });
        executor.finishFrameExecution();
        executor.setBarrierApi(VulkanBarrierApi::Synchronization2);
        CHECK(executor.barrierApi() == VulkanBarrierApi::Synchronization2);
        executor.cleanupAfterDeviceIdle();
        return true;
    }

    // ---- R3b.3: callback registry ----------------------------------------------

    enum class EventKind : uint8_t { Barrier, Execute, Active, RangeBegin, RangeEnd };

    struct Event {
        EventKind kind = EventKind::Barrier;
        uint32_t pass = RenderGraph::InvalidIndex;
        const char* name = nullptr;
        uint32_t barriers = 0;
    };

    // One ordered log for barrier calls, callbacks and GPU ranges.
    struct EventLog final : VulkanBarrierSink {
        void pipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
            std::span<const VkBufferMemoryBarrier> buffers,
            std::span<const VkImageMemoryBarrier> images) override {
            push({ EventKind::Barrier, currentPass, nullptr,
                static_cast<uint32_t>(buffers.size() + images.size()) });
        }
        void pipelineBarrier2(VkCommandBuffer, const VkDependencyInfo& dependency) override {
            push({ EventKind::Barrier, currentPass, nullptr,
                dependency.bufferMemoryBarrierCount + dependency.imageMemoryBarrierCount });
        }
        void push(const Event& event) noexcept {
            if (count < events.size()) events[count++] = event;
        }
        [[nodiscard]] std::span<const Event> recorded() const noexcept {
            return std::span(events.data(), count);
        }
        [[nodiscard]] size_t countOf(EventKind kind) const noexcept {
            return static_cast<size_t>(std::count_if(events.begin(), events.begin() + count,
                [kind](const Event& event) { return event.kind == kind; }));
        }

        std::array<Event, 512> events{};
        size_t count = 0;
        uint32_t currentPass = RenderGraph::InvalidIndex;
        uint32_t nextToken = 1;
    };

    VulkanGpuRangeSink rangeSinkFor(EventLog& log) {
        return { &log,
            [](void* owner, const char* name) {
                auto& self = *static_cast<EventLog*>(owner);
                self.push({ EventKind::RangeBegin, RenderGraph::InvalidIndex, name });
                return VulkanGpuRangeToken{ 0, self.nextToken++, true };
            },
            [](void* owner, VulkanGpuRangeToken& token) {
                auto& self = *static_cast<EventLog*>(owner);
                self.push({ EventKind::RangeEnd, token.endQuery, nullptr });
                token.active = false;
            } };
    }

    // Owner of the test callbacks: per-pass activity and a record of what the
    // callback saw.
    struct CallbackOwner;
    struct PassOwner {
        CallbackOwner* shared = nullptr;
        uint32_t pass = 0;
    };

    struct CallbackOwner {
        EventLog* log = nullptr;
        std::array<PassOwner, 8> passes{};
        std::array<bool, 8> active{ true, true, true, true, true, true, true, true };
        std::array<uint32_t, 8> executions{};
        VkCommandBuffer seenCommandBuffer = VK_NULL_HANDLE;
        uint32_t seenFrame = RenderGraph::InvalidIndex;
        uint32_t seenImageIndex = RenderGraph::InvalidIndex;
        bool reentryRejected = false;
        bool tryReentry = false;
        RenderGraph::GraphResourceId image{};
        VkImage seenImage = VK_NULL_HANDLE;
    };

    // One owner per pass (PassOwner), sharing the CallbackOwner state.
    VulkanPassCallbacks testCallbacks(CallbackOwner& owner, uint32_t pass,
        const char* range = nullptr,
        GpuRangePlacement placement = GpuRangePlacement::BeforeBarriers) {
        VulkanPassCallbacks callbacks{};
        owner.passes[pass] = { &owner, pass };
        callbacks.owner = &owner.passes[pass];
        callbacks.active = [](void* self, const VulkanFrameRecordContext& frame) {
            const auto& passOwner = *static_cast<PassOwner*>(self);
            auto& state = *passOwner.shared;
            state.seenFrame = frame.frameIndex;
            state.log->push({ EventKind::Active, passOwner.pass });
            return state.active[passOwner.pass];
        };
        callbacks.execute = [](void* self, VulkanPassContext& context) {
            auto& state = *static_cast<PassOwner*>(self)->shared;
            if (context.pass.order != static_cast<PassOwner*>(self)->pass)
                state.log->push({ EventKind::Execute, RenderGraph::InvalidIndex });
            state.log->push({ EventKind::Execute, context.pass.order });
            ++state.executions[context.pass.order];
            state.seenCommandBuffer = context.commandBuffer;
            state.seenImageIndex = context.frame.imageIndex;
            if (state.image.isValid())
                state.seenImage = context.graph.image(context.frame.frameIndex, state.image).image;
            if (state.tryReentry) {
                try { context.graph.beginPass(context.commandBuffer, RenderGraph::PassId{ 4 }); }
                catch (const std::logic_error&) { state.reentryRejected = true; }
            }
        };
        callbacks.gpuRange = range;
        callbacks.placement = placement;
        return callbacks;
    }

    // p0..p4 alternate accesses of one image, so every executed pass records
    // a barrier.
    RenderGraph::CompiledGraph chainGraph() {
        RenderGraph::RenderGraphBuilder builder;
        auto image = builder.createResource("image", imageDesc());
        std::array<RenderGraph::PassHandle, 5> passes{};
        for (uint32_t index = 0; index < 5; ++index)
            passes[index] = builder.addPass("p" + std::to_string(index));
        image = builder.write(passes[0], image, Access::ColorAttachment, RenderGraph::LoadOp::Clear);
        builder.read(passes[1], image, Access::SampledRead);
        image = builder.write(passes[2], image, Access::StorageReadWrite);
        builder.read(passes[3], image, Access::SampledRead);
        builder.read(passes[4], image, Access::TransferSource);
        builder.exportResource(image, Access::TransferSource);
        RenderGraph::CompileResult result = builder.compile();
        if (!result.succeeded()) throw std::runtime_error("chain graph failed to compile");
        return std::move(*result.graph);
    }

    struct ChainFixture {
        FakeResourceFactory factory;
        EventLog log;
        CallbackOwner owner;
        VulkanRenderGraphExecutor executor;

        ChainFixture() {
            owner.log = &log;
            executor.setBarrierSink(&log);
            executor.init(factory, 2);
            executor.setBarrierApi(VulkanBarrierApi::Synchronization2);
            executor.rebuild(chainGraph());
            executor.setGpuRangeSink(rangeSinkFor(log));
        }
        static RenderGraph::PassId pass(uint32_t order) { return RenderGraph::PassId{ order }; }
    };

    bool testCallbackDrainOrder() {
        ChainFixture fixture;
        auto& executor = fixture.executor;
        auto& log = fixture.log;
        executor.registerPass(ChainFixture::pass(1), testCallbacks(fixture.owner, 1));
        executor.registerPass(ChainFixture::pass(3), testCallbacks(fixture.owner, 3));
        fixture.owner.active[3] = false;
        CHECK(executor.isRegistered(ChainFixture::pass(1)));
        CHECK(!executor.isRegistered(ChainFixture::pass(2)));

        executor.beginFrameExecution(0);
        executor.beginPass(FakeCommandBuffer, ChainFixture::pass(0));
        // Draining p1 happens inside beginPass(p2), p3 inside beginPass(p4).
        executor.beginPass(FakeCommandBuffer, ChainFixture::pass(2));
        executor.beginPass(FakeCommandBuffer, ChainFixture::pass(4));
        executor.finishFrameExecution();

        const auto events = log.recorded();
        // p0 barrier | p1 active, barrier, execute | p2 barrier | p3 inactive | p4 barrier
        CHECK(events.size() == 7);
        CHECK(events[0].kind == EventKind::Barrier);
        CHECK(events[1].kind == EventKind::Active && events[1].pass == 1);
        CHECK(events[2].kind == EventKind::Barrier);
        CHECK(events[3].kind == EventKind::Execute && events[3].pass == 1);
        CHECK(events[4].kind == EventKind::Barrier);
        CHECK(events[5].kind == EventKind::Active && events[5].pass == 3);
        CHECK(events[6].kind == EventKind::Barrier);
        CHECK(fixture.owner.executions[1] == 1 && fixture.owner.executions[3] == 0);
        CHECK(fixture.owner.seenCommandBuffer == FakeCommandBuffer);
        CHECK(fixture.owner.seenFrame == 0);
        return true;
    }

    bool testDrainRejectsUnregisteredSkips() {
        ChainFixture fixture;
        auto& executor = fixture.executor;
        executor.registerPass(ChainFixture::pass(2), testCallbacks(fixture.owner, 2));
        CHECK(throws([&] { executor.registerPass(ChainFixture::pass(2), testCallbacks(fixture.owner, 2)); }));
        CHECK(throws([&] { executor.registerPass(ChainFixture::pass(9), testCallbacks(fixture.owner, 0)); }));
        CHECK(throws([&] { executor.registerPass(ChainFixture::pass(0), VulkanPassCallbacks{}); }));
        executor.beginFrameExecution(0);
        CHECK(throws([&] { executor.registerPass(ChainFixture::pass(4), testCallbacks(fixture.owner, 4)); }));
        CHECK(throws([&] { executor.unregisterPass(ChainFixture::pass(2)); }));
        executor.beginPass(FakeCommandBuffer, ChainFixture::pass(0));
        // Draining up to p3 would skip the unregistered p1.
        CHECK(throws([&] { executor.beginPass(FakeCommandBuffer, ChainFixture::pass(3)); }));
        CHECK(throws([&] { executor.skipPass(ChainFixture::pass(3)); }));
        // A registered pass is never begun or skipped imperatively.
        CHECK(throws([&] { executor.beginPass(FakeCommandBuffer, ChainFixture::pass(2)); }));
        CHECK(throws([&] { executor.skipPass(ChainFixture::pass(2)); }));
        CHECK(fixture.owner.executions[2] == 0);
        executor.skipPass(ChainFixture::pass(1));
        // The string form drains the registered pass at the cursor first.
        executor.beginPass(FakeCommandBuffer, "p3");
        CHECK(fixture.owner.executions[2] == 1);
        CHECK(throws([&] { executor.finishFrameExecution(); }));
        executor.skipPass("p4");
        executor.finishFrameExecution();
        return true;
    }

    bool testFinishDrainsAndRollback() {
        ChainFixture fixture;
        auto& executor = fixture.executor;
        executor.registerPass(ChainFixture::pass(3), testCallbacks(fixture.owner, 3));
        executor.registerPass(ChainFixture::pass(4), testCallbacks(fixture.owner, 4));
        executor.beginFrameExecution(0);
        // No beginPass yet: the drain needs an explicit record context.
        executor.skipPass(ChainFixture::pass(0));
        executor.skipPass(ChainFixture::pass(1));
        executor.skipPass(ChainFixture::pass(2));
        CHECK(throws([&] { executor.finishFrameExecution(); }));
        executor.setFrameRecordContext({ FakeCommandBuffer, 7, 2 });
        executor.finishFrameExecution();
        CHECK(fixture.owner.executions[3] == 1 && fixture.owner.executions[4] == 1);
        CHECK(fixture.owner.seenImageIndex == 2);
        CHECK(fixture.owner.seenFrame == 0);   // the executing slot, not the argument

        // Rollback: unregister p4 and restore its imperative call.
        executor.unregisterPass(ChainFixture::pass(4));
        CHECK(!executor.isRegistered(ChainFixture::pass(4)));
        executor.onFrameFenceCompleted(1);
        executor.beginFrameExecution(1);
        executor.beginPass(FakeCommandBuffer, ChainFixture::pass(0));
        executor.skipPass(ChainFixture::pass(1));
        executor.skipPass(ChainFixture::pass(2));
        // finish drains p3, then fails: p4 is imperative again.
        CHECK(throws([&] { executor.finishFrameExecution(); }));
        executor.beginPass(FakeCommandBuffer, ChainFixture::pass(4));
        executor.finishFrameExecution();
        CHECK(fixture.owner.executions[3] == 2 && fixture.owner.executions[4] == 1);

        // A rebuild clears every registration.
        executor.rebuild(chainGraph());
        CHECK(!executor.isRegistered(ChainFixture::pass(3)));
        return true;
    }

    bool testGpuRangePlacementAndGroups() {
        ChainFixture fixture;
        auto& executor = fixture.executor;
        auto& log = fixture.log;
        executor.registerPass(ChainFixture::pass(1), testCallbacks(fixture.owner, 1, "gpu.before"));
        executor.registerPass(ChainFixture::pass(2), testCallbacks(fixture.owner, 2, "gpu.after",
            GpuRangePlacement::AfterBarriers));
        executor.registerPass(ChainFixture::pass(3), testCallbacks(fixture.owner, 3));
        CHECK(throws([&] { executor.registerRangeGroup({ "gpu.group", ChainFixture::pass(0),
            ChainFixture::pass(2) }); }));   // p0 is imperative
        CHECK(throws([&] { executor.registerRangeGroup({ nullptr, ChainFixture::pass(1),
            ChainFixture::pass(2) }); }));
        executor.registerRangeGroup({ "gpu.group", ChainFixture::pass(1), ChainFixture::pass(3) });
        CHECK(throws([&] { executor.registerRangeGroup({ "gpu.other", ChainFixture::pass(3),
            ChainFixture::pass(3) }); }));   // overlap

        executor.beginFrameExecution(0);
        executor.beginPass(FakeCommandBuffer, ChainFixture::pass(0));
        log.count = 0;
        executor.beginPass(FakeCommandBuffer, ChainFixture::pass(4));
        executor.finishFrameExecution();
        auto events = log.recorded();
        // Group opens before p1's range; p1's range wraps its barrier; p2's
        // range starts after its barrier; the group closes after p3.
        const EventKind expected[] = {
            EventKind::Active, EventKind::RangeBegin, EventKind::RangeBegin, EventKind::Barrier,
            EventKind::Execute, EventKind::RangeEnd,
            EventKind::Active, EventKind::Barrier, EventKind::RangeBegin, EventKind::Execute,
            EventKind::RangeEnd,
            EventKind::Active, EventKind::Barrier, EventKind::Execute, EventKind::RangeEnd,
            EventKind::Barrier };
        CHECK(events.size() == std::size(expected));
        for (size_t index = 0; index < events.size(); ++index)
            CHECK(events[index].kind == expected[index]);
        CHECK(std::string_view(events[1].name) == "gpu.group");
        CHECK(std::string_view(events[2].name) == "gpu.before");
        CHECK(std::string_view(events[8].name) == "gpu.after");
        CHECK(events[14].pass == 1);   // the group token closes last

        // The group opens at the first active pass; with every pass inactive
        // no range is recorded.
        for (const bool everyInactive : { false, true }) {
            fixture.owner.active[1] = false;
            fixture.owner.active[2] = !everyInactive;
            fixture.owner.active[3] = !everyInactive;
            executor.onFrameFenceCompleted(1);
            executor.beginFrameExecution(1);
            // Only skips this frame: the drain needs an explicit context.
            executor.setFrameRecordContext({ FakeCommandBuffer, 1 });
            executor.skipPass(ChainFixture::pass(0));
            log.count = 0;
            executor.skipPass(ChainFixture::pass(4));
            executor.finishFrameExecution();
            events = log.recorded();
            if (everyInactive) {
                CHECK(log.countOf(EventKind::RangeBegin) == 0);
                CHECK(log.countOf(EventKind::Execute) == 0);
            }
            else {
                CHECK(events[0].kind == EventKind::Active);           // p1 inactive
                CHECK(events[1].kind == EventKind::Active);           // p2 asked
                CHECK(events[2].kind == EventKind::RangeBegin &&
                    std::string_view(events[2].name) == "gpu.group");
                CHECK(log.countOf(EventKind::RangeBegin) == 2);       // group + gpu.after
                CHECK(log.countOf(EventKind::RangeEnd) == 2);
                CHECK(events[log.count - 1].kind == EventKind::RangeEnd);
            }
        }
        // Unregistering a grouped pass dissolves the group.
        executor.unregisterPass(ChainFixture::pass(2));
        executor.registerRangeGroup({ "gpu.single", ChainFixture::pass(3), ChainFixture::pass(3) });
        return true;
    }

    bool testCallbackContextAndReentry() {
        ChainFixture fixture;
        auto& executor = fixture.executor;
        fixture.owner.image = executor.resourceId("image");
        fixture.owner.tryReentry = true;
        executor.registerPass(ChainFixture::pass(1), testCallbacks(fixture.owner, 1));
        executor.onFrameFenceCompleted(1);
        executor.beginFrameExecution(1);
        executor.setFrameRecordContext({ FakeCommandBuffer, 1, 5 });
        executor.beginPass(FakeCommandBuffer, ChainFixture::pass(0));
        executor.skipPass(ChainFixture::pass(2));
        CHECK(fixture.owner.reentryRejected);
        CHECK(fixture.owner.seenImageIndex == 5);
        CHECK(fixture.owner.seenImage == executor.image(1, fixture.owner.image).image);
        executor.skipPass(ChainFixture::pass(3));
        executor.skipPass(ChainFixture::pass(4));
        executor.finishFrameExecution();
        CHECK(throws([&] { executor.setFrameRecordContext({ FakeCommandBuffer }); }));
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
        { "synchronization2 mapping", testSynchronization2Mapping },
        { "collapse rule", testCollapseRule },
        { "production passes never collapse", testProductionPassesNeverCollapse },
        { "batched synchronization2 matches legacy emission",
            testBatchedSynchronization2MatchesLegacyEmission },
        { "callback drain order", testCallbackDrainOrder },
        { "drain rejects unregistered skips", testDrainRejectsUnregisteredSkips },
        { "finish drains and rollback", testFinishDrainsAndRollback },
        { "GPU range placement and groups", testGpuRangePlacementAndGroups },
        { "callback context and reentry", testCallbackContextAndReentry },
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
