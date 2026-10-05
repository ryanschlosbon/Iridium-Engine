#include "qualification/vulkan/VulkanQualificationExtension.h"

#include "qualification/vulkan/VulkanReadbackAnalysis.h"
#include "renderer/vulkan/VulkanCommandList.h"
#include "renderer/vulkan/VulkanDepthPyramid.h"
#include "renderer/vulkan/VulkanFrameScheduler.h"
#include "renderer/vulkan/VulkanFrameTargets.h"
#include "renderer/vulkan/VulkanReflectionProbeCaptureTargets.h"
#include "renderer/vulkan/VulkanRenderGraphExecutor.h"
#include "renderer/transparency/LayeredAtlas.h"
#include "renderer/transparency/LayeredGlass.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Iridium {

    namespace {
        [[nodiscard]] uint32_t captureSourceBytesPerPixel(VkFormat format) noexcept {
            switch (format) {
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_B8G8R8A8_SRGB:
                return 4;
            case VK_FORMAT_R16G16B16A16_SFLOAT:
                return 8;
            default:
                return 0;
            }
        }

        [[nodiscard]] std::optional<FrameCapturePixelFormat> capturePixelFormat(
            VkFormat format) noexcept {
            switch (format) {
            case VK_FORMAT_R8G8B8A8_SRGB:
                return FrameCapturePixelFormat::Rgba8Srgb;
            case VK_FORMAT_B8G8R8A8_SRGB:
                return FrameCapturePixelFormat::Bgra8Srgb;
            case VK_FORMAT_R16G16B16A16_SFLOAT:
                return FrameCapturePixelFormat::Rgba32Float;
            default:
                return std::nullopt;
            }
        }

        [[nodiscard]] std::span<const std::byte> mappedBytes(
            const VulkanBufferResource& buffer) noexcept {
            return { static_cast<const std::byte*>(buffer.mapped),
                static_cast<size_t>(buffer.size) };
        }

        // Swap-and-pop over every pending entry recorded in `frameIndex`.
        template<typename Pending, typename Analyze>
        void drainSlot(std::vector<Pending>& pending, uint32_t frameIndex,
            VulkanResourceAllocator& allocator, const char* unmappedMessage,
            Analyze&& analyze) {
            size_t index = 0u;
            while (index < pending.size()) {
                Pending& entry = pending[index];
                if (entry.frameIndex != frameIndex) {
                    ++index;
                    continue;
                }
                if (entry.readback.mapped == nullptr) {
                    allocator.destroy(entry.readback);
                    throw std::runtime_error(unmappedMessage);
                }
                analyze(entry);
                allocator.destroy(entry.readback);
                if (index + 1u != pending.size())
                    pending[index] = std::move(pending.back());
                pending.pop_back();
            }
        }

        template<typename Pending>
        void pushPending(std::vector<Pending>& pending, Pending&& entry,
            VulkanResourceAllocator& allocator) {
            try {
                pending.push_back(std::move(entry));
            }
            catch (...) {
                allocator.destroy(entry.readback);
                throw;
            }
        }
    }

    // --------------------------------------------------------------------
    // Lifecycle
    // --------------------------------------------------------------------

    void VulkanQualificationExtension::configureQualification(
        const QualificationBackendConfig& config) {
        if (attached())
            throw std::logic_error(
                "Qualification extension is configured before backend initialization");
        oracle_.configure(VulkanIndirectOracleConfig{
            .shadowIndirect = config.shadowIndirectOracle,
            .gpuLod = config.gpuLodOracle,
            .probeLod = config.probeLodOracle,
            .depthOcclusion = config.depthOcclusionOracle,
            .virtualShadowDepth = config.virtualShadowDepthOracle,
        });
        validateProbeCaptureTargets_ = config.validateProbeCaptureTargets;
        indirectStreamDigestEnabled_ = config.indirectStreamDigest;
        casterRevisionOracleEnabled_ = config.casterRevisionOracle;
        aliasPoison_ = config.aliasPoison;
        indirectStreamDigest_.setOutput(
            indirectStreamDigestEnabled_ ? &std::cout : nullptr);
        casterRevisionOracle_.setOutput(
            casterRevisionOracleEnabled_ ? &std::cout : nullptr);
    }

    VulkanGraphHooks VulkanQualificationExtension::graphHooks() const noexcept {
        // Capture-validation hooks are declared whenever their tier/pyramid is
        // resident (requests arrive per frame); the VSM depth snapshot only
        // with its oracle.
        return { .depthPyramidValidation = true, .layeredValidation = true,
            .virtualShadowDepthSnapshot =
                oracle_.enabled(VulkanIndirectOracleView::VirtualShadowDepth),
            .sceneColorCapture = true };
    }

    void VulkanQualificationExtension::onBackendInitialized(
        const VulkanBackendServices& services) {
        services_ = services;
        oracle_.attach(services.allocator, services.profiler);
        if (validateProbeCaptureTargets_) runProbeCaptureTargetValidation();
    }

    void VulkanQualificationExtension::runProbeCaptureTargetValidation() {
        VulkanReflectionProbeCaptureTargets& targets = *services_.probeCaptureTargets;
        const SceneEntityUuid validationOwner = *SceneEntityUuid::parse(
            "019fb73d-5a80-7000-8000-000000000999");
        const auto& validationTarget = targets.acquire(validationOwner, 1, 128);
        if (!validationTarget.rawRadiance.isValid() ||
            !validationTarget.depth.isValid() ||
            !validationTarget.prefilteredRadiance.isValid())
            throw std::runtime_error(
                "Reflection-probe validation capture allocation failed");
        targets.promote(validationOwner, 1);
        if (targets.capturesInFlight() != 0 ||
            targets.stagingLogicalBytes() != 0 ||
            targets.publishedCount() != 1 ||
            targets.published(validationOwner) == nullptr)
            throw std::runtime_error(
                "Reflection-probe validation capture promotion failed");
        [[maybe_unused]] const auto& validationRefreshTarget =
            targets.acquire(validationOwner, 2, 128);
        targets.abandon(validationOwner, 2);
        if (targets.capturesInFlight() != 0 ||
            targets.stagingLogicalBytes() != 0 ||
            targets.publishedCount() != 1 ||
            targets.published(validationOwner) == nullptr)
            throw std::runtime_error(
                "Reflection-probe validation refresh retirement failed");
        targets.remove(validationOwner);
        if (targets.publishedCount() != 0 ||
            targets.publishedLogicalBytes() != 0)
            throw std::runtime_error(
                "Reflection-probe validation owner retirement failed");
    }

    void VulkanQualificationExtension::onBeforeDeviceDestroy() {
        // The backend has collected every slot; all streams have retired.
        if (indirectStreamDigestEnabled_) indirectStreamDigest_.finish();
        if (casterRevisionOracleEnabled_) casterRevisionOracle_.finish();
        destroyAliasPoisonBuffers();
        if (attached()) {
            VulkanResourceAllocator& allocator = *services_.allocator;
            for (PendingFrameCapture& pending : pendingFrameCaptures_)
                allocator.destroy(pending.readback);
            for (PendingOrdinary2CaptureValidation& pending : pendingOrdinary2_)
                allocator.destroy(pending.readback);
            for (PendingDeepLayeredCaptureValidation& pending : pendingDeepLayered_)
                allocator.destroy(pending.readback);
            for (PendingDepthPyramidCaptureValidation& pending : pendingDepthPyramid_)
                allocator.destroy(pending.readback);
        }
        pendingFrameCaptures_.clear();
        completedFrameCaptures_.clear();
        armedFrameCapture_.reset();
        pendingOrdinary2_.clear();
        completedOrdinary2_.clear();
        ordinary2Request_.reset();
        pendingDeepLayered_.clear();
        completedDeepLayered_.clear();
        deepLayeredRequest_.reset();
        pendingDepthPyramid_.clear();
        completedDepthPyramid_.clear();
        depthPyramidRequest_.reset();
        oracle_.destroyDeviceResources();
        services_ = {};
    }

    VulkanBufferResource VulkanQualificationExtension::createReadback(
        VkDeviceSize bytes) {
        return services_.allocator->createBuffer(bytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            true, ProfileMemoryCategory::CaptureReadback);
    }

    // --------------------------------------------------------------------
    // Hooks
    // --------------------------------------------------------------------

    bool VulkanQualificationExtension::wantsHook(
        const VulkanHookContext& context) const {
        switch (context.point) {
        case VulkanHookPoint::SceneColorComplete:
            return armedFrameCapture_ &&
                armedFrameCapture_->point == FrameCapturePoint::SceneLinear;
        case VulkanHookPoint::FinalCaptureHook:
            return armedFrameCapture_ &&
                armedFrameCapture_->point != FrameCapturePoint::SceneLinear;
        case VulkanHookPoint::DepthPyramidValidation:
            return depthPyramidRequest_.has_value();
        case VulkanHookPoint::Ordinary2Validation: {
            const auto* payload =
                std::get_if<VulkanOrdinary2HookPayload>(&context.payload);
            return ordinary2Request_ && payload != nullptr &&
                payload->drawCount != 0u;
        }
        case VulkanHookPoint::DeepLayeredValidation: {
            const auto* payload =
                std::get_if<VulkanDeepLayeredHookPayload>(&context.payload);
            return deepLayeredRequest_ && payload != nullptr &&
                deepLayeredRequest_->quality == payload->quality &&
                payload->drawCount != 0u;
        }
        case VulkanHookPoint::DeepLayeredResolveCounts:
            return std::ranges::any_of(pendingDeepLayered_,
                [&](const PendingDeepLayeredCaptureValidation& pending) {
                    return pending.frameIndex == context.slot;
                });
        case VulkanHookPoint::VirtualShadowDepthSnapshot:
            return oracle_.enabled(VulkanIndirectOracleView::VirtualShadowDepth);
        case VulkanHookPoint::FrameGraphBegin:
            return aliasPoison_;
        }
        return false;
    }

    void VulkanQualificationExtension::onHook(const VulkanHookContext& context) {
        if (!attached())
            throw std::logic_error(
                "Qualification extension hook before backend initialization");
        switch (context.point) {
        case VulkanHookPoint::SceneColorComplete:
        case VulkanHookPoint::FinalCaptureHook: {
            const auto& payload = std::get<VulkanCaptureHookPayload>(context.payload);
            const ArmedFrameCapture armed = *armedFrameCapture_;
            armedFrameCapture_.reset();
            VulkanCaptureHookPayload source = payload;
            source.point = armed.point;
            if (armed.point == FrameCapturePoint::SceneResolved) {
                if (payload.sceneResolved == nullptr)
                    throw std::logic_error("Scene-resolved capture has no resolved source.");
                source.source = payload.sceneResolved;
                source.format = payload.sceneResolvedFormat;
            }
            recordFrameCapture(armed.captureId, context.cmd, context.slot, source);
            return;
        }
        case VulkanHookPoint::DepthPyramidValidation:
            recordDepthPyramidReadback(context,
                std::get<VulkanDepthPyramidHookPayload>(context.payload));
            return;
        case VulkanHookPoint::Ordinary2Validation:
            recordOrdinary2Readback(context,
                std::get<VulkanOrdinary2HookPayload>(context.payload));
            return;
        case VulkanHookPoint::DeepLayeredValidation:
            recordDeepLayeredReadback(context,
                std::get<VulkanDeepLayeredHookPayload>(context.payload));
            return;
        case VulkanHookPoint::DeepLayeredResolveCounts:
            recordDeepResolveCounts(context,
                std::get<VulkanDeepResolveCountsPayload>(context.payload));
            return;
        case VulkanHookPoint::VirtualShadowDepthSnapshot:
            oracle_.recordVirtualShadowDepthSnapshot(context.cmd, context.slot,
                std::get<VulkanVirtualShadowDepthPayload>(context.payload));
            return;
        case VulkanHookPoint::FrameGraphBegin:
            recordAliasPoison(context);
            return;
        }
    }

    // --------------------------------------------------------------------
    // Alias-heap poison (M7R R4b.5)
    // --------------------------------------------------------------------

    void VulkanQualificationExtension::recordAliasPoison(const VulkanHookContext& context) {
        if (!aliasPoison_ || services_.graph == nullptr) return;
        VulkanRenderGraphExecutor& graph = *services_.graph;
        VulkanResourceAllocator& allocator = *services_.allocator;
        if (aliasPoisonSlots_.size() <= context.slot)
            aliasPoisonSlots_.resize(context.slot + 1u);
        AliasPoisonSlot& slot = aliasPoisonSlots_[context.slot];
        // A rebuild replaces the heaps. This slot's earlier frames have
        // retired (its fence was waited before the frame began), so its old
        // buffers are unused; their heaps may already be freed.
        const uint64_t rebuildCount = graph.stats().rebuildCount;
        if (!slot.valid || slot.rebuildCount != rebuildCount) {
            for (VulkanBufferResource& buffer : slot.buffers) allocator.destroy(buffer);
            slot.buffers.clear();
            for (const VulkanAliasHeapResource& heap : graph.aliasHeaps(context.slot))
                slot.buffers.push_back(allocator.createAliasingBuffer(heap,
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT));
            slot.rebuildCount = rebuildCount;
            slot.valid = true;
            uint64_t bytes = 0;
            for (const VulkanBufferResource& buffer : slot.buffers) bytes += buffer.size;
            std::cout << "IRIDIUM_ALIAS_POISON {\"slot\":" << context.slot
                << ",\"rebuild\":" << rebuildCount << ",\"heaps\":" << slot.buffers.size()
                << ",\"bytes\":" << bytes << "}\n";
        }
        if (slot.buffers.empty()) return;
        // A quiet NaN as both one 32-bit float and two 16-bit floats; also
        // an implausible uint identity and depth.
        constexpr uint32_t Poison = 0x7FC07FC0u;
        for (const VulkanBufferResource& buffer : slot.buffers)
            vkCmdFillBuffer(context.cmd, buffer.buffer, 0, VK_WHOLE_SIZE, Poison);
        // The heaps' first uses transition from UNDEFINED with their own
        // (possibly empty) source scopes, so order the fill before all of it.
        if (graph.barrierApi() == VulkanBarrierApi::Synchronization2) {
            VkMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            VkDependencyInfo dependency{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(context.cmd, &dependency);
        }
        else {
            VkMemoryBarrier barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(context.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        }
    }

    void VulkanQualificationExtension::destroyAliasPoisonBuffers() noexcept {
        if (services_.allocator != nullptr)
            for (AliasPoisonSlot& slot : aliasPoisonSlots_)
                for (VulkanBufferResource& buffer : slot.buffers)
                    services_.allocator->destroy(buffer);
        aliasPoisonSlots_.clear();
    }

    void VulkanQualificationExtension::onFrameSlotRetired(uint32_t slot) {
        if (!attached()) return;
        collectFrameCapturesForSlot(slot);
        collectOrdinary2ValidationsForSlot(slot);
        collectDeepLayeredValidationsForSlot(slot);
        collectDepthPyramidValidationsForSlot(slot);
    }

    // --------------------------------------------------------------------
    // Frame captures
    // --------------------------------------------------------------------

    void VulkanQualificationExtension::armFrameCapture(uint64_t captureId,
        FrameCapturePoint point) {
        if (armedFrameCapture_)
            throw std::invalid_argument(
                "Frame capture permits one armed request at a time");
        armedFrameCapture_ = ArmedFrameCapture{ captureId, point };
    }

    void VulkanQualificationExtension::recordFrameCapture(uint64_t captureId,
        VkCommandBuffer cmd, uint32_t slot, const VulkanCaptureHookPayload& source) {
        if (!attached() || source.source == nullptr)
            throw std::logic_error("Frame capture requires an initialized backend.");
        const VkExtent2D extent = source.extent;
        const VkFormat format = source.format;
        const uint32_t sourceBytesPerPixel = captureSourceBytesPerPixel(format);
        const uint32_t outputBytesPerPixel =
            format == VK_FORMAT_R16G16B16A16_SFLOAT ? 16u : 4u;
        if (!capturePixelFormat(format) || sourceBytesPerPixel == 0) {
            throw std::runtime_error(
                "Frame capture requires a supported sRGB or FP16 scene target.");
        }
        if (extent.width == 0 || extent.height == 0) {
            throw std::runtime_error("Frame capture requires a non-empty render extent.");
        }
        const uint64_t pixelCount = static_cast<uint64_t>(extent.width) *
            static_cast<uint64_t>(extent.height);
        if (pixelCount > std::numeric_limits<uint64_t>::max() /
            sourceBytesPerPixel) {
            throw std::overflow_error("Frame capture byte count exceeds uint64_t.");
        }
        const uint64_t byteCount = pixelCount * sourceBytesPerPixel;
        if (byteCount > std::numeric_limits<size_t>::max() ||
            static_cast<uint64_t>(extent.width) * sourceBytesPerPixel >
                std::numeric_limits<uint32_t>::max() ||
            pixelCount > std::numeric_limits<size_t>::max() /
                outputBytesPerPixel ||
            static_cast<uint64_t>(extent.width) * outputBytesPerPixel >
                std::numeric_limits<uint32_t>::max()) {
            throw std::overflow_error("Frame capture dimensions exceed the readback contract.");
        }
        const bool duplicatePending = std::ranges::any_of(pendingFrameCaptures_,
            [captureId](const PendingFrameCapture& capture) {
                return capture.captureId == captureId;
            });
        const bool duplicateCompleted = std::ranges::any_of(completedFrameCaptures_,
            [captureId](const FrameCapture& capture) {
                return capture.captureId == captureId;
            });
        if (duplicatePending || duplicateCompleted) {
            throw std::invalid_argument("Frame capture IDs must be unique.");
        }

        PendingFrameCapture pending{};
        pending.captureId = captureId;
        pending.frameIndex = slot;
        pending.extent = extent;
        pending.format = format;
        pending.point = source.point;
        pending.readback = createReadback(byteCount);
        pushPending(pendingFrameCaptures_,
            std::move(pending), *services_.allocator);

        PendingFrameCapture& recorded = pendingFrameCaptures_.back();
        VulkanCommandList commandList(cmd);
        commandList.transition(recorded.readback, ResourceState::CopyDestination);
        VkBufferImageCopy copy{};
        copy.bufferOffset = 0;
        copy.bufferRowLength = 0;
        copy.bufferImageHeight = 0;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = 0;
        copy.imageSubresource.baseArrayLayer = 0;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = { extent.width, extent.height, 1 };
        commandList.copyImageToBuffer(*source.source, recorded.readback, copy);
    }

    void VulkanQualificationExtension::collectFrameCapturesForSlot(
        uint32_t frameIndex) {
        drainSlot(pendingFrameCaptures_, frameIndex, *services_.allocator,
            "A completed frame capture has invalid readback state.",
            [&](const PendingFrameCapture& pending) {
                const auto pixelFormat = capturePixelFormat(pending.format);
                if (!pixelFormat)
                    throw std::runtime_error(
                        "A completed frame capture has invalid readback state.");
                completedFrameCaptures_.push_back(convertFrameCaptureReadback({
                    .captureId = pending.captureId,
                    .width = pending.extent.width,
                    .height = pending.extent.height,
                    .pixelFormat = *pixelFormat,
                    .point = pending.point,
                    .halfFloatSource =
                        pending.format == VK_FORMAT_R16G16B16A16_SFLOAT,
                }, mappedBytes(pending.readback)));
            });
    }

    std::vector<FrameCapture> VulkanQualificationExtension::collectFrameCaptures(
        bool waitForPending) {
        if (waitForPending && !pendingFrameCaptures_.empty()) {
            services_.scheduler->waitForAllFrames();
            for (uint32_t frameIndex = 0;
                frameIndex < VulkanFrameScheduler::FramesInFlight; ++frameIndex) {
                collectFrameCapturesForSlot(frameIndex);
            }
        }
        std::vector<FrameCapture> result = std::move(completedFrameCaptures_);
        completedFrameCaptures_.clear();
        return result;
    }

    // --------------------------------------------------------------------
    // Ordinary2 capture validation
    // --------------------------------------------------------------------

    void VulkanQualificationExtension::armOrdinary2CaptureValidation(
        uint64_t validationId) {
        const bool duplicatePending = std::ranges::any_of(pendingOrdinary2_,
            [validationId](const PendingOrdinary2CaptureValidation& pending) {
                return pending.validationId == validationId;
            });
        const bool duplicateCompleted = std::ranges::any_of(completedOrdinary2_,
            [validationId](const Ordinary2CaptureValidationResult& completed) {
                return completed.validationId == validationId;
            });
        if (ordinary2Request_ || duplicatePending || duplicateCompleted) {
            throw std::invalid_argument(
                "Ordinary2 capture validation permits one unique request at a time");
        }
        ordinary2Request_ = validationId;
    }

    void VulkanQualificationExtension::recordOrdinary2Readback(
        const VulkanHookContext& context,
        const VulkanOrdinary2HookPayload& payload) {
        const VkExtent2D atlas = payload.atlasExtent;
        if (atlas.width == 0u || atlas.height == 0u) {
            throw std::logic_error(
                "Ordinary2 validation readback requires a resident atlas");
        }
        const uint64_t pixelCount =
            static_cast<uint64_t>(atlas.width) * atlas.height;
        constexpr uint64_t BytesPerImagePixel = sizeof(uint32_t);
        constexpr uint64_t InterfaceImageCount = 4u;
        if (pixelCount > std::numeric_limits<VkDeviceSize>::max() /
                (BytesPerImagePixel * InterfaceImageCount + 8u)) {
            throw std::overflow_error(
                "Ordinary2 validation readback exceeds VkDeviceSize");
        }
        const VkDeviceSize imageBytes = pixelCount * BytesPerImagePixel;
        const VkDeviceSize localColorBytes = pixelCount * 8u;
        const VkDeviceSize totalBytes =
            imageBytes * InterfaceImageCount + localColorBytes;

        PendingOrdinary2CaptureValidation pending{};
        pending.validationId = *ordinary2Request_;
        pending.frameIndex = context.slot;
        pending.extent = atlas;
        pending.expectedDrawCount = payload.drawCount;
        pending.workItemCount = payload.workItemCount;
        pending.readback = createReadback(totalBytes);
        pushPending(pendingOrdinary2_,
            std::move(pending), *services_.allocator);
        ordinary2Request_.reset();

        PendingOrdinary2CaptureValidation& recorded = pendingOrdinary2_.back();
        VulkanCommandList commandList(context.cmd);
        commandList.transition(recorded.readback,
            ResourceState::CopyDestination);
        VulkanFrameContextTargets& targets =
            services_.frameTargets->get(context.slot);
        const auto copyImage = [&](const VulkanImageResource& image,
                VkImageAspectFlags aspect, VkDeviceSize offset) {
            VkBufferImageCopy copy{};
            copy.bufferOffset = offset;
            copy.imageSubresource.aspectMask = aspect;
            copy.imageSubresource.layerCount = 1u;
            copy.imageExtent = { atlas.width, atlas.height, 1u };
            commandList.copyImageToBuffer(image, recorded.readback, copy);
        };
        copyImage(targets.layeredEntryIdentity, VK_IMAGE_ASPECT_COLOR_BIT, 0u);
        copyImage(targets.layeredEntryDepth, VK_IMAGE_ASPECT_DEPTH_BIT,
            imageBytes);
        copyImage(targets.layeredExitIdentity, VK_IMAGE_ASPECT_COLOR_BIT,
            imageBytes * 2u);
        copyImage(targets.layeredExitDepth, VK_IMAGE_ASPECT_DEPTH_BIT,
            imageBytes * 3u);
        copyImage(targets.layeredLocalColor, VK_IMAGE_ASPECT_COLOR_BIT,
            imageBytes * InterfaceImageCount);
    }

    void VulkanQualificationExtension::collectOrdinary2ValidationsForSlot(
        uint32_t frameIndex) {
        drainSlot(pendingOrdinary2_, frameIndex, *services_.allocator,
            "Completed Ordinary2 readback is not host mapped",
            [&](const PendingOrdinary2CaptureValidation& pending) {
                completedOrdinary2_.push_back(analyzeOrdinary2CaptureReadback({
                    .validationId = pending.validationId,
                    .width = pending.extent.width,
                    .height = pending.extent.height,
                    .expectedDrawCount = pending.expectedDrawCount,
                    .workItemCount = pending.workItemCount,
                }, mappedBytes(pending.readback)));
            });
    }

    std::vector<Ordinary2CaptureValidationResult>
    VulkanQualificationExtension::collectOrdinary2CaptureValidations(
        bool waitForPending) {
        if (waitForPending && !pendingOrdinary2_.empty()) {
            services_.scheduler->waitForAllFrames();
            for (uint32_t frameIndex = 0;
                frameIndex < VulkanFrameScheduler::FramesInFlight; ++frameIndex) {
                collectOrdinary2ValidationsForSlot(frameIndex);
            }
        }
        std::vector<Ordinary2CaptureValidationResult> result =
            std::move(completedOrdinary2_);
        completedOrdinary2_.clear();
        return result;
    }

    // --------------------------------------------------------------------
    // Deep layered (Hero4 / Cinematic8) capture validation
    // --------------------------------------------------------------------

    void VulkanQualificationExtension::armDeepLayeredCaptureValidation(
        uint64_t validationId, TransparencyQuality quality) {
        if (quality != TransparencyQuality::Hero4 &&
            quality != TransparencyQuality::Cinematic8) {
            throw std::invalid_argument(
                "Deep layered validation requires Hero4 or Cinematic8");
        }
        const bool duplicatePending = std::ranges::any_of(pendingDeepLayered_,
            [validationId](const PendingDeepLayeredCaptureValidation& pending) {
                return pending.validationId == validationId;
            });
        const bool duplicateCompleted = std::ranges::any_of(completedDeepLayered_,
            [validationId](const DeepLayeredCaptureValidationResult& completed) {
                return completed.validationId == validationId;
            });
        if (deepLayeredRequest_ || duplicatePending || duplicateCompleted) {
            throw std::invalid_argument(
                "Deep layered validation permits one unique request at a time");
        }
        deepLayeredRequest_ =
            DeepLayeredCaptureValidationRequest{ validationId, quality };
    }

    void VulkanQualificationExtension::recordDeepLayeredReadback(
        const VulkanHookContext& context,
        const VulkanDeepLayeredHookPayload& payload) {
        const TransparencyQuality quality = payload.quality;
        const uint32_t interfaceCount = payload.interfaceCount;
        VulkanFrameContextTargets& targets =
            services_.frameTargets->get(context.slot);
        VulkanFrameContextTargets::DeepLayeredTier& tier = quality ==
                TransparencyQuality::Hero4
            ? targets.hero4 : targets.cinematic8;
        if (!tier.active() || tier.interfaceCount != interfaceCount ||
            tier.atlasExtent.width == 0u || tier.atlasExtent.height == 0u) {
            throw std::logic_error(
                "Deep validation readback requires a complete resident tier");
        }
        const uint64_t pixelCount =
            static_cast<uint64_t>(tier.atlasExtent.width) *
            tier.atlasExtent.height;
        constexpr uint64_t BytesPerImagePixel = sizeof(uint32_t);
        const uint64_t interfaceImageCount = interfaceCount * 2ull;
        const uint64_t bytesPerPixel =
            BytesPerImagePixel * interfaceImageCount + 8ull;
        if (pixelCount > std::numeric_limits<VkDeviceSize>::max() /
                bytesPerPixel) {
            throw std::overflow_error(
                "Deep validation readback exceeds VkDeviceSize");
        }
        const VkDeviceSize imageBytes = pixelCount * BytesPerImagePixel;
        const VkExtent2D tileExtent{
            (tier.atlasExtent.width +
                kDeepLayeredEarlyTerminationTileSize - 1u) /
                kDeepLayeredEarlyTerminationTileSize,
            (tier.atlasExtent.height +
                kDeepLayeredEarlyTerminationTileSize - 1u) /
                kDeepLayeredEarlyTerminationTileSize };
        const VkDeviceSize tileImageBytes = static_cast<VkDeviceSize>(
            tileExtent.width) * tileExtent.height * sizeof(uint32_t);
        const VkDeviceSize interfaceAndLocalBytes = pixelCount * bytesPerPixel;
        if (tileImageBytes > ((std::numeric_limits<VkDeviceSize>::max)() -
                interfaceAndLocalBytes) / interfaceCount) {
            throw std::overflow_error(
                "Deep validation tile readback exceeds VkDeviceSize");
        }
        const VkDeviceSize totalBytes = interfaceAndLocalBytes +
            tileImageBytes * interfaceCount;

        PendingDeepLayeredCaptureValidation pending{};
        pending.validationId = deepLayeredRequest_->validationId;
        pending.frameIndex = context.slot;
        pending.extent = tier.atlasExtent;
        pending.quality = quality;
        pending.interfaceCount = interfaceCount;
        pending.expectedDrawCount = payload.drawCount;
        pending.workItemCount = payload.workItemCount;
        pending.readback = createReadback(totalBytes);
        pushPending(
            pendingDeepLayered_, std::move(pending), *services_.allocator);
        deepLayeredRequest_.reset();

        PendingDeepLayeredCaptureValidation& recorded =
            pendingDeepLayered_.back();
        VulkanCommandList commandList(context.cmd);
        commandList.transition(recorded.readback,
            ResourceState::CopyDestination);
        const auto copyImage = [&](const VulkanImageResource& image,
                VkImageAspectFlags aspect, VkDeviceSize offset) {
            VkBufferImageCopy copy{};
            copy.bufferOffset = offset;
            copy.imageSubresource.aspectMask = aspect;
            copy.imageSubresource.layerCount = 1u;
            copy.imageExtent = { tier.atlasExtent.width,
                tier.atlasExtent.height, 1u };
            commandList.copyImageToBuffer(image, recorded.readback, copy);
        };
        for (uint32_t interfaceIndex = 0u;
            interfaceIndex < interfaceCount; ++interfaceIndex) {
            copyImage(tier.interfaceIdentity[interfaceIndex],
                VK_IMAGE_ASPECT_COLOR_BIT,
                imageBytes * (interfaceIndex * 2u));
            copyImage(tier.interfaceDepth[interfaceIndex],
                VK_IMAGE_ASPECT_DEPTH_BIT,
                imageBytes * (interfaceIndex * 2u + 1u));
        }
        copyImage(tier.localColor, VK_IMAGE_ASPECT_COLOR_BIT,
            imageBytes * interfaceImageCount);
        const VkDeviceSize tileBaseOffset = imageBytes *
            interfaceImageCount + pixelCount * 8ull;
        for (uint32_t interfaceIndex = 0u;
            interfaceIndex < interfaceCount; ++interfaceIndex) {
            if (!deepLayeredTerminationInterface(interfaceIndex,
                    interfaceCount)) {
                continue;
            }
            VkBufferImageCopy copy{};
            copy.bufferOffset = tileBaseOffset +
                tileImageBytes * interfaceIndex;
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1u;
            copy.imageExtent = { tileExtent.width, tileExtent.height, 1u };
            commandList.copyImageToBuffer(
                tier.tileTermination[interfaceIndex], recorded.readback, copy);
        }
    }

    void VulkanQualificationExtension::recordDeepResolveCounts(
        const VulkanHookContext& context,
        const VulkanDeepResolveCountsPayload& payload) {
        for (PendingDeepLayeredCaptureValidation& pending : pendingDeepLayered_) {
            if (pending.frameIndex == context.slot) {
                pending.sceneResolveDrawCount = payload.sceneResolveDrawCounts[
                    layeredQualityTierIndex(pending.quality)];
            }
        }
    }

    void VulkanQualificationExtension::collectDeepLayeredValidationsForSlot(
        uint32_t frameIndex) {
        drainSlot(pendingDeepLayered_, frameIndex, *services_.allocator,
            "Completed deep layered readback is not host mapped",
            [&](const PendingDeepLayeredCaptureValidation& pending) {
                completedDeepLayered_.push_back(analyzeDeepLayeredCaptureReadback({
                    .validationId = pending.validationId,
                    .quality = pending.quality,
                    .width = pending.extent.width,
                    .height = pending.extent.height,
                    .interfaceCount = pending.interfaceCount,
                    .expectedDrawCount = pending.expectedDrawCount,
                    .sceneResolveDrawCount = pending.sceneResolveDrawCount,
                    .compatibilityForwardDrawCount =
                        pending.compatibilityForwardDrawCount,
                    .workItemCount = pending.workItemCount,
                }, mappedBytes(pending.readback)));
            });
    }

    std::vector<DeepLayeredCaptureValidationResult>
    VulkanQualificationExtension::collectDeepLayeredCaptureValidations(
        bool waitForPending) {
        if (waitForPending && !pendingDeepLayered_.empty()) {
            services_.scheduler->waitForAllFrames();
            for (uint32_t frameIndex = 0u;
                frameIndex < VulkanFrameScheduler::FramesInFlight; ++frameIndex) {
                collectDeepLayeredValidationsForSlot(frameIndex);
            }
        }
        std::vector<DeepLayeredCaptureValidationResult> result =
            std::move(completedDeepLayered_);
        completedDeepLayered_.clear();
        return result;
    }

    // --------------------------------------------------------------------
    // Depth-pyramid capture validation
    // --------------------------------------------------------------------

    void VulkanQualificationExtension::armDepthPyramidCaptureValidation(
        uint64_t validationId) {
        if (services_.depthPyramid == nullptr) {
            throw std::logic_error(
                "Depth-pyramid validation requires the experimental build path");
        }
        const bool duplicatePending = std::ranges::any_of(pendingDepthPyramid_,
            [validationId](const PendingDepthPyramidCaptureValidation& pending) {
                return pending.validationId == validationId;
            });
        const bool duplicateCompleted = std::ranges::any_of(completedDepthPyramid_,
            [validationId](const DepthPyramidCaptureValidationResult& completed) {
                return completed.validationId == validationId;
            });
        if (depthPyramidRequest_ || duplicatePending || duplicateCompleted) {
            throw std::invalid_argument(
                "Depth-pyramid validation permits one unique request at a time");
        }
        depthPyramidRequest_ = validationId;
    }

    void VulkanQualificationExtension::recordDepthPyramidReadback(
        const VulkanHookContext& context,
        const VulkanDepthPyramidHookPayload& payload) {
        const VulkanImageResource& source =
            services_.frameTargets->get(context.slot).depth;
        if (!source.isValid() || source.format != VK_FORMAT_D32_SFLOAT ||
            services_.depthPyramid == nullptr) {
            throw std::logic_error(
                "Depth-pyramid validation requires a live D32 source image");
        }
        const uint32_t mipCount = depthPyramidMipCount(
            { source.extent.width, source.extent.height });
        uint64_t pyramidTexels = 0;
        for (uint32_t mip = 0; mip < mipCount; ++mip) {
            const DepthPyramidExtent mipExtent = depthPyramidMipExtent(
                { source.extent.width, source.extent.height }, mip);
            pyramidTexels += static_cast<uint64_t>(mipExtent.width) *
                mipExtent.height;
        }
        const uint64_t sourceTexels = static_cast<uint64_t>(source.extent.width) *
            source.extent.height;
        const uint64_t maximumFloatCount =
            (std::numeric_limits<VkDeviceSize>::max)() / sizeof(float);
        if (sourceTexels > maximumFloatCount ||
            pyramidTexels > maximumFloatCount - sourceTexels) {
            throw std::overflow_error(
                "Depth-pyramid validation readback exceeds VkDeviceSize");
        }
        const VkDeviceSize readbackBytes =
            (sourceTexels + pyramidTexels) * sizeof(float);
        PendingDepthPyramidCaptureValidation pending{};
        pending.validationId = *depthPyramidRequest_;
        pending.frameIndex = context.slot;
        pending.extent = source.extent;
        pending.mipCount = mipCount;
        pending.readback = createReadback(readbackBytes);
        pushPending(
            pendingDepthPyramid_, std::move(pending), *services_.allocator);
        depthPyramidRequest_.reset();

        PendingDepthPyramidCaptureValidation& recorded =
            pendingDepthPyramid_.back();
        VulkanCommandList commands(context.cmd);
        commands.transition(recorded.readback, ResourceState::CopyDestination);
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
        copy.imageExtent = { source.extent.width, source.extent.height, 1 };
        vkCmdCopyImageToBuffer(context.cmd, source.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, recorded.readback.buffer,
            1, &copy);
        const VkDeviceSize offset = sourceTexels * sizeof(float);
        services_.depthPyramid->recordHistoryReadback(context.cmd,
            payload.retainedView, recorded.readback.buffer, offset);
    }

    void VulkanQualificationExtension::collectDepthPyramidValidationsForSlot(
        uint32_t frameIndex) {
        drainSlot(pendingDepthPyramid_, frameIndex, *services_.allocator,
            "Completed depth-pyramid readback is not host mapped",
            [&](const PendingDepthPyramidCaptureValidation& pending) {
                completedDepthPyramid_.push_back(analyzeDepthPyramidCaptureReadback({
                    .validationId = pending.validationId,
                    .width = pending.extent.width,
                    .height = pending.extent.height,
                    .mipCount = pending.mipCount,
                }, mappedBytes(pending.readback)));
            });
    }

    std::vector<DepthPyramidCaptureValidationResult>
    VulkanQualificationExtension::collectDepthPyramidCaptureValidations(
        bool waitForPending) {
        if (waitForPending && !pendingDepthPyramid_.empty()) {
            services_.scheduler->waitForAllFrames();
            for (uint32_t frameIndex = 0;
                frameIndex < VulkanFrameScheduler::FramesInFlight; ++frameIndex) {
                services_.depthPyramid->onFrameFenceCompleted(frameIndex,
                    services_.scheduler->completedSerial());
                collectDepthPyramidValidationsForSlot(frameIndex);
            }
        }
        std::vector<DepthPyramidCaptureValidationResult> result =
            std::move(completedDepthPyramid_);
        completedDepthPyramid_.clear();
        return result;
    }

} // namespace Iridium
