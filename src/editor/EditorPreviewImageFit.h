#pragma once
#include <glm/glm.hpp>
#include <algorithm>

namespace Iridium {
    struct EditorPreviewImageFit {
        glm::vec2 uvMinimum{0};
        glm::vec2 uvMaximum{1};
        float projectionScale = 1;
    };
    // Fill a view using shared scratch resolution, preserving the vertical FOV
    // and geometry aspect through paired projection adjustment and UV cropping.
    inline EditorPreviewImageFit previewImageFit(float sourceAspect, float viewAspect) {
        EditorPreviewImageFit result;
        if (!(sourceAspect > 0) || !(viewAspect > 0)) return result;
        const glm::vec2 span(std::min(1.0f, viewAspect / sourceAspect),
            std::min(1.0f, sourceAspect / viewAspect));
        result.uvMinimum = (glm::vec2(1) - span) * .5f;
        result.uvMaximum = glm::vec2(1) - result.uvMinimum;
        result.projectionScale = span.y;
        return result;
    }
}
