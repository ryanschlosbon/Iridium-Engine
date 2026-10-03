// M7R R3b executor tests: index addressing and the barrier sink. All
// recording goes through a VulkanBarrierSink against fake resources, so no
// device is needed.

#include "profiling/CpuAllocationProfile.h"
#include "renderer/vulkan/VulkanProductionRenderGraph.h"
#include "renderer/vulkan/VulkanRenderGraphExecutor.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
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
        // Resolved ids address the frame slot's physical resources.
        const auto scene = executor.resourceId("scene.color");
        CHECK(executor.image(1, scene).isValid());
        CHECK(&executor.image(1, scene) != &executor.image(0, scene));
        const auto headers = executor.resourceId(kClusterHeaderResourceName);
        CHECK(executor.buffer(0, headers).isValid());
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

    bool testIdOrderChecks() {
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
        CHECK(throws([&] { executor.beginPass(FakeCommandBuffer,
            RenderGraph::PassId{} ); }));
        executor.beginPass(FakeCommandBuffer, produce);
        CHECK(throws([&] { executor.beginPass(FakeCommandBuffer, produce); }));
        executor.skipPass(consume);
        CHECK(throws([&] { executor.finishFrameExecution(); }));
        executor.beginPass(FakeCommandBuffer, again);
        CHECK(throws([&] { executor.skipPass(RenderGraph::PassId{ 3 }); }));
        executor.finishFrameExecution();

        // Begin and skip address the same cursor.
        executor.onFrameFenceCompleted(1);
        executor.beginFrameExecution(1);
        executor.skipPass(produce);
        executor.beginPass(FakeCommandBuffer, consume);
        executor.skipPass(again);
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
        // produce: 3; then (R4a) the frame-end exports, since the skipped
        // consumers left every exported resource away from its final access.
        CHECK(second.size() == 6);
        CHECK(second[0].oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        CHECK(second[1].oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        CHECK(second[2].srcAccess == (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT));
        CHECK(second[3].handle == colorImage &&
            second[3].oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
            second[3].newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        CHECK(second[4].handle == depthImage &&
            second[4].oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
            second[4].newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        CHECK(second[5].handle == dataBuffer &&
            second[5].srcAccess == VK_ACCESS_2_SHADER_WRITE_BIT &&
            second[5].dstAccess == (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT));

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
        CHECK(throws([&] { executor.bindExternalBuffer(0, executor.findResource("unknown"),
            handle, 256); }));
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
        RecordingBarrierSink sink;
        VulkanRenderGraphExecutor executor;
        executor.setBarrierSink(&sink);
        executor.init(factory, 1);
        CHECK(executor.barrierApi() == VulkanBarrierApi::Synchronization1);
        CHECK(!vulkanDeviceSupportsSynchronization2(VK_NULL_HANDLE));
        executor.rebuild(smallGraph());
        executor.beginFrameExecution(0);
        CHECK(throws([&] { executor.setBarrierApi(VulkanBarrierApi::Synchronization2); }));
        for (uint32_t pass = 0; pass < 3; ++pass) executor.skipPass(RenderGraph::PassId{ pass });
        // R4a: the three exports still move to their final access, which
        // needs the frame's command buffer.
        CHECK(throws([&] { executor.finishFrameExecution(); }));
        executor.setFrameRecordContext({ FakeCommandBuffer, 0 });
        executor.finishFrameExecution();
        CHECK(sink.recorded().size() == 3 && sink.sync1Calls == 3);
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
        // Beginning p3 drains the registered p2 before it.
        executor.beginPass(FakeCommandBuffer, executor.passId("p3"));
        CHECK(fixture.owner.executions[2] == 1);
        CHECK(throws([&] { executor.finishFrameExecution(); }));
        executor.skipPass(executor.passId("p4"));
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
                // The group closes after p3; the skipped p4 leaves the export
                // (TransferSource) to the frame-end barrier (R4a).
                CHECK(events[log.count - 2].kind == EventKind::RangeEnd);
                CHECK(events[log.count - 1].kind == EventKind::Barrier);
            }
        }
        // Unregistering a grouped pass dissolves the group.
        executor.unregisterPass(ChainFixture::pass(2));
        executor.registerRangeGroup({ "gpu.single", ChainFixture::pass(3), ChainFixture::pass(3) });
        return true;
    }

    // AroundBarriers (R3c.2): the range covers only the pass's barriers and is
    // closed before its execute callback records.
    bool testGpuRangeAroundBarriers() {
        ChainFixture fixture;
        auto& executor = fixture.executor;
        auto& log = fixture.log;
        executor.registerPass(ChainFixture::pass(1), testCallbacks(fixture.owner, 1,
            "gpu.transition", GpuRangePlacement::AroundBarriers));
        executor.beginFrameExecution(0);
        executor.beginPass(FakeCommandBuffer, ChainFixture::pass(0));
        log.count = 0;
        executor.drainRegisteredThrough(ChainFixture::pass(1));
        const auto events = log.recorded();
        const EventKind expected[] = { EventKind::Active, EventKind::RangeBegin,
            EventKind::Barrier, EventKind::RangeEnd, EventKind::Execute };
        CHECK(events.size() == std::size(expected));
        for (size_t index = 0; index < events.size(); ++index)
            CHECK(events[index].kind == expected[index]);
        CHECK(std::string_view(events[1].name) == "gpu.transition");
        CHECK(log.countOf(EventKind::RangeEnd) == 1);
        executor.skipPass(ChainFixture::pass(2));
        executor.skipPass(ChainFixture::pass(3));
        executor.skipPass(ChainFixture::pass(4));
        executor.finishFrameExecution();
        return true;
    }

    // R3c explicit drain points: the owner runs its registered passes where
    // they used to be recorded, in compiled order, never past an imperative one.
    bool testExplicitDrainPoints() {
        ChainFixture fixture;
        auto& executor = fixture.executor;
        auto& log = fixture.log;
        executor.registerPass(ChainFixture::pass(1), testCallbacks(fixture.owner, 1, "gpu.p1"));
        executor.registerPass(ChainFixture::pass(2), testCallbacks(fixture.owner, 2));
        executor.registerPass(ChainFixture::pass(4), testCallbacks(fixture.owner, 4));
        executor.beginFrameExecution(0);
        executor.beginPass(FakeCommandBuffer, ChainFixture::pass(0));
        // An undeclared (invalid) pass is a no-op; an unregistered target throws.
        executor.drainRegisteredThrough(RenderGraph::PassId{});
        CHECK(throws([&] { executor.drainRegisteredThrough(ChainFixture::pass(3)); }));
        CHECK(fixture.owner.executions[1] == 0);
        log.count = 0;
        executor.drainRegisteredThrough(ChainFixture::pass(1));
        // Only p1 ran: active, range, barrier, execute, range end.
        CHECK(fixture.owner.executions[1] == 1 && fixture.owner.executions[2] == 0);
        CHECK(log.count == 5 && log.recorded()[1].kind == EventKind::RangeBegin);
        CHECK(throws([&] { executor.drainRegisteredThrough(ChainFixture::pass(1)); }));
        executor.drainRegisteredThrough(ChainFixture::pass(2));
        CHECK(fixture.owner.executions[2] == 1);
        // Draining through p4 would skip the imperative p3.
        CHECK(throws([&] { executor.drainRegisteredThrough(ChainFixture::pass(4)); }));
        CHECK(fixture.owner.executions[4] == 0);
        executor.beginPass(FakeCommandBuffer, ChainFixture::pass(3));
        executor.drainRegisteredThrough(ChainFixture::pass(4));
        CHECK(fixture.owner.executions[4] == 1);
        executor.finishFrameExecution();
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

    // ---- R3b.10: History lifetime -----------------------------------------------

    // scene -> resolve (reads taa.previous, writes taa.current) -> post (copies
    // taa.current out, so the slot ends in TransferSource).
    RenderGraph::CompiledGraph historyGraph(uint32_t width = 64) {
        RenderGraph::RenderGraphBuilder builder;
        RenderGraph::ResourceDesc desc = imageDesc();
        desc.image.extent.width = width;
        const auto history = builder.createHistory("taa", desc);
        auto scene = builder.createResource("scene", desc);
        auto output = builder.createResource("output", desc);
        const auto scenePass = builder.addPass("scene");
        const auto resolve = builder.addPass("resolve");
        const auto post = builder.addPass("post", RenderGraph::QueueClass::Transfer);
        scene = builder.write(scenePass, scene, Access::ColorAttachment, RenderGraph::LoadOp::Clear);
        builder.read(resolve, scene, Access::SampledRead);
        builder.read(resolve, history.previous, Access::SampledRead);
        const auto current = builder.write(resolve, history.current, Access::ColorAttachment,
            RenderGraph::LoadOp::Clear);
        builder.read(post, current, Access::TransferSource);
        output = builder.write(post, output, Access::TransferDestination);
        builder.exportResource(output, Access::TransferDestination);
        RenderGraph::CompileResult result = builder.compile();
        if (!result.succeeded()) throw std::runtime_error("history graph failed to compile");
        return std::move(*result.graph);
    }

    struct HistoryFixture {
        FakeResourceFactory factory;
        RecordingBarrierSink sink;
        VulkanRenderGraphExecutor executor;
        RenderGraph::GraphResourceId previous;
        RenderGraph::GraphResourceId current;

        explicit HistoryFixture(uint32_t width = 64) {
            executor.setBarrierSink(&sink);
            executor.init(factory, 2);
            executor.setBarrierApi(VulkanBarrierApi::Synchronization2);
            rebuild(width);
        }
        void rebuild(uint32_t width) {
            executor.rebuild(historyGraph(width));
            previous = executor.resourceId("taa.previous");
            current = executor.resourceId("taa.current");
        }
        // The barrier recorded for `image` by the pass just begun, if any.
        const RecordedBarrier* barrierFor(VkImage image) const {
            for (const RecordedBarrier& barrier : sink.recorded())
                if (barrier.handle == reinterpret_cast<uint64_t>(image)) return &barrier;
            return nullptr;
        }
    };

    struct HistoryFrame {
        VkImage previous = VK_NULL_HANDLE;
        VkImage current = VK_NULL_HANDLE;
        bool valid = false;
        RecordedBarrier previousBarrier{};
        bool previousBarriered = false;
    };

    // Runs one frame on slot frame % 2; `runResolve` false skips the writer.
    HistoryFrame runHistoryFrame(HistoryFixture& fixture, uint32_t frame,
        RenderGraph::ViewHistoryContext view, bool runResolve = true) {
        auto& executor = fixture.executor;
        const uint32_t slot = frame % 2;
        executor.onFrameFenceCompleted(slot);
        executor.beginFrameExecution(slot, view);
        HistoryFrame result{};
        result.previous = executor.image(slot, fixture.previous).image;
        result.current = executor.image(slot, fixture.current).image;
        result.valid = executor.historyValid(fixture.previous);
        executor.beginPass(FakeCommandBuffer, executor.passId("scene"));
        if (runResolve) {
            fixture.sink.clear();
            executor.beginPass(FakeCommandBuffer, executor.passId("resolve"));
            if (const RecordedBarrier* barrier = fixture.barrierFor(result.previous)) {
                result.previousBarrier = *barrier;
                result.previousBarriered = true;
            }
            // The flip at the writer keeps the frame's mapping stable.
            if (executor.image(slot, fixture.previous).image != result.previous ||
                executor.image(slot, fixture.current).image != result.current)
                result.previous = result.current = VK_NULL_HANDLE;
            executor.beginPass(FakeCommandBuffer, executor.passId("post"));
        }
        else {
            executor.skipPass(executor.passId("resolve"));
            executor.skipPass(executor.passId("post"));
        }
        executor.finishFrameExecution();
        return result;
    }

    bool testHistoryPairLifetime() {
        HistoryFixture fixture;
        auto& executor = fixture.executor;
        const RenderGraph::CompiledGraph& graph = *executor.compiledGraph();
        CHECK(graph.historyPairs().size() == 1);
        CHECK(graph.historySlots().size() == 2);
        CHECK(graph.resources()[fixture.previous.logical].physicalSlot == RenderGraph::InvalidIndex);
        CHECK(graph.resources()[fixture.current.logical].physicalSlot == RenderGraph::InvalidIndex);
        CHECK(executor.stats().historySlotCount == 2);
        CHECK(executor.stats().physicalSlotCount == graph.physicalSlots().size());
        CHECK(fixture.factory.createCount == graph.physicalSlots().size() * 2 + 2);

        // The pair is distinct from every per-frame resource.
        std::vector<VkImage> images;
        for (uint32_t slot = 0; slot < 2; ++slot) {
            images.push_back(executor.image(slot, executor.resourceId("scene")).image);
            images.push_back(executor.image(slot, executor.resourceId("output")).image);
        }
        images.push_back(executor.image(0, fixture.previous).image);
        images.push_back(executor.image(0, fixture.current).image);
        std::vector<VkImage> sorted = images;
        std::sort(sorted.begin(), sorted.end());
        CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
        // Global: both frame slots see the same pair images.
        CHECK(executor.image(0, fixture.previous).image == executor.image(1, fixture.previous).image);

        std::vector<HistoryFrame> frames;
        for (uint32_t frame = 0; frame < 6; ++frame)
            frames.push_back(runHistoryFrame(fixture, frame, { 1, 0 }));
        CHECK(!frames[0].valid);
        for (uint32_t frame = 0; frame < frames.size(); ++frame) {
            CHECK(frames[frame].previous != VK_NULL_HANDLE);   // stable within the frame
            CHECK(frames[frame].previous != frames[frame].current);
            if (frame == 0) continue;
            CHECK(frames[frame].valid);
            // previous(N+1) == current(N): the pair alternates.
            CHECK(frames[frame].previous == frames[frame - 1].current);
            CHECK(frames[frame].current == frames[frame - 1].previous);
        }
        // Frame 0: nothing valid, the previous slot starts undefined.
        CHECK(frames[0].previousBarriered);
        CHECK(frames[0].previousBarrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
        // Frame 1 (the other frame slot) continues from the state frame 0 left:
        // the slot was copied from (TransferSource), not reset per slot.
        CHECK(frames[1].previousBarriered);
        CHECK(frames[1].previousBarrier.oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        CHECK(frames[1].previousBarrier.srcAccess == VK_ACCESS_2_TRANSFER_READ_BIT);
        CHECK(frames[1].previousBarrier.newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        CHECK(throws([&] { (void)executor.historyValid(executor.resourceId("scene")); }));
        executor.cleanupAfterDeviceIdle();
        CHECK(fixture.factory.destroyCount == fixture.factory.createCount);
        return true;
    }

    bool testHistoryValidity() {
        HistoryFixture fixture;
        auto& executor = fixture.executor;
        uint32_t frame = 0;
        const auto next = [&](RenderGraph::ViewHistoryContext view, bool writer = true) {
            return runHistoryFrame(fixture, frame++, view, writer);
        };
        CHECK(!next({ 1, 0 }).valid);                 // first frame
        CHECK(next({ 1, 0 }).valid);                  // after a written frame
        const HistoryFrame reset = next({ 1, 1 });    // reset revision changed
        CHECK(!reset.valid);
        // Invalid previous contents are discarded: UNDEFINED, but the source
        // scope still covers the slot's last (transfer) access.
        CHECK(reset.previousBarriered);
        CHECK(reset.previousBarrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
        CHECK(reset.previousBarrier.srcStages == VK_PIPELINE_STAGE_2_TRANSFER_BIT);
        CHECK(reset.previousBarrier.srcAccess == VK_ACCESS_2_TRANSFER_READ_BIT);
        CHECK(next({ 1, 1 }).valid);
        CHECK(!next({ 2, 1 }).valid);                 // view identity changed
        const HistoryFrame beforeSkip = next({ 2, 1 }, false);   // writer skipped
        CHECK(beforeSkip.valid);
        const HistoryFrame afterSkip = next({ 2, 1 });
        CHECK(!afterSkip.valid);                      // previous is two frames old
        CHECK(afterSkip.previous == beforeSkip.previous);        // no flip on a skip
        CHECK(next({ 2, 1 }).valid);
        // The single-argument begin reuses the last view: no invalidation.
        executor.onFrameFenceCompleted(frame % 2);
        executor.beginFrameExecution(frame % 2);
        CHECK(executor.historyValid(fixture.previous));
        for (const char* pass : { "scene", "resolve", "post" })
            executor.beginPass(FakeCommandBuffer, executor.passId(pass));
        executor.finishFrameExecution();
        ++frame;

        fixture.rebuild(64);                          // rebuild
        CHECK(!next({ 2, 1 }).valid);
        CHECK(next({ 2, 1 }).valid);
        fixture.rebuild(128);                         // resize
        CHECK(!next({ 2, 1 }).valid);
        CHECK(next({ 2, 1 }).valid);
        executor.cleanupAfterDeviceIdle();
        return true;
    }

    bool testHistoryRetirement() {
        HistoryFixture fixture;
        auto& executor = fixture.executor;
        const size_t poolSlots = executor.compiledGraph()->physicalSlots().size();
        (void)runHistoryFrame(fixture, 0, { 1, 0 });
        const size_t destroyedBefore = fixture.factory.destroyCount;
        fixture.rebuild(64);
        CHECK(fixture.factory.destroyCount == destroyedBefore);
        CHECK(executor.stats().historySlotCount == 2);
        executor.onFrameFenceCompleted(0);
        // Slot 0's pool resources go; the old pair waits for every slot.
        CHECK(fixture.factory.destroyCount == destroyedBefore + poolSlots);
        executor.onFrameFenceCompleted(1);
        CHECK(fixture.factory.destroyCount == destroyedBefore + poolSlots * 2 + 2);
        executor.cleanupAfterDeviceIdle();
        CHECK(fixture.factory.destroyCount == fixture.factory.createCount);
        return true;
    }

    bool testProductionDeclaresNoHistory() {
        for (const NamedTopology& topology : productionTopologies()) {
            CHECK(topology.graph.historyPairs().empty());
            CHECK(topology.graph.historySlots().empty());
            CHECK(topology.graph.historyResources().empty());
        }
        return true;
    }

    // ---- R3b.6 groundwork: imported images and variable-size buffers ------------

    RenderGraph::ResourceDesc importedImage(Access initial,
        RenderGraph::Format format = RenderGraph::Format::Rgba16Float) {
        RenderGraph::ResourceDesc desc = imageDesc(format);
        desc.lifetime = RenderGraph::ResourceLifetime::External;
        desc.imported = true;
        desc.initialAccess = initial;
        return desc;
    }

    VulkanImageResource fakeImage(uintptr_t handle,
        VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT) {
        VulkanImageResource image{};
        image.image = reinterpret_cast<VkImage>(handle);
        image.view = reinterpret_cast<VkImageView>(handle + 1);
        image.format = format;
        image.extent = { 64, 32 };
        return image;
    }

    RenderGraph::CompiledGraph importGraph() {
        RenderGraph::RenderGraphBuilder builder;
        auto owned = builder.createResource("ext.owned", importedImage(Access::SampledRead));
        auto perFrame = builder.createResource("ext.per-frame", importedImage(Access::Present));
        auto owner = builder.createResource("ext.owner", importedImage(Access::SampledRead));
        const auto write = builder.addPass("write");
        const auto read = builder.addPass("read");
        owned = builder.write(write, owned, Access::StorageWrite);
        perFrame = builder.write(write, perFrame, Access::ColorAttachment, RenderGraph::LoadOp::Clear);
        owner = builder.write(write, owner, Access::TransferDestination);
        builder.read(read, owned, Access::SampledRead);
        builder.read(read, perFrame, Access::SampledRead);
        builder.read(read, owner, Access::SampledRead);
        RenderGraph::CompileResult result = builder.compile();
        if (!result.succeeded()) throw std::runtime_error("import graph failed to compile");
        return std::move(*result.graph);
    }

    bool testExternalImagePolicies() {
        FakeResourceFactory factory;
        RecordingBarrierSink sink;
        VulkanRenderGraphExecutor executor;
        executor.setBarrierSink(&sink);
        executor.init(factory, 2);
        executor.setBarrierApi(VulkanBarrierApi::Synchronization2);
        executor.rebuild(importGraph());
        const auto owned = executor.resourceId("ext.owned");
        const auto perFrame = executor.resourceId("ext.per-frame");
        const auto owner = executor.resourceId("ext.owner");
        const VulkanImageResource ownedImage = fakeImage(0x5000);
        const auto perFramePolicy = ExternalSyncPolicy::ownerManaged();

        // Validation of the binding itself.
        CHECK(throws([&] { executor.bindExternalImage(0, owned,
            fakeImage(0x5100, VK_FORMAT_R8G8B8A8_UNORM), Access::SampledRead); }));
        VulkanImageResource layered = fakeImage(0x5200);
        layered.arrayLayers = 6;
        CHECK(throws([&] { executor.bindExternalImage(0, owned, layered, Access::SampledRead); }));
        CHECK(throws([&] { executor.bindExternalImage(2, owned, ownedImage, Access::SampledRead); }));
        CHECK(throws([&] { executor.bindExternalImage(0, RenderGraph::GraphResourceId{ 9 },
            ownedImage, Access::SampledRead); }));

        executor.bindExternalImage(VulkanGlobalBinding, owned, ownedImage, Access::SampledRead);
        CHECK(throws([&] { executor.bindExternalImage(0, owned, ownedImage, Access::SampledRead); }));
        CHECK(throws([&] { executor.bindExternalImage(0, owner, ownedImage, Access::SampledRead,
            ExternalSyncPolicy::ownerManaged()); }));   // aliases ext.owned
        executor.bindExternalImage(0, perFrame, fakeImage(0x6000), Access::Undefined, perFramePolicy);
        CHECK(!executor.validateFrame(1));           // per-frame import unbound on slot 1
        executor.bindExternalImage(1, perFrame, fakeImage(0x6100), Access::Undefined, perFramePolicy);
        executor.bindExternalImage(0, owner, fakeImage(0x7000), Access::SampledRead,
            ExternalSyncPolicy::ownerManaged());
        executor.bindExternalImage(1, owner, fakeImage(0x7000), Access::SampledRead,
            ExternalSyncPolicy::ownerManaged());     // non-executor-owned may share slots
        CHECK(executor.validateFrame(0) && executor.validateFrame(1));
        CHECK(executor.image(1, owned).image == ownedImage.image);
        CHECK(executor.image(1, perFrame).image == reinterpret_cast<VkImage>(uintptr_t{ 0x6100 }));

        // Frame 0: only the executor-owned import is barriered.
        executor.beginFrameExecution(0);
        CHECK(throws([&] { executor.bindExternalImage(VulkanGlobalBinding, owned, ownedImage,
            Access::SampledRead); }));
        CHECK(throws([&] { executor.bindExternalImage(0, perFrame, fakeImage(0x6000),
            Access::Undefined, perFramePolicy); }));
        sink.clear();
        executor.beginPass(FakeCommandBuffer, executor.passId("write"));
        CHECK(sink.recorded().size() == 1);
        CHECK(sink.recorded()[0].handle == 0x5000);
        CHECK(sink.recorded()[0].oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        CHECK(sink.recorded()[0].newLayout == VK_IMAGE_LAYOUT_GENERAL);
        CHECK(sink.recorded()[0].range.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
        CHECK(executor.externalImageAccess(0, perFrame) == Access::Undefined);   // untracked
        sink.clear();
        executor.beginPass(FakeCommandBuffer, executor.passId("read"));
        CHECK(sink.recorded().size() == 1);
        CHECK(sink.recorded()[0].newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        executor.finishFrameExecution();
        CHECK(executor.externalImageAccess(VulkanGlobalBinding, owned) == Access::SampledRead);
        CHECK(executor.externalImageAccess(0, owner) == Access::SampledRead);

        // Frame 1 (slot 1): the global binding's state carried over.
        executor.beginFrameExecution(1);
        sink.clear();
        executor.beginPass(FakeCommandBuffer, executor.passId("write"));
        CHECK(sink.recorded().size() == 1);
        CHECK(sink.recorded()[0].oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        executor.beginPass(FakeCommandBuffer, executor.passId("read"));
        executor.finishFrameExecution();

        // Frame 2 (slot 0) rebinds the per-frame import after its fence.
        executor.onFrameFenceCompleted(0);
        executor.bindExternalImage(0, perFrame, fakeImage(0x6000), Access::Undefined, perFramePolicy);
        executor.beginFrameExecution(0);
        executor.beginPass(FakeCommandBuffer, executor.passId("write"));
        executor.beginPass(FakeCommandBuffer, executor.passId("read"));
        executor.finishFrameExecution();
        executor.cleanupAfterDeviceIdle();
        CHECK(factory.createCount == 0);
        return true;
    }

    // Production imports (R3b.6, R4a: every import is executor-owned).
    // Shadow maps are globals bound SampledRead: their writing pass moves the
    // whole image DS_RO -> DS_ATT (never from UNDEFINED, the contents are
    // kept) and the next reader moves it back, so every frame ends
    // SampledRead. The swapchain is bound per frame after acquire, current =
    // Present and discarded on first use: the UI pass transitions it from
    // UNDEFINED with the Present (BOTTOM_OF_PIPE) source scope, which chains
    // with the acquire wait, and the frame-end export moves it to PRESENT.
    bool testProductionImportedImagePolicies() {
        FakeResourceFactory factory;
        RecordingBarrierSink sink;
        VulkanRenderGraphExecutor executor;
        executor.setBarrierSink(&sink);
        executor.init(factory, 2);
        executor.setBarrierApi(VulkanBarrierApi::Synchronization2);
        executor.rebuild(buildVulkanProductionRenderGraph({ 640, 360 },
            VK_FORMAT_B8G8R8A8_SRGB));
        const VulkanProductionGraphIds ids = resolveVulkanProductionGraphIds(executor);
        CHECK(ids.swapchain.isValid() && ids.shadowDirectionalMap.isValid() &&
            ids.shadowSpotMap.isValid());
        const auto shadowImage = [](uintptr_t handle, uint32_t resolution,
            uint32_t layers) {
            VulkanImageResource image{};
            image.image = reinterpret_cast<VkImage>(handle);
            image.view = reinterpret_cast<VkImageView>(handle + 1);
            image.format = VK_FORMAT_D32_SFLOAT;
            image.extent = { resolution, resolution };
            image.arrayLayers = layers;
            return image;
        };
        const auto swapchain = [](uintptr_t handle) {
            VulkanImageResource image{};
            image.image = reinterpret_cast<VkImage>(handle);
            image.format = VK_FORMAT_B8G8R8A8_SRGB;
            image.extent = { 640, 360 };
            return image;
        };
        const auto present = ExternalSyncPolicy::discardOnFirstUse();
        const auto shadow = ExternalSyncPolicy::executorOwned();
        struct ShadowImport {
            RenderGraph::GraphResourceId id;
            VulkanImageResource image;
            const char* writer;
        };
        constexpr std::array<uint32_t, 3> PointResolutions{ 256, 512, 1024 };
        constexpr std::array<uint32_t, 3> PointCapacities{ kPointShadowPool256Capacity,
            kPointShadowPool512Capacity, kPointShadowPool1024Capacity };
        std::vector<ShadowImport> shadows{
            { ids.shadowDirectionalMap,
                shadowImage(0xB000, 4096, kDirectionalShadowLayerCount),
                "shadow.directional" },
            { ids.shadowSpotMap, shadowImage(0xB100, 8192, 1), "shadow.spot" },
        };
        for (uint32_t tier = 0; tier < 3; ++tier)
            shadows.push_back({ ids.shadowPointMaps[tier],
                shadowImage(0xB200 + 0x10 * tier, PointResolutions[tier],
                    PointCapacities[tier] * 6u), "shadow.point" });
        // Like the backend before a slot's first acquire: both on image 0.
        executor.bindExternalImage(0, ids.swapchain, swapchain(0xA000), Access::Present,
            present);
        executor.bindExternalImage(1, ids.swapchain, swapchain(0xA000), Access::Present,
            present);
        for (const ShadowImport& import : shadows)
            executor.bindExternalImage(VulkanGlobalBinding, import.id, import.image,
                Access::SampledRead, shadow);
        // A pool whose capacity differs from the declaration is rejected.
        CHECK(throws([&] { executor.bindExternalImage(VulkanGlobalBinding,
            ids.shadowPointMaps[0], shadowImage(0xB300, 256, 6), Access::SampledRead,
            shadow); }));
        CHECK(executor.validateFrame(0) && executor.validateFrame(1));

        for (uint32_t frame = 0; frame < 4; ++frame) {
            const uint32_t slot = frame % 2;
            const uintptr_t acquired = frame >= 2 ? 0xA010 : 0xA000;
            if (frame >= 2) {
                executor.onFrameFenceCompleted(slot);
                executor.bindExternalImage(slot, ids.swapchain, swapchain(acquired),
                    Access::Present, present);
            }
            sink.clear();
            executor.beginFrameExecution(slot);
            std::vector<std::string_view> barrierPass;
            for (const auto& pass : executor.compiledGraph()->passes()) {
                executor.beginPass(FakeCommandBuffer, executor.passId(pass.name));
                barrierPass.resize(sink.recorded().size(), pass.name);
            }
            executor.finishFrameExecution();
            barrierPass.resize(sink.recorded().size(), "frame-end");
            const auto barriersOn = [&](uint64_t handle) {
                std::vector<size_t> found;
                for (size_t index = 0; index < sink.recorded().size(); ++index)
                    if (sink.recorded()[index].handle == handle) found.push_back(index);
                return found;
            };
            for (const ShadowImport& import : shadows) {
                const std::vector<size_t> found =
                    barriersOn(reinterpret_cast<uint64_t>(import.image.image));
                CHECK(executor.externalImageAccess(VulkanGlobalBinding, import.id) ==
                    Access::SampledRead);
                CHECK(found.size() == 2);
                if (found.size() != 2) continue;
                const RecordedBarrier& toWrite = sink.recorded()[found[0]];
                const RecordedBarrier& toRead = sink.recorded()[found[1]];
                CHECK(barrierPass[found[0]] == import.writer);
                CHECK(barrierPass[found[1]] == "probe.capture");
                CHECK(toWrite.oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
                CHECK(toWrite.newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
                CHECK(toWrite.range.baseArrayLayer == 0 &&
                    toWrite.range.layerCount == import.image.arrayLayers);
                CHECK(toRead.oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
                CHECK(toRead.newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
                CHECK(toRead.srcAccess == (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT));
            }
            const std::vector<size_t> presented = barriersOn(acquired);
            CHECK(presented.size() == 2);
            if (presented.size() == 2) {
                const RecordedBarrier& toColor = sink.recorded()[presented[0]];
                const RecordedBarrier& toPresent = sink.recorded()[presented[1]];
                CHECK(barrierPass[presented[0]] == "ui-present");
                CHECK(toColor.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
                CHECK(toColor.newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                CHECK(toColor.srcStages == VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
                CHECK(toColor.dstStages == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
                CHECK(barrierPass[presented[1]] == "frame-end");
                CHECK(toPresent.oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                CHECK(toPresent.newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
                CHECK(toPresent.srcAccess == VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT ||
                    (toPresent.srcAccess & VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT) != 0);
            }
            CHECK(executor.externalImageAccess(slot, ids.swapchain) == Access::Present);
        }
        executor.cleanupAfterDeviceIdle();
        return true;
    }

    bool testVariableSizeImportedBuffer() {
        const auto build = [](bool variable) {
            RenderGraph::RenderGraphBuilder builder;
            RenderGraph::ResourceDesc desc = bufferDesc(256);
            desc.lifetime = RenderGraph::ResourceLifetime::External;
            desc.imported = true;
            desc.initialAccess = Access::StorageRead;
            desc.buffer.variableSize = variable;
            auto buffer = builder.createResource("commands", desc);
            buffer = builder.write(builder.addPass("compact"), buffer, Access::StorageWrite);
            builder.read(builder.addPass("draw"), buffer, Access::IndirectRead);
            return std::move(*builder.compile().graph);
        };
        const RenderGraph::CompiledGraph fixed = build(false);
        const RenderGraph::CompiledGraph variable = build(true);
        CHECK(fixed.topologyHash() != variable.topologyHash());
        RenderGraph::RenderGraphBuilder rejecting;
        RenderGraph::ResourceDesc transient = bufferDesc();
        transient.buffer.variableSize = true;
        CHECK(throws([&] { (void)rejecting.createResource("bad", transient); }));

        for (const bool isVariable : { false, true }) {
            FakeResourceFactory factory;
            RecordingBarrierSink sink;
            VulkanRenderGraphExecutor executor;
            executor.setBarrierSink(&sink);
            executor.init(factory, 1);
            executor.rebuild(isVariable ? variable : fixed);
            const auto id = executor.resourceId("commands");
            CHECK(throws([&] { executor.bindExternalBuffer(0, id,
                reinterpret_cast<VkBuffer>(uintptr_t{ 0x900 }), 128, Access::StorageRead); }));
            executor.bindExternalBuffer(0, id, reinterpret_cast<VkBuffer>(uintptr_t{ 0x900 }),
                4096, Access::StorageRead);
            executor.beginFrameExecution(0);
            executor.beginPass(FakeCommandBuffer, executor.passId("compact"));
            executor.beginPass(FakeCommandBuffer, executor.passId("draw"));
            executor.finishFrameExecution();
            CHECK(sink.recorded().size() == 2);
            for (const RecordedBarrier& barrier : sink.recorded())
                CHECK(barrier.size == (isVariable ? 4096u : 256u));
            executor.cleanupAfterDeviceIdle();
        }
        return true;
    }

    // Steady frames with everything R3b adds (History, a callback pass with a
    // GPU range, imported image and buffer, batched barriers) allocate nothing.
    struct AllocationOwner {
        uint32_t executions = 0;
    };

    bool testSteadyFramesAllocateNothing() {
        RenderGraph::RenderGraphBuilder builder;
        RenderGraph::ResourceDesc desc = imageDesc();
        const auto history = builder.createHistory("taa", desc);
        auto scene = builder.createResource("scene", desc);
        auto output = builder.createResource("output", desc);
        auto depth = builder.createResource("imported.depth",
            importedImage(Access::SampledRead, RenderGraph::Format::D32Float));
        RenderGraph::ResourceDesc bufferImport = bufferDesc(256);
        bufferImport.lifetime = RenderGraph::ResourceLifetime::External;
        bufferImport.imported = true;
        bufferImport.initialAccess = Access::StorageRead;
        bufferImport.buffer.variableSize = true;
        auto commands = builder.createResource("imported.commands", bufferImport);
        const auto scenePass = builder.addPass("scene");
        const auto resolve = builder.addPass("resolve");
        const auto post = builder.addPass("post");
        scene = builder.write(scenePass, scene, Access::ColorAttachment, RenderGraph::LoadOp::Clear);
        builder.read(scenePass, depth, Access::SampledRead);
        builder.read(resolve, scene, Access::SampledRead);
        builder.read(resolve, history.previous, Access::SampledRead);
        const auto current = builder.write(resolve, history.current, Access::ColorAttachment,
            RenderGraph::LoadOp::Clear);
        builder.read(post, current, Access::TransferSource);
        output = builder.write(post, output, Access::TransferDestination);
        commands = builder.write(post, commands, Access::StorageWrite);
        builder.exportResource(output, Access::TransferDestination);
        const auto compiled = builder.compile();
        CHECK(compiled.succeeded());

        FakeResourceFactory factory;
        RecordingBarrierSink sink;
        EventLog ranges;
        VulkanRenderGraphExecutor executor;
        executor.setBarrierSink(&sink);
        executor.init(factory, 2);
        executor.setBarrierApi(VulkanBarrierApi::Synchronization2);
        executor.rebuild(*compiled.graph);
        executor.setGpuRangeSink(rangeSinkFor(ranges));
        AllocationOwner owner;
        VulkanPassCallbacks callbacks{};
        callbacks.owner = &owner;
        callbacks.execute = [](void* self, VulkanPassContext&) {
            ++static_cast<AllocationOwner*>(self)->executions;
        };
        callbacks.gpuRange = "gpu.post";
        callbacks.placement = GpuRangePlacement::AfterBarriers;
        const auto scenePassId = executor.passId("scene");
        const auto resolveId = executor.passId("resolve");
        const auto postId = executor.passId("post");
        const auto previousId = executor.resourceId("taa.previous");
        executor.registerPass(postId, callbacks);
        executor.bindExternalImage(VulkanGlobalBinding, executor.resourceId("imported.depth"),
            fakeImage(0xA000, VK_FORMAT_D32_SFLOAT), Access::SampledRead);
        for (uint32_t slot = 0; slot < 2; ++slot)
            executor.bindExternalBuffer(slot, executor.resourceId("imported.commands"),
                reinterpret_cast<VkBuffer>(uintptr_t{ 0xB000 + slot }), 1024, Access::StorageRead);

        const auto frame = [&](uint32_t index) {
            const uint32_t slot = index % 2;
            sink.clear();
            ranges.count = 0;
            executor.onFrameFenceCompleted(slot);
            executor.beginFrameExecution(slot, { 3, 0 });
            (void)executor.historyValid(previousId);
            (void)executor.image(slot, previousId);
            executor.beginPass(FakeCommandBuffer, scenePassId);
            if (index % 3 == 2) executor.skipPass(resolveId);
            else executor.beginPass(FakeCommandBuffer, resolveId);
            executor.finishFrameExecution();   // drains the registered post pass
        };
        for (uint32_t index = 0; index < 4; ++index) frame(index);
        // The profiler is live in this binary (not vacuously zero).
        beginCpuAllocationFrame();
        auto probe = std::make_unique<std::array<uint8_t, 64>>();
        CHECK(endCpuAllocationFrame().allocationCount == 1 && probe != nullptr);
        beginCpuAllocationFrame();
        for (uint32_t index = 4; index < 40; ++index) frame(index);
        const CpuAllocationFrameSample sample = endCpuAllocationFrame();
        CHECK(owner.executions == 40);
        CHECK(sink.overflow == 0);
        std::cout << "  36 steady frames: " << sample.allocationCount << " allocations, "
            << sample.requestedBytes << " bytes\n";
        CHECK(sample.allocationCount == 0);
        executor.cleanupAfterDeviceIdle();
        return true;
    }

    // ---- M7R R4b.4 transient aliasing ----------------------------------------

    // first (clear) -> readFirst -> second (clear, reuses first's memory) ->
    // readSecond, plus a dedicated exported output. Equal sizes, so the
    // planner puts `second` at `first`'s offset with `first` as predecessor.
    RenderGraph::CompiledGraph aliasingGraph(bool aliasing) {
        RenderGraph::RenderGraphBuilder builder;
        auto first = builder.createResource("first", imageDesc());
        auto second = builder.createResource("second", imageDesc());
        auto output = builder.createResource("output", imageDesc());
        const auto writeFirst = builder.addPass("write-first");
        const auto readFirst = builder.addPass("read-first");
        const auto writeSecond = builder.addPass("write-second");
        const auto readSecond = builder.addPass("read-second");
        first = builder.write(writeFirst, first, Access::ColorAttachment,
            RenderGraph::LoadOp::Clear);
        builder.read(readFirst, first, Access::SampledRead);
        output = builder.write(readFirst, output, Access::ColorAttachment,
            RenderGraph::LoadOp::Clear);
        second = builder.write(writeSecond, second, Access::StorageWrite,
            RenderGraph::LoadOp::DontCare);
        builder.declareWholeResourceWrite(second);
        builder.read(readSecond, second, Access::SampledRead);
        output = builder.write(readSecond, output, Access::ColorAttachment,
            RenderGraph::LoadOp::Load);
        builder.exportResource(output, Access::SampledRead);
        RenderGraph::CompileResult result = builder.compile(
            RenderGraph::CompileOptions{ .transientAliasing = aliasing });
        if (!result.succeeded()) throw std::runtime_error("aliasing graph failed to compile");
        return std::move(*result.graph);
    }

    const RecordedBarrier* findBarrier(std::span<const RecordedBarrier> barriers,
        VkImage image) {
        for (const RecordedBarrier& barrier : barriers)
            if (barrier.handle == reinterpret_cast<uint64_t>(image)) return &barrier;
        return nullptr;
    }

    bool testAliasedFirstUseBarriers() {
        FakeResourceFactory factory;
        RecordingBarrierSink sink;
        VulkanRenderGraphExecutor executor;
        executor.setBarrierSink(&sink);
        executor.init(factory, 2);
        executor.setBarrierApi(VulkanBarrierApi::Synchronization2);
        executor.rebuild(aliasingGraph(true));
        const RenderGraph::CompiledGraph& graph = *executor.compiledGraph();
        const auto firstId = executor.resourceId("first");
        const auto secondId = executor.resourceId("second");
        CHECK(graph.physicalSlots()[graph.resources()[firstId.logical].physicalSlot].aliased);
        CHECK(graph.physicalSlots()[graph.resources()[secondId.logical].physicalSlot].aliased);
        CHECK(!graph.physicalSlots()[graph.resources()[
            executor.resourceId("output").logical].physicalSlot].aliased);
        const VulkanGraphStats stats = executor.stats();
        CHECK(stats.transientAliasing && stats.aliasHeapCount == 1 &&
            stats.aliasedResourceCount == 2);
        // Default fake requirement: 64 KiB each, packed into one 64 KiB heap.
        CHECK(stats.aliasedRequestedBytes == 2u * 2u * 65536u);
        CHECK(stats.aliasHeapCommittedBytes == 2u * 65536u);
        CHECK(executor.aliasHeaps(0).size() == 1 && executor.aliasHeaps(1).size() == 1);

        const auto pass = [&](const char* name) { return executor.passId(name); };
        const VulkanGraphAccessInfo sampled = getVulkanGraphAccessInfo(
            Access::SampledRead, RenderGraph::ResourceType::Image);
        const VulkanGraphAccessInfo color = getVulkanGraphAccessInfo(
            Access::ColorAttachment, RenderGraph::ResourceType::Image);
        for (uint32_t frame = 0; frame < 4; ++frame) {
            const uint32_t slot = frame % 2;
            executor.onFrameFenceCompleted(slot);
            executor.beginFrameExecution(slot);
            const VkImage firstImage = executor.image(slot, firstId).image;
            const VkImage secondImage = executor.image(slot, secondId).image;
            // first: its frame's first use discards (UNDEFINED) and waits on
            // nothing (no predecessor; the slot's last frame has retired).
            sink.clear();
            executor.beginPass(FakeCommandBuffer, pass("write-first"));
            const RecordedBarrier* barrier = findBarrier(sink.recorded(), firstImage);
            CHECK(barrier != nullptr);
            CHECK(barrier->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
            CHECK(barrier->newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            CHECK(barrier->srcStages == VK_PIPELINE_STAGE_2_NONE && barrier->srcAccess == 0);
            CHECK(barrier->dstStages == color.stages && barrier->dstAccess == color.access);
            // Frame 3 skips first's reader: second then waits on the
            // predecessor's tracked (attachment write) access instead.
            const bool skipReader = frame == 3;
            if (skipReader) executor.skipPass(pass("read-first"));
            else executor.beginPass(FakeCommandBuffer, pass("read-first"));
            sink.clear();
            executor.beginPass(FakeCommandBuffer, pass("write-second"));
            barrier = findBarrier(sink.recorded(), secondImage);
            CHECK(barrier != nullptr);
            CHECK(barrier->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
            CHECK(barrier->newLayout == VK_IMAGE_LAYOUT_GENERAL);
            const VulkanGraphAccessInfo& source = skipReader ? color : sampled;
            CHECK(barrier->srcStages == source.stages && barrier->srcAccess == source.access);
            executor.beginPass(FakeCommandBuffer, pass("read-second"));
            executor.finishFrameExecution();
        }

        // Skip-path guard: second's writer skipped while its reader runs.
        executor.onFrameFenceCompleted(0);
        executor.beginFrameExecution(0);
        executor.beginPass(FakeCommandBuffer, pass("write-first"));
        executor.beginPass(FakeCommandBuffer, pass("read-first"));
        executor.skipPass(pass("write-second"));
        CHECK(throws([&] { executor.beginPass(FakeCommandBuffer, pass("read-second")); }));
        executor.cleanupAfterDeviceIdle();

        // Aliasing off: the same graph keeps exact-descriptor reuse (first and
        // second share one slot) and allocates no heap.
        FakeResourceFactory plainFactory;
        VulkanRenderGraphExecutor plain;
        plain.init(plainFactory, 2);
        plain.rebuild(aliasingGraph(false));
        CHECK(!plain.stats().transientAliasing && plain.stats().aliasHeapCount == 0);
        CHECK(plain.aliasHeaps(0).empty());
        const RenderGraph::CompiledGraph& off = *plain.compiledGraph();
        CHECK(off.resources()[plain.resourceId("first").logical].physicalSlot ==
            off.resources()[plain.resourceId("second").logical].physicalSlot);
        plain.cleanupAfterDeviceIdle();
        return true;
    }

    bool testAliasedSteadyFramesAllocateNothing() {
        FakeResourceFactory factory;
        RecordingBarrierSink sink;
        VulkanRenderGraphExecutor executor;
        executor.setBarrierSink(&sink);
        executor.init(factory, 2);
        executor.setBarrierApi(VulkanBarrierApi::Synchronization2);
        executor.rebuild(aliasingGraph(true));
        const RenderGraph::PassId passes[] = { executor.passId("write-first"),
            executor.passId("read-first"), executor.passId("write-second"),
            executor.passId("read-second") };
        const auto frame = [&](uint32_t index) {
            const uint32_t slot = index % 2;
            sink.clear();
            executor.onFrameFenceCompleted(slot);
            executor.beginFrameExecution(slot);
            for (const RenderGraph::PassId pass : passes) {
                if (index % 3 == 1 && pass == passes[1]) executor.skipPass(pass);
                else executor.beginPass(FakeCommandBuffer, pass);
            }
            executor.finishFrameExecution();
        };
        for (uint32_t index = 0; index < 4; ++index) frame(index);
        beginCpuAllocationFrame();
        for (uint32_t index = 4; index < 40; ++index) frame(index);
        const CpuAllocationFrameSample sample = endCpuAllocationFrame();
        CHECK(sink.overflow == 0);
        CHECK(sample.allocationCount == 0);
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
        { "id order checks", testIdOrderChecks },
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
        { "GPU range around barriers", testGpuRangeAroundBarriers },
        { "explicit drain points", testExplicitDrainPoints },
        { "callback context and reentry", testCallbackContextAndReentry },
        { "History pair lifetime", testHistoryPairLifetime },
        { "History validity", testHistoryValidity },
        { "History retirement", testHistoryRetirement },
        { "production declares no History", testProductionDeclaresNoHistory },
        { "external image policies", testExternalImagePolicies },
        { "production imported-image policies", testProductionImportedImagePolicies },
        { "variable-size imported buffer", testVariableSizeImportedBuffer },
        { "steady frames allocate nothing", testSteadyFramesAllocateNothing },
        { "aliased first-use barriers and skip guard", testAliasedFirstUseBarriers },
        { "aliased steady frames allocate nothing", testAliasedSteadyFramesAllocateNothing },
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
