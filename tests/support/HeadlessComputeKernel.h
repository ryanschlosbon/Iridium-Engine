#pragma once

// A compute pipeline whose descriptor-set and push-constant layout is derived
// from the module's own SPIR-V reflection. Used to run production compute
// shaders (and test kernels) against host-visible buffers for CPU parity checks.

#include "HeadlessVulkanDevice.h"
#include "SpirvInspector.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <utility>
#include <vector>

namespace Iridium::Test {

    class HeadlessComputeKernel {
    public:
        HeadlessComputeKernel(const HeadlessVulkanDevice& device,
            const std::filesystem::path& spvPath);
        ~HeadlessComputeKernel();

        HeadlessComputeKernel(const HeadlessComputeKernel&) = delete;
        HeadlessComputeKernel& operator=(const HeadlessComputeKernel&) = delete;

        [[nodiscard]] const SpirvModule& module() const noexcept { return module_; }

        // Binds whole buffers by (set, binding), pushes `pushConstants` and
        // dispatches `groupCountX` workgroups, then waits for completion.
        void dispatch(const std::map<std::pair<uint32_t, uint32_t>, VkBuffer>& buffers,
            std::span<const std::byte> pushConstants, uint32_t groupCountX);

    private:
        const HeadlessVulkanDevice& device_;
        SpirvModule module_;
        std::vector<SpirvDescriptorBinding> bindings_;
        std::vector<VkDescriptorSetLayout> setLayouts_;
        VkDescriptorPool pool_ = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> sets_;
        VkPipelineLayout layout_ = VK_NULL_HANDLE;
        VkShaderModule shader_ = VK_NULL_HANDLE;
        VkPipeline pipeline_ = VK_NULL_HANDLE;
        uint32_t pushBytes_ = 0;
    };

} // namespace Iridium::Test
