#pragma once

#include "DescriptorAllocator.h"
#include "VulkanFrameTargets.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Iridium {

    // Owns only lighting descriptor-set handles. DescriptorAllocator owns the
    // underlying pools; target images and the environment texture remain external.
    class VulkanSceneDescriptors final {
    public:
        VulkanSceneDescriptors() = default;
        VulkanSceneDescriptors(const VulkanSceneDescriptors&) = delete;
        VulkanSceneDescriptors& operator=(const VulkanSceneDescriptors&) = delete;

        void init(VkDevice device, ::DescriptorAllocator& allocator, VkDescriptorSetLayout layout);
        // The caller guarantees GPU idle. Old sets are freed before replacements;
        // the most recent environment descriptor is reapplied automatically.
        void rebuild(const VulkanFrameTargets& frameTargets);
        void setEnvironment(VkDescriptorImageInfo environment);
        void cleanup();

        [[nodiscard]] VkDescriptorSet get(uint32_t imageIndex) const;
        [[nodiscard]] size_t size() const noexcept;

    private:
        // Binding contract: 0 depth, 1 normal, 2 albedo, 3 environment,
        // 4 opaque scene copy, 5 glass depth, 6 emissive. Target bindings use the shared
        // frame-target sampler; environment uses its provided sampler and view.
        VkDevice device_ = VK_NULL_HANDLE;
        ::DescriptorAllocator* allocator_ = nullptr;
        VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> sets_;
        VkDescriptorImageInfo environment_{};
        bool hasEnvironment_ = false;
    };

} // namespace Iridium
