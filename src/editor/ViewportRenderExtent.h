#pragma once

#include "renderer/rhi/RhiResourceTypes.h"

#include <cstdint>
#include <optional>

namespace Iridium {

    [[nodiscard]] RenderExtent viewportPixelExtent(
        float logicalWidth, float logicalHeight,
        float framebufferScaleX, float framebufferScaleY,
        uint32_t minimumDimension = 64,
        uint32_t maximumDimension = 8192) noexcept;

    class ViewportRenderExtentPolicy {
    public:
        explicit ViewportRenderExtentPolicy(
            uint64_t debounceMilliseconds = 100) noexcept
            : debounceMilliseconds_(debounceMilliseconds) {}

        // nowMilliseconds is supplied by the caller to keep policy tests fully
        // deterministic. Empty/minimized requests retain the last active target.
        [[nodiscard]] std::optional<RenderExtent> observe(
            RenderExtent requested, RenderExtent active,
            uint64_t nowMilliseconds) noexcept;
        void reportFailure(uint64_t nowMilliseconds) noexcept;
        void reset() noexcept;

    private:
        RenderExtent candidate_{};
        RenderExtent emitted_{};
        uint64_t candidateSinceMilliseconds_ = 0;
        uint64_t debounceMilliseconds_ = 100;
        bool hasCandidate_ = false;
    };

} // namespace Iridium
