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

} // namespace

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

VulkanGraphPhysicalResource VulkanAllocatorGraphResourceFactory::create(
    const RenderGraph::PhysicalResourceSlot& slot) {
    if (allocator_ == nullptr || slot.logicalResources.empty()) {
        throw std::logic_error("Invalid Vulkan graph resource allocation request");
    }
    VulkanGraphPhysicalResource resource{};
    resource.type = slot.type;
    if (slot.type == RenderGraph::ResourceType::Image) {
        if (slot.image.extent.depth != 1 || slot.image.mipLevels == 0 ||
            slot.image.arrayLayers == 0 || slot.image.samples != 1) {
            throw std::invalid_argument(
                "Vulkan graph images require 2D, nonempty mip/layer ranges, and one sample");
        }
        const VkFormat format = toVkFormat(slot.image.format);
        const VkImageUsageFlags usage = imageUsage(slot.usages);
        if (usage == 0) {
            throw std::invalid_argument("Render-graph image has no Vulkan usage");
        }
        resource.image = allocator_->createImage2D(
            { slot.image.extent.width, slot.image.extent.height }, format, usage,
            aspectForFormat(format), category_, slot.image.mipLevels,
            slot.image.arrayLayers, 0,
            slot.image.arrayLayers == 1 ? VK_IMAGE_VIEW_TYPE_2D :
                VK_IMAGE_VIEW_TYPE_2D_ARRAY);
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

void VulkanGraphResourcePool::rebuild(const RenderGraph::CompiledGraph& graph) {
    if (factory_ == nullptr || active_.empty()) {
        throw std::logic_error("Vulkan graph resource pool is not initialized");
    }

    std::vector<std::vector<VulkanGraphPhysicalResource>> candidate(active_.size());
    try {
        for (auto& frame : candidate) {
            frame.reserve(graph.physicalSlots().size());
            for (const RenderGraph::PhysicalResourceSlot& slot :
                graph.physicalSlots()) {
                frame.push_back(factory_->create(slot));
            }
        }
        for (size_t frameIndex = 0; frameIndex < active_.size(); ++frameIndex) {
            retired_[frameIndex].reserve(retired_[frameIndex].size() +
                active_[frameIndex].size());
        }
    }
    catch (...) {
        for (auto& frame : candidate) {
            destroyResources(frame);
        }
        throw;
    }

    for (size_t frameIndex = 0; frameIndex < active_.size(); ++frameIndex) {
        auto& retired = retired_[frameIndex];
        auto& active = active_[frameIndex];
        std::move(active.begin(), active.end(), std::back_inserter(retired));
        active.clear();
    }
    active_ = std::move(candidate);
}

void VulkanGraphResourcePool::onFrameFenceCompleted(uint32_t frameIndex) {
    if (frameIndex >= retired_.size()) {
        throw std::out_of_range("Render-graph frame index is out of range");
    }
    destroyResources(retired_[frameIndex]);
}

void VulkanGraphResourcePool::cleanupAfterDeviceIdle() noexcept {
    if (factory_ != nullptr) {
        for (auto& frame : active_) {
            destroyResources(frame);
        }
        for (auto& frame : retired_) {
            destroyResources(frame);
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
    return active_[frameIndex].size();
}

size_t VulkanGraphResourcePool::retiredResourceCount(uint32_t frameIndex) const {
    if (frameIndex >= retired_.size()) {
        throw std::out_of_range("Render-graph frame index is out of range");
    }
    return retired_[frameIndex].size();
}

uint64_t VulkanGraphResourcePool::requestedBytes() const noexcept {
    uint64_t result = 0;
    for (const auto& frames : { &active_, &retired_ }) {
        for (const auto& frame : *frames) {
            for (const VulkanGraphPhysicalResource& resource : frame) {
                result += resourceRequestedBytes(resource);
            }
        }
    }
    return result;
}

uint64_t VulkanGraphResourcePool::committedBytes() const noexcept {
    uint64_t result = 0;
    for (const auto& frames : { &active_, &retired_ }) {
        for (const auto& frame : *frames) {
            for (const VulkanGraphPhysicalResource& resource : frame) {
                result += resourceCommittedBytes(resource);
            }
        }
    }
    return result;
}

const VulkanGraphPhysicalResource& VulkanGraphResourcePool::resource(
    uint32_t frameIndex, uint32_t physicalSlot) const {
    if (frameIndex >= active_.size() || physicalSlot >= active_[frameIndex].size()) {
        throw std::out_of_range("Render-graph physical resource is out of range");
    }
    return active_[frameIndex][physicalSlot];
}

void VulkanGraphResourcePool::destroyResources(
    std::vector<VulkanGraphPhysicalResource>& resources) noexcept {
    for (VulkanGraphPhysicalResource& resource : resources) {
        factory_->destroy(resource);
    }
    resources.clear();
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
}

void VulkanRenderGraphExecutor::init(VulkanGraphResourceFactory& factory,
    uint32_t frameCount) {
    if (allocatorFactory_ || resources_.frameCount() != 0 || frameCount == 0) {
        throw std::logic_error("Vulkan graph executor initialized incorrectly");
    }
    resources_.init(factory, frameCount);
}

void VulkanRenderGraphExecutor::rebuild(RenderGraph::CompiledGraph graph) {
    if (resources_.frameCount() == 0) {
        throw std::logic_error("Vulkan graph executor is not initialized");
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

    resources_.rebuild(graph);
    const uint64_t hash = graph.topologyHash();
    passCount_ = static_cast<uint32_t>(graph.passes().size());
    logicalResourceCount_ = static_cast<uint32_t>(graph.resources().size());
    physicalSlotCount_ = static_cast<uint32_t>(graph.physicalSlots().size());
    frameAccess_.assign(resources_.frameCount(),
        std::vector<RenderGraph::Access>(physicalSlotCount_,
            RenderGraph::Access::Undefined));
    cache_.store(std::move(graph));
    barriers_ = std::move(candidateBarriers);
    topologyHash_ = hash;
    ++rebuildCount_;
}

void VulkanRenderGraphExecutor::onFrameFenceCompleted(uint32_t frameIndex) {
    resources_.onFrameFenceCompleted(frameIndex);
}

bool VulkanRenderGraphExecutor::validateFrame(uint32_t frameIndex) noexcept {
    const RenderGraph::CompiledGraph* graph = cache_.find(topologyHash_);
    if (graph == nullptr) {
        ++cacheMissCount_;
        return false;
    }
    try {
        return frameIndex < resources_.frameCount() &&
            resources_.activeResourceCount(frameIndex) == graph->physicalSlots().size();
    }
    catch (...) {
        return false;
    }
}

void VulkanRenderGraphExecutor::beginFrameExecution(uint32_t frameIndex) {
    if (!validateFrame(frameIndex) || executingFrame_ != RenderGraph::InvalidIndex) {
        throw std::logic_error("Render-graph frame execution began in an invalid state");
    }
    executingFrame_ = frameIndex;
    nextPass_ = 0;
}

const RenderGraph::CompiledGraph& VulkanRenderGraphExecutor::executingGraph() const {
    if (executingFrame_ == RenderGraph::InvalidIndex) {
        throw std::logic_error("Render-graph frame execution is not active");
    }
    const RenderGraph::CompiledGraph* graph = cache_.find(topologyHash_);
    if (graph == nullptr) {
        throw std::logic_error("Render-graph compiled plan is unavailable");
    }
    return *graph;
}

void VulkanRenderGraphExecutor::transitionPhysicalResource(
    VkCommandBuffer commandBuffer, uint32_t physicalSlot,
    RenderGraph::Access access) {
    if (commandBuffer == VK_NULL_HANDLE || executingFrame_ >= frameAccess_.size() ||
        physicalSlot >= frameAccess_[executingFrame_].size()) {
        throw std::out_of_range("Render-graph resource transition is out of range");
    }
    RenderGraph::Access& current = frameAccess_[executingFrame_][physicalSlot];
    if (current == access && access != RenderGraph::Access::StorageWrite &&
        access != RenderGraph::Access::StorageReadWrite) {
        return;
    }
    const VulkanGraphPhysicalResource& physical = resources_.resource(
        executingFrame_, physicalSlot);
    const VulkanGraphAccessInfo before = accessInfoForAspect(
        current, physical.type,
        physical.type == RenderGraph::ResourceType::Image
            ? physical.image.aspect : 0);
    const VulkanGraphAccessInfo after = accessInfoForAspect(
        access, physical.type,
        physical.type == RenderGraph::ResourceType::Image
            ? physical.image.aspect : 0);
    if (physical.type == RenderGraph::ResourceType::Image) {
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = before.access;
        barrier.dstAccessMask = after.access;
        barrier.oldLayout = before.layout;
        barrier.newLayout = after.layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = physical.image.image;
        barrier.subresourceRange.aspectMask = physical.image.aspect;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = physical.image.mipLevels;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = physical.image.arrayLayers;
        vkCmdPipelineBarrier(commandBuffer, before.stages, after.stages, 0,
            0, nullptr, 0, nullptr, 1, &barrier);
    }
    else {
        VkBufferMemoryBarrier barrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        barrier.srcAccessMask = before.access;
        barrier.dstAccessMask = after.access;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = physical.buffer.buffer;
        barrier.offset = 0;
        barrier.size = physical.buffer.size;
        vkCmdPipelineBarrier(commandBuffer, before.stages, after.stages, 0,
            0, nullptr, 1, &barrier, 0, nullptr);
    }
    current = access;
}

void VulkanRenderGraphExecutor::beginPass(VkCommandBuffer commandBuffer,
    std::string_view passName) {
    const RenderGraph::CompiledGraph& graph = executingGraph();
    if (nextPass_ >= graph.passes().size() ||
        graph.passes()[nextPass_].name != passName) {
        throw std::logic_error("Render-graph pass order does not match the compiled plan");
    }
    const RenderGraph::CompiledPass& pass = graph.passes()[nextPass_];
    for (uint32_t index = 0; index < pass.usageCount; ++index) {
        const RenderGraph::CompiledUsage& usage =
            graph.usages()[pass.firstUsage + index];
        const RenderGraph::CompiledResource& resource =
            graph.resources()[usage.logicalResourceIndex];
        if (resource.physicalSlot == RenderGraph::InvalidIndex) {
            continue;
        }
        transitionPhysicalResource(commandBuffer, resource.physicalSlot,
            usage.access);
    }
    ++nextPass_;
}

void VulkanRenderGraphExecutor::skipPass(std::string_view passName) {
    const RenderGraph::CompiledGraph& graph = executingGraph();
    if (nextPass_ >= graph.passes().size() ||
        graph.passes()[nextPass_].name != passName) {
        throw std::logic_error("Render-graph skipped pass is out of order");
    }
    ++nextPass_;
}

void VulkanRenderGraphExecutor::finishFrameExecution() {
    const RenderGraph::CompiledGraph& graph = executingGraph();
    if (nextPass_ != graph.passes().size()) {
        throw std::logic_error("Render-graph frame ended before all passes were handled");
    }
    executingFrame_ = RenderGraph::InvalidIndex;
    nextPass_ = 0;
}

void VulkanRenderGraphExecutor::transitionImage(VkCommandBuffer commandBuffer,
    std::string_view logicalName, RenderGraph::Access access) {
    const RenderGraph::CompiledGraph& graph = executingGraph();
    const auto found = std::find_if(graph.resources().begin(), graph.resources().end(),
        [logicalName](const RenderGraph::CompiledResource& resource) {
            return resource.name == logicalName;
        });
    if (found == graph.resources().end() ||
        found->physicalSlot == RenderGraph::InvalidIndex) {
        throw std::out_of_range("Render-graph transition resource was not found");
    }
    if (found->desc.type != RenderGraph::ResourceType::Image) {
        throw std::logic_error("Named image transition expected an image resource");
    }
    transitionPhysicalResource(commandBuffer, found->physicalSlot, access);
}

void VulkanRenderGraphExecutor::cleanupAfterDeviceIdle() noexcept {
    resources_.cleanupAfterDeviceIdle();
    cache_.clear();
    barriers_.clear();
    frameAccess_.clear();
    executingFrame_ = RenderGraph::InvalidIndex;
    nextPass_ = 0;
    allocatorFactory_.reset();
    topologyHash_ = 0;
    passCount_ = 0;
    logicalResourceCount_ = 0;
    physicalSlotCount_ = 0;
}

VulkanGraphStats VulkanRenderGraphExecutor::stats() const noexcept {
    return {
        .enabled = topologyHash_ != 0,
        .topologyHash = topologyHash_,
        .passCount = passCount_,
        .logicalResourceCount = logicalResourceCount_,
        .physicalSlotCount = physicalSlotCount_,
        .barrierCount = static_cast<uint32_t>(barriers_.size()),
        .frameCount = resources_.frameCount(),
        .requestedBytes = resources_.requestedBytes(),
        .committedBytes = resources_.committedBytes(),
        .rebuildCount = rebuildCount_,
        .cacheMissCount = cacheMissCount_,
    };
}

const VulkanImageResource& VulkanRenderGraphExecutor::imageResource(
    uint32_t frameIndex, std::string_view logicalName) const {
    const RenderGraph::CompiledGraph* graph = cache_.find(topologyHash_);
    if (graph == nullptr) {
        throw std::logic_error("Render-graph compiled plan is unavailable");
    }
    const auto found = std::find_if(graph->resources().begin(),
        graph->resources().end(), [logicalName](const RenderGraph::CompiledResource& resource) {
            return resource.name == logicalName;
        });
    if (found == graph->resources().end() ||
        found->desc.type != RenderGraph::ResourceType::Image ||
        found->physicalSlot == RenderGraph::InvalidIndex) {
        throw std::out_of_range("Render-graph image resource was not found");
    }
    const VulkanGraphPhysicalResource& physical = resources_.resource(
        frameIndex, found->physicalSlot);
    if (physical.type != RenderGraph::ResourceType::Image || !physical.image.isValid()) {
        throw std::logic_error("Render-graph physical image is invalid");
    }
    return physical.image;
}

const VulkanBufferResource& VulkanRenderGraphExecutor::bufferResource(
    uint32_t frameIndex, std::string_view logicalName) const {
    const RenderGraph::CompiledGraph* graph = cache_.find(topologyHash_);
    if (graph == nullptr) {
        throw std::logic_error("Render-graph compiled plan is unavailable");
    }
    const auto found = std::find_if(graph->resources().begin(),
        graph->resources().end(), [logicalName](const RenderGraph::CompiledResource& resource) {
            return resource.name == logicalName;
        });
    if (found == graph->resources().end() ||
        found->desc.type != RenderGraph::ResourceType::Buffer ||
        found->physicalSlot == RenderGraph::InvalidIndex) {
        throw std::out_of_range("Render-graph buffer resource was not found");
    }
    const VulkanGraphPhysicalResource& physical = resources_.resource(
        frameIndex, found->physicalSlot);
    if (physical.type != RenderGraph::ResourceType::Buffer ||
        !physical.buffer.isValid()) {
        throw std::logic_error("Render-graph physical buffer is invalid");
    }
    return physical.buffer;
}

} // namespace Iridium
