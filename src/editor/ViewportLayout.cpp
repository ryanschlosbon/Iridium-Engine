#include "editor/ViewportLayout.h"

#include <algorithm>
#include <cmath>

namespace Iridium {

    ViewportFitRect fitViewportAspect(
        float availableWidth,
        float availableHeight,
        float targetAspect) noexcept {
        if (!std::isfinite(availableWidth) ||
            !std::isfinite(availableHeight) ||
            !std::isfinite(targetAspect) ||
            availableWidth <= 0.0f ||
            availableHeight <= 0.0f ||
            targetAspect <= 0.0f) {
            return {};
        }

        ViewportFitRect result;
        const float availableAspect =
            availableWidth / availableHeight;
        if (availableAspect > targetAspect) {
            result.height = availableHeight;
            result.width =
                result.height * targetAspect;
            result.offsetX =
                (availableWidth - result.width) *
                0.5f;
        }
        else {
            result.width = availableWidth;
            result.height =
                result.width / targetAspect;
            result.offsetY =
                (availableHeight - result.height) *
                0.5f;
        }
        return result;
    }

} // namespace Iridium
