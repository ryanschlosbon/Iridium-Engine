#pragma once

#include "renderer/rhi/FrameCapture.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace Iridium {

    struct TgaImage {
        uint32_t width = 0;
        uint32_t height = 0;
        // Canonical top-left rows, four bytes per pixel in BGRA order.
        std::vector<std::byte> bgra8;
    };

    void writeFrameCaptureTga(const std::filesystem::path& path,
        const FrameCapture& capture);
    void writeTga(const std::filesystem::path& path, const TgaImage& image);
    [[nodiscard]] TgaImage readTga(const std::filesystem::path& path);

} // namespace Iridium
