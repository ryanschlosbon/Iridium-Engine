#include "renderer/vulkan/VulkanOutputTransport.h"

#include <array>
#include <stdexcept>

namespace Iridium {

    namespace {

        constexpr VkSurfaceFormatKHR SdrCandidates[] = {
            { VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
            { VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        };

        constexpr VkSurfaceFormatKHR ScRgbCandidates[] = {
            { VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT },
        };

        constexpr VkSurfaceFormatKHR Hdr10Candidates[] = {
            { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT },
            { VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT },
        };

        template <size_t CandidateCount>
        [[nodiscard]] bool findCandidate(
            std::span<const VkSurfaceFormatKHR> available,
            const VkSurfaceFormatKHR (&candidates)[CandidateCount],
            VkSurfaceFormatKHR& selected) {
            for (const VkSurfaceFormatKHR candidate : candidates) {
                for (const VkSurfaceFormatKHR offered : available) {
                    if (offered.colorSpace != candidate.colorSpace) continue;
                    if (offered.format == candidate.format ||
                        offered.format == VK_FORMAT_UNDEFINED) {
                        selected = candidate;
                        return true;
                    }
                }
            }
            return false;
        }

        [[nodiscard]] bool findSdrFallback(
            std::span<const VkSurfaceFormatKHR> available,
            VkSurfaceFormatKHR& selected) {
            return findCandidate(available, SdrCandidates, selected);
        }

    } // namespace

    VulkanOutputTransportSelection selectVulkanOutputTransport(
        Color::OutputTransport requested,
        std::span<const VkSurfaceFormatKHR> availableFormats,
        bool hdrMetadataSupported) {
        if (availableFormats.empty()) {
            throw std::runtime_error("surface exposes no output formats");
        }

        VulkanOutputTransportSelection result{};
        result.requested = requested;
        result.effective = requested;

        bool found = false;
        switch (requested) {
        case Color::OutputTransport::SdrSrgb:
            found = findCandidate(availableFormats, SdrCandidates, result.surfaceFormat);
            break;
        case Color::OutputTransport::ScRgb:
            found = findCandidate(availableFormats, ScRgbCandidates, result.surfaceFormat);
            break;
        case Color::OutputTransport::Hdr10Pq:
            found = findCandidate(availableFormats, Hdr10Candidates, result.surfaceFormat);
            break;
        }

        if (!found) {
            if (requested == Color::OutputTransport::SdrSrgb ||
                !findSdrFallback(availableFormats, result.surfaceFormat)) {
                throw std::runtime_error(
                    "required SDR Rec.709/sRGB surface format is unavailable");
            }
            result.effective = Color::OutputTransport::SdrSrgb;
            result.usedSdrFallback = true;
            result.diagnostic = requested == Color::OutputTransport::ScRgb
                ? "scRGB surface format/color-space pair unavailable; selected SDR fallback"
                : "HDR10/PQ surface format/color-space pair unavailable; selected SDR fallback";
        }

        result.hdrMetadataEnabled =
            result.effective == Color::OutputTransport::Hdr10Pq && hdrMetadataSupported;
        if (result.effective == Color::OutputTransport::Hdr10Pq &&
            !hdrMetadataSupported) {
            result.diagnostic =
                "HDR10/PQ selected without VK_EXT_hdr_metadata; static display metadata is unavailable";
        }
        return result;
    }

} // namespace Iridium
