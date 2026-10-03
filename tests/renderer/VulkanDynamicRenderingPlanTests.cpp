// M7R R4a.0 executor groundwork for dynamic rendering, recorded through a
// VulkanBarrierSink against fake resources (no device): per-pass rendering
// plans, VulkanPassContext::beginRendering/endRendering, the same-access
// attachment re-barrier on passes flagged as migrated, discard-on-first-use
// imports and the frame-end export transitions. No production pass is flagged
// in R4a.0, so the production topologies must record exactly what R3 did.

#include "profiling/CpuAllocationProfile.h"
#include "renderer/vulkan/VulkanProductionRenderGraph.h"
#include "renderer/vulkan/VulkanRenderGraphExecutor.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

    using namespace Iridium;
    using RenderGraph::Access;
    using RenderGraph::ClearValue;
    using RenderGraph::LoadOp;
    using RenderGraph::StoreOp;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    const VkCommandBuffer FakeCommandBuffer =
        reinterpret_cast<VkCommandBuffer>(uintptr_t{ 0xC0 });

    template <typename Function>
    bool throws(Function&& function) {
        try { function(); }
        catch (const std::exception&) { return true; }
        return false;
    }

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
            }
            else {
                result.buffer.buffer = reinterpret_cast<VkBuffer>(base);
                result.buffer.memory = reinterpret_cast<VkDeviceMemory>(base + 1);
                result.buffer.size = slot.buffer.size;
            }
            return result;
        }
        void destroy(VulkanGraphPhysicalResource& resource) noexcept override {
            resource = {};
        }
        size_t createCount = 0;
    };

    struct RecordedBarrier {
        uint64_t image = 0;
        VkPipelineStageFlags2 srcStages = 0;
        VkAccessFlags2 srcAccess = 0;
        VkPipelineStageFlags2 dstStages = 0;
        VkAccessFlags2 dstAccess = 0;
        VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    struct RecordedAttachment {
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_MAX_ENUM;
        VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_MAX_ENUM;
        VkClearValue clear{};
    };

    struct RecordedRendering {
        VkRect2D area{};
        uint32_t layers = 0;
        uint32_t colorCount = 0;
        std::array<RecordedAttachment, VulkanMaxColorAttachments> color{};
        bool hasDepth = false;
        RecordedAttachment depth{};
        bool hasStencil = false;
    };

    // Sync2 only; fixed storage so steady frames recorded here allocate nothing.
    class RecordingSink final : public VulkanBarrierSink {
    public:
        void pipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
            std::span<const VkBufferMemoryBarrier>, std::span<const VkImageMemoryBarrier>) override {
            ++sync1Calls;
        }
        void pipelineBarrier2(VkCommandBuffer commandBuffer,
            const VkDependencyInfo& dependency) override {
            if (commandBuffer != FakeCommandBuffer) ++wrongCommandBuffer;
            ++barrierCalls;
            bufferBarriers += dependency.bufferMemoryBarrierCount;
            for (uint32_t index = 0; index < dependency.imageMemoryBarrierCount; ++index) {
                const VkImageMemoryBarrier2& value = dependency.pImageMemoryBarriers[index];
                if (imageCount == images.size()) { ++overflow; continue; }
                images[imageCount++] = { reinterpret_cast<uint64_t>(value.image),
                    value.srcStageMask, value.srcAccessMask, value.dstStageMask,
                    value.dstAccessMask, value.oldLayout, value.newLayout };
            }
        }
        void beginRendering(VkCommandBuffer commandBuffer,
            const VkRenderingInfo& info) override {
            if (commandBuffer != FakeCommandBuffer) ++wrongCommandBuffer;
            if (renderingCount == renderings.size()) { ++overflow; return; }
            RecordedRendering& recorded = renderings[renderingCount++];
            recorded = {};
            recorded.area = info.renderArea;
            recorded.layers = info.layerCount;
            recorded.colorCount = info.colorAttachmentCount;
            const auto copy = [](const VkRenderingAttachmentInfo& value) {
                return RecordedAttachment{ value.imageView, value.imageLayout, value.loadOp,
                    value.storeOp, value.clearValue };
            };
            for (uint32_t index = 0; index < info.colorAttachmentCount; ++index)
                recorded.color[index] = copy(info.pColorAttachments[index]);
            recorded.hasDepth = info.pDepthAttachment != nullptr;
            if (recorded.hasDepth) recorded.depth = copy(*info.pDepthAttachment);
            recorded.hasStencil = info.pStencilAttachment != nullptr;
        }
        void endRendering(VkCommandBuffer commandBuffer) override {
            if (commandBuffer != FakeCommandBuffer) ++wrongCommandBuffer;
            ++endCount;
        }
        void clear() noexcept {
            imageCount = renderingCount = 0;
            barrierCalls = bufferBarriers = endCount = 0;
        }
        [[nodiscard]] std::span<const RecordedBarrier> barriers() const noexcept {
            return std::span(images.data(), imageCount);
        }

        std::array<RecordedBarrier, 256> images{};
        size_t imageCount = 0;
        std::array<RecordedRendering, 16> renderings{};
        size_t renderingCount = 0;
        uint32_t barrierCalls = 0;
        uint32_t bufferBarriers = 0;
        uint32_t endCount = 0;
        uint32_t sync1Calls = 0;
        uint32_t wrongCommandBuffer = 0;
        uint32_t overflow = 0;
    };

    RenderGraph::ResourceDesc imageDesc(RenderGraph::Format format =
        RenderGraph::Format::Rgba16Float, uint32_t width = 64, uint32_t height = 32) {
        RenderGraph::ResourceDesc desc{};
        desc.type = RenderGraph::ResourceType::Image;
        desc.image.format = format;
        desc.image.extent = { width, height, 1 };
        return desc;
    }

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

    bool sameClear(const VkClearValue& value, const ClearValue& expected, bool depth) {
        if (depth) {
            uint32_t bits = 0;
            std::memcpy(&bits, &value.depthStencil.depth, sizeof(bits));
            return bits == std::bit_cast<uint32_t>(expected.depth) &&
                value.depthStencil.stencil == expected.stencil;
        }
        for (uint32_t channel = 0; channel < 4; ++channel)
            if (value.color.uint32[channel] != expected.colorBits[channel]) return false;
        return true;
    }

    // "gbuffer" clears three colours (mixed float/uint) and depth; "forward"
    // loads colour and writable depth; "transparent" loads colour over
    // read-only depth; "sample" only samples (empty plan).
    const ClearValue Grey = ClearValue::color(0.25f, 0.5f, 0.75f, 1.0f);
    const ClearValue Flags = ClearValue::colorUint(7u, 0u, 0u, 9u);
    const ClearValue NegativeZero = ClearValue::color(-0.0f, 0.0f, 0.0f, 0.0f);
    const ClearValue NearDepth = ClearValue::depthStencil(0.0f, 3u);

    RenderGraph::CompiledGraph planGraph() {
        RenderGraph::RenderGraphBuilder builder;
        auto a = builder.createResource("a", imageDesc());
        auto b = builder.createResource("b", imageDesc(RenderGraph::Format::R32Uint, 32, 32));
        auto c = builder.createResource("c", imageDesc());
        auto depth = builder.createResource("depth", imageDesc(RenderGraph::Format::D32Float));
        const auto gbuffer = builder.addPass("gbuffer");
        const auto forward = builder.addPass("forward");
        const auto transparent = builder.addPass("transparent");
        const auto sample = builder.addPass("sample");
        a = builder.write(gbuffer, a, Access::ColorAttachment, LoadOp::Clear, StoreOp::Store, Grey);
        b = builder.write(gbuffer, b, Access::ColorAttachment, LoadOp::Clear, StoreOp::Store, Flags);
        c = builder.write(gbuffer, c, Access::ColorAttachment, LoadOp::Clear, StoreOp::Store,
            NegativeZero);
        depth = builder.write(gbuffer, depth, Access::DepthAttachmentWrite, LoadOp::Clear,
            StoreOp::Store, NearDepth);
        builder.read(forward, b, Access::SampledRead);
        a = builder.write(forward, a, Access::ColorAttachment, LoadOp::Load);
        depth = builder.write(forward, depth, Access::DepthAttachmentWrite, LoadOp::Load);
        builder.read(transparent, depth, Access::DepthAttachmentRead);
        a = builder.write(transparent, a, Access::ColorAttachment, LoadOp::Load);
        builder.read(sample, a, Access::SampledRead);
        builder.read(sample, c, Access::SampledRead);
        builder.read(sample, depth, Access::SampledRead);
        builder.exportResource(a, Access::SampledRead);
        RenderGraph::CompileResult result = builder.compile();
        if (!result.succeeded()) throw std::runtime_error("plan graph failed to compile");
        return std::move(*result.graph);
    }

    bool testRenderingPlans() {
        FakeResourceFactory factory;
        VulkanRenderGraphExecutor executor;
        executor.init(factory, 2);
        CHECK(throws([&] { (void)executor.renderingPlan(RenderGraph::PassId{ 0 }); }));
        executor.rebuild(planGraph());
        const uint32_t a = executor.resourceId("a").logical;
        const uint32_t b = executor.resourceId("b").logical;
        const uint32_t c = executor.resourceId("c").logical;
        const uint32_t depth = executor.resourceId("depth").logical;

        const VulkanPassRenderingPlan& gbuffer = executor.renderingPlan(executor.passId("gbuffer"));
        CHECK(gbuffer.valid && !gbuffer.empty());
        CHECK(gbuffer.colorCount == 3 && gbuffer.hasDepth());
        // Usage-declaration order, as the framebuffers had it.
        CHECK(gbuffer.color[0].logicalResourceIndex == a);
        CHECK(gbuffer.color[1].logicalResourceIndex == b);
        CHECK(gbuffer.color[2].logicalResourceIndex == c);
        CHECK(gbuffer.depth[0].logicalResourceIndex == depth);
        for (uint32_t index = 0; index < 3; ++index) {
            CHECK(gbuffer.color[index].layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            CHECK(gbuffer.color[index].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
            CHECK(gbuffer.color[index].storeOp == VK_ATTACHMENT_STORE_OP_STORE);
        }
        CHECK(gbuffer.depth[0].layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        CHECK(sameClear(gbuffer.color[0].clearValue, Grey, false));
        CHECK(sameClear(gbuffer.color[1].clearValue, Flags, false));
        CHECK(gbuffer.color[2].clearValue.color.uint32[0] == 0x8000'0000u);   // -0.0f kept
        CHECK(sameClear(gbuffer.depth[0].clearValue, NearDepth, true));
        // The smallest attachment bounds the default render area.
        CHECK(gbuffer.renderExtent().width == 32 && gbuffer.renderExtent().height == 32);

        const VulkanPassRenderingPlan& forward = executor.renderingPlan(executor.passId("forward"));
        CHECK(forward.colorCount == 1 && forward.hasDepth());
        CHECK(forward.color[0].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
        CHECK(forward.depth[0].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
        CHECK(forward.depth[0].storeOp == VK_ATTACHMENT_STORE_OP_STORE);
        CHECK(forward.renderExtent().width == 64 && forward.renderExtent().height == 32);

        // Read-only depth: DS_RO layout, LOAD, STORE_OP_NONE (finding 4).
        const VulkanPassRenderingPlan& transparent =
            executor.renderingPlan(executor.passId("transparent"));
        CHECK(transparent.colorCount == 1 && transparent.hasDepth());
        CHECK(transparent.depth[0].layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        CHECK(transparent.depth[0].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
        CHECK(transparent.depth[0].storeOp == VK_ATTACHMENT_STORE_OP_NONE);

        CHECK(executor.renderingPlan(executor.passId("sample")).empty());
        CHECK(throws([&] { (void)executor.renderingPlan(RenderGraph::PassId{ 99 }); }));
        executor.cleanupAfterDeviceIdle();
        return true;
    }

    // The production plans carry the render passes' attachments, ops and
    // clear values (R4 design inventory).
    bool testProductionRenderingPlans() {
        constexpr VkExtent2D Scene{ 1920, 1080 };
        for (const bool hdr10 : { false, true }) {
            FakeResourceFactory factory;
            VulkanRenderGraphExecutor executor;
            executor.init(factory, 2);
            executor.rebuild(buildVulkanProductionRenderGraph(Scene, Scene,
                hdr10 ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_B8G8R8A8_SRGB,
                hdr10 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_B8G8R8A8_SRGB, hdr10,
                GBufferLayout::CanonicalReference, {}, 4096, 8192, true,
                { { 960, 528 }, { 1920, 528 }, { 1920, 1072 }, true },
                { .depthPyramid = true }));
            const VulkanProductionGraphIds ids = resolveVulkanProductionGraphIds(executor);
            const VulkanPassRenderingPlan& gbuffer = executor.renderingPlan(ids.gbuffer);
            CHECK(gbuffer.valid && gbuffer.colorCount == 5 && gbuffer.hasDepth());
            const std::array order{ ids.gbufferNormal, ids.gbufferAlbedo, ids.gbufferEmissive,
                ids.gbufferF0Roughness, ids.gbufferMaterialFlags };
            for (uint32_t index = 0; index < order.size(); ++index)
                CHECK(gbuffer.color[index].logicalResourceIndex == order[index].logical);
            const ClearValue black = ClearValue::color(0.0f, 0.0f, 0.0f, 1.0f);
            CHECK(sameClear(gbuffer.color[0].clearValue, black, false));
            CHECK(sameClear(gbuffer.color[2].clearValue, ClearValue::color(0, 0, 0, 0), false));
            CHECK(sameClear(gbuffer.color[4].clearValue, ClearValue::colorUint(0u), false));
            CHECK(sameClear(gbuffer.depth[0].clearValue, ClearValue::depthStencil(1.0f), true));
            CHECK(gbuffer.renderExtent().width == Scene.width &&
                gbuffer.renderExtent().height == Scene.height);

            const VulkanPassRenderingPlan& lighting = executor.renderingPlan(ids.lighting);
            CHECK(lighting.colorCount == 1 && !lighting.hasDepth() &&
                lighting.color[0].logicalResourceIndex == ids.sceneColor.logical &&
                lighting.color[0].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR &&
                sameClear(lighting.color[0].clearValue, black, false));
            const VulkanPassRenderingPlan& opaque = executor.renderingPlan(ids.forwardOpaque);
            CHECK(opaque.colorCount == 1 && opaque.hasDepth() &&
                opaque.depth[0].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
                opaque.depth[0].storeOp == VK_ATTACHMENT_STORE_OP_STORE);
            for (const RenderGraph::PassId pass : { ids.sortedForward, ids.ordinary2ComposeHook,
                    ids.deepComposeHook, ids.oitAccumulate }) {
                const VulkanPassRenderingPlan& plan = executor.renderingPlan(pass);
                CHECK(plan.hasDepth() &&
                    plan.depth[0].layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL &&
                    plan.depth[0].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
                    plan.depth[0].storeOp == VK_ATTACHMENT_STORE_OP_NONE);
            }
            const VulkanPassRenderingPlan& oit = executor.renderingPlan(ids.oitAccumulate);
            CHECK(oit.colorCount == 2 &&
                sameClear(oit.color[1].clearValue, ClearValue::color(1, 0, 0, 0), false));
            const VulkanPassRenderingPlan& directional =
                executor.renderingPlan(ids.shadowDirectional);
            CHECK(directional.colorCount == 0 && directional.hasDepth() &&
                directional.depth[0].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR &&
                directional.renderExtent().width == 4096);
            CHECK(executor.renderingPlan(ids.shadowSpot).depth[0].loadOp ==
                VK_ATTACHMENT_LOAD_OP_LOAD);
            // The point pass chooses among its three pools per scope.
            const VulkanPassRenderingPlan& point = executor.renderingPlan(ids.shadowPoint);
            CHECK(point.valid && point.colorCount == 0 && point.depthCount == 3);
            for (uint32_t tier = 0; tier < 3; ++tier) {
                CHECK(point.depth[tier].logicalResourceIndex == ids.shadowPointMaps[tier].logical);
                CHECK(point.depth[tier].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
                CHECK(point.renderExtent(tier).width == (256u << tier));
            }
            const VulkanPassRenderingPlan& present = executor.renderingPlan(
                hdr10 ? ids.hdr10EncodePresent : ids.ui);
            CHECK(present.colorCount == 1 &&
                present.color[0].logicalResourceIndex == ids.swapchain.logical &&
                sameClear(present.color[0].clearValue, black, false));
            // Compute and transfer passes have no plan.
            CHECK(executor.renderingPlan(ids.cluster.clear).empty());
            CHECK(executor.renderingPlan(ids.depthPyramidBuild).empty());
            for (uint32_t pass = 0; pass < executor.compiledGraph()->passes().size(); ++pass)
                CHECK(executor.renderingPlan(RenderGraph::PassId{ pass }).valid);
            executor.cleanupAfterDeviceIdle();
        }
        return true;
    }

    // ---- beginRendering / endRendering ----------------------------------------

    struct RenderingOwner {
        enum class Mode : uint8_t { Default, Overrides, Nested, EndWithoutBegin, LeaveOpen,
            PerLayer };
        Mode mode = Mode::Default;
        bool rejected = false;
        uint32_t executions = 0;
    };

    VulkanPassCallbacks renderingCallbacks(RenderingOwner& owner, bool dynamicRendering) {
        VulkanPassCallbacks callbacks{};
        callbacks.owner = &owner;
        callbacks.dynamicRendering = dynamicRendering;
        callbacks.execute = [](void* self, VulkanPassContext& context) {
            auto& state = *static_cast<RenderingOwner*>(self);
            ++state.executions;
            using Mode = RenderingOwner::Mode;
            switch (state.mode) {
            case Mode::Default:
                context.beginRendering();
                context.endRendering();
                break;
            case Mode::Overrides: {
                VulkanRenderingOverrides overrides{};
                overrides.renderArea = { { 4, 2 }, { 16, 8 } };
                overrides.colorViews[1] = reinterpret_cast<VkImageView>(uintptr_t{ 0x7771 });
                overrides.depthView = reinterpret_cast<VkImageView>(uintptr_t{ 0x7772 });
                overrides.loadInsteadOfClear = true;
                context.beginRendering(overrides);
                context.endRendering();
                break;
            }
            case Mode::PerLayer:
                // Separate scopes per layer view (shadow cascades).
                for (uintptr_t layer = 0; layer < 3; ++layer) {
                    VulkanRenderingOverrides overrides{};
                    overrides.depthView = reinterpret_cast<VkImageView>(0x8800 + layer);
                    context.beginRendering(overrides);
                    context.endRendering();
                }
                break;
            case Mode::Nested:
                context.beginRendering();
                try { context.beginRendering(); }
                catch (const std::logic_error&) { state.rejected = true; }
                context.endRendering();
                break;
            case Mode::EndWithoutBegin:
                try { context.endRendering(); }
                catch (const std::logic_error&) { state.rejected = true; }
                break;
            case Mode::LeaveOpen:
                context.beginRendering();
                break;
            }
        };
        return callbacks;
    }

    struct PlanFixture {
        FakeResourceFactory factory;
        RecordingSink sink;
        VulkanRenderGraphExecutor executor;
        RenderingOwner gbuffer;
        RenderingOwner forward;
        RenderingOwner transparent;

        explicit PlanFixture(bool flagged = true) {
            executor.setBarrierSink(&sink);
            executor.init(factory, 2);
            executor.setBarrierApi(VulkanBarrierApi::Synchronization2);
            executor.rebuild(planGraph());
            executor.registerPass(executor.passId("gbuffer"), renderingCallbacks(gbuffer, flagged));
            executor.registerPass(executor.passId("forward"), renderingCallbacks(forward, flagged));
            executor.registerPass(executor.passId("transparent"),
                renderingCallbacks(transparent, flagged));
        }
        void runFrame(uint32_t slot) {
            executor.onFrameFenceCompleted(slot);
            executor.beginFrameExecution(slot);
            executor.setFrameRecordContext({ FakeCommandBuffer, slot });
            executor.drainRegisteredThrough(executor.passId("transparent"));
            executor.beginPass(FakeCommandBuffer, executor.passId("sample"));
            executor.finishFrameExecution();
        }
    };

    bool testBeginRenderingThroughPlan() {
        PlanFixture fixture;
        auto& executor = fixture.executor;
        auto& sink = fixture.sink;
        fixture.forward.mode = RenderingOwner::Mode::Overrides;
        fixture.transparent.mode = RenderingOwner::Mode::PerLayer;
        for (uint32_t slot = 0; slot < 2; ++slot) {
            sink.clear();
            fixture.runFrame(slot);
            CHECK(sink.renderingCount == 1 + 1 + 3 && sink.endCount == 5);
            const RecordedRendering& gbuffer = sink.renderings[0];
            const auto view = [&](const char* name) {
                return executor.image(slot, executor.resourceId(name)).view;
            };
            CHECK(gbuffer.area.offset.x == 0 && gbuffer.area.offset.y == 0 &&
                gbuffer.area.extent.width == 32 && gbuffer.area.extent.height == 32);
            CHECK(gbuffer.layers == 1 && gbuffer.colorCount == 3 && gbuffer.hasDepth &&
                !gbuffer.hasStencil);
            CHECK(gbuffer.color[0].view == view("a") && gbuffer.color[1].view == view("b") &&
                gbuffer.color[2].view == view("c") && gbuffer.depth.view == view("depth"));
            CHECK(gbuffer.color[0].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR &&
                gbuffer.color[0].storeOp == VK_ATTACHMENT_STORE_OP_STORE &&
                gbuffer.color[0].layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            CHECK(sameClear(gbuffer.color[0].clear, Grey, false));
            CHECK(sameClear(gbuffer.color[1].clear, Flags, false));
            CHECK(sameClear(gbuffer.depth.clear, NearDepth, true));
            CHECK(gbuffer.depth.layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

            // Overrides: render area, a per-attachment view; nothing clears
            // here, so loadInsteadOfClear changes nothing.
            const RecordedRendering& forward = sink.renderings[1];
            CHECK(forward.area.offset.x == 4 && forward.area.extent.width == 16 &&
                forward.area.extent.height == 8);
            CHECK(forward.colorCount == 1 && forward.color[0].view == view("a"));
            CHECK(forward.depth.view == reinterpret_cast<VkImageView>(uintptr_t{ 0x7772 }));
            CHECK(forward.color[0].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);

            for (uintptr_t layer = 0; layer < 3; ++layer) {
                const RecordedRendering& transparent = sink.renderings[2 + layer];
                CHECK(transparent.depth.view == reinterpret_cast<VkImageView>(0x8800 + layer));
                CHECK(transparent.depth.layout ==
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
                CHECK(transparent.depth.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
                    transparent.depth.storeOp == VK_ATTACHMENT_STORE_OP_NONE);
            }
        }
        CHECK(sink.wrongCommandBuffer == 0 && sink.overflow == 0 && sink.sync1Calls == 0);

        // loadInsteadOfClear turns the plan's clears into loads.
        fixture.gbuffer.mode = RenderingOwner::Mode::Overrides;
        fixture.forward.mode = RenderingOwner::Mode::Default;
        sink.clear();
        fixture.runFrame(0);
        const RecordedRendering& loaded = sink.renderings[0];
        CHECK(loaded.color[0].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
            loaded.color[2].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
            loaded.depth.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
        CHECK(loaded.color[1].view == reinterpret_cast<VkImageView>(uintptr_t{ 0x7771 }));
        CHECK(loaded.color[0].view == executor.image(0, executor.resourceId("a")).view);
        executor.cleanupAfterDeviceIdle();
        return true;
    }

    bool testRenderingScopeRules() {
        // Not flagged: beginRendering is rejected.
        {
            PlanFixture fixture(false);
            fixture.executor.beginFrameExecution(0);
            fixture.executor.setFrameRecordContext({ FakeCommandBuffer, 0 });
            CHECK(throws([&] { fixture.executor.drainRegisteredThrough(
                fixture.executor.passId("gbuffer")); }));
            CHECK(fixture.sink.renderingCount == 0);
        }
        // Nesting and an unmatched end are rejected inside the callback.
        {
            PlanFixture fixture;
            fixture.gbuffer.mode = RenderingOwner::Mode::Nested;
            fixture.forward.mode = RenderingOwner::Mode::EndWithoutBegin;
            fixture.runFrame(0);
            CHECK(fixture.gbuffer.rejected && fixture.forward.rejected);
            CHECK(fixture.sink.renderingCount == 2 && fixture.sink.endCount == 2);
        }
        // A callback returning with rendering open fails its pass.
        {
            PlanFixture fixture;
            fixture.forward.mode = RenderingOwner::Mode::LeaveOpen;
            fixture.executor.beginFrameExecution(0);
            fixture.executor.setFrameRecordContext({ FakeCommandBuffer, 0 });
            CHECK(throws([&] { fixture.executor.drainRegisteredThrough(
                fixture.executor.passId("transparent")); }));
        }
        // A pass without attachments has no plan to begin.
        {
            FakeResourceFactory factory;
            RecordingSink sink;
            VulkanRenderGraphExecutor executor;
            executor.setBarrierSink(&sink);
            executor.init(factory, 1);
            executor.rebuild(planGraph());
            RenderingOwner owner;
            executor.registerPass(executor.passId("sample"), renderingCallbacks(owner, true));
            executor.beginFrameExecution(0);
            executor.setFrameRecordContext({ FakeCommandBuffer, 0 });
            for (const char* pass : { "gbuffer", "forward", "transparent" })
                executor.skipPass(executor.passId(pass));
            CHECK(throws([&] { executor.finishFrameExecution(); }));
            CHECK(owner.executions == 1 && sink.renderingCount == 0);
        }
        // Too many colour attachments: an invalid plan that only fails on use.
        {
            RenderGraph::RenderGraphBuilder builder;
            const auto pass = builder.addPass("wide");
            for (uint32_t index = 0; index <= VulkanMaxColorAttachments; ++index) {
                auto target = builder.createResource("t" + std::to_string(index), imageDesc());
                target = builder.write(pass, target, Access::ColorAttachment, LoadOp::Clear);
                builder.exportResource(target, Access::ColorAttachment);
            }
            const auto compiled = builder.compile();
            CHECK(compiled.succeeded());
            FakeResourceFactory factory;
            RecordingSink sink;
            VulkanRenderGraphExecutor executor;
            executor.setBarrierSink(&sink);
            executor.init(factory, 1);
            executor.rebuild(*compiled.graph);
            CHECK(!executor.renderingPlan(executor.passId("wide")).valid);
            RenderingOwner owner;
            executor.registerPass(executor.passId("wide"), renderingCallbacks(owner, true));
            executor.beginFrameExecution(0);
            executor.setFrameRecordContext({ FakeCommandBuffer, 0 });
            CHECK(throws([&] { executor.finishFrameExecution(); }));
        }
        return true;
    }

    // Several depth usages (the point-shadow pools): each scope binds one.
    struct DepthChoiceOwner {
        bool outOfRangeRejected = false;
    };

    bool testDepthCandidates() {
        RenderGraph::RenderGraphBuilder builder;
        auto small = builder.createResource("small",
            imageDesc(RenderGraph::Format::D32Float, 16, 16));
        auto large = builder.createResource("large",
            imageDesc(RenderGraph::Format::D32Float, 64, 64));
        const auto pass = builder.addPass("pools");
        small = builder.write(pass, small, Access::DepthAttachmentWrite, LoadOp::Clear);
        large = builder.write(pass, large, Access::DepthAttachmentWrite, LoadOp::Clear);
        builder.exportResource(small, Access::DepthAttachmentWrite);
        builder.exportResource(large, Access::DepthAttachmentWrite);
        const auto compiled = builder.compile();
        CHECK(compiled.succeeded());
        FakeResourceFactory factory;
        RecordingSink sink;
        VulkanRenderGraphExecutor executor;
        executor.setBarrierSink(&sink);
        executor.init(factory, 1);
        executor.setBarrierApi(VulkanBarrierApi::Synchronization2);
        executor.rebuild(*compiled.graph);
        const VulkanPassRenderingPlan& plan = executor.renderingPlan(executor.passId("pools"));
        CHECK(plan.valid && plan.depthCount == 2);
        CHECK(plan.renderExtent(0).width == 16 && plan.renderExtent(1).width == 64);
        DepthChoiceOwner owner;
        VulkanPassCallbacks callbacks{};
        callbacks.owner = &owner;
        callbacks.dynamicRendering = true;
        callbacks.execute = [](void* self, VulkanPassContext& context) {
            for (uint32_t index = 0; index < 2; ++index) {
                VulkanRenderingOverrides overrides{};
                overrides.depthIndex = index;
                context.beginRendering(overrides);
                context.endRendering();
            }
            VulkanRenderingOverrides outOfRange{};
            outOfRange.depthIndex = 2;
            try { context.beginRendering(outOfRange); }
            catch (const std::invalid_argument&) {
                static_cast<DepthChoiceOwner*>(self)->outOfRangeRejected = true;
            }
        };
        executor.registerPass(executor.passId("pools"), callbacks);
        executor.beginFrameExecution(0);
        executor.setFrameRecordContext({ FakeCommandBuffer, 0 });
        executor.finishFrameExecution();
        CHECK(owner.outOfRangeRejected);
        CHECK(sink.renderingCount == 2);
        CHECK(sink.renderings[0].depth.view == executor.image(0, executor.resourceId("small")).view);
        CHECK(sink.renderings[0].area.extent.width == 16);
        CHECK(sink.renderings[1].depth.view == executor.image(0, executor.resourceId("large")).view);
        CHECK(sink.renderings[1].area.extent.width == 64 && sink.renderings[1].colorCount == 0);
        executor.cleanupAfterDeviceIdle();
        return true;
    }

    // ---- same-access attachment re-barrier ------------------------------------

    bool testAttachmentRebarrierOnlyOnFlaggedPasses() {
        for (const bool flagged : { false, true }) {
            PlanFixture fixture(flagged);
            if (!flagged) {
                // Unflagged passes cannot begin rendering; record nothing.
                VulkanPassCallbacks none{};
                none.owner = &fixture.forward;
                none.execute = [](void*, VulkanPassContext&) {};
                for (const char* pass : { "gbuffer", "forward", "transparent" }) {
                    fixture.executor.unregisterPass(fixture.executor.passId(pass));
                    fixture.executor.registerPass(fixture.executor.passId(pass), none);
                }
            }
            auto& executor = fixture.executor;
            auto& sink = fixture.sink;
            fixture.runFrame(0);
            const uint64_t a = reinterpret_cast<uint64_t>(
                executor.image(0, executor.resourceId("a")).image);
            const uint64_t depth = reinterpret_cast<uint64_t>(
                executor.image(0, executor.resourceId("depth")).image);
            // Second frame of the slot: every state carried over.
            sink.clear();
            fixture.runFrame(0);
            // Re-barriers keep the layout and order only attachment writes
            // (DS_RO -> DS_RO for the sampled depth is an ordinary access change).
            std::vector<RecordedBarrier> sameLayout;
            for (const RecordedBarrier& barrier : sink.barriers())
                if (barrier.oldLayout == barrier.newLayout &&
                    (barrier.srcAccess == VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT ||
                     barrier.srcAccess == VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT))
                    sameLayout.push_back(barrier);
            if (!flagged) {
                // R3 emission: same-access attachment writes are not barriered.
                CHECK(sameLayout.empty());
                continue;
            }
            // forward: a (colour) and depth; transparent: a (colour) only.
            CHECK(sameLayout.size() == 3);
            const RecordedBarrier& color = sameLayout[0];
            CHECK(color.image == a &&
                color.oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            CHECK(color.srcStages == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT &&
                color.srcAccess == VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            CHECK(color.dstStages == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT &&
                color.dstAccess == (VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT));
            const RecordedBarrier& depthWrite = sameLayout[1];
            CHECK(depthWrite.image == depth &&
                depthWrite.oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            const VkPipelineStageFlags2 tests = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            CHECK(depthWrite.srcStages == tests && depthWrite.dstStages == tests);
            CHECK(depthWrite.srcAccess == VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
            CHECK(depthWrite.dstAccess == (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT));
            CHECK(sameLayout[2].image == a);
            // Read-only depth is a layout change, never a same-access re-barrier.
            bool readOnly = false;
            for (const RecordedBarrier& barrier : sink.barriers())
                readOnly |= barrier.image == depth &&
                    barrier.newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            CHECK(readOnly);
            executor.cleanupAfterDeviceIdle();
        }
        return true;
    }

    // Executor-owned imports get the same re-barrier.
    bool testAttachmentRebarrierOnImports() {
        RenderGraph::RenderGraphBuilder builder;
        auto target = builder.createResource("target", importedImage(Access::SampledRead));
        const auto first = builder.addPass("first");
        const auto second = builder.addPass("second");
        target = builder.write(first, target, Access::ColorAttachment, LoadOp::Load);
        target = builder.write(second, target, Access::ColorAttachment, LoadOp::Load);
        const auto compiled = builder.compile();
        CHECK(compiled.succeeded());
        FakeResourceFactory factory;
        RecordingSink sink;
        VulkanRenderGraphExecutor executor;
        executor.setBarrierSink(&sink);
        executor.init(factory, 1);
        executor.setBarrierApi(VulkanBarrierApi::Synchronization2);
        executor.rebuild(*compiled.graph);
        executor.bindExternalImage(VulkanGlobalBinding, executor.resourceId("target"),
            fakeImage(0x5000), Access::SampledRead);
        RenderingOwner one;
        RenderingOwner two;
        executor.registerPass(executor.passId("first"), renderingCallbacks(one, true));
        executor.registerPass(executor.passId("second"), renderingCallbacks(two, true));
        executor.beginFrameExecution(0);
        executor.setFrameRecordContext({ FakeCommandBuffer, 0 });
        executor.finishFrameExecution();
        CHECK(sink.barriers().size() == 2);
        CHECK(sink.barriers()[0].oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        CHECK(sink.barriers()[1].oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
            sink.barriers()[1].newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
            sink.barriers()[1].srcAccess == VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        CHECK(sink.renderingCount == 2 &&
            sink.renderings[1].color[0].view == fakeImage(0x5000).view);
        executor.cleanupAfterDeviceIdle();
        return true;
    }

    // ---- discard on first use and frame-end exports ---------------------------

    RenderGraph::CompiledGraph presentGraph() {
        RenderGraph::RenderGraphBuilder builder;
        auto swapchain = builder.createResource("swapchain",
            importedImage(Access::Present, RenderGraph::Format::Bgra8Srgb));
        const auto ui = builder.addPass("ui");
        swapchain = builder.write(ui, swapchain, Access::ColorAttachment, LoadOp::Clear,
            StoreOp::Store, ClearValue::color(0.0f, 0.0f, 0.0f, 1.0f));
        builder.exportResource(swapchain, Access::Present);
        RenderGraph::CompileResult result = builder.compile();
        if (!result.succeeded()) throw std::runtime_error("present graph failed to compile");
        return std::move(*result.graph);
    }

    bool testDiscardOnFirstUseAndPresentExport() {
        FakeResourceFactory factory;
        RecordingSink sink;
        VulkanRenderGraphExecutor executor;
        executor.setBarrierSink(&sink);
        executor.init(factory, 2);
        executor.setBarrierApi(VulkanBarrierApi::Synchronization2);
        executor.rebuild(presentGraph());
        const auto swapchain = executor.resourceId("swapchain");
        const auto image = [](uintptr_t handle) {
            return fakeImage(handle, VK_FORMAT_B8G8R8A8_SRGB);
        };
        constexpr auto discard = ExternalSyncPolicy::discardOnFirstUse();
        static_assert(discard.mode == ExternalSyncMode::ExecutorOwned && discard.discard);
        CHECK(throws([&] { executor.bindExternalImage(0, swapchain, image(0xA000),
            Access::Present, { ExternalSyncMode::RenderPassManaged, Access::Undefined,
                Access::Present, true }); }));
        CHECK(throws([&] { executor.bindExternalImage(0, swapchain, image(0xA000),
            Access::Present, { ExternalSyncMode::OwnerManaged, {}, {}, true }); }));
        // Like the backend before the first acquire: both slots on one image.
        executor.bindExternalImage(0, swapchain, image(0xA000), Access::Present, discard);
        executor.bindExternalImage(1, swapchain, image(0xA000), Access::Present, discard);
        RenderingOwner owner;
        executor.registerPass(executor.passId("ui"), renderingCallbacks(owner, true));

        for (uint32_t frame = 0; frame < 4; ++frame) {
            const uint32_t slot = frame % 2;
            const uintptr_t acquired = frame % 3 == 0 ? 0xA000 : 0xA100;
            executor.onFrameFenceCompleted(slot);
            executor.bindExternalImage(slot, swapchain, image(acquired), Access::Present,
                discard);
            sink.clear();
            executor.beginFrameExecution(slot);
            executor.setFrameRecordContext({ FakeCommandBuffer, slot });
            executor.finishFrameExecution();
            CHECK(sink.barrierCalls == 2 && sink.barriers().size() == 2);
            // First use: UNDEFINED, ordered after the tracked Present access
            // (BOTTOM_OF_PIPE, which chains with the acquire wait in sync2).
            const RecordedBarrier& acquire = sink.barriers()[0];
            CHECK(acquire.image == acquired);
            CHECK(acquire.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
                acquire.newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            CHECK(acquire.srcStages == VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT &&
                acquire.srcAccess == 0);
            CHECK(acquire.dstStages == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
            // Frame end: the export moves it to PRESENT_SRC.
            const RecordedBarrier& present = sink.barriers()[1];
            CHECK(present.image == acquired &&
                present.oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
                present.newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            CHECK(present.srcStages == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT &&
                present.srcAccess == (VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT));
            CHECK(present.dstStages == VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT &&
                present.dstAccess == 0);
            CHECK(sink.renderingCount == 1 &&
                sink.renderings[0].color[0].view == image(acquired).view &&
                sink.renderings[0].color[0].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
            CHECK(executor.externalImageAccess(slot, swapchain) == Access::Present);
        }
        CHECK(sink.wrongCommandBuffer == 0);

        // A state-tracking executor-owned binding may not share an image
        // with another slot (slot 1 holds 0xA000).
        executor.onFrameFenceCompleted(0);
        CHECK(throws([&] { executor.bindExternalImage(0, swapchain, image(0xA000),
            Access::Present); }));
        executor.cleanupAfterDeviceIdle();
        return true;
    }

    bool testDiscardRequiresAWriteAndRearms() {
        RenderGraph::RenderGraphBuilder builder;
        auto target = builder.createResource("target", importedImage(Access::SampledRead));
        const auto read = builder.addPass("read");
        const auto write = builder.addPass("write");
        builder.read(read, target, Access::SampledRead);
        target = builder.write(write, target, Access::ColorAttachment, LoadOp::Clear);
        const auto compiled = builder.compile();
        CHECK(compiled.succeeded());
        FakeResourceFactory factory;
        RecordingSink sink;
        VulkanRenderGraphExecutor executor;
        executor.setBarrierSink(&sink);
        executor.init(factory, 2);
        executor.setBarrierApi(VulkanBarrierApi::Synchronization2);
        executor.rebuild(*compiled.graph);
        const auto id = executor.resourceId("target");
        executor.bindExternalImage(VulkanGlobalBinding, id, fakeImage(0x5000),
            Access::SampledRead, ExternalSyncPolicy::discardOnFirstUse());
        // Reading discarded contents is rejected.
        executor.beginFrameExecution(0);
        CHECK(throws([&] { executor.beginPass(FakeCommandBuffer, executor.passId("read")); }));
        executor.cleanupAfterDeviceIdle();

        FakeResourceFactory factory2;
        VulkanRenderGraphExecutor global;
        global.setBarrierSink(&sink);
        global.init(factory2, 2);
        global.setBarrierApi(VulkanBarrierApi::Synchronization2);
        global.rebuild(*compiled.graph);
        global.bindExternalImage(VulkanGlobalBinding, global.resourceId("target"),
            fakeImage(0x5000), Access::SampledRead, ExternalSyncPolicy::discardOnFirstUse());
        // A global discard binding discards again every frame; the skipped
        // reader leaves the first use to the writer.
        for (uint32_t frame = 0; frame < 3; ++frame) {
            const uint32_t slot = frame % 2;
            global.onFrameFenceCompleted(slot);
            sink.clear();
            global.beginFrameExecution(slot);
            global.skipPass(global.passId("read"));
            global.beginPass(FakeCommandBuffer, global.passId("write"));
            global.finishFrameExecution();
            CHECK(sink.barriers().size() == 1);
            CHECK(sink.barriers()[0].oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
            CHECK(sink.barriers()[0].srcStages == (frame == 0
                ? (VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
                : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT));
        }
        global.cleanupAfterDeviceIdle();
        return true;
    }

    // Render-pass- and owner-managed imports are left alone at frame end; the
    // production topologies (no flagged pass) record nothing there, and the
    // same barriers as without the R4a code paths.
    bool testProductionFrameEndRecordsNothing() {
        for (const bool hdr10 : { false, true }) {
            FakeResourceFactory factory;
            RecordingSink sink;
            VulkanRenderGraphExecutor executor;
            executor.setBarrierSink(&sink);
            executor.init(factory, 2);
            executor.setBarrierApi(VulkanBarrierApi::Synchronization2);
            executor.rebuild(buildVulkanProductionRenderGraph({ 640, 360 }, { 640, 360 },
                hdr10 ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_B8G8R8A8_SRGB,
                hdr10 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_B8G8R8A8_SRGB, hdr10,
                GBufferLayout::CanonicalReference, {}, 4096, 8192, true, {},
                { .virtualShadowWorkingSetBytes = 8'192 }));
            const VulkanProductionGraphIds ids = resolveVulkanProductionGraphIds(executor);
            VulkanImageResource swapchain{};
            swapchain.image = reinterpret_cast<VkImage>(uintptr_t{ 0xA000 });
            swapchain.format = hdr10 ? VK_FORMAT_A2B10G10R10_UNORM_PACK32
                                     : VK_FORMAT_B8G8R8A8_SRGB;
            for (uint32_t slot = 0; slot < 2; ++slot) {
                executor.bindExternalImage(slot, ids.swapchain, swapchain, Access::Undefined,
                    ExternalSyncPolicy::renderPassManaged(Access::Undefined, Access::Present));
                executor.bindExternalBuffer(slot, ids.virtualShadowWorkingSet,
                    reinterpret_cast<VkBuffer>(uintptr_t{ 0x900 + slot }), 8'192);
            }
            for (uint32_t frame = 0; frame < 3; ++frame) {
                const uint32_t slot = frame % 2;
                executor.onFrameFenceCompleted(slot);
                executor.bindExternalImage(slot, ids.swapchain, swapchain, Access::Undefined,
                    ExternalSyncPolicy::renderPassManaged(Access::Undefined, Access::Present));
                executor.beginFrameExecution(slot);
                for (const auto& pass : executor.compiledGraph()->passes())
                    executor.beginPass(FakeCommandBuffer, executor.passId(pass.name));
                const uint32_t before = sink.barrierCalls;
                executor.finishFrameExecution();
                CHECK(sink.barrierCalls == before);
                CHECK(sink.renderingCount == 0);
            }
            executor.cleanupAfterDeviceIdle();
        }
        return true;
    }

    // Steady frames that begin rendering and re-barrier allocate nothing.
    bool testRenderingFramesAllocateNothing() {
        PlanFixture fixture;
        fixture.transparent.mode = RenderingOwner::Mode::PerLayer;
        for (uint32_t frame = 0; frame < 4; ++frame) {
            fixture.sink.clear();
            fixture.runFrame(frame % 2);
        }
        beginCpuAllocationFrame();
        auto probe = std::make_unique<std::array<uint8_t, 64>>();
        CHECK(endCpuAllocationFrame().allocationCount == 1 && probe != nullptr);
        beginCpuAllocationFrame();
        for (uint32_t frame = 4; frame < 40; ++frame) {
            fixture.sink.clear();
            fixture.runFrame(frame % 2);
        }
        const CpuAllocationFrameSample sample = endCpuAllocationFrame();
        std::cout << "  36 rendering frames: " << sample.allocationCount << " allocations\n";
        CHECK(sample.allocationCount == 0);
        CHECK(fixture.gbuffer.executions == 40 && fixture.sink.overflow == 0);
        fixture.executor.cleanupAfterDeviceIdle();
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        std::string_view name;
        bool (*function)();
    };
    constexpr TestCase tests[] = {
        { "rendering plans", testRenderingPlans },
        { "production rendering plans", testProductionRenderingPlans },
        { "begin rendering through the plan", testBeginRenderingThroughPlan },
        { "rendering scope rules", testRenderingScopeRules },
        { "depth candidates", testDepthCandidates },
        { "attachment re-barrier only on flagged passes",
            testAttachmentRebarrierOnlyOnFlaggedPasses },
        { "attachment re-barrier on imports", testAttachmentRebarrierOnImports },
        { "discard on first use and the present export",
            testDiscardOnFirstUseAndPresentExport },
        { "discard requires a write and re-arms", testDiscardRequiresAWriteAndRearms },
        { "production frame end records nothing", testProductionFrameEndRecordsNothing },
        { "rendering frames allocate nothing", testRenderingFramesAllocateNothing },
    };
    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.function()) {
                std::cout << "[pass] " << test.name << '\n';
                continue;
            }
            std::cerr << "[fail] " << test.name << '\n';
        }
        catch (const std::exception& exception) {
            std::cerr << "[fail] " << test.name << ": " << exception.what() << '\n';
        }
        ++failures;
    }
    std::cout << std::size(tests) - failures << '/' << std::size(tests) << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
