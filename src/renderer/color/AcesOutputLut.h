#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace Iridium::Color {

    struct AcesOutputLut {
        uint32_t size = 0;
        float minimumLog2 = 0.0f;
        float maximumLog2 = 0.0f;
        std::vector<float> rgba32f;

        [[nodiscard]] uint32_t width() const noexcept { return size * size; }
        [[nodiscard]] uint32_t height() const noexcept { return size; }
    };

    [[nodiscard]] AcesOutputLut loadAcesOutputLut(
        const std::filesystem::path& path);
    [[nodiscard]] std::array<float, 3> sampleAcesOutputLutEncoded(
        const AcesOutputLut& lut, std::array<float, 3> sceneAcesCg) noexcept;

} // namespace Iridium::Color
