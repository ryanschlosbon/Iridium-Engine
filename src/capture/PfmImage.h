#pragma once

#include "renderer/rhi/FrameCapture.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Iridium {

    struct PfmImage {
        uint32_t width = 0;
        uint32_t height = 0;
        // Canonical top-left row order, interleaved RGB float32 values.
        std::vector<float> rgb32f;
    };

    void writeFrameCapturePfm(const std::filesystem::path& path,
        const FrameCapture& capture);
    [[nodiscard]] PfmImage readPfm(const std::filesystem::path& path);

} // namespace Iridium
