#pragma once

#include <algorithm>
#include <cstdint>

namespace Iridium {

    // Keeps the large refraction products resident only while metric
    // transparency needs them. Enabling is immediate; disabling is delayed to
    // avoid repeatedly rebuilding all scene targets while an editor user
    // toggles or briefly hides refractive entities.
    class TransparencyPyramidResidency final {
    public:
        static constexpr uint32_t DefaultInactiveFrameThreshold = 120;

        explicit TransparencyPyramidResidency(
            uint32_t inactiveFrameThreshold = DefaultInactiveFrameThreshold)
            : inactiveFrameThreshold_((std::max)(
                inactiveFrameThreshold, 1u)) {}

        void observe(bool requiredThisFrame) noexcept {
            if (requiredThisFrame) {
                inactiveFrames_ = 0;
                requestedEnabled_ = true;
                return;
            }
            if (!enabled_) {
                inactiveFrames_ = 0;
                requestedEnabled_ = false;
                return;
            }
            if (inactiveFrames_ < inactiveFrameThreshold_)
                ++inactiveFrames_;
            if (inactiveFrames_ == inactiveFrameThreshold_)
                requestedEnabled_ = false;
        }

        [[nodiscard]] bool enabled() const noexcept { return enabled_; }
        [[nodiscard]] bool requestedEnabled() const noexcept {
            return requestedEnabled_;
        }
        [[nodiscard]] bool changePending() const noexcept {
            return enabled_ != requestedEnabled_;
        }
        [[nodiscard]] bool requiresFallback(bool requiredThisFrame) const
            noexcept {
            return requiredThisFrame && !enabled_;
        }
        [[nodiscard]] uint32_t inactiveFrames() const noexcept {
            return inactiveFrames_;
        }

        void publishRequested() noexcept {
            enabled_ = requestedEnabled_;
            inactiveFrames_ = 0;
        }

        void rejectRequested() noexcept {
            requestedEnabled_ = enabled_;
            inactiveFrames_ = 0;
        }

        void restore(bool enabled) noexcept {
            enabled_ = enabled;
            requestedEnabled_ = enabled;
            inactiveFrames_ = 0;
        }

    private:
        uint32_t inactiveFrameThreshold_ = DefaultInactiveFrameThreshold;
        uint32_t inactiveFrames_ = 0;
        bool enabled_ = false;
        bool requestedEnabled_ = false;
    };

} // namespace Iridium
