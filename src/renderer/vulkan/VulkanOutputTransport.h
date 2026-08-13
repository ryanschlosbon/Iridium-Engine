#pragma once

#include "renderer/color/OutputTransformConfig.h"

#include <span>
#include <string>
#include <vulkan/vulkan.h>

namespace Iridium {

    struct VulkanOutputTransportSelection {
        Color::OutputTransport requested = Color::OutputTransport::SdrSrgb;
        Color::OutputTransport effective = Color::OutputTransport::SdrSrgb;
        VkSurfaceFormatKHR surfaceFormat{};
        bool usedSdrFallback = false;
        bool hdrMetadataEnabled = false;
        std::string diagnostic;
    };

    [[nodiscard]] VulkanOutputTransportSelection selectVulkanOutputTransport(
        Color::OutputTransport requested,
        std::span<const VkSurfaceFormatKHR> availableFormats,
        bool hdrMetadataSupported);

} // namespace Iridium
