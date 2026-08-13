#include "renderer/vulkan/VulkanOutputTransport.h"

#include <exception>
#include <iostream>
#include <vector>

namespace {

    using Iridium::Color::OutputTransport;
    using Iridium::selectVulkanOutputTransport;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    constexpr VkSurfaceFormatKHR Sdr{
        VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    constexpr VkSurfaceFormatKHR ScRgb{
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT };
    constexpr VkSurfaceFormatKHR Hdr10{
        VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT };

    bool testExactSelections() {
        const std::vector formats{ Hdr10, Sdr, ScRgb };
        const auto sdr = selectVulkanOutputTransport(
            OutputTransport::SdrSrgb, formats, true);
        CHECK(sdr.effective == OutputTransport::SdrSrgb);
        CHECK(sdr.surfaceFormat.format == Sdr.format);
        CHECK(!sdr.usedSdrFallback);
        CHECK(!sdr.hdrMetadataEnabled);

        const auto scRgb = selectVulkanOutputTransport(
            OutputTransport::ScRgb, formats, true);
        CHECK(scRgb.effective == OutputTransport::ScRgb);
        CHECK(scRgb.surfaceFormat.format == ScRgb.format);
        CHECK(!scRgb.usedSdrFallback);
        CHECK(!scRgb.hdrMetadataEnabled);

        const auto hdr10 = selectVulkanOutputTransport(
            OutputTransport::Hdr10Pq, formats, true);
        CHECK(hdr10.effective == OutputTransport::Hdr10Pq);
        CHECK(hdr10.surfaceFormat.format == Hdr10.format);
        CHECK(hdr10.hdrMetadataEnabled);
        return true;
    }

    bool testExplicitSdrFallbacks() {
        const std::vector formats{ Sdr };
        const auto scRgb = selectVulkanOutputTransport(
            OutputTransport::ScRgb, formats, false);
        CHECK(scRgb.effective == OutputTransport::SdrSrgb);
        CHECK(scRgb.usedSdrFallback);
        CHECK(scRgb.diagnostic.find("scRGB") != std::string::npos);

        const auto hdr10 = selectVulkanOutputTransport(
            OutputTransport::Hdr10Pq, formats, true);
        CHECK(hdr10.effective == OutputTransport::SdrSrgb);
        CHECK(hdr10.usedSdrFallback);
        CHECK(!hdr10.hdrMetadataEnabled);
        CHECK(hdr10.diagnostic.find("HDR10") != std::string::npos);
        return true;
    }

    bool testHdrMetadataIsOptionalAndReported() {
        const std::vector formats{ Sdr, Hdr10 };
        const auto selection = selectVulkanOutputTransport(
            OutputTransport::Hdr10Pq, formats, false);
        CHECK(selection.effective == OutputTransport::Hdr10Pq);
        CHECK(!selection.usedSdrFallback);
        CHECK(!selection.hdrMetadataEnabled);
        CHECK(selection.diagnostic.find("VK_EXT_hdr_metadata") != std::string::npos);
        return true;
    }

    bool testAlternativeHdr10Packing() {
        const VkSurfaceFormatKHR alternative{
            VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT };
        const std::vector formats{ Sdr, alternative };
        const auto selection = selectVulkanOutputTransport(
            OutputTransport::Hdr10Pq, formats, true);
        CHECK(selection.surfaceFormat.format == alternative.format);
        return true;
    }

    bool testUndefinedFormatUsesRequestedSdrFormat() {
        const VkSurfaceFormatKHR undefined{
            VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        const std::vector formats{ undefined };
        const auto selection = selectVulkanOutputTransport(
            OutputTransport::SdrSrgb, formats, false);
        CHECK(selection.surfaceFormat.format == VK_FORMAT_B8G8R8A8_SRGB);
        CHECK(selection.surfaceFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
        return true;
    }

    bool testMissingSdrIsFatal() {
        const std::vector formats{ ScRgb };
        try {
            (void)selectVulkanOutputTransport(
                OutputTransport::Hdr10Pq, formats, false);
        }
        catch (const std::runtime_error&) {
            return true;
        }
        return false;
    }

} // namespace

int main() {
    const struct TestCase { const char* name; bool (*run)(); } tests[] = {
        { "exact transport selections", testExactSelections },
        { "explicit SDR fallbacks", testExplicitSdrFallbacks },
        { "optional HDR metadata", testHdrMetadataIsOptionalAndReported },
        { "alternative HDR10 packing", testAlternativeHdr10Packing },
        { "undefined SDR format", testUndefinedFormatUsesRequestedSdrFormat },
        { "missing SDR is fatal", testMissingSdrIsFatal },
    };
    for (const TestCase& test : tests) {
        if (!test.run()) {
            std::cerr << "[FAIL] " << test.name << '\n';
            return 1;
        }
        std::cout << "[PASS] " << test.name << '\n';
    }
    return 0;
}
