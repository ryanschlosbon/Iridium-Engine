#pragma once

namespace Iridium {

    struct ViewportFitRect {
        float offsetX = 0.0f;
        float offsetY = 0.0f;
        float width = 0.0f;
        float height = 0.0f;

        constexpr bool operator==(
            const ViewportFitRect&) const noexcept = default;
    };

    [[nodiscard]] ViewportFitRect fitViewportAspect(
        float availableWidth,
        float availableHeight,
        float targetAspect) noexcept;

} // namespace Iridium
