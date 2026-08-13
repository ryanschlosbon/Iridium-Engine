#include "editor/ViewportRenderExtent.h"

#include <algorithm>
#include <cmath>

namespace Iridium {
    namespace {

        [[nodiscard]] bool same(RenderExtent lhs, RenderExtent rhs) noexcept {
            return lhs.width == rhs.width && lhs.height == rhs.height;
        }

        [[nodiscard]] bool empty(RenderExtent extent) noexcept {
            return extent.width == 0 || extent.height == 0;
        }

    } // namespace

    RenderExtent viewportPixelExtent(float logicalWidth, float logicalHeight,
        float framebufferScaleX, float framebufferScaleY,
        uint32_t minimumDimension, uint32_t maximumDimension) noexcept {
        if (!std::isfinite(logicalWidth) || !std::isfinite(logicalHeight) ||
            !std::isfinite(framebufferScaleX) ||
            !std::isfinite(framebufferScaleY) ||
            logicalWidth <= 0.0f || logicalHeight <= 0.0f ||
            framebufferScaleX <= 0.0f || framebufferScaleY <= 0.0f ||
            minimumDimension == 0 || maximumDimension < minimumDimension) {
            return {};
        }
        const double scaledWidth = static_cast<double>(logicalWidth) *
            framebufferScaleX;
        const double scaledHeight = static_cast<double>(logicalHeight) *
            framebufferScaleY;
        if (scaledWidth < minimumDimension || scaledHeight < minimumDimension) {
            return {};
        }
        return {
            .width = static_cast<uint32_t>(std::clamp(
                std::llround(scaledWidth),
                static_cast<long long>(minimumDimension),
                static_cast<long long>(maximumDimension))),
            .height = static_cast<uint32_t>(std::clamp(
                std::llround(scaledHeight),
                static_cast<long long>(minimumDimension),
                static_cast<long long>(maximumDimension))),
        };
    }

    std::optional<RenderExtent> ViewportRenderExtentPolicy::observe(
        RenderExtent requested, RenderExtent active,
        uint64_t nowMilliseconds) noexcept {
        if (empty(requested) || same(requested, active)) {
            reset();
            return std::nullopt;
        }
        if (!hasCandidate_ || !same(candidate_, requested)) {
            candidate_ = requested;
            emitted_ = {};
            candidateSinceMilliseconds_ = nowMilliseconds;
            hasCandidate_ = true;
            if (debounceMilliseconds_ == 0) {
                emitted_ = candidate_;
                return emitted_;
            }
            return std::nullopt;
        }
        if (!empty(emitted_) && same(emitted_, candidate_)) {
            return std::nullopt;
        }
        if (nowMilliseconds - candidateSinceMilliseconds_ <
            debounceMilliseconds_) {
            return std::nullopt;
        }
        emitted_ = candidate_;
        return emitted_;
    }

    void ViewportRenderExtentPolicy::reportFailure(
        uint64_t nowMilliseconds) noexcept {
        emitted_ = {};
        candidateSinceMilliseconds_ = nowMilliseconds;
    }

    void ViewportRenderExtentPolicy::reset() noexcept {
        candidate_ = {};
        emitted_ = {};
        candidateSinceMilliseconds_ = 0;
        hasCandidate_ = false;
    }

} // namespace Iridium
