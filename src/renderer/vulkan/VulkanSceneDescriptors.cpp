#include "VulkanSceneDescriptors.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace Iridium {

    namespace {
        VkWriteDescriptorSet imageWrite(VkDescriptorSet set, uint32_t binding,
            const VkDescriptorImageInfo& image) {
            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = set;
            write.dstBinding = binding;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &image;
            return write;
        }

        VkWriteDescriptorSet imageArrayWrite(VkDescriptorSet set,
            uint32_t binding, std::span<const VkDescriptorImageInfo> images) {
            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = set;
            write.dstBinding = binding;
            write.descriptorCount = static_cast<uint32_t>(images.size());
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = images.data();
            return write;
        }

        VkWriteDescriptorSet bufferWrite(VkDescriptorSet set, uint32_t binding,
            const VkDescriptorBufferInfo& buffer) {
            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = set;
            write.dstBinding = binding;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &buffer;
            return write;
        }
    }

    void VulkanSceneDescriptors::init(VkDevice device, ::DescriptorAllocator& allocator,
        VkDescriptorSetLayout layout) {
        if (device == VK_NULL_HANDLE || layout == VK_NULL_HANDLE) {
            throw std::invalid_argument("Scene descriptors require a valid device and layout.");
        }
        if (device_ != VK_NULL_HANDLE || allocator_ != nullptr || layout_ != VK_NULL_HANDLE) {
            throw std::logic_error("Scene descriptors were initialized twice.");
        }
        device_ = device;
        allocator_ = &allocator;
        layout_ = layout;
    }

    void VulkanSceneDescriptors::rebuild(const VulkanFrameTargets& frameTargets) {
        if (device_ == VK_NULL_HANDLE || allocator_ == nullptr || layout_ == VK_NULL_HANDLE) {
            throw std::logic_error("Scene descriptors are not initialized.");
        }
        if (frameTargets.size() == 0) {
            throw std::invalid_argument("Scene descriptors require at least one frame target.");
        }

        if (!sets_.empty()) {
            allocator_->free(std::span<const VkDescriptorSet>(sets_));
            sets_.clear();
        }

        try {
            sets_.reserve(frameTargets.size());
            for (size_t i = 0; i < frameTargets.size(); ++i) {
                sets_.push_back(allocator_->allocate(layout_));
                const VulkanFrameContextTargets& target = frameTargets.get(i);
                const VkDescriptorImageInfo depth{ frameTargets.sampler(), target.depth.view,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
                const VkDescriptorImageInfo normal{ frameTargets.sampler(), target.normal.view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                const VkDescriptorImageInfo albedo{ frameTargets.sampler(), target.albedo.view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                const VkDescriptorImageInfo emissive{ frameTargets.sampler(), target.emissive.view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                const VkDescriptorImageInfo f0Roughness{ frameTargets.sampler(),
                    target.f0Roughness.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                const VkDescriptorImageInfo materialFlags{ frameTargets.integerSampler(),
                    target.materialFlags.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                const bool hasRefractionPyramids =
                    target.refractionColorPyramid.view != VK_NULL_HANDLE &&
                    target.refractionDepthPyramid.view != VK_NULL_HANDLE;
                // Vulkan descriptors must remain valid even while the optional
                // graph products are absent. The normal cache is already in a
                // sampled-read layout throughout forward transparency; using it
                // for both unreachable bindings avoids extra fallback images and
                // avoids aliasing attachments that are writable in this pass.
                const VkDescriptorImageInfo refractionColor{
                    hasRefractionPyramids ? frameTargets.pyramidSampler()
                        : frameTargets.sampler(),
                    hasRefractionPyramids
                        ? target.refractionColorPyramid.view
                        : target.normal.view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                const VkDescriptorImageInfo refractionDepth{
                    hasRefractionPyramids ? frameTargets.depthPyramidSampler()
                        : frameTargets.sampler(),
                    hasRefractionPyramids
                        ? target.refractionDepthPyramid.view
                        : target.normal.view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

                std::array<VkWriteDescriptorSet, 33> writes{};
                writes[0] = imageWrite(sets_.back(), 0, depth);
                writes[1] = imageWrite(sets_.back(), 1, normal);
                writes[2] = imageWrite(sets_.back(), 2, albedo);
                writes[3] = imageWrite(sets_.back(), 4, refractionColor);
                writes[4] = imageWrite(sets_.back(), 5, refractionDepth);
                writes[5] = imageWrite(sets_.back(), 6, emissive);
                uint32_t writeCount = 6;
                if (target.f0Roughness.view != VK_NULL_HANDLE &&
                    target.materialFlags.view != VK_NULL_HANDLE) {
                    writes[writeCount++] = imageWrite(sets_.back(), 7, f0Roughness);
                    writes[writeCount++] = imageWrite(sets_.back(), 8, materialFlags);
                }
                if (hasEnvironmentImages_) {
                    writes[writeCount++] = imageWrite(sets_.back(), 16,
                        environmentImages_.irradiance);
                    writes[writeCount++] = imageWrite(sets_.back(), 17,
                        environmentImages_.prefilteredRadiance);
                    writes[writeCount++] = imageWrite(sets_.back(), 18,
                        environmentImages_.brdfLut);
                    writes[writeCount++] = imageWrite(sets_.back(), 19,
                        environmentImages_.skyRadiance);
                }
                if (hasDirectionalShadow_ &&
                    directionalShadow_.frameData.size() == frameTargets.size()) {
                    writes[writeCount++] = imageWrite(sets_.back(), 20,
                        directionalShadow_.image);
                    writes[writeCount] = bufferWrite(sets_.back(), 21,
                        directionalShadow_.frameData[i]);
                    writes[writeCount++].descriptorType =
                        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                }
                if (hasSpotShadow_ &&
                    spotShadow_.frameData.size() == frameTargets.size()) {
                    writes[writeCount++] = imageWrite(sets_.back(), 22,
                        spotShadow_.image);
                    writes[writeCount] = bufferWrite(sets_.back(), 23,
                        spotShadow_.frameData[i]);
                    writes[writeCount++].descriptorType =
                        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                }
                if (hasPointShadow_ &&
                    pointShadow_.frameData.size() == frameTargets.size()) {
                    for (uint32_t tier = 0; tier < 3; ++tier)
                        writes[writeCount++] = imageWrite(sets_.back(),
                            24u + tier, pointShadow_.images[tier]);
                    writes[writeCount] = bufferWrite(sets_.back(), 27,
                        pointShadow_.frameData[i]);
                    writes[writeCount++].descriptorType =
                        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                }
                if (lightBuffers_.size() == frameTargets.size()) {
                    writes[writeCount++] = bufferWrite(sets_.back(), 9,
                        lightBuffers_[i]);
                }
                if (clusterBuffers_.size() == frameTargets.size()) {
                    const auto& cluster = clusterBuffers_[i];
                    writes[writeCount++] = bufferWrite(sets_.back(), 10,
                        cluster.global);
                    writes[writeCount++] = bufferWrite(sets_.back(), 11,
                        cluster.headers);
                    writes[writeCount++] = bufferWrite(sets_.back(), 12,
                        cluster.indices);
                    writes[writeCount++] = bufferWrite(sets_.back(), 13,
                        cluster.fallback);
                    writes[writeCount++] = bufferWrite(sets_.back(), 14,
                        cluster.diagnostics);
                    writes[writeCount] = bufferWrite(sets_.back(), 15,
                        cluster.parameters);
                    writes[writeCount++].descriptorType =
                        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                }
                if (reflectionProbeBuffers_.size() == frameTargets.size()) {
                    const auto& probes = reflectionProbeBuffers_[i];
                    writes[writeCount++] = bufferWrite(sets_.back(), 28,
                        probes.records);
                    writes[writeCount++] = bufferWrite(sets_.back(), 29,
                        probes.headers);
                    writes[writeCount++] = bufferWrite(sets_.back(), 30,
                        probes.indices);
                }
                if (hasReflectionProbeImages_)
                    writes[writeCount++] = imageArrayWrite(sets_.back(), 31,
                        reflectionProbeImages_);
                vkUpdateDescriptorSets(device_, writeCount, writes.data(), 0, nullptr);
            }
        } catch (...) {
            if (!sets_.empty()) {
                allocator_->free(std::span<const VkDescriptorSet>(sets_));
                sets_.clear();
            }
            throw;
        }
    }

    void VulkanSceneDescriptors::setEnvironmentImages(
        const VulkanEnvironmentImageDescriptors& environment) {
        const auto valid = [](const VkDescriptorImageInfo& image) {
            return image.imageView != VK_NULL_HANDLE &&
                image.sampler != VK_NULL_HANDLE &&
                image.imageLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        };
        if (!valid(environment.irradiance) ||
            !valid(environment.prefilteredRadiance) ||
            !valid(environment.brdfLut) || !valid(environment.skyRadiance)) {
            throw std::invalid_argument("Invalid environment image descriptors.");
        }
        if (device_ == VK_NULL_HANDLE || allocator_ == nullptr) {
            throw std::logic_error("Scene descriptors are not initialized.");
        }
        environmentImages_ = environment;
        hasEnvironmentImages_ = true;
        for (VkDescriptorSet set : sets_) {
            std::array<VkWriteDescriptorSet, 4> writes{
                imageWrite(set, 16, environmentImages_.irradiance),
                imageWrite(set, 17, environmentImages_.prefilteredRadiance),
                imageWrite(set, 18, environmentImages_.brdfLut),
                imageWrite(set, 19, environmentImages_.skyRadiance),
            };
            vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()),
                writes.data(), 0, nullptr);
        }
    }

    void VulkanSceneDescriptors::setReflectionProbeBuffers(
        std::span<const VulkanReflectionProbeBufferDescriptors> buffers) {
        if (device_ == VK_NULL_HANDLE || allocator_ == nullptr)
            throw std::logic_error("Scene descriptors are not initialized.");
        if (buffers.empty() || (!sets_.empty() && buffers.size() != sets_.size()))
            throw std::invalid_argument(
                "Invalid reflection-probe frame buffers.");
        for (const auto& frame : buffers) {
            if (frame.records.buffer == VK_NULL_HANDLE ||
                frame.headers.buffer == VK_NULL_HANDLE ||
                frame.indices.buffer == VK_NULL_HANDLE ||
                frame.records.range == 0 || frame.headers.range == 0 ||
                frame.indices.range == 0)
                throw std::invalid_argument(
                    "Invalid reflection-probe buffer descriptor.");
        }
        reflectionProbeBuffers_.assign(buffers.begin(), buffers.end());
        for (size_t frame = 0; frame < sets_.size(); ++frame) {
            std::array<VkWriteDescriptorSet, 3> writes{
                bufferWrite(sets_[frame], 28,
                    reflectionProbeBuffers_[frame].records),
                bufferWrite(sets_[frame], 29,
                    reflectionProbeBuffers_[frame].headers),
                bufferWrite(sets_[frame], 30,
                    reflectionProbeBuffers_[frame].indices),
            };
            vkUpdateDescriptorSets(device_, 3, writes.data(), 0, nullptr);
        }
    }

    void VulkanSceneDescriptors::setReflectionProbeImages(
        std::span<const VkDescriptorImageInfo> images) {
        if (device_ == VK_NULL_HANDLE || allocator_ == nullptr)
            throw std::logic_error("Scene descriptors are not initialized.");
        if (images.size() != reflectionProbeImages_.size())
            throw std::invalid_argument(
                "Reflection-probe image table has the wrong capacity.");
        for (const auto& image : images) {
            if (image.imageView == VK_NULL_HANDLE ||
                image.sampler == VK_NULL_HANDLE ||
                image.imageLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                throw std::invalid_argument(
                    "Invalid reflection-probe image descriptor.");
        }
        std::copy(images.begin(), images.end(), reflectionProbeImages_.begin());
        hasReflectionProbeImages_ = true;
        for (VkDescriptorSet set : sets_) {
            const VkWriteDescriptorSet write = imageArrayWrite(set, 31,
                reflectionProbeImages_);
            vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        }
    }

    void VulkanSceneDescriptors::setDirectionalShadow(
        const VulkanDirectionalShadowDescriptors& shadow) {
        if (device_ == VK_NULL_HANDLE || allocator_ == nullptr)
            throw std::logic_error("Scene descriptors are not initialized.");
        if (shadow.image.imageView == VK_NULL_HANDLE ||
            shadow.image.sampler == VK_NULL_HANDLE ||
            shadow.image.imageLayout !=
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL ||
            shadow.frameData.empty() ||
            (!sets_.empty() && shadow.frameData.size() != sets_.size()))
            throw std::invalid_argument(
                "Invalid directional shadow descriptors.");
        for (const auto& buffer : shadow.frameData)
            if (buffer.buffer == VK_NULL_HANDLE || buffer.range == 0)
                throw std::invalid_argument(
                    "Invalid directional shadow frame buffer.");
        directionalShadow_ = shadow;
        hasDirectionalShadow_ = true;
        for (size_t frame = 0; frame < sets_.size(); ++frame) {
            std::array<VkWriteDescriptorSet, 2> writes{
                imageWrite(sets_[frame], 20, directionalShadow_.image),
                bufferWrite(sets_[frame], 21,
                    directionalShadow_.frameData[frame]) };
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            vkUpdateDescriptorSets(device_, 2, writes.data(), 0, nullptr);
        }
    }

    void VulkanSceneDescriptors::setSpotShadow(
        const VulkanSpotShadowDescriptors& shadow) {
        if (device_ == VK_NULL_HANDLE || allocator_ == nullptr)
            throw std::logic_error("Scene descriptors are not initialized.");
        if (shadow.image.imageView == VK_NULL_HANDLE ||
            shadow.image.sampler == VK_NULL_HANDLE ||
            shadow.image.imageLayout !=
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL ||
            shadow.frameData.empty() ||
            (!sets_.empty() && shadow.frameData.size() != sets_.size()))
            throw std::invalid_argument("Invalid spot shadow descriptors.");
        for (const auto& buffer : shadow.frameData)
            if (buffer.buffer == VK_NULL_HANDLE || buffer.range == 0)
                throw std::invalid_argument(
                    "Invalid spot shadow frame buffer.");
        spotShadow_ = shadow;
        hasSpotShadow_ = true;
        for (size_t frame = 0; frame < sets_.size(); ++frame) {
            std::array<VkWriteDescriptorSet, 2> writes{
                imageWrite(sets_[frame], 22, spotShadow_.image),
                bufferWrite(sets_[frame], 23,
                    spotShadow_.frameData[frame]) };
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            vkUpdateDescriptorSets(device_, 2, writes.data(), 0, nullptr);
        }
    }

    void VulkanSceneDescriptors::setPointShadow(
        const VulkanPointShadowDescriptors& shadow) {
        if (device_ == VK_NULL_HANDLE || allocator_ == nullptr)
            throw std::logic_error("Scene descriptors are not initialized.");
        for (const auto& image : shadow.images)
            if (image.imageView == VK_NULL_HANDLE ||
                image.sampler == VK_NULL_HANDLE ||
                image.imageLayout !=
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)
                throw std::invalid_argument(
                    "Invalid point shadow image descriptor.");
        if (shadow.frameData.empty() ||
            (!sets_.empty() && shadow.frameData.size() != sets_.size()))
            throw std::invalid_argument("Invalid point shadow descriptors.");
        for (const auto& buffer : shadow.frameData)
            if (buffer.buffer == VK_NULL_HANDLE || buffer.range == 0)
                throw std::invalid_argument(
                    "Invalid point shadow frame buffer.");
        pointShadow_ = shadow;
        hasPointShadow_ = true;
        for (size_t frame = 0; frame < sets_.size(); ++frame) {
            std::array<VkWriteDescriptorSet, 4> writes{};
            for (uint32_t tier = 0; tier < 3; ++tier)
                writes[tier] = imageWrite(sets_[frame], 24u + tier,
                    pointShadow_.images[tier]);
            writes[3] = bufferWrite(sets_[frame], 27,
                pointShadow_.frameData[frame]);
            writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            vkUpdateDescriptorSets(device_,
                static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }

    void VulkanSceneDescriptors::setLightBuffers(
        std::span<const VkDescriptorBufferInfo> buffers) {
        if (device_ == VK_NULL_HANDLE || allocator_ == nullptr) {
            throw std::logic_error("Scene descriptors are not initialized.");
        }
        if (buffers.empty()) {
            throw std::invalid_argument("Scene light buffers cannot be empty.");
        }
        for (const VkDescriptorBufferInfo& buffer : buffers) {
            if (buffer.buffer == VK_NULL_HANDLE || buffer.range == 0) {
                throw std::invalid_argument("Invalid scene light buffer descriptor.");
            }
        }
        if (!sets_.empty() && sets_.size() != buffers.size()) {
            throw std::invalid_argument(
                "Scene light buffer count must match the frame contexts.");
        }
        lightBuffers_.assign(buffers.begin(), buffers.end());
        for (size_t index = 0; index < sets_.size(); ++index) {
            const VkWriteDescriptorSet write = bufferWrite(
                sets_[index], 9, lightBuffers_[index]);
            vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        }
    }

    void VulkanSceneDescriptors::setClusterBuffers(
        std::span<const VulkanClusterSceneBufferDescriptors> buffers) {
        if (device_ == VK_NULL_HANDLE || allocator_ == nullptr) {
            throw std::logic_error("Scene descriptors are not initialized.");
        }
        if (buffers.empty() || (!sets_.empty() && sets_.size() != buffers.size())) {
            throw std::invalid_argument(
                "Scene cluster buffers must match the frame contexts.");
        }
        for (const auto& cluster : buffers) {
            for (const VkDescriptorBufferInfo* buffer : {
                    &cluster.global, &cluster.headers, &cluster.indices,
                    &cluster.fallback, &cluster.diagnostics,
                    &cluster.parameters }) {
                if (buffer->buffer == VK_NULL_HANDLE || buffer->range == 0) {
                    throw std::invalid_argument(
                        "Invalid scene cluster buffer descriptor.");
                }
            }
        }
        clusterBuffers_.assign(buffers.begin(), buffers.end());
        for (size_t frame = 0; frame < sets_.size(); ++frame) {
            const auto& cluster = clusterBuffers_[frame];
            std::array<VkDescriptorBufferInfo, 6> infos{
                cluster.global, cluster.headers, cluster.indices,
                cluster.fallback, cluster.diagnostics, cluster.parameters };
            std::array<VkWriteDescriptorSet, 6> writes{};
            for (uint32_t index = 0; index < writes.size(); ++index) {
                writes[index] = bufferWrite(sets_[frame], 10u + index,
                    infos[index]);
            }
            writes.back().descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()),
                writes.data(), 0, nullptr);
        }
    }

    void VulkanSceneDescriptors::cleanup() {
        if (allocator_ != nullptr && !sets_.empty()) {
            allocator_->free(std::span<const VkDescriptorSet>(sets_));
        }
        sets_.clear();
        environmentImages_ = {};
        directionalShadow_ = {};
        spotShadow_ = {};
        pointShadow_ = {};
        lightBuffers_.clear();
        clusterBuffers_.clear();
        reflectionProbeBuffers_.clear();
        reflectionProbeImages_ = {};
        hasEnvironmentImages_ = false;
        hasDirectionalShadow_ = false;
        hasSpotShadow_ = false;
        hasPointShadow_ = false;
        hasReflectionProbeImages_ = false;
        device_ = VK_NULL_HANDLE;
        allocator_ = nullptr;
        layout_ = VK_NULL_HANDLE;
    }

    VkDescriptorSet VulkanSceneDescriptors::get(uint32_t frameIndex) const {
        if (frameIndex >= sets_.size()) {
            throw std::out_of_range("Scene descriptor frame index is out of range.");
        }
        return sets_[frameIndex];
    }

    size_t VulkanSceneDescriptors::size() const noexcept {
        return sets_.size();
    }

} // namespace Iridium
