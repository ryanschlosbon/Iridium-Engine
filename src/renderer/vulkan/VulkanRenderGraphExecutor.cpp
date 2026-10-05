#include "renderer/vulkan/VulkanRenderGraphExecutor.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace Iridium {
namespace {

    bool hasUsage(RenderGraph::UsageMask mask, RenderGraph::Access access) noexcept {
        return (mask & RenderGraph::usageBit(access)) != 0;
    }

    VkImageAspectFlags aspectForFormat(VkFormat format) {
        if (format == VK_FORMAT_D32_SFLOAT) {
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }

    VkImageUsageFlags imageUsage(RenderGraph::UsageMask usages) {
        VkImageUsageFlags result = 0;
        if (hasUsage(usages, RenderGraph::Access::ColorAttachment)) {
            result |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
        if (hasUsage(usages, RenderGraph::Access::DepthAttachmentWrite) ||
            hasUsage(usages, RenderGraph::Access::DepthAttachmentRead)) {
            result |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        }
        if (hasUsage(usages, RenderGraph::Access::SampledRead)) {
            result |= VK_IMAGE_USAGE_SAMPLED_BIT;
        }
        if (hasUsage(usages, RenderGraph::Access::StorageRead) ||
            hasUsage(usages, RenderGraph::Access::StorageWrite) ||
            hasUsage(usages, RenderGraph::Access::StorageReadWrite)) {
            result |= VK_IMAGE_USAGE_STORAGE_BIT;
        }
        if (hasUsage(usages, RenderGraph::Access::TransferSource)) {
            result |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }
        if (hasUsage(usages, RenderGraph::Access::TransferDestination)) {
            result |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        }
        return result;
    }

    VkBufferUsageFlags bufferUsage(RenderGraph::UsageMask usages) {
        VkBufferUsageFlags result = 0;
        if (hasUsage(usages, RenderGraph::Access::StorageRead) ||
            hasUsage(usages, RenderGraph::Access::StorageWrite) ||
            hasUsage(usages, RenderGraph::Access::StorageReadWrite)) {
            result |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        }
        if (hasUsage(usages, RenderGraph::Access::TransferSource)) {
            result |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        }
        if (hasUsage(usages, RenderGraph::Access::TransferDestination)) {
            result |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        }
        if (hasUsage(usages, RenderGraph::Access::VertexRead)) {
            result |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        }
        if (hasUsage(usages, RenderGraph::Access::IndexRead)) {
            result |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        }
        if (hasUsage(usages, RenderGraph::Access::IndirectRead)) {
            result |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        }
        return result;
    }

    uint64_t resourceRequestedBytes(
        const VulkanGraphPhysicalResource& resource) noexcept {
        return resource.type == RenderGraph::ResourceType::Image
            ? resource.image.allocation.requestedBytes
            : resource.buffer.allocation.requestedBytes;
    }

    uint64_t resourceCommittedBytes(
        const VulkanGraphPhysicalResource& resource) noexcept {
        return resource.type == RenderGraph::ResourceType::Image
            ? resource.image.allocation.committedBytes
            : resource.buffer.allocation.committedBytes;
    }

    VulkanGraphAccessInfo accessInfoForAspect(RenderGraph::Access access,
        RenderGraph::ResourceType type, VkImageAspectFlags aspect) {
        VulkanGraphAccessInfo info = getVulkanGraphAccessInfo(access, type);
        if (type == RenderGraph::ResourceType::Image &&
            access == RenderGraph::Access::SampledRead &&
            (aspect & (VK_IMAGE_ASPECT_DEPTH_BIT |
                VK_IMAGE_ASPECT_STENCIL_BIT)) != 0) {
            info.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        }
        return info;
    }

    class VulkanCommandBarrierSink final : public VulkanBarrierSink {
    public:
        void pipelineBarrier(VkCommandBuffer commandBuffer,
            VkPipelineStageFlags sourceStages, VkPipelineStageFlags destinationStages,
            std::span<const VkBufferMemoryBarrier> buffers,
            std::span<const VkImageMemoryBarrier> images) override {
            vkCmdPipelineBarrier(commandBuffer, sourceStages, destinationStages, 0,
                0, nullptr, static_cast<uint32_t>(buffers.size()), buffers.data(),
                static_cast<uint32_t>(images.size()), images.data());
        }
        void pipelineBarrier2(VkCommandBuffer commandBuffer,
            const VkDependencyInfo& dependency) override {
            vkCmdPipelineBarrier2(commandBuffer, &dependency);
        }
    };

    // R3b.2 equivalence-first mapping: every sync1 stage and access bit the
    // graph uses has the same value in the *2 enums, so a cast preserves the
    // dependency. Add an assert here before using any new bit.
    static_assert(VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    static_assert(VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT == VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
    static_assert(VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT == VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
    static_assert(VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT == VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
    static_assert(VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT == VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    static_assert(VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT ==
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
    static_assert(VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT ==
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
    static_assert(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT ==
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    static_assert(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    static_assert(VK_PIPELINE_STAGE_2_TRANSFER_BIT == VK_PIPELINE_STAGE_TRANSFER_BIT);
    static_assert(VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    static_assert(VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT == VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
    static_assert(VK_ACCESS_2_INDEX_READ_BIT == VK_ACCESS_INDEX_READ_BIT);
    static_assert(VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT == VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
    static_assert(VK_ACCESS_2_SHADER_READ_BIT == VK_ACCESS_SHADER_READ_BIT);
    static_assert(VK_ACCESS_2_SHADER_WRITE_BIT == VK_ACCESS_SHADER_WRITE_BIT);
    static_assert(VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT == VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
    static_assert(VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT == VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    static_assert(VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT ==
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    static_assert(VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT ==
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    static_assert(VK_ACCESS_2_TRANSFER_READ_BIT == VK_ACCESS_TRANSFER_READ_BIT);
    static_assert(VK_ACCESS_2_TRANSFER_WRITE_BIT == VK_ACCESS_TRANSFER_WRITE_BIT);

    // The sync1 fallback's inverse: a NONE source scope is TOP_OF_PIPE.
    VkPipelineStageFlags toSourceStages1(VkPipelineStageFlags2 stages) noexcept {
        return stages == VK_PIPELINE_STAGE_2_NONE
            ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
            : static_cast<VkPipelineStageFlags>(stages);
    }

    // ---- R4a dynamic rendering ----------------------------------------------

    bool isAttachmentWriteAccess(RenderGraph::Access access) noexcept {
        return access == RenderGraph::Access::ColorAttachment ||
            access == RenderGraph::Access::DepthAttachmentWrite;
    }

    // Source half of a same-access attachment re-barrier (finding 1 of the R4
    // design): a render pass's EXTERNAL dependency used to order consecutive
    // attachment writes; under dynamic rendering the executor orders the
    // previous pass's attachment writes before this pass's attachment
    // reads/writes. Layout and stages are unchanged.
    VulkanGraphAccessInfo attachmentWriteSource(VulkanGraphAccessInfo info) noexcept {
        info.access &= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        return info;
    }

    VkAttachmentLoadOp toVkLoadOp(RenderGraph::LoadOp op) noexcept {
        switch (op) {
        case RenderGraph::LoadOp::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case RenderGraph::LoadOp::Load: return VK_ATTACHMENT_LOAD_OP_LOAD;
        case RenderGraph::LoadOp::DontCare: break;
        }
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }

    VkAttachmentStoreOp toVkStoreOp(RenderGraph::StoreOp op) noexcept {
        switch (op) {
        case RenderGraph::StoreOp::Store: return VK_ATTACHMENT_STORE_OP_STORE;
        case RenderGraph::StoreOp::None: return VK_ATTACHMENT_STORE_OP_NONE;
        case RenderGraph::StoreOp::DontCare: break;
        }
        return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }

    VulkanPassRenderingPlan buildRenderingPlan(const RenderGraph::CompiledGraph& graph,
        const RenderGraph::CompiledPass& pass) {
        VulkanPassRenderingPlan plan{};
        for (uint32_t index = 0; index < pass.usageCount; ++index) {
            const RenderGraph::CompiledUsage& usage = graph.usages()[pass.firstUsage + index];
            const bool color = usage.write &&
                usage.access == RenderGraph::Access::ColorAttachment;
            const bool depth = usage.write
                ? usage.access == RenderGraph::Access::DepthAttachmentWrite
                : usage.access == RenderGraph::Access::DepthAttachmentRead;
            const RenderGraph::CompiledResource& resource =
                graph.resources()[usage.logicalResourceIndex];
            if ((!color && !depth) || resource.desc.type != RenderGraph::ResourceType::Image)
                continue;
            const VkFormat format = toVkFormat(resource.desc.image.format);
            VulkanRenderingAttachmentPlan attachment{};
            attachment.logicalResourceIndex = usage.logicalResourceIndex;
            attachment.layout = accessInfoForAspect(usage.access, resource.desc.type,
                aspectForFormat(format)).layout;
            attachment.loadOp = toVkLoadOp(usage.loadOp);
            attachment.storeOp = toVkStoreOp(usage.storeOp);
            attachment.extent = { resource.desc.image.extent.width,
                resource.desc.image.extent.height };
            if (color) {
                for (uint32_t channel = 0; channel < 4; ++channel)
                    attachment.clearValue.color.uint32[channel] =
                        usage.clearValue.colorBits[channel];
                if (plan.colorCount == plan.color.size()) {
                    plan.valid = false;
                    continue;
                }
                plan.color[plan.colorCount++] = attachment;
            }
            else {
                attachment.clearValue.depthStencil = { usage.clearValue.depth,
                    usage.clearValue.stencil };
                if (plan.depthCount == plan.depth.size()) {
                    plan.valid = false;
                    continue;
                }
                plan.depth[plan.depthCount++] = attachment;
            }
        }
        return plan;
    }

} // namespace

void VulkanBarrierSink::beginRendering(VkCommandBuffer commandBuffer,
    const VkRenderingInfo& rendering) {
    vkCmdBeginRendering(commandBuffer, &rendering);
}

void VulkanBarrierSink::endRendering(VkCommandBuffer commandBuffer) {
    vkCmdEndRendering(commandBuffer);
}

VulkanBarrierSink& vulkanCommandBarrierSink() noexcept {
    static VulkanCommandBarrierSink sink;
    return sink;
}

bool vulkanDeviceSupportsSynchronization2(VkPhysicalDevice physicalDevice) noexcept {
    if (physicalDevice == VK_NULL_HANDLE) return false;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_3) return false;
    VkPhysicalDeviceVulkan13Features vulkan13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceFeatures2 features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    features.pNext = &vulkan13;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features);
    return vulkan13.synchronization2 == VK_TRUE;
}

VkPipelineStageFlags2 toVulkanSourceStages2(VkPipelineStageFlags stages) noexcept {
    // A TOP_OF_PIPE source scope waits on nothing: sync2 spells that NONE.
    return stages == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
        ? VK_PIPELINE_STAGE_2_NONE
        : static_cast<VkPipelineStageFlags2>(stages);
}

VkPipelineStageFlags2 toVulkanDestinationStages2(VkPipelineStageFlags stages) noexcept {
    return static_cast<VkPipelineStageFlags2>(stages);
}

VulkanGraphAccessInfo getVulkanGraphAccessInfo(RenderGraph::Access access,
    RenderGraph::ResourceType type) {
    const bool image = type == RenderGraph::ResourceType::Image;
    switch (access) {
    case RenderGraph::Access::Undefined:
        return { VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
            VK_IMAGE_LAYOUT_UNDEFINED };
    case RenderGraph::Access::ColorAttachment:
        return { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            image ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED };
    case RenderGraph::Access::DepthAttachmentWrite:
        return { VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            image ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL :
                VK_IMAGE_LAYOUT_UNDEFINED };
    case RenderGraph::Access::DepthAttachmentRead:
        return { VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
            image ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL :
                VK_IMAGE_LAYOUT_UNDEFINED };
    case RenderGraph::Access::SampledRead:
        return { VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT,
            image ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL :
                VK_IMAGE_LAYOUT_UNDEFINED };
    case RenderGraph::Access::StorageRead:
        return { VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT,
            image ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED };
    case RenderGraph::Access::StorageWrite:
        return { VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT,
            image ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED };
    case RenderGraph::Access::StorageReadWrite:
        return { VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            image ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED };
    case RenderGraph::Access::TransferSource:
        return { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            image ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED };
    case RenderGraph::Access::TransferDestination:
        return { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            image ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED };
    case RenderGraph::Access::VertexRead:
        return { VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
            VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED };
    case RenderGraph::Access::IndexRead:
        return { VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
            VK_ACCESS_INDEX_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED };
    case RenderGraph::Access::IndirectRead:
        return { VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
            VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED };
    case RenderGraph::Access::Present:
        return { VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
            image ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED };
    }
    throw std::invalid_argument("Unsupported render-graph access");
}

VkFormat toVkFormat(RenderGraph::Format format) {
    switch (format) {
    case RenderGraph::Format::Rgba8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case RenderGraph::Format::Bgra8Srgb: return VK_FORMAT_B8G8R8A8_SRGB;
    case RenderGraph::Format::Rgb10A2Unorm:
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case RenderGraph::Format::Rgba16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case RenderGraph::Format::R16Float: return VK_FORMAT_R16_SFLOAT;
    case RenderGraph::Format::Rg16Snorm: return VK_FORMAT_R16G16_SNORM;
    case RenderGraph::Format::R11G11B10Float: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case RenderGraph::Format::R16Uint: return VK_FORMAT_R16_UINT;
    case RenderGraph::Format::R32Uint: return VK_FORMAT_R32_UINT;
    case RenderGraph::Format::R32Float: return VK_FORMAT_R32_SFLOAT;
    case RenderGraph::Format::D32Float: return VK_FORMAT_D32_SFLOAT;
    case RenderGraph::Format::Undefined: break;
    }
    throw std::invalid_argument("Unsupported render-graph format");
}

RenderGraph::Format toGraphFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM: return RenderGraph::Format::Rgba8Unorm;
    case VK_FORMAT_B8G8R8A8_SRGB: return RenderGraph::Format::Bgra8Srgb;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return RenderGraph::Format::Rgb10A2Unorm;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return RenderGraph::Format::Rgba16Float;
    case VK_FORMAT_R16_SFLOAT: return RenderGraph::Format::R16Float;
    case VK_FORMAT_R16G16_SNORM: return RenderGraph::Format::Rg16Snorm;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return RenderGraph::Format::R11G11B10Float;
    case VK_FORMAT_R16_UINT: return RenderGraph::Format::R16Uint;
    case VK_FORMAT_R32_UINT: return RenderGraph::Format::R32Uint;
    case VK_FORMAT_R32_SFLOAT: return RenderGraph::Format::R32Float;
    case VK_FORMAT_D32_SFLOAT: return RenderGraph::Format::D32Float;
    default: break;
    }
    throw std::invalid_argument("Unsupported Vulkan format for render graph");
}

namespace {

    // How the allocator factory creates a slot's image (dedicated or aliased).
    struct GraphImageParameters {
        VkExtent2D extent{};
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageUsageFlags usage = 0;
        VkImageAspectFlags aspect = 0;
        uint32_t mipLevels = 1;
        uint32_t arrayLayers = 1;
        VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
    };

    GraphImageParameters graphImageParameters(const RenderGraph::PhysicalResourceSlot& slot) {
        if (slot.image.extent.depth != 1 || slot.image.mipLevels == 0 ||
            slot.image.arrayLayers == 0 || slot.image.samples != 1) {
            throw std::invalid_argument(
                "Vulkan graph images require 2D, nonempty mip/layer ranges, and one sample");
        }
        GraphImageParameters parameters{};
        parameters.extent = { slot.image.extent.width, slot.image.extent.height };
        parameters.format = toVkFormat(slot.image.format);
        parameters.usage = imageUsage(slot.usages);
        if (parameters.usage == 0) {
            throw std::invalid_argument("Render-graph image has no Vulkan usage");
        }
        parameters.aspect = aspectForFormat(parameters.format);
        parameters.mipLevels = slot.image.mipLevels;
        parameters.arrayLayers = slot.image.arrayLayers;
        parameters.viewType = slot.image.arrayLayers == 1 ? VK_IMAGE_VIEW_TYPE_2D :
            VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        return parameters;
    }

} // namespace

RenderGraph::TransientMemoryRequirement VulkanGraphResourceFactory::aliasRequirement(
    const RenderGraph::PhysicalResourceSlot& slot) {
    // Nominal: 8 bytes per texel and layer (x 2 with mips), 64 KiB aligned.
    constexpr uint64_t Alignment = uint64_t{ 1 } << 16;
    const RenderGraph::ImageDesc& image = slot.image;
    uint64_t bytes = uint64_t{ image.extent.width } * image.extent.height *
        image.extent.depth * image.arrayLayers * 8u * (image.mipLevels > 1 ? 2u : 1u);
    bytes = (std::max)(bytes, Alignment);
    return { (bytes + Alignment - 1) / Alignment * Alignment, Alignment, 1u };
}

VulkanAliasHeapResource VulkanGraphResourceFactory::createAliasHeap(
    const RenderGraph::AliasHeap& heap, uint64_t requestedBytes) {
    VulkanAliasHeapResource result{};
    result.size = heap.size;
    result.allocation.requestedBytes = requestedBytes;
    result.allocation.committedBytes = heap.size;
    return result;
}

void VulkanGraphResourceFactory::destroyAliasHeap(VulkanAliasHeapResource& heap) noexcept {
    heap = {};
}

VulkanGraphPhysicalResource VulkanGraphResourceFactory::createAliased(
    const RenderGraph::PhysicalResourceSlot& slot, const VulkanAliasHeapResource&,
    uint64_t) {
    return create(slot);
}

RenderGraph::TransientMemoryRequirement
VulkanAllocatorGraphResourceFactory::aliasRequirement(
    const RenderGraph::PhysicalResourceSlot& slot) {
    if (allocator_ == nullptr || slot.type != RenderGraph::ResourceType::Image)
        throw std::logic_error("Only graph images can be aliased");
    const GraphImageParameters parameters = graphImageParameters(slot);
    const VkMemoryRequirements requirements = allocator_->imageMemoryRequirements(
        parameters.extent, parameters.format, parameters.usage, parameters.mipLevels,
        parameters.arrayLayers, 0);
    return { requirements.size, requirements.alignment, requirements.memoryTypeBits };
}

VulkanAliasHeapResource VulkanAllocatorGraphResourceFactory::createAliasHeap(
    const RenderGraph::AliasHeap& heap, uint64_t requestedBytes) {
    if (allocator_ == nullptr)
        throw std::logic_error("Invalid Vulkan graph alias heap request");
    return allocator_->createAliasHeap(heap.size, heap.alignment, heap.typeMask,
        category_, requestedBytes);
}

void VulkanAllocatorGraphResourceFactory::destroyAliasHeap(
    VulkanAliasHeapResource& heap) noexcept {
    if (allocator_ != nullptr) allocator_->destroy(heap);
    heap = {};
}

VulkanGraphPhysicalResource VulkanAllocatorGraphResourceFactory::createAliased(
    const RenderGraph::PhysicalResourceSlot& slot, const VulkanAliasHeapResource& heap,
    uint64_t offset) {
    if (allocator_ == nullptr || slot.logicalResources.size() != 1 ||
        slot.type != RenderGraph::ResourceType::Image)
        throw std::logic_error("Invalid Vulkan graph aliased image request");
    const GraphImageParameters parameters = graphImageParameters(slot);
    VulkanGraphPhysicalResource resource{};
    resource.type = slot.type;
    resource.image = allocator_->createAliasingImage2D(heap, offset, parameters.extent,
        parameters.format, parameters.usage, parameters.aspect, parameters.mipLevels,
        parameters.arrayLayers, 0, parameters.viewType);
    return resource;
}

VulkanGraphPhysicalResource VulkanAllocatorGraphResourceFactory::create(
    const RenderGraph::PhysicalResourceSlot& slot) {
    if (allocator_ == nullptr || slot.logicalResources.empty()) {
        throw std::logic_error("Invalid Vulkan graph resource allocation request");
    }
    VulkanGraphPhysicalResource resource{};
    resource.type = slot.type;
    if (slot.type == RenderGraph::ResourceType::Image) {
        const GraphImageParameters parameters = graphImageParameters(slot);
        resource.image = allocator_->createImage2D(parameters.extent, parameters.format,
            parameters.usage, parameters.aspect, category_, parameters.mipLevels,
            parameters.arrayLayers, 0, parameters.viewType);
    }
    else {
        const VkBufferUsageFlags usage = bufferUsage(slot.usages);
        if (usage == 0) {
            throw std::invalid_argument("Render-graph buffer has no Vulkan usage");
        }
        resource.buffer = allocator_->createBuffer(slot.buffer.size, usage,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false,
            category_);
    }
    return resource;
}

void VulkanAllocatorGraphResourceFactory::destroy(
    VulkanGraphPhysicalResource& resource) noexcept {
    if (allocator_ == nullptr) {
        resource = {};
        return;
    }
    if (resource.type == RenderGraph::ResourceType::Image) {
        allocator_->destroy(resource.image);
    }
    else {
        allocator_->destroy(resource.buffer);
    }
    resource = {};
}

VulkanGraphResourcePool::~VulkanGraphResourcePool() {
    cleanupAfterDeviceIdle();
}

void VulkanGraphResourcePool::init(VulkanGraphResourceFactory& factory,
    uint32_t frameCount) {
    if (factory_ != nullptr || frameCount == 0) {
        throw std::logic_error("Vulkan graph resource pool initialized incorrectly");
    }
    factory_ = &factory;
    active_.resize(frameCount);
    retired_.resize(frameCount);
}

void VulkanGraphResourcePool::rebuild(const RenderGraph::CompiledGraph& graph,
    const RenderGraph::AliasPlan* plan) {
    if (factory_ == nullptr || active_.empty()) {
        throw std::logic_error("Vulkan graph resource pool is not initialized");
    }
    bool anyAliased = false;
    for (const RenderGraph::PhysicalResourceSlot& slot : graph.physicalSlots())
        anyAliased = anyAliased || slot.aliased;
    if (anyAliased && plan == nullptr)
        throw std::logic_error("Aliased render-graph slots need an alias plan");

    // What each heap's members would need unaliased (accounting only).
    std::vector<uint64_t> heapRequested;
    if (anyAliased) {
        heapRequested.assign(plan->heaps.size(), 0);
        for (const RenderGraph::AliasPlacement& placement : plan->placements)
            if (placement.placed()) heapRequested.at(placement.heap) += placement.size;
    }

    std::vector<FrameResources> candidate(active_.size());
    try {
        for (FrameResources& frame : candidate) {
            if (anyAliased) {
                frame.heaps.reserve(plan->heaps.size());
                for (size_t heap = 0; heap < plan->heaps.size(); ++heap)
                    frame.heaps.push_back(factory_->createAliasHeap(plan->heaps[heap],
                        heapRequested[heap]));
            }
            frame.resources.reserve(graph.physicalSlots().size());
            for (const RenderGraph::PhysicalResourceSlot& slot :
                graph.physicalSlots()) {
                if (!slot.aliased) {
                    frame.resources.push_back(factory_->create(slot));
                    continue;
                }
                const RenderGraph::AliasPlacement& placement =
                    plan->placements.at(slot.logicalResources.at(0));
                if (!placement.placed())
                    throw std::logic_error("Aliased render-graph slot has no placement");
                frame.resources.push_back(factory_->createAliased(slot,
                    frame.heaps.at(placement.heap), placement.offset));
            }
        }
        for (size_t frameIndex = 0; frameIndex < active_.size(); ++frameIndex)
            retired_[frameIndex].reserve(retired_[frameIndex].size() + 1);
    }
    catch (...) {
        for (FrameResources& frame : candidate) {
            destroyFrame(frame);
        }
        throw;
    }

    for (size_t frameIndex = 0; frameIndex < active_.size(); ++frameIndex) {
        FrameResources& active = active_[frameIndex];
        if (!active.resources.empty() || !active.heaps.empty())
            retired_[frameIndex].push_back(std::move(active));
        active = {};
    }
    active_ = std::move(candidate);
}

void VulkanGraphResourcePool::onFrameFenceCompleted(uint32_t frameIndex) {
    if (frameIndex >= retired_.size()) {
        throw std::out_of_range("Render-graph frame index is out of range");
    }
    for (FrameResources& frame : retired_[frameIndex]) destroyFrame(frame);
    retired_[frameIndex].clear();
}

void VulkanGraphResourcePool::cleanupAfterDeviceIdle() noexcept {
    if (factory_ != nullptr) {
        for (FrameResources& frame : active_) {
            destroyFrame(frame);
        }
        for (auto& frames : retired_) {
            for (FrameResources& frame : frames) destroyFrame(frame);
        }
    }
    active_.clear();
    retired_.clear();
    factory_ = nullptr;
}

size_t VulkanGraphResourcePool::activeResourceCount(uint32_t frameIndex) const {
    if (frameIndex >= active_.size()) {
        throw std::out_of_range("Render-graph frame index is out of range");
    }
    return active_[frameIndex].resources.size();
}

size_t VulkanGraphResourcePool::retiredResourceCount(uint32_t frameIndex) const {
    if (frameIndex >= retired_.size()) {
        throw std::out_of_range("Render-graph frame index is out of range");
    }
    size_t count = 0;
    for (const FrameResources& frame : retired_[frameIndex])
        count += frame.resources.size();
    return count;
}

uint64_t VulkanGraphResourcePool::requestedBytes() const noexcept {
    uint64_t result = 0;
    const auto add = [&](const FrameResources& frame) {
        for (const VulkanGraphPhysicalResource& resource : frame.resources)
            result += resourceRequestedBytes(resource);
    };
    for (const FrameResources& frame : active_) add(frame);
    for (const auto& frames : retired_)
        for (const FrameResources& frame : frames) add(frame);
    return result;
}

uint64_t VulkanGraphResourcePool::committedBytes() const noexcept {
    uint64_t result = aliasHeapCommittedBytes();
    const auto add = [&](const FrameResources& frame) {
        for (const VulkanGraphPhysicalResource& resource : frame.resources)
            result += resourceCommittedBytes(resource);
    };
    for (const FrameResources& frame : active_) add(frame);
    for (const auto& frames : retired_)
        for (const FrameResources& frame : frames) add(frame);
    return result;
}

uint64_t VulkanGraphResourcePool::aliasHeapCommittedBytes() const noexcept {
    uint64_t result = 0;
    const auto add = [&](const FrameResources& frame) {
        for (const VulkanAliasHeapResource& heap : frame.heaps)
            result += heap.allocation.committedBytes;
    };
    for (const FrameResources& frame : active_) add(frame);
    for (const auto& frames : retired_)
        for (const FrameResources& frame : frames) add(frame);
    return result;
}

const VulkanGraphPhysicalResource& VulkanGraphResourcePool::resource(
    uint32_t frameIndex, uint32_t physicalSlot) const {
    if (frameIndex >= active_.size() ||
        physicalSlot >= active_[frameIndex].resources.size()) {
        throw std::out_of_range("Render-graph physical resource is out of range");
    }
    return active_[frameIndex].resources[physicalSlot];
}

std::span<const VulkanAliasHeapResource> VulkanGraphResourcePool::aliasHeaps(
    uint32_t frameIndex) const {
    if (frameIndex >= active_.size())
        throw std::out_of_range("Render-graph frame index is out of range");
    return active_[frameIndex].heaps;
}

void VulkanGraphResourcePool::destroyFrame(FrameResources& frame) noexcept {
    // Images bound into a heap go before the heap's memory.
    for (VulkanGraphPhysicalResource& resource : frame.resources) {
        factory_->destroy(resource);
    }
    for (VulkanAliasHeapResource& heap : frame.heaps) {
        factory_->destroyAliasHeap(heap);
    }
    frame.resources.clear();
    frame.heaps.clear();
}

void VulkanRenderGraphExecutor::init(VulkanResourceAllocator& allocator,
    uint32_t frameCount, ProfileMemoryCategory category) {
    if (allocatorFactory_ || resources_.frameCount() != 0 || frameCount == 0) {
        throw std::logic_error("Vulkan graph executor initialized incorrectly");
    }
    allocatorFactory_.emplace(allocator, category);
    try {
        resources_.init(*allocatorFactory_, frameCount);
    }
    catch (...) {
        allocatorFactory_.reset();
        throw;
    }
    factory_ = &*allocatorFactory_;
    barrierApi_ = vulkanDeviceSupportsSynchronization2(allocator.physicalDevice())
        ? VulkanBarrierApi::Synchronization2
        : VulkanBarrierApi::Synchronization1;
}

void VulkanRenderGraphExecutor::init(VulkanGraphResourceFactory& factory,
    uint32_t frameCount) {
    if (allocatorFactory_ || resources_.frameCount() != 0 || frameCount == 0) {
        throw std::logic_error("Vulkan graph executor initialized incorrectly");
    }
    resources_.init(factory, frameCount);
    factory_ = &factory;
    barrierApi_ = VulkanBarrierApi::Synchronization1;
}

void VulkanRenderGraphExecutor::setBarrierApi(VulkanBarrierApi api) {
    if (executingFrame_ != RenderGraph::InvalidIndex)
        throw std::logic_error("Barrier API cannot change during frame execution");
    barrierApi_ = api;
}

void VulkanRenderGraphExecutor::rebuild(RenderGraph::CompiledGraph graph) {
    if (resources_.frameCount() == 0 || factory_ == nullptr) {
        throw std::logic_error("Vulkan graph executor is not initialized");
    }
    if (executingFrame_ != RenderGraph::InvalidIndex) {
        throw std::logic_error("Render-graph rebuild during frame execution");
    }

    std::vector<VulkanGraphBarrierIntent> candidateBarriers;
    candidateBarriers.reserve(graph.transitions().size());
    for (const RenderGraph::CompiledTransition& transition : graph.transitions()) {
        const RenderGraph::CompiledResource& resource =
            graph.resources()[transition.logicalResourceIndex];
        const VkImageAspectFlags aspect = resource.desc.type ==
            RenderGraph::ResourceType::Image
            ? aspectForFormat(toVkFormat(resource.desc.image.format))
            : 0;
        candidateBarriers.push_back({ transition.passOrderIndex,
            transition.logicalResourceIndex, resource.physicalSlot,
            accessInfoForAspect(transition.before, resource.desc.type, aspect),
            accessInfoForAspect(transition.after, resource.desc.type, aspect) });
    }

    // R4b.4: a plan compiled with transient aliasing places its aliased slots
    // in alias heaps. Requirements come from the factory (device image
    // requirements); the plan is per rebuild and is not kept.
    std::optional<RenderGraph::AliasPlan> aliasPlan;
    std::vector<uint8_t> candidateAliased(graph.physicalSlots().size(), 0);
    std::vector<uint32_t> candidateAliasedSlots;
    std::vector<uint32_t> candidatePredecessorFirst;
    std::vector<uint32_t> candidatePredecessorSlots;
    for (const RenderGraph::PhysicalResourceSlot& slot : graph.physicalSlots()) {
        if (!slot.aliased) continue;
        if (slot.type != RenderGraph::ResourceType::Image ||
            slot.logicalResources.size() != 1)
            throw std::logic_error("Aliased render-graph slots hold exactly one image");
        candidateAliased[slot.slotIndex] = 1;
        candidateAliasedSlots.push_back(slot.slotIndex);
    }
    if (!candidateAliasedSlots.empty()) {
        std::vector<RenderGraph::TransientMemoryRequirement> requirements(
            graph.resources().size());
        for (const uint32_t slot : candidateAliasedSlots) {
            const RenderGraph::PhysicalResourceSlot& physical = graph.physicalSlots()[slot];
            requirements[physical.logicalResources[0]] = factory_->aliasRequirement(physical);
        }
        aliasPlan = RenderGraph::planTransientAliasing(graph, requirements);
        // Predecessors by physical slot: the slots whose memory this slot
        // reuses after they are done in the frame.
        candidatePredecessorFirst.assign(graph.physicalSlots().size() + 1, 0);
        for (uint32_t slot = 0; slot < graph.physicalSlots().size(); ++slot) {
            candidatePredecessorFirst[slot] =
                static_cast<uint32_t>(candidatePredecessorSlots.size());
            if (candidateAliased[slot] == 0) continue;
            const uint32_t logical = graph.physicalSlots()[slot].logicalResources[0];
            if (!aliasPlan->placements[logical].placed())
                throw std::logic_error("Aliased render-graph slot was not placed");
            for (const uint32_t predecessor : aliasPlan->aliasPredecessors(logical))
                candidatePredecessorSlots.push_back(
                    graph.resources()[predecessor].physicalSlot);
        }
        candidatePredecessorFirst[graph.physicalSlots().size()] =
            static_cast<uint32_t>(candidatePredecessorSlots.size());
    }

    // History slots are global, created once per plan outside the per-frame
    // pool. Create them first so a failure leaves the active plan untouched.
    std::vector<VulkanGraphPhysicalResource> candidateHistory;
    candidateHistory.reserve(graph.historySlots().size());
    try {
        for (const RenderGraph::PhysicalResourceSlot& slot : graph.historySlots())
            candidateHistory.push_back(factory_->create(slot));
        retiredHistory_.reserve(retiredHistory_.size() + historyResources_.size());
        resources_.rebuild(graph, aliasPlan ? &*aliasPlan : nullptr);
    }
    catch (...) {
        for (VulkanGraphPhysicalResource& resource : candidateHistory)
            factory_->destroy(resource);
        throw;
    }
    // Old history may still be referenced by in-flight frame slots: retire it
    // until every slot's fence has completed.
    const uint32_t allFrames = resources_.frameCount() >= 32
        ? 0xFFFF'FFFFu : (1u << resources_.frameCount()) - 1u;
    for (VulkanGraphPhysicalResource& resource : historyResources_)
        retiredHistory_.push_back({ resource, allFrames });
    historyResources_ = std::move(candidateHistory);

    const uint64_t hash = graph.topologyHash();
    passCount_ = static_cast<uint32_t>(graph.passes().size());
    logicalResourceCount_ = static_cast<uint32_t>(graph.resources().size());
    physicalSlotCount_ = static_cast<uint32_t>(graph.physicalSlots().size());
    frameAccess_.assign(resources_.frameCount(),
        std::vector<RenderGraph::Access>(physicalSlotCount_,
            RenderGraph::Access::Undefined));
    externalBuffers_.assign(resources_.frameCount(),
        std::vector<ExternalBufferBinding>(logicalResourceCount_));
    externalBufferTracked_.assign(logicalResourceCount_, false);
    // Binding rows are sized by the first bindExternalImage (a plan without
    // imported-image bindings allocates none); rows exist whenever any
    // externalImageScope_ entry is set.
    externalImages_.clear();
    externalImageScope_.assign(logicalResourceCount_, 0);
    frameRetired_.assign(resources_.frameCount(), true);
    aliasedSlot_ = std::move(candidateAliased);
    aliasedSlots_ = std::move(candidateAliasedSlots);
    aliasPredecessorFirst_ = std::move(candidatePredecessorFirst);
    aliasPredecessorSlots_ = std::move(candidatePredecessorSlots);
    aliasHeapCount_ = aliasPlan ? static_cast<uint32_t>(aliasPlan->heaps.size()) : 0u;
    aliasedRequestedBytes_ = aliasPlan ? aliasPlan->requestedBytes : 0u;
    aliasPeakLiveBytes_ = aliasPlan ? aliasPlan->peakLiveBytes : 0u;
    cache_.store(std::move(graph));
    barriers_ = std::move(candidateBarriers);
    topologyHash_ = hash;
    ++rebuildCount_;

    // The name maps view strings owned by the cached plan, so they are rebuilt
    // whenever the bound plan changes. Duplicate names resolve to the first
    // entry, as the former linear searches did.
    graph_ = cache_.find(topologyHash_);
    passNames_.clear();
    resourceNames_.clear();
    passNames_.reserve(graph_->passes().size());
    resourceNames_.reserve(graph_->resources().size());
    for (uint32_t index = 0; index < graph_->passes().size(); ++index)
        passNames_.try_emplace(graph_->passes()[index].name, index);
    for (uint32_t index = 0; index < graph_->resources().size(); ++index)
        resourceNames_.try_emplace(graph_->resources()[index].name, index);

    // R4a: rendering plans per pass, and the exports finishFrameExecution
    // transitions (one barrier each at most).
    renderingPlans_.clear();
    renderingPlans_.reserve(passCount_);
    for (const RenderGraph::CompiledPass& pass : graph_->passes())
        renderingPlans_.push_back(buildRenderingPlan(*graph_, pass));
    exportedResources_.clear();
    for (const RenderGraph::CompiledResource& resource : graph_->resources())
        if (resource.exported) exportedResources_.push_back(resource.logicalResourceIndex);
    callbackPass_ = RenderGraph::InvalidIndex;
    renderingOpen_ = false;

    // A pass emits at most one barrier per usage (fewer after collapsing).
    uint32_t batchCapacity = std::max(1u,
        static_cast<uint32_t>(exportedResources_.size()));
    for (const RenderGraph::CompiledPass& pass : graph_->passes())
        batchCapacity = std::max(batchCapacity, pass.usageCount);
    imageBatch_.assign(batchCapacity, VkImageMemoryBarrier2{});
    bufferBatch_.assign(batchCapacity, VkBufferMemoryBarrier2{});
    batchOrder_.assign(batchCapacity, 0u);
    imageBatchCount_ = bufferBatchCount_ = batchOrderCount_ = 0;

    // Pass ids change with the plan, so registrations do not survive it.
    callbacks_.assign(passCount_, VulkanPassCallbacks{});
    passGroup_.assign(passCount_, RenderGraph::InvalidIndex);
    rangeGroups_.clear();
    rangeGroups_.reserve(std::max(passCount_, 1u));
    registeredCount_ = 0;
    openGroup_ = RenderGraph::InvalidIndex;

    // History: fresh slots, fresh state, nothing valid. Writer passes are
    // those writing a pair's `current`.
    const size_t pairCount = graph_->historyPairs().size();
    historyAccess_.assign(historyResources_.size(), RenderGraph::Access::Undefined);
    historyParity_.assign(pairCount, 0);
    historyWriterBegun_.assign(pairCount, 0);
    historyDiscarded_.assign(pairCount, 0);
    passHistoryWrites_.clear();
    passHistoryWriteFirst_.assign(passCount_ + 1, 0);
    for (uint32_t order = 0; order < passCount_; ++order) {
        passHistoryWriteFirst_[order] = static_cast<uint32_t>(passHistoryWrites_.size());
        const RenderGraph::CompiledPass& pass = graph_->passes()[order];
        for (uint32_t index = 0; index < pass.usageCount; ++index) {
            const RenderGraph::CompiledUsage& usage = graph_->usages()[pass.firstUsage + index];
            const RenderGraph::CompiledResource& resource =
                graph_->resources()[usage.logicalResourceIndex];
            if (!usage.write || resource.historyRole != RenderGraph::HistoryRole::Current)
                continue;
            if (std::find(passHistoryWrites_.begin() + passHistoryWriteFirst_[order],
                    passHistoryWrites_.end(), resource.historyPair) == passHistoryWrites_.end())
                passHistoryWrites_.push_back(resource.historyPair);
        }
    }
    passHistoryWriteFirst_[passCount_] = static_cast<uint32_t>(passHistoryWrites_.size());
    historyValidity_.resetForGraph(*graph_);
}

void VulkanRenderGraphExecutor::onFrameFenceCompleted(uint32_t frameIndex) {
    resources_.onFrameFenceCompleted(frameIndex);
    frameRetired_.at(frameIndex) = true;
    for (size_t index = 0; index < retiredHistory_.size();) {
        RetiredHistory& retired = retiredHistory_[index];
        retired.pendingFrames &= ~(frameIndex < 32 ? 1u << frameIndex : 0u);
        if (retired.pendingFrames == 0) {
            factory_->destroy(retired.resource);
            retired = retiredHistory_.back();
            retiredHistory_.pop_back();
        }
        else {
            ++index;
        }
    }
}

RenderGraph::PassId VulkanRenderGraphExecutor::findPass(
    std::string_view name) const noexcept {
    const auto found = passNames_.find(name);
    return found == passNames_.end() ? RenderGraph::PassId{}
                                     : RenderGraph::PassId{ found->second };
}

RenderGraph::GraphResourceId VulkanRenderGraphExecutor::findResource(
    std::string_view name) const noexcept {
    const auto found = resourceNames_.find(name);
    return found == resourceNames_.end() ? RenderGraph::GraphResourceId{}
                                         : RenderGraph::GraphResourceId{ found->second };
}

RenderGraph::PassId VulkanRenderGraphExecutor::passId(std::string_view name) const {
    const RenderGraph::PassId id = findPass(name);
    if (!id.isValid())
        throw std::out_of_range("Render-graph pass was not found");
    return id;
}

RenderGraph::GraphResourceId VulkanRenderGraphExecutor::resourceId(
    std::string_view name) const {
    const RenderGraph::GraphResourceId id = findResource(name);
    if (!id.isValid())
        throw std::out_of_range("Render-graph resource was not found");
    return id;
}

const RenderGraph::CompiledGraph& VulkanRenderGraphExecutor::boundGraph() const {
    if (graph_ == nullptr) {
        throw std::logic_error("Render-graph compiled plan is unavailable");
    }
    return *graph_;
}

void VulkanRenderGraphExecutor::bindExternalBuffer(uint32_t frameIndex,
    RenderGraph::GraphResourceId id, VkBuffer buffer, VkDeviceSize size,
    RenderGraph::Access initialAccess) {
    if (initialAccess == RenderGraph::Access::ColorAttachment ||
        initialAccess == RenderGraph::Access::DepthAttachmentWrite ||
        initialAccess == RenderGraph::Access::DepthAttachmentRead ||
        initialAccess == RenderGraph::Access::SampledRead || initialAccess == RenderGraph::Access::Present)
        throw std::invalid_argument("External graph buffer cannot have image-only access");
    (void)getVulkanGraphAccessInfo(initialAccess, RenderGraph::ResourceType::Buffer);
    if (graph_ == nullptr || !buffer || frameIndex >= externalBuffers_.size() ||
        !frameRetired_[frameIndex] || executingFrame_ == frameIndex)
        throw std::invalid_argument("External graph buffer binding requires a retired frame slot");
    if (id.logical >= graph_->resources().size())
        throw std::invalid_argument("External graph buffer binding is incompatible with the declared resource");
    const RenderGraph::CompiledResource& found = graph_->resources()[id.logical];
    if (!found.desc.imported ||
        found.desc.type != RenderGraph::ResourceType::Buffer ||
        found.physicalSlot != RenderGraph::InvalidIndex || size < found.desc.buffer.size)
        throw std::invalid_argument("External graph buffer binding is incompatible with the declared resource");
    const uint32_t index = id.logical;
    for (uint32_t slot = 0; slot < externalBuffers_.size(); ++slot)
        for (uint32_t resource = 0; resource < externalBuffers_[slot].size(); ++resource)
            if ((slot != frameIndex || resource != index) &&
                externalBuffers_[slot][resource].buffer == buffer)
                throw std::invalid_argument("Tracked external graph buffers must not alias "
                    "resources or frame slots: binding '" + graph_->resources()[index].name +
                    "' (slot " + std::to_string(frameIndex) + ") reuses the handle bound to '" +
                    graph_->resources()[resource].name + "' (slot " + std::to_string(slot) + ")");
    externalBuffers_[frameIndex][index] = {buffer, size, initialAccess};
    externalBufferTracked_[index] = true;
}

void VulkanRenderGraphExecutor::unbindExternalBuffer(uint32_t frameIndex,
    RenderGraph::GraphResourceId id) {
    if (graph_ == nullptr || frameIndex >= externalBuffers_.size() ||
        !frameRetired_[frameIndex] || executingFrame_ == frameIndex)
        throw std::invalid_argument("External graph buffer unbinding requires a retired frame slot");
    if (id.logical >= externalBufferTracked_.size())
        throw std::invalid_argument("External graph buffer unbinding targets an unknown resource");
    externalBuffers_[frameIndex][id.logical] = {};
    bool anyBound = false;
    for (const auto& slot : externalBuffers_)
        anyBound = anyBound || slot[id.logical].buffer != VK_NULL_HANDLE;
    externalBufferTracked_[id.logical] = anyBound;
}

bool VulkanRenderGraphExecutor::validateFrame(uint32_t frameIndex) noexcept {
    const RenderGraph::CompiledGraph* graph = cache_.find(topologyHash_);
    if (graph == nullptr) {
        ++cacheMissCount_;
        return false;
    }
    try {
        for (uint32_t index = 0; index < externalBufferTracked_.size(); ++index)
            if (externalBufferTracked_[index] && (frameIndex >= externalBuffers_.size() ||
                !externalBuffers_[frameIndex][index].buffer)) return false;
        if (frameIndex >= resources_.frameCount()) return false;
        for (uint32_t index = 0; index < externalImageScope_.size(); ++index) {
            if (externalImageScope_[index] == 0) continue;
            const uint32_t row = externalImageScope_[index] == 2
                ? resources_.frameCount() : frameIndex;
            if (!externalImages_[row][index].bound) return false;
        }
        if (historyResources_.size() != graph->historySlots().size()) return false;
        for (const VulkanGraphPhysicalResource& resource : historyResources_)
            if (!resource.isValid()) return false;
        return resources_.activeResourceCount(frameIndex) == graph->physicalSlots().size();
    }
    catch (...) {
        return false;
    }
}

void VulkanRenderGraphExecutor::beginFrameExecution(uint32_t frameIndex) {
    beginFrameExecution(frameIndex, lastView_);
}

void VulkanRenderGraphExecutor::beginFrameExecution(uint32_t frameIndex,
    const RenderGraph::ViewHistoryContext& view) {
    if (!validateFrame(frameIndex) || executingFrame_ != RenderGraph::InvalidIndex) {
        throw std::logic_error("Render-graph frame execution began in an invalid state");
    }
    executingFrame_ = frameIndex;
    frameRetired_[frameIndex] = false;
    nextPass_ = 0;
    hasRecordContext_ = false;
    recordContext_ = {};
    openGroup_ = RenderGraph::InvalidIndex;
    lastView_ = view;
    historyValidity_.beginFrame(view);
    std::fill(historyWriterBegun_.begin(), historyWriterBegun_.end(), uint8_t{ 0 });
    std::fill(historyDiscarded_.begin(), historyDiscarded_.end(), uint8_t{ 0 });
    // R4b.4: aliased slots hold no contents across frames (their memory is
    // reused within the frame), so each frame's first use starts Undefined.
    for (const uint32_t slot : aliasedSlots_)
        frameAccess_[frameIndex][slot] = RenderGraph::Access::Undefined;
    // R4a: discard-on-first-use bindings discard again each frame.
    if (!externalImages_.empty()) {
        for (const uint32_t row : { frameIndex, resources_.frameCount() })
            for (ExternalImageBinding& binding : externalImages_[row])
                binding.discardPending = binding.bound && binding.policy.discard;
    }
}

bool VulkanRenderGraphExecutor::historyValid(RenderGraph::GraphResourceId id) const {
    const RenderGraph::CompiledGraph& graph = boundGraph();
    if (id.logical >= graph.resources().size() ||
        graph.resources()[id.logical].historyPair == RenderGraph::InvalidIndex)
        throw std::invalid_argument("Render-graph resource is not a History resource");
    return historyValidity_.pairValid(graph.resources()[id.logical].historyPair);
}

uint32_t VulkanRenderGraphExecutor::historySlot(
    const RenderGraph::CompiledResource& resource) const noexcept {
    // `current` is the slot this frame's writer writes: before the writer
    // begins that is the slot not written last (parity ^ 1); the flip at the
    // writer makes it `parity`. Either way the mapping is stable in a frame
    // and `previous` is always the slot written by the last writer.
    const uint32_t pair = resource.historyPair;
    const uint32_t current = historyWriterBegun_[pair] != 0
        ? historyParity_[pair] : (historyParity_[pair] ^ 1u);
    const uint32_t half = resource.historyRole == RenderGraph::HistoryRole::Current
        ? current : (current ^ 1u);
    return pair * 2 + half;
}

const VulkanRenderGraphExecutor::ExternalImageBinding*
VulkanRenderGraphExecutor::externalImageBinding(uint32_t frameIndex,
    uint32_t logical) const noexcept {
    if (logical >= externalImageScope_.size() || externalImageScope_[logical] == 0)
        return nullptr;
    const uint32_t row = externalImageScope_[logical] == 2 ? resources_.frameCount()
        : frameIndex;
    if (row >= externalImages_.size()) return nullptr;
    const ExternalImageBinding& binding = externalImages_[row][logical];
    return binding.bound ? &binding : nullptr;
}

void VulkanRenderGraphExecutor::bindExternalImage(uint32_t frameOrGlobal,
    RenderGraph::GraphResourceId id, const VulkanImageResource& image,
    RenderGraph::Access current, ExternalSyncPolicy policy) {
    const RenderGraph::CompiledGraph& graph = boundGraph();
    const bool global = frameOrGlobal == VulkanGlobalBinding;
    if (global ? executingFrame_ != RenderGraph::InvalidIndex
               : frameOrGlobal >= resources_.frameCount() || !frameRetired_[frameOrGlobal] ||
                 executingFrame_ == frameOrGlobal)
        throw std::invalid_argument(
            "External graph image binding requires a retired frame slot or no frame in flight");
    if (id.logical >= graph.resources().size())
        throw std::invalid_argument("External graph image binding targets an unknown resource");
    const RenderGraph::CompiledResource& resource = graph.resources()[id.logical];
    const RenderGraph::ImageDesc& desc = resource.desc.image;
    if (!resource.desc.imported || resource.desc.type != RenderGraph::ResourceType::Image ||
        resource.physicalSlot != RenderGraph::InvalidIndex)
        throw std::invalid_argument("External graph image binding requires an imported image");
    if (image.image == VK_NULL_HANDLE ||
        (image.format != VK_FORMAT_UNDEFINED && image.format != toVkFormat(desc.format)) ||
        ((image.extent.width != 0 || image.extent.height != 0) &&
            (image.extent.width != desc.extent.width ||
             image.extent.height != desc.extent.height)) ||
        image.mipLevels != desc.mipLevels || image.arrayLayers != desc.arrayLayers)
        throw std::invalid_argument("External graph image does not match its declaration");
    (void)getVulkanGraphAccessInfo(current, RenderGraph::ResourceType::Image);
    if (policy.discard && policy.mode != ExternalSyncMode::ExecutorOwned)
        throw std::invalid_argument("Only executor-owned imports can discard on first use");
    const uint8_t scope = global ? 2 : 1;
    if (externalImageScope_[id.logical] != 0 && externalImageScope_[id.logical] != scope)
        throw std::invalid_argument("External graph image is already bound with another scope");
    for (uint32_t row = 0; row < externalImages_.size(); ++row) {
        for (uint32_t logical = 0; logical < externalImages_[row].size(); ++logical) {
            const ExternalImageBinding& other = externalImages_[row][logical];
            if (!other.bound || other.image.image != image.image) continue;
            if (logical != id.logical)
                throw std::invalid_argument("External graph images must not alias resources");
            // Executor-owned state is per binding: an image shared by frame
            // slots needs one global binding, unless both bindings discard it
            // on first use (no state carries over, e.g. the swapchain).
            const auto tracksState = [](const ExternalSyncPolicy& value) {
                return value.mode == ExternalSyncMode::ExecutorOwned && !value.discard;
            };
            if (!global && row != frameOrGlobal &&
                (tracksState(policy) || tracksState(other.policy)))
                throw std::invalid_argument(
                    "Executor-owned external images shared by frame slots need a global binding");
        }
    }
    if (externalImages_.empty()) {
        externalImages_.assign(resources_.frameCount() + 1,
            std::vector<ExternalImageBinding>(logicalResourceCount_));
    }
    ExternalImageBinding& binding = externalImages_[global ? resources_.frameCount()
        : frameOrGlobal][id.logical];
    binding.image = image;
    if (binding.image.aspect == 0)
        binding.image.aspect = aspectForFormat(toVkFormat(desc.format));
    binding.access = current;
    binding.policy = policy;
    binding.bound = true;
    binding.discardPending = policy.discard;
    externalImageScope_[id.logical] = scope;
}

RenderGraph::Access VulkanRenderGraphExecutor::externalImageAccess(
    uint32_t frameOrGlobal, RenderGraph::GraphResourceId id) const {
    (void)boundGraph();
    const uint32_t frame = frameOrGlobal == VulkanGlobalBinding ? 0 : frameOrGlobal;
    const ExternalImageBinding* binding = externalImageBinding(frame, id.logical);
    if (binding == nullptr)
        throw std::out_of_range("External graph image is not bound");
    return binding->access;
}

const RenderGraph::CompiledGraph& VulkanRenderGraphExecutor::executingGraph() const {
    if (executingFrame_ == RenderGraph::InvalidIndex) {
        throw std::logic_error("Render-graph frame execution is not active");
    }
    return boundGraph();
}

void VulkanRenderGraphExecutor::setBarrierSink(VulkanBarrierSink* sink) noexcept {
    sink_ = sink != nullptr ? sink : &vulkanCommandBarrierSink();
}

void VulkanRenderGraphExecutor::queueImageBarrier(const VulkanImageResource& image,
    const VulkanGraphAccessInfo& before, const VulkanGraphAccessInfo& after) {
    // Collapse rule: a second usage of the same image in one pass extends the
    // first barrier to the last "after" state instead of adding a barrier.
    for (uint32_t index = 0; index < imageBatchCount_; ++index) {
        VkImageMemoryBarrier2& existing = imageBatch_[index];
        if (existing.image == image.image) {
            existing.dstStageMask = toVulkanDestinationStages2(after.stages);
            existing.dstAccessMask = after.access;
            existing.newLayout = after.layout;
            return;
        }
    }
    if (imageBatchCount_ >= imageBatch_.size() || batchOrderCount_ >= batchOrder_.size())
        throw std::logic_error("Render-graph barrier batch capacity exceeded");
    VkImageMemoryBarrier2& barrier = imageBatch_[imageBatchCount_];
    barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    barrier.srcStageMask = toVulkanSourceStages2(before.stages);
    barrier.srcAccessMask = before.access;
    barrier.dstStageMask = toVulkanDestinationStages2(after.stages);
    barrier.dstAccessMask = after.access;
    barrier.oldLayout = before.layout;
    barrier.newLayout = after.layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange.aspectMask = image.aspect;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = image.mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = image.arrayLayers;
    batchOrder_[batchOrderCount_++] = imageBatchCount_++ | BatchImageBit;
}

void VulkanRenderGraphExecutor::queueBufferBarrier(VkBuffer buffer, VkDeviceSize size,
    const VulkanGraphAccessInfo& before, const VulkanGraphAccessInfo& after) {
    for (uint32_t index = 0; index < bufferBatchCount_; ++index) {
        VkBufferMemoryBarrier2& existing = bufferBatch_[index];
        if (existing.buffer == buffer) {
            existing.dstStageMask = toVulkanDestinationStages2(after.stages);
            existing.dstAccessMask = after.access;
            return;
        }
    }
    if (bufferBatchCount_ >= bufferBatch_.size() || batchOrderCount_ >= batchOrder_.size())
        throw std::logic_error("Render-graph barrier batch capacity exceeded");
    VkBufferMemoryBarrier2& barrier = bufferBatch_[bufferBatchCount_];
    barrier = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
    barrier.srcStageMask = toVulkanSourceStages2(before.stages);
    barrier.srcAccessMask = before.access;
    barrier.dstStageMask = toVulkanDestinationStages2(after.stages);
    barrier.dstAccessMask = after.access;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.offset = 0;
    barrier.size = size;
    batchOrder_[batchOrderCount_++] = bufferBatchCount_++;
}

void VulkanRenderGraphExecutor::flushBarriers(VkCommandBuffer commandBuffer) {
    if (batchOrderCount_ == 0) return;
    if (barrierApi_ == VulkanBarrierApi::Synchronization2) {
        // One dependency per pass. Every barrier carries its own scopes, so
        // batching barriers on different resources changes no dependency.
        VkDependencyInfo dependency{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.bufferMemoryBarrierCount = bufferBatchCount_;
        dependency.pBufferMemoryBarriers = bufferBatchCount_ != 0 ? bufferBatch_.data() : nullptr;
        dependency.imageMemoryBarrierCount = imageBatchCount_;
        dependency.pImageMemoryBarriers = imageBatchCount_ != 0 ? imageBatch_.data() : nullptr;
        sink_->pipelineBarrier2(commandBuffer, dependency);
    }
    else {
        // Fallback: the pre-R3b.2 recording, one call per barrier in usage order.
        for (uint32_t order = 0; order < batchOrderCount_; ++order) {
            const uint32_t entry = batchOrder_[order];
            if ((entry & BatchImageBit) != 0) {
                const VkImageMemoryBarrier2& source = imageBatch_[entry & ~BatchImageBit];
                VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                barrier.srcAccessMask = static_cast<VkAccessFlags>(source.srcAccessMask);
                barrier.dstAccessMask = static_cast<VkAccessFlags>(source.dstAccessMask);
                barrier.oldLayout = source.oldLayout;
                barrier.newLayout = source.newLayout;
                barrier.srcQueueFamilyIndex = source.srcQueueFamilyIndex;
                barrier.dstQueueFamilyIndex = source.dstQueueFamilyIndex;
                barrier.image = source.image;
                barrier.subresourceRange = source.subresourceRange;
                sink_->pipelineBarrier(commandBuffer, toSourceStages1(source.srcStageMask),
                    static_cast<VkPipelineStageFlags>(source.dstStageMask),
                    {}, std::span(&barrier, 1));
            }
            else {
                const VkBufferMemoryBarrier2& source = bufferBatch_[entry];
                VkBufferMemoryBarrier barrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
                barrier.srcAccessMask = static_cast<VkAccessFlags>(source.srcAccessMask);
                barrier.dstAccessMask = static_cast<VkAccessFlags>(source.dstAccessMask);
                barrier.srcQueueFamilyIndex = source.srcQueueFamilyIndex;
                barrier.dstQueueFamilyIndex = source.dstQueueFamilyIndex;
                barrier.buffer = source.buffer;
                barrier.offset = source.offset;
                barrier.size = source.size;
                sink_->pipelineBarrier(commandBuffer, toSourceStages1(source.srcStageMask),
                    static_cast<VkPipelineStageFlags>(source.dstStageMask),
                    std::span(&barrier, 1), {});
            }
        }
    }
    imageBatchCount_ = bufferBatchCount_ = batchOrderCount_ = 0;
}

void VulkanRenderGraphExecutor::queuePhysicalTransition(uint32_t physicalSlot,
    RenderGraph::Access access, bool attachmentRebarrier) {
    if (executingFrame_ >= frameAccess_.size() ||
        physicalSlot >= frameAccess_[executingFrame_].size()) {
        throw std::out_of_range("Render-graph resource transition is out of range");
    }
    RenderGraph::Access& current = frameAccess_[executingFrame_][physicalSlot];
    // Same-access rule: skip unless the access writes storage, or (R4a) a
    // dynamic-rendering pass writes the same attachment access again.
    const bool rebarrier = attachmentRebarrier && current == access &&
        isAttachmentWriteAccess(access);
    if (current == access && !rebarrier && access != RenderGraph::Access::StorageWrite &&
        access != RenderGraph::Access::StorageReadWrite) {
        return;
    }
    const VulkanGraphPhysicalResource& physical = resources_.resource(
        executingFrame_, physicalSlot);
    const VkImageAspectFlags aspect = physical.type == RenderGraph::ResourceType::Image
        ? physical.image.aspect : 0;
    VulkanGraphAccessInfo before = accessInfoForAspect(current, physical.type, aspect);
    if (rebarrier) before = attachmentWriteSource(before);
    const VulkanGraphAccessInfo after = accessInfoForAspect(access, physical.type, aspect);
    if (physical.type == RenderGraph::ResourceType::Image)
        queueImageBarrier(physical.image, before, after);
    else
        queueBufferBarrier(physical.buffer.buffer, physical.buffer.size, before, after);
    current = access;
}

void VulkanRenderGraphExecutor::queueAliasedFirstUse(
    const RenderGraph::CompiledResource& resource, const RenderGraph::CompiledUsage& usage) {
    // Skip-path guard: an aliased image's memory holds another resource's
    // bytes until its first-use writer discards it, so nothing may use it
    // this frame unless that writer ran (and it must not load).
    if (!usage.write || usage.passOrderIndex != resource.firstUse ||
        usage.loadOp == RenderGraph::LoadOp::Load)
        throw std::logic_error("Render-graph aliased transient '" + resource.name +
            "' is used before its first-use writer ran this frame");
    const uint32_t slot = resource.physicalSlot;
    const VulkanGraphPhysicalResource& physical = resources_.resource(executingFrame_, slot);
    // Source scope: whatever the predecessors in this memory did this frame
    // (their tracked access, so a skipped last reader narrows nothing);
    // none ran => nothing to wait for (the slot's previous frame retired
    // with its fence).
    VulkanGraphAccessInfo before{ VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
        VK_IMAGE_LAYOUT_UNDEFINED };
    bool waits = false;
    for (uint32_t index = aliasPredecessorFirst_[slot];
         index < aliasPredecessorFirst_[slot + 1]; ++index) {
        const uint32_t predecessor = aliasPredecessorSlots_[index];
        const RenderGraph::Access access = frameAccess_[executingFrame_][predecessor];
        if (access == RenderGraph::Access::Undefined) continue;
        const VulkanGraphAccessInfo info = accessInfoForAspect(access,
            RenderGraph::ResourceType::Image,
            resources_.resource(executingFrame_, predecessor).image.aspect);
        if (!waits) before.stages = 0;
        waits = true;
        before.stages |= info.stages;
        before.access |= info.access;
    }
    const VulkanGraphAccessInfo after = accessInfoForAspect(usage.access,
        physical.type, physical.image.aspect);
    queueImageBarrier(physical.image, before, after);
    frameAccess_[executingFrame_][slot] = usage.access;
}

void VulkanRenderGraphExecutor::queueHistoryUsage(
    const RenderGraph::CompiledResource& resource, RenderGraph::Access access,
    bool attachmentRebarrier) {
    const uint32_t slot = historySlot(resource);
    RenderGraph::Access& current = historyAccess_[slot];
    const bool rebarrier = attachmentRebarrier && current == access &&
        isAttachmentWriteAccess(access);
    if (current == access && !rebarrier && access != RenderGraph::Access::StorageWrite &&
        access != RenderGraph::Access::StorageReadWrite) {
        return;
    }
    const VulkanGraphPhysicalResource& physical = historyResources_[slot];
    const VkImageAspectFlags aspect = physical.type == RenderGraph::ResourceType::Image
        ? physical.image.aspect : 0;
    VulkanGraphAccessInfo before = accessInfoForAspect(current, physical.type, aspect);
    if (rebarrier) before = attachmentWriteSource(before);
    const VulkanGraphAccessInfo after = accessInfoForAspect(access, physical.type, aspect);
    const uint32_t pair = resource.historyPair;
    if (physical.type == RenderGraph::ResourceType::Image) {
        // Invalid previous contents are discarded: the layout transition
        // starts from UNDEFINED, while the source scope still covers the
        // slot's last tracked access so in-flight use stays ordered.
        if (resource.historyRole == RenderGraph::HistoryRole::Previous &&
            !historyValidity_.pairValid(pair) && historyDiscarded_[pair] == 0) {
            before.layout = VK_IMAGE_LAYOUT_UNDEFINED;
            historyDiscarded_[pair] = 1;
        }
        queueImageBarrier(physical.image, before, after);
    }
    else {
        queueBufferBarrier(physical.buffer.buffer, physical.buffer.size, before, after);
    }
    current = access;
}

void VulkanRenderGraphExecutor::queueExternalImageUsage(ExternalImageBinding& binding,
    const RenderGraph::CompiledUsage& usage, bool attachmentRebarrier) {
    const VkImageAspectFlags aspect = binding.image.aspect;
    switch (binding.policy.mode) {
    case ExternalSyncMode::ExecutorOwned: {
        // R4a discardOnFirstUse: the frame's first use is a write from
        // UNDEFINED, ordered after the tracked access.
        const bool discard = binding.discardPending;
        if (discard) {
            if (!usage.write)
                throw std::logic_error(
                    "A discard-on-first-use import must be written before it is read");
            binding.discardPending = false;
        }
        const bool rebarrier = attachmentRebarrier && binding.access == usage.access &&
            isAttachmentWriteAccess(usage.access);
        if (!discard && !rebarrier && binding.access == usage.access &&
            usage.access != RenderGraph::Access::StorageWrite &&
            usage.access != RenderGraph::Access::StorageReadWrite) {
            return;
        }
        VulkanGraphAccessInfo before = accessInfoForAspect(binding.access,
            RenderGraph::ResourceType::Image, aspect);
        if (rebarrier) before = attachmentWriteSource(before);
        if (discard) before.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        const VulkanGraphAccessInfo after = accessInfoForAspect(usage.access,
            RenderGraph::ResourceType::Image, aspect);
        queueImageBarrier(binding.image, before, after);
        binding.access = usage.access;
        return;
    }
    case ExternalSyncMode::OwnerManaged:
        return;
    }
}

void VulkanRenderGraphExecutor::beginPassAt(VkCommandBuffer commandBuffer,
    uint32_t passOrder) {
    const RenderGraph::CompiledGraph& graph = *graph_;
    const RenderGraph::CompiledPass& pass = graph.passes()[passOrder];
    // A pass that threw mid-batch must not leak barriers into the next one.
    imageBatchCount_ = bufferBatchCount_ = batchOrderCount_ = 0;
    // A History pair's parity flips when its writer actually begins (a
    // skipped writer leaves it, and the pair goes invalid next frame).
    for (uint32_t index = passHistoryWriteFirst_[passOrder];
         index < passHistoryWriteFirst_[passOrder + 1]; ++index) {
        const uint32_t pair = passHistoryWrites_[index];
        if (historyWriterBegun_[pair] == 0) {
            historyParity_[pair] ^= 1u;
            historyWriterBegun_[pair] = 1;
            historyValidity_.markWritten(pair);
        }
    }
    // R4a: only passes migrated to dynamic rendering re-barrier same-access
    // attachment writes; every other pass keeps the R3b emission.
    const bool migrated = callbacks_[passOrder].dynamicRendering;
    for (uint32_t index = 0; index < pass.usageCount; ++index) {
        const RenderGraph::CompiledUsage& usage =
            graph.usages()[pass.firstUsage + index];
        const RenderGraph::CompiledResource& resource =
            graph.resources()[usage.logicalResourceIndex];
        const bool rebarrier = migrated && usage.write;
        if (resource.physicalSlot != RenderGraph::InvalidIndex) {
            if (aliasedSlot_[resource.physicalSlot] != 0 &&
                frameAccess_[executingFrame_][resource.physicalSlot] ==
                    RenderGraph::Access::Undefined) {
                queueAliasedFirstUse(resource, usage);
                continue;
            }
            queuePhysicalTransition(resource.physicalSlot, usage.access, rebarrier);
            continue;
        }
        if (resource.historyPair != RenderGraph::InvalidIndex) {
            queueHistoryUsage(resource, usage.access, rebarrier);
            continue;
        }
        if (externalBufferTracked_[usage.logicalResourceIndex]) {
            auto& binding = externalBuffers_[executingFrame_][usage.logicalResourceIndex];
            const auto before = getVulkanGraphAccessInfo(binding.access, RenderGraph::ResourceType::Buffer);
            const auto after = getVulkanGraphAccessInfo(usage.access, RenderGraph::ResourceType::Buffer);
            // Same-access writes still need ordering. State survives slot reuse.
            const VkAccessFlags writes = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            if (binding.access != usage.access || (before.access & writes))
                queueBufferBarrier(binding.buffer, resource.desc.buffer.variableSize
                    ? binding.size : resource.desc.buffer.size, before, after);
            binding.access = usage.access;
            continue;
        }
        if (externalImageScope_[usage.logicalResourceIndex] != 0) {
            const uint32_t row = externalImageScope_[usage.logicalResourceIndex] == 2
                ? resources_.frameCount() : executingFrame_;
            ExternalImageBinding& binding = externalImages_[row][usage.logicalResourceIndex];
            if (binding.bound) queueExternalImageUsage(binding, usage, rebarrier);
        }
        // Unbound imports are skipped: their synchronization is not known.
    }
    flushBarriers(commandBuffer);
    ++nextPass_;
}

void VulkanRenderGraphExecutor::requireCursorAt(uint32_t passOrder,
    const char* message) const {
    if (passOrder != nextPass_ || nextPass_ >= graph_->passes().size())
        throw std::logic_error(message);
}

void VulkanRenderGraphExecutor::noteCommandBuffer(VkCommandBuffer commandBuffer) {
    if (!hasRecordContext_) {
        recordContext_ = { commandBuffer, executingFrame_ };
        hasRecordContext_ = true;
    }
}

void VulkanRenderGraphExecutor::runRegisteredPass(uint32_t passOrder) {
    const VulkanPassCallbacks& callbacks = callbacks_[passOrder];
    if (!hasRecordContext_ || recordContext_.commandBuffer == VK_NULL_HANDLE)
        throw std::logic_error(
            "Render-graph callback pass needs a frame record context or command buffer");
    inCallback_ = true;
    try {
        const bool active = callbacks.active == nullptr ||
            callbacks.active(callbacks.owner, recordContext_);
        const uint32_t group = passGroup_[passOrder];
        if (active) {
            if (group != RenderGraph::InvalidIndex && openGroup_ != group &&
                rangeSink_.enabled()) {
                groupToken_ = rangeSink_.begin(rangeSink_.owner, rangeGroups_[group].name);
                openGroup_ = group;
            }
            const bool ranged = callbacks.gpuRange != nullptr && rangeSink_.enabled();
            const bool aroundBarriers =
                callbacks.placement == GpuRangePlacement::AroundBarriers;
            VulkanGpuRangeToken token{};
            if (ranged && (aroundBarriers ||
                    callbacks.placement == GpuRangePlacement::BeforeBarriers))
                token = rangeSink_.begin(rangeSink_.owner, callbacks.gpuRange);
            beginPassAt(recordContext_.commandBuffer, passOrder);
            if (ranged && aroundBarriers)
                rangeSink_.end(rangeSink_.owner, token);
            if (ranged && callbacks.placement == GpuRangePlacement::AfterBarriers)
                token = rangeSink_.begin(rangeSink_.owner, callbacks.gpuRange);
            VulkanPassContext context{ recordContext_, *this,
                RenderGraph::PassId{ passOrder }, recordContext_.commandBuffer };
            callbackPass_ = passOrder;
            callbacks.execute(callbacks.owner, context);
            callbackPass_ = RenderGraph::InvalidIndex;
            if (renderingOpen_)
                throw std::logic_error(
                    "Render-graph pass callback returned with dynamic rendering open");
            if (ranged && !aroundBarriers) rangeSink_.end(rangeSink_.owner, token);
        }
        else {
            ++nextPass_;
        }
        if (group != RenderGraph::InvalidIndex && openGroup_ == group &&
            rangeGroups_[group].lastPass.order == passOrder) {
            rangeSink_.end(rangeSink_.owner, groupToken_);
            openGroup_ = RenderGraph::InvalidIndex;
        }
    }
    catch (...) {
        inCallback_ = false;
        callbackPass_ = RenderGraph::InvalidIndex;
        renderingOpen_ = false;
        throw;
    }
    inCallback_ = false;
}

void VulkanRenderGraphExecutor::drainRegisteredBefore(uint32_t passOrder) {
    while (nextPass_ < passOrder) {
        if (callbacks_[nextPass_].execute == nullptr)
            throw std::logic_error("Render-graph pass order does not match the compiled plan: "
                "an unregistered pass would be skipped");
        runRegisteredPass(nextPass_);
    }
}

void VulkanRenderGraphExecutor::drainRegisteredAtCursor() {
    while (nextPass_ < callbacks_.size() && callbacks_[nextPass_].execute != nullptr)
        runRegisteredPass(nextPass_);
}

void VulkanRenderGraphExecutor::beginPass(VkCommandBuffer commandBuffer,
    RenderGraph::PassId pass) {
    if (commandBuffer == VK_NULL_HANDLE)
        throw std::invalid_argument("Render-graph pass requires a command buffer");
    const RenderGraph::CompiledGraph& graph = executingGraph();
    if (inCallback_)
        throw std::logic_error("Render-graph passes cannot begin inside a pass callback");
    if (pass.order >= graph.passes().size() || pass.order < nextPass_ ||
        callbacks_[pass.order].execute != nullptr) {
        throw std::logic_error("Render-graph pass order does not match the compiled plan");
    }
    noteCommandBuffer(commandBuffer);
    if (registeredCount_ != 0) drainRegisteredBefore(pass.order);
    requireCursorAt(pass.order, "Render-graph pass order does not match the compiled plan");
    beginPassAt(commandBuffer, pass.order);
}

void VulkanRenderGraphExecutor::skipPass(RenderGraph::PassId pass) {
    const RenderGraph::CompiledGraph& graph = executingGraph();
    if (inCallback_)
        throw std::logic_error("Render-graph passes cannot be skipped inside a pass callback");
    if (pass.order >= graph.passes().size() || pass.order < nextPass_ ||
        callbacks_[pass.order].execute != nullptr) {
        throw std::logic_error("Render-graph skipped pass is out of order");
    }
    if (registeredCount_ != 0) drainRegisteredBefore(pass.order);
    requireCursorAt(pass.order, "Render-graph skipped pass is out of order");
    ++nextPass_;
}

void VulkanRenderGraphExecutor::drainRegisteredThrough(RenderGraph::PassId last) {
    const RenderGraph::CompiledGraph& graph = executingGraph();
    if (inCallback_)
        throw std::logic_error("Render-graph passes cannot drain inside a pass callback");
    if (!last.isValid()) return;
    if (last.order >= graph.passes().size() || callbacks_[last.order].execute == nullptr)
        throw std::logic_error("Render-graph drain target is not a registered pass");
    if (last.order < nextPass_)
        throw std::logic_error("Render-graph drain target was already handled");
    drainRegisteredBefore(last.order + 1);
}

void VulkanRenderGraphExecutor::finishFrameExecution() {
    const RenderGraph::CompiledGraph& graph = executingGraph();
    if (inCallback_)
        throw std::logic_error("Render-graph frame cannot finish inside a pass callback");
    if (registeredCount_ != 0) drainRegisteredAtCursor();
    if (nextPass_ != graph.passes().size()) {
        throw std::logic_error("Render-graph frame ended before all passes were handled");
    }
    if ((!hasRecordContext_ || recordContext_.commandBuffer == VK_NULL_HANDLE) &&
        queueFrameEndExports(false) != 0)
        throw std::logic_error(
            "Render-graph frame-end exports need a frame record context or command buffer");
    if (queueFrameEndExports(true) != 0) flushBarriers(recordContext_.commandBuffer);
    executingFrame_ = RenderGraph::InvalidIndex;
    nextPass_ = 0;
    hasRecordContext_ = false;
    recordContext_ = {};
    // Between frames `current` maps to the next frame's write target.
    historyValidity_.endFrame();
    std::fill(historyWriterBegun_.begin(), historyWriterBegun_.end(), uint8_t{ 0 });
}

uint32_t VulkanRenderGraphExecutor::queueFrameEndExports(bool record) {
    // The compiled transitions at passOrderIndex == passCount, applied to the
    // tracked state (skipped passes or bound states may differ from the
    // compile-time walk). Only state changes move: an export never
    // re-barriers in place. Without `record` this only counts them.
    imageBatchCount_ = bufferBatchCount_ = batchOrderCount_ = 0;
    const RenderGraph::CompiledGraph& graph = *graph_;
    uint32_t pending = 0;
    for (const uint32_t logical : exportedResources_) {
        const RenderGraph::CompiledResource& resource = graph.resources()[logical];
        const RenderGraph::Access final = resource.finalAccess;
        if (resource.physicalSlot != RenderGraph::InvalidIndex) {
            if (frameAccess_[executingFrame_][resource.physicalSlot] == final) continue;
            ++pending;
            if (record) queuePhysicalTransition(resource.physicalSlot, final);
            continue;
        }
        if (resource.historyPair != RenderGraph::InvalidIndex) {
            if (historyAccess_[historySlot(resource)] == final) continue;
            ++pending;
            if (record) queueHistoryUsage(resource, final);
            continue;
        }
        if (externalBufferTracked_[logical]) {
            ExternalBufferBinding& binding = externalBuffers_[executingFrame_][logical];
            if (binding.buffer == VK_NULL_HANDLE || binding.access == final) continue;
            ++pending;
            if (!record) continue;
            queueBufferBarrier(binding.buffer, resource.desc.buffer.variableSize
                    ? binding.size : resource.desc.buffer.size,
                getVulkanGraphAccessInfo(binding.access, RenderGraph::ResourceType::Buffer),
                getVulkanGraphAccessInfo(final, RenderGraph::ResourceType::Buffer));
            binding.access = final;
            continue;
        }
        if (externalImageScope_[logical] != 0) {
            const uint32_t row = externalImageScope_[logical] == 2
                ? resources_.frameCount() : executingFrame_;
            ExternalImageBinding& binding = externalImages_[row][logical];
            if (!binding.bound || binding.policy.mode != ExternalSyncMode::ExecutorOwned ||
                binding.access == final)
                continue;
            ++pending;
            if (!record) continue;
            queueImageBarrier(binding.image,
                accessInfoForAspect(binding.access, RenderGraph::ResourceType::Image,
                    binding.image.aspect),
                accessInfoForAspect(final, RenderGraph::ResourceType::Image,
                    binding.image.aspect));
            binding.access = final;
        }
    }
    return pending;
}

const VulkanPassRenderingPlan& VulkanRenderGraphExecutor::renderingPlan(
    RenderGraph::PassId pass) const {
    (void)boundGraph();
    if (pass.order >= renderingPlans_.size())
        throw std::out_of_range("Render-graph rendering plan was not found");
    return renderingPlans_[pass.order];
}

void VulkanRenderGraphExecutor::beginPassRendering(RenderGraph::PassId pass,
    VkCommandBuffer commandBuffer, const VulkanRenderingOverrides& overrides) {
    if (!inCallback_ || pass.order != callbackPass_)
        throw std::logic_error("Dynamic rendering begins only inside its pass callback");
    if (!callbacks_[pass.order].dynamicRendering)
        throw std::logic_error("Render-graph pass is not registered for dynamic rendering");
    if (renderingOpen_)
        throw std::logic_error("Dynamic rendering scopes cannot nest");
    const VulkanPassRenderingPlan& plan = renderingPlans_[pass.order];
    if (!plan.valid || plan.empty())
        throw std::logic_error("Render-graph pass has no valid rendering plan");
    if (overrides.layerCount == 0)
        throw std::invalid_argument("Dynamic rendering needs at least one layer");
    if (plan.depthCount != 0 && overrides.depthIndex >= plan.depthCount)
        throw std::invalid_argument("Dynamic rendering depth candidate is out of range");

    const auto attachment = [&](const VulkanRenderingAttachmentPlan& planned,
        VkImageView view) {
        VkRenderingAttachmentInfo info{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        info.imageView = view != VK_NULL_HANDLE ? view
            : image(executingFrame_, RenderGraph::GraphResourceId{
                planned.logicalResourceIndex }).view;
        info.imageLayout = planned.layout;
        info.resolveMode = VK_RESOLVE_MODE_NONE;
        info.loadOp = overrides.loadInsteadOfClear &&
            planned.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR
            ? VK_ATTACHMENT_LOAD_OP_LOAD : planned.loadOp;
        info.storeOp = planned.storeOp;
        info.clearValue = planned.clearValue;
        return info;
    };
    std::array<VkRenderingAttachmentInfo, VulkanMaxColorAttachments> colors{};
    for (uint32_t index = 0; index < plan.colorCount; ++index)
        colors[index] = attachment(plan.color[index], overrides.colorViews[index]);
    VkRenderingAttachmentInfo depth{};
    if (plan.depthCount != 0)
        depth = attachment(plan.depth[overrides.depthIndex], overrides.depthView);

    VkRenderingInfo rendering{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    rendering.renderArea = overrides.renderArea.extent.width == 0 &&
        overrides.renderArea.extent.height == 0
        ? VkRect2D{ { 0, 0 }, plan.renderExtent(overrides.depthIndex) }
        : overrides.renderArea;
    rendering.layerCount = overrides.layerCount;
    rendering.colorAttachmentCount = plan.colorCount;
    rendering.pColorAttachments = plan.colorCount != 0 ? colors.data() : nullptr;
    rendering.pDepthAttachment = plan.depthCount != 0 ? &depth : nullptr;
    sink_->beginRendering(commandBuffer, rendering);
    renderingOpen_ = true;
}

void VulkanRenderGraphExecutor::endPassRendering(RenderGraph::PassId pass,
    VkCommandBuffer commandBuffer) {
    if (!inCallback_ || pass.order != callbackPass_ || !renderingOpen_)
        throw std::logic_error("Dynamic rendering ends only the scope its pass callback began");
    sink_->endRendering(commandBuffer);
    renderingOpen_ = false;
}

void VulkanPassContext::beginRendering(const VulkanRenderingOverrides& overrides) {
    graph.beginPassRendering(pass, commandBuffer, overrides);
}

void VulkanPassContext::endRendering() {
    graph.endPassRendering(pass, commandBuffer);
}

const VulkanPassRenderingPlan& VulkanPassContext::renderingPlan() const {
    return graph.renderingPlan(pass);
}

void VulkanRenderGraphExecutor::setFrameRecordContext(
    const VulkanFrameRecordContext& context) {
    (void)executingGraph();
    if (context.commandBuffer == VK_NULL_HANDLE)
        throw std::invalid_argument("Render-graph record context requires a command buffer");
    recordContext_ = context;
    recordContext_.frameIndex = executingFrame_;
    hasRecordContext_ = true;
}

void VulkanRenderGraphExecutor::registerPass(RenderGraph::PassId pass,
    const VulkanPassCallbacks& callbacks) {
    const RenderGraph::CompiledGraph& graph = boundGraph();
    if (executingFrame_ != RenderGraph::InvalidIndex)
        throw std::logic_error("Render-graph passes cannot be registered during frame execution");
    if (pass.order >= graph.passes().size() || callbacks.execute == nullptr)
        throw std::invalid_argument("Render-graph pass registration is invalid");
    if (callbacks_[pass.order].execute != nullptr)
        throw std::logic_error("Render-graph pass is already registered");
    callbacks_[pass.order] = callbacks;
    ++registeredCount_;
}

void VulkanRenderGraphExecutor::unregisterPass(RenderGraph::PassId pass) {
    (void)boundGraph();
    if (executingFrame_ != RenderGraph::InvalidIndex)
        throw std::logic_error("Render-graph passes cannot be unregistered during frame execution");
    if (pass.order >= callbacks_.size() || callbacks_[pass.order].execute == nullptr)
        return;
    const uint32_t group = passGroup_[pass.order];
    if (group != RenderGraph::InvalidIndex) {
        const VulkanRangeGroup& value = rangeGroups_[group];
        for (uint32_t order = value.firstPass.order; order <= value.lastPass.order; ++order)
            passGroup_[order] = RenderGraph::InvalidIndex;
        rangeGroups_[group] = {};
    }
    callbacks_[pass.order] = {};
    --registeredCount_;
}

bool VulkanRenderGraphExecutor::isRegistered(RenderGraph::PassId pass) const noexcept {
    return pass.order < callbacks_.size() && callbacks_[pass.order].execute != nullptr;
}

void VulkanRenderGraphExecutor::registerRangeGroup(const VulkanRangeGroup& group) {
    (void)boundGraph();
    if (executingFrame_ != RenderGraph::InvalidIndex)
        throw std::logic_error("Render-graph range groups cannot change during frame execution");
    if (group.name == nullptr || group.firstPass.order > group.lastPass.order ||
        group.lastPass.order >= callbacks_.size())
        throw std::invalid_argument("Render-graph range group is invalid");
    for (uint32_t order = group.firstPass.order; order <= group.lastPass.order; ++order) {
        if (callbacks_[order].execute == nullptr)
            throw std::logic_error("Render-graph range groups span registered passes only");
        if (passGroup_[order] != RenderGraph::InvalidIndex)
            throw std::logic_error("Render-graph range groups must not overlap");
    }
    uint32_t index = 0;
    while (index < rangeGroups_.size() && rangeGroups_[index].name != nullptr) ++index;
    if (index == rangeGroups_.size()) {
        if (rangeGroups_.size() == rangeGroups_.capacity())
            throw std::logic_error("Render-graph range group capacity exceeded");
        rangeGroups_.push_back(group);
    }
    else {
        rangeGroups_[index] = group;
    }
    for (uint32_t order = group.firstPass.order; order <= group.lastPass.order; ++order)
        passGroup_[order] = index;
}

VulkanGpuRangeSink VulkanGpuRangeSink::forScheduler(
    VulkanFrameScheduler& scheduler) noexcept {
    return { &scheduler,
        [](void* owner, const char* name) {
            return static_cast<VulkanFrameScheduler*>(owner)->beginGpuRange(name);
        },
        [](void* owner, VulkanGpuRangeToken& token) {
            static_cast<VulkanFrameScheduler*>(owner)->endGpuRange(token);
        } };
}

void VulkanRenderGraphExecutor::destroyHistoryResources() noexcept {
    if (factory_ != nullptr) {
        for (VulkanGraphPhysicalResource& resource : historyResources_)
            factory_->destroy(resource);
        for (RetiredHistory& retired : retiredHistory_)
            factory_->destroy(retired.resource);
    }
    historyResources_.clear();
    retiredHistory_.clear();
}

VulkanRenderGraphExecutor::~VulkanRenderGraphExecutor() {
    destroyHistoryResources();
}

void VulkanRenderGraphExecutor::cleanupAfterDeviceIdle() noexcept {
    destroyHistoryResources();
    resources_.cleanupAfterDeviceIdle();
    cache_.clear();
    graph_ = nullptr;
    passNames_.clear();
    resourceNames_.clear();
    imageBatch_.clear();
    bufferBatch_.clear();
    batchOrder_.clear();
    imageBatchCount_ = bufferBatchCount_ = batchOrderCount_ = 0;
    callbacks_.clear();
    renderingPlans_.clear();
    exportedResources_.clear();
    callbackPass_ = RenderGraph::InvalidIndex;
    renderingOpen_ = false;
    passGroup_.clear();
    rangeGroups_.clear();
    registeredCount_ = 0;
    hasRecordContext_ = false;
    recordContext_ = {};
    inCallback_ = false;
    openGroup_ = RenderGraph::InvalidIndex;
    historyAccess_.clear();
    historyParity_.clear();
    historyWriterBegun_.clear();
    historyDiscarded_.clear();
    passHistoryWrites_.clear();
    passHistoryWriteFirst_.clear();
    historyValidity_ = {};
    lastView_ = {};
    externalImages_.clear();
    externalImageScope_.clear();
    aliasedSlot_.clear();
    aliasedSlots_.clear();
    aliasPredecessorFirst_.clear();
    aliasPredecessorSlots_.clear();
    aliasHeapCount_ = 0;
    aliasedRequestedBytes_ = 0;
    aliasPeakLiveBytes_ = 0;
    barriers_.clear();
    frameAccess_.clear();
    externalBuffers_.clear(); externalBufferTracked_.clear(); frameRetired_.clear();
    executingFrame_ = RenderGraph::InvalidIndex;
    nextPass_ = 0;
    factory_ = nullptr;
    allocatorFactory_.reset();
    topologyHash_ = 0;
    passCount_ = 0;
    logicalResourceCount_ = 0;
    physicalSlotCount_ = 0;
}

VulkanGraphStats VulkanRenderGraphExecutor::stats() const noexcept {
    uint64_t historyRequested = 0;
    uint64_t historyCommitted = 0;
    for (const VulkanGraphPhysicalResource& resource : historyResources_) {
        historyRequested += resourceRequestedBytes(resource);
        historyCommitted += resourceCommittedBytes(resource);
    }
    for (const RetiredHistory& retired : retiredHistory_) {
        historyRequested += resourceRequestedBytes(retired.resource);
        historyCommitted += resourceCommittedBytes(retired.resource);
    }
    return {
        .enabled = topologyHash_ != 0,
        .topologyHash = topologyHash_,
        .passCount = passCount_,
        .logicalResourceCount = logicalResourceCount_,
        .physicalSlotCount = physicalSlotCount_,
        .barrierCount = static_cast<uint32_t>(barriers_.size()),
        .frameCount = resources_.frameCount(),
        .requestedBytes = resources_.requestedBytes() + historyRequested,
        .committedBytes = resources_.committedBytes() + historyCommitted,
        .rebuildCount = rebuildCount_,
        .cacheMissCount = cacheMissCount_,
        .historySlotCount = static_cast<uint32_t>(historyResources_.size()),
        .transientAliasing = !aliasedSlots_.empty(),
        .aliasHeapCount = aliasHeapCount_,
        .aliasedResourceCount = static_cast<uint32_t>(aliasedSlots_.size()),
        .aliasedRequestedBytes = aliasedRequestedBytes_ * resources_.frameCount(),
        .aliasHeapCommittedBytes = resources_.aliasHeapCommittedBytes(),
        .aliasPeakLiveBytes = aliasPeakLiveBytes_,
    };
}

std::span<const VulkanAliasHeapResource> VulkanRenderGraphExecutor::aliasHeaps(
    uint32_t frameIndex) const {
    if (frameIndex >= resources_.frameCount())
        throw std::out_of_range("Render-graph frame index is out of range");
    return resources_.aliasHeaps(frameIndex);
}

const VulkanImageResource& VulkanRenderGraphExecutor::image(uint32_t frameIndex,
    RenderGraph::GraphResourceId id) const {
    const RenderGraph::CompiledGraph& graph = boundGraph();
    if (id.logical >= graph.resources().size())
        throw std::out_of_range("Render-graph image resource was not found");
    const RenderGraph::CompiledResource& found = graph.resources()[id.logical];
    if (found.desc.type != RenderGraph::ResourceType::Image)
        throw std::out_of_range("Render-graph image resource was not found");
    if (found.historyPair != RenderGraph::InvalidIndex) {
        // Global: the same image for every frame slot, by this frame's parity.
        return historyResources_[historySlot(found)].image;
    }
    if (found.physicalSlot == RenderGraph::InvalidIndex) {
        if (frameIndex >= resources_.frameCount())
            throw std::out_of_range("Render-graph frame index is out of range");
        if (const ExternalImageBinding* binding = externalImageBinding(frameIndex, id.logical))
            return binding->image;
        throw std::out_of_range("Render-graph image resource was not found");
    }
    const VulkanGraphPhysicalResource& physical = resources_.resource(
        frameIndex, found.physicalSlot);
    if (physical.type != RenderGraph::ResourceType::Image || !physical.image.isValid()) {
        throw std::logic_error("Render-graph physical image is invalid");
    }
    return physical.image;
}

const VulkanBufferResource& VulkanRenderGraphExecutor::buffer(uint32_t frameIndex,
    RenderGraph::GraphResourceId id) const {
    const RenderGraph::CompiledGraph& graph = boundGraph();
    if (id.logical >= graph.resources().size())
        throw std::out_of_range("Render-graph buffer resource was not found");
    const RenderGraph::CompiledResource& found = graph.resources()[id.logical];
    if (found.desc.type != RenderGraph::ResourceType::Buffer)
        throw std::out_of_range("Render-graph buffer resource was not found");
    if (found.historyPair != RenderGraph::InvalidIndex)
        return historyResources_[historySlot(found)].buffer;
    if (found.physicalSlot == RenderGraph::InvalidIndex)
        throw std::out_of_range("Render-graph buffer resource was not found");
    const VulkanGraphPhysicalResource& physical = resources_.resource(
        frameIndex, found.physicalSlot);
    if (physical.type != RenderGraph::ResourceType::Buffer ||
        !physical.buffer.isValid()) {
        throw std::logic_error("Render-graph physical buffer is invalid");
    }
    return physical.buffer;
}

} // namespace Iridium
