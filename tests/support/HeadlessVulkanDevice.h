#pragma once

// A surface-free Vulkan 1.3 device for hardware qualification tests: instance with
// the Khronos validation layer and a debug messenger that counts validation
// errors, one graphics+compute queue, and the core features the production
// VkContext enables (descriptor indexing, draw-indirect count, ...). Tests create
// production passes against it and assert that validation stays silent.

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace Iridium::Test {

    struct HeadlessVulkanBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        void* mapped = nullptr; // host-visible, coherent, persistently mapped
    };

    class HeadlessVulkanDevice {
    public:
        struct Options {
            const char* applicationName = "Iridium headless test";
            bool synchronizationValidation = false;
        };

        HeadlessVulkanDevice();
        explicit HeadlessVulkanDevice(const Options& options);
        ~HeadlessVulkanDevice();

        HeadlessVulkanDevice(const HeadlessVulkanDevice&) = delete;
        HeadlessVulkanDevice& operator=(const HeadlessVulkanDevice&) = delete;

        [[nodiscard]] VkInstance instance() const noexcept { return instance_; }
        [[nodiscard]] VkPhysicalDevice physicalDevice() const noexcept { return physical_; }
        [[nodiscard]] VkDevice device() const noexcept { return device_; }
        [[nodiscard]] VkQueue queue() const noexcept { return queue_; }
        [[nodiscard]] uint32_t queueFamily() const noexcept { return queueFamily_; }
        [[nodiscard]] const VkPhysicalDeviceProperties& properties() const noexcept {
            return properties_;
        }
        [[nodiscard]] bool hasMemoryBudget() const noexcept { return memoryBudget_; }
        [[nodiscard]] uint32_t maxUpdateAfterBindDescriptors() const noexcept {
            return maxUpdateAfterBindDescriptors_;
        }

        // Validation-layer errors reported since construction (or the last reset).
        [[nodiscard]] uint32_t validationErrors() const noexcept { return errors_.load(); }
        void resetValidationErrors() noexcept { errors_.store(0); }
        [[nodiscard]] const std::vector<std::string>& validationMessages() const noexcept {
            return messages_;
        }

        [[nodiscard]] HeadlessVulkanBuffer createHostBuffer(VkDeviceSize size,
            VkBufferUsageFlags usage) const;
        void destroy(HeadlessVulkanBuffer& buffer) const noexcept;

        [[nodiscard]] VkShaderModule createShaderModule(
            const std::filesystem::path& spvPath) const;

        // Records into a one-shot primary command buffer, submits and waits idle.
        void submitAndWait(const std::function<void(VkCommandBuffer)>& record) const;

    private:
        static VKAPI_ATTR VkBool32 VKAPI_CALL message(
            VkDebugUtilsMessageSeverityFlagBitsEXT severity,
            VkDebugUtilsMessageTypeFlagsEXT type,
            const VkDebugUtilsMessengerCallbackDataEXT* data, void* user);
        [[nodiscard]] uint32_t memoryType(uint32_t typeBits,
            VkMemoryPropertyFlags flags) const;

        VkInstance instance_ = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
        VkPhysicalDevice physical_ = VK_NULL_HANDLE;
        VkDevice device_ = VK_NULL_HANDLE;
        VkQueue queue_ = VK_NULL_HANDLE;
        VkCommandPool commandPool_ = VK_NULL_HANDLE;
        uint32_t queueFamily_ = UINT32_MAX;
        VkPhysicalDeviceProperties properties_{};
        VkPhysicalDeviceMemoryProperties memory_{};
        bool memoryBudget_ = false;
        uint32_t maxUpdateAfterBindDescriptors_ = 0;
        std::atomic<uint32_t> errors_ = 0;
        std::vector<std::string> messages_;
    };

} // namespace Iridium::Test
