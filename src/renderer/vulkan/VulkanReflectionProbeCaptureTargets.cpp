#include "renderer/vulkan/VulkanReflectionProbeCaptureTargets.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace Iridium {
namespace {

    void requireSuccess(VkResult result, const char* operation) {
        if (result != VK_SUCCESS)
            throw std::runtime_error(std::string(operation) + " failed");
    }

} // namespace

void VulkanReflectionProbeCaptureTargets::validateConfig(
    const VulkanReflectionProbeCaptureTargetConfig& config) {
    if (config.maximumOwners == 0 || config.maximumCapturesInFlight == 0 ||
        config.maximumCapturesInFlight > config.maximumOwners ||
        config.maximumStagingBytes == 0 || config.maximumPublishedBytes == 0)
        throw std::invalid_argument(
            "Reflection-probe capture target policy is invalid");
}

void VulkanReflectionProbeCaptureTargets::init(VkDevice device,
    VkPhysicalDevice physicalDevice, VulkanResourceAllocator& allocator,
    VkRenderPass captureRenderPass,
    VulkanReflectionProbeCaptureTargetConfig config) {
    if (device_ != VK_NULL_HANDLE || allocator_ != nullptr)
        throw std::logic_error(
            "Reflection-probe capture targets were initialized twice");
    if (device == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE ||
        captureRenderPass == VK_NULL_HANDLE)
        throw std::invalid_argument(
            "Reflection-probe capture targets require a Vulkan device");
    validateConfig(config);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);
    if (properties.limits.maxImageDimensionCube < 128)
        throw std::runtime_error(
            "Vulkan cube-image limit cannot hold a reflection probe");
    device_ = device;
    allocator_ = &allocator;
    captureRenderPass_ = captureRenderPass;
    config_ = config;
    maximumCubeDimension_ = properties.limits.maxImageDimensionCube;
    owners_.reserve(config.maximumOwners);
}

VkImageView VulkanReflectionProbeCaptureTargets::createView(VkImage image,
    VkFormat format, VkImageAspectFlags aspect, VkImageViewType type,
    uint32_t baseMip, uint32_t mipCount, uint32_t baseLayer,
    uint32_t layerCount) const {
    VkImageViewCreateInfo create{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    create.image = image;
    create.viewType = type;
    create.format = format;
    create.subresourceRange = { aspect, baseMip, mipCount,
        baseLayer, layerCount };
    VkImageView result = VK_NULL_HANDLE;
    requireSuccess(vkCreateImageView(device_, &create, nullptr, &result),
        "vkCreateImageView(reflection probe capture)");
    return result;
}

VulkanReflectionProbeCaptureTargets::OwnerState*
VulkanReflectionProbeCaptureTargets::find(SceneEntityUuid owner) noexcept {
    const auto found = std::ranges::find(owners_, owner, &OwnerState::owner);
    return found == owners_.end() ? nullptr : &*found;
}

const VulkanReflectionProbeCaptureTargets::OwnerState*
VulkanReflectionProbeCaptureTargets::find(
    SceneEntityUuid owner) const noexcept {
    const auto found = std::ranges::find(owners_, owner, &OwnerState::owner);
    return found == owners_.end() ? nullptr : &*found;
}

const VulkanReflectionProbeCaptureStaging&
VulkanReflectionProbeCaptureTargets::acquire(SceneEntityUuid owner,
    uint64_t captureTicket, uint32_t resolution) {
    if (device_ == VK_NULL_HANDLE || allocator_ == nullptr)
        throw std::logic_error(
            "Reflection-probe capture targets are not initialized");
    if (owner.isNil() || captureTicket == 0)
        throw std::invalid_argument(
            "Reflection-probe capture target identity is invalid");
    if (resolution > maximumCubeDimension_)
        throw std::invalid_argument(
            "Reflection-probe capture exceeds the Vulkan cube-image limit");
    const ReflectionProbeCaptureStorageFootprint footprint =
        reflectionProbeCaptureStorageFootprint(resolution);
    OwnerState* state = find(owner);
    if (state != nullptr && state->hasStaging) {
        if (state->staging.captureTicket != captureTicket ||
            state->staging.resolution != resolution)
            throw std::logic_error(
                "A different capture is already staged for this probe");
        return state->staging;
    }
    if (capturesInFlight() >= config_.maximumCapturesInFlight)
        throw std::overflow_error(
            "Reflection-probe in-flight capture capacity is exhausted");
    if (footprint.totalStagingBytes > config_.maximumStagingBytes -
            (std::min)(config_.maximumStagingBytes, stagingLogicalBytes_))
        throw std::overflow_error(
            "Reflection-probe capture staging VRAM budget is exhausted");
    if (state == nullptr) {
        if (owners_.size() >= config_.maximumOwners)
            throw std::overflow_error(
                "Reflection-probe capture owner capacity is exhausted");
        owners_.push_back({ .owner = owner });
        state = &owners_.back();
    }

    VulkanReflectionProbeCaptureStaging staging;
    staging.owner = owner;
    staging.captureTicket = captureTicket;
    staging.resolution = resolution;
    staging.mipLevels = reflectionProbeCaptureMipCount(resolution);
    staging.logicalBytes = footprint.totalStagingBytes;
    try {
        staging.rawRadiance = allocator_->createImage2D(
            { resolution, resolution }, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, ProfileMemoryCategory::Environment,
            1, kReflectionProbeCaptureFaceCount,
            VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, VK_IMAGE_VIEW_TYPE_CUBE);
        staging.depth = allocator_->createImage2D(
            { resolution, resolution }, VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT, ProfileMemoryCategory::Environment,
            1, kReflectionProbeCaptureFaceCount, 0,
            VK_IMAGE_VIEW_TYPE_2D_ARRAY);
        staging.prefilteredRadiance = allocator_->createImage2D(
            { resolution, resolution }, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, ProfileMemoryCategory::Environment,
            staging.mipLevels, kReflectionProbeCaptureFaceCount,
            VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, VK_IMAGE_VIEW_TYPE_CUBE);
        for (uint32_t face = 0;
            face < kReflectionProbeCaptureFaceCount; ++face) {
            staging.rawFaceViews[face] = createView(
                staging.rawRadiance.image, staging.rawRadiance.format,
                VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D,
                0, 1, face, 1);
            staging.depthFaceViews[face] = createView(staging.depth.image,
                staging.depth.format, VK_IMAGE_ASPECT_DEPTH_BIT,
                VK_IMAGE_VIEW_TYPE_2D, 0, 1, face, 1);
            const std::array attachments{
                staging.rawFaceViews[face], staging.depthFaceViews[face] };
            VkFramebufferCreateInfo framebuffer{
                VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            framebuffer.renderPass = captureRenderPass_;
            framebuffer.attachmentCount = static_cast<uint32_t>(
                attachments.size());
            framebuffer.pAttachments = attachments.data();
            framebuffer.width = resolution;
            framebuffer.height = resolution;
            framebuffer.layers = 1;
            requireSuccess(vkCreateFramebuffer(device_, &framebuffer, nullptr,
                &staging.framebuffers[face]),
                "vkCreateFramebuffer(reflection probe capture)");
        }
        staging.prefilteredMipArrayViews.reserve(staging.mipLevels);
        for (uint32_t mip = 0; mip < staging.mipLevels; ++mip) {
            staging.prefilteredMipArrayViews.push_back(createView(
                staging.prefilteredRadiance.image,
                staging.prefilteredRadiance.format,
                VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                mip, 1, 0, kReflectionProbeCaptureFaceCount));
        }
    }
    catch (...) {
        destroyStaging(staging);
        if (!state->hasPublished) {
            const auto found = std::ranges::find(owners_, owner,
                &OwnerState::owner);
            if (found != owners_.end()) owners_.erase(found);
        }
        throw;
    }
    state->staging = std::move(staging);
    state->hasStaging = true;
    stagingLogicalBytes_ += footprint.totalStagingBytes;
    return state->staging;
}

void VulkanReflectionProbeCaptureTargets::destroyStaging(
    VulkanReflectionProbeCaptureStaging& staging) noexcept {
    if (device_ != VK_NULL_HANDLE) {
        for (VkFramebuffer framebuffer : staging.framebuffers)
            if (framebuffer != VK_NULL_HANDLE)
                vkDestroyFramebuffer(device_, framebuffer, nullptr);
        for (VkImageView view : staging.rawFaceViews)
            if (view != VK_NULL_HANDLE)
                vkDestroyImageView(device_, view, nullptr);
        for (VkImageView view : staging.depthFaceViews)
            if (view != VK_NULL_HANDLE)
                vkDestroyImageView(device_, view, nullptr);
        for (VkImageView view : staging.prefilteredMipArrayViews)
            if (view != VK_NULL_HANDLE)
                vkDestroyImageView(device_, view, nullptr);
    }
    if (allocator_ != nullptr) {
        allocator_->destroy(staging.rawRadiance);
        allocator_->destroy(staging.depth);
        allocator_->destroy(staging.prefilteredRadiance);
    }
    staging = {};
}

void VulkanReflectionProbeCaptureTargets::abandon(SceneEntityUuid owner,
    uint64_t captureTicket) {
    OwnerState* state = find(owner);
    if (state == nullptr || !state->hasStaging ||
        state->staging.captureTicket != captureTicket)
        throw std::invalid_argument(
            "Reflection-probe capture staging ticket is invalid");
    stagingLogicalBytes_ -= state->staging.logicalBytes;
    destroyStaging(state->staging);
    state->hasStaging = false;
    if (!state->hasPublished) remove(owner);
}

void VulkanReflectionProbeCaptureTargets::promote(SceneEntityUuid owner,
    uint64_t captureTicket) {
    OwnerState* state = find(owner);
    if (state == nullptr || !state->hasStaging ||
        state->staging.captureTicket != captureTicket)
        throw std::invalid_argument(
            "Reflection-probe capture promotion ticket is invalid");
    const uint64_t newPublishedBytes =
        reflectionProbeCaptureStorageFootprint(
            state->staging.resolution).prefilteredRadianceBytes;
    const uint64_t replacedPublishedBytes = state->hasPublished
        ? state->publishedLogicalBytes : 0;
    const uint64_t otherPublishedBytes = publishedLogicalBytes_ -
        (std::min)(publishedLogicalBytes_, replacedPublishedBytes);
    if (newPublishedBytes > config_.maximumPublishedBytes -
            (std::min)(config_.maximumPublishedBytes, otherPublishedBytes))
        throw std::overflow_error(
            "Reflection-probe published VRAM budget is exhausted");
    stagingLogicalBytes_ -= state->staging.logicalBytes;
    if (state->hasPublished) {
        publishedLogicalBytes_ -= state->publishedLogicalBytes;
        allocator_->destroy(state->published);
    }
    for (VkFramebuffer& framebuffer : state->staging.framebuffers) {
        if (framebuffer != VK_NULL_HANDLE)
            vkDestroyFramebuffer(device_, framebuffer, nullptr);
        framebuffer = VK_NULL_HANDLE;
    }
    for (VkImageView& view : state->staging.rawFaceViews) {
        if (view != VK_NULL_HANDLE)
            vkDestroyImageView(device_, view, nullptr);
        view = VK_NULL_HANDLE;
    }
    for (VkImageView& view : state->staging.depthFaceViews) {
        if (view != VK_NULL_HANDLE)
            vkDestroyImageView(device_, view, nullptr);
        view = VK_NULL_HANDLE;
    }
    for (VkImageView view : state->staging.prefilteredMipArrayViews)
        if (view != VK_NULL_HANDLE)
            vkDestroyImageView(device_, view, nullptr);
    state->staging.prefilteredMipArrayViews.clear();
    allocator_->destroy(state->staging.rawRadiance);
    allocator_->destroy(state->staging.depth);
    state->published = state->staging.prefilteredRadiance;
    state->staging.prefilteredRadiance = {};
    state->publishedLogicalBytes = newPublishedBytes;
    state->hasPublished = true;
    state->hasStaging = false;
    state->staging = {};
    publishedLogicalBytes_ += newPublishedBytes;
}

const VulkanReflectionProbeCaptureStaging*
VulkanReflectionProbeCaptureTargets::staging(SceneEntityUuid owner,
    uint64_t captureTicket) const noexcept {
    const OwnerState* state = find(owner);
    return state != nullptr && state->hasStaging &&
        state->staging.captureTicket == captureTicket
        ? &state->staging : nullptr;
}

const VulkanImageResource* VulkanReflectionProbeCaptureTargets::published(
    SceneEntityUuid owner) const noexcept {
    const OwnerState* state = find(owner);
    return state != nullptr && state->hasPublished
        ? &state->published : nullptr;
}

uint32_t VulkanReflectionProbeCaptureTargets::capturesInFlight() const noexcept {
    return static_cast<uint32_t>(std::ranges::count_if(owners_,
        [](const OwnerState& owner) { return owner.hasStaging; }));
}

uint32_t VulkanReflectionProbeCaptureTargets::publishedCount() const noexcept {
    return static_cast<uint32_t>(std::ranges::count_if(owners_,
        [](const OwnerState& owner) { return owner.hasPublished; }));
}

void VulkanReflectionProbeCaptureTargets::destroyOwner(
    OwnerState& owner) noexcept {
    if (owner.hasStaging) {
        stagingLogicalBytes_ -= owner.staging.logicalBytes;
        destroyStaging(owner.staging);
    }
    if (owner.hasPublished && allocator_ != nullptr) {
        publishedLogicalBytes_ -= owner.publishedLogicalBytes;
        allocator_->destroy(owner.published);
    }
    owner = {};
}

void VulkanReflectionProbeCaptureTargets::remove(
    SceneEntityUuid owner) noexcept {
    const auto found = std::ranges::find(owners_, owner, &OwnerState::owner);
    if (found == owners_.end()) return;
    destroyOwner(*found);
    owners_.erase(found);
}

void VulkanReflectionProbeCaptureTargets::cleanup() noexcept {
    for (OwnerState& owner : owners_) destroyOwner(owner);
    owners_.clear();
    stagingLogicalBytes_ = 0;
    publishedLogicalBytes_ = 0;
    maximumCubeDimension_ = 0;
    captureRenderPass_ = VK_NULL_HANDLE;
    allocator_ = nullptr;
    device_ = VK_NULL_HANDLE;
}

} // namespace Iridium
