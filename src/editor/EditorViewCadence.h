#pragma once

#include <chrono>
#include <array>
#include <optional>
#include <stdexcept>

namespace Iridium {
    // Per-view rendering cadence, not an application sleep or simulation clock.
    // A caller marks a view rendered only after submitting it successfully.
    // Each independent view owns one instance, including its own last image.
    class EditorViewCadence {
    public:
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;

        void setBackgroundFramesPerSecond(unsigned fps) {
            if (fps != 30 && fps != 60)
                throw std::invalid_argument("Background view cadence must be 30 or 60 FPS");
            if (fps_ != fps) { fps_ = fps; invalidate(); }
        }
        [[nodiscard]] unsigned backgroundFramesPerSecond() const noexcept { return fps_; }
        void invalidate() noexcept { dirty_ = true; }
        [[nodiscard]] bool shouldRender(TimePoint now, bool visible, bool focused) const noexcept {
            if (!visible) return false;
            if (focused || dirty_ || !lastRender_ || now < *lastRender_) return true;
            return now - *lastRender_ >= std::chrono::duration<double>(1.0 / fps_);
        }
        void rendered(TimePoint now) noexcept { lastRender_ = now; dirty_ = false; }
        void reset() noexcept { lastRender_.reset(); dirty_ = true; }

    private:
        unsigned fps_ = 30;
        bool dirty_ = true;
        std::optional<TimePoint> lastRender_;
    };

    // One render submission is shared by two retained views. An overdue
    // background view must not consume consecutive turns and starve focus.
    class EditorViewScheduler {
    public:
        unsigned choose(EditorViewCadence::TimePoint now, unsigned focused,
            bool sceneVisible, bool viewerVisible, unsigned backgroundFps) {
            for (auto& cadence : cadence_) cadence.setBackgroundFramesPerSecond(backgroundFps);
            if (!viewerVisible) return 0;
            if (!sceneVisible) return 1;
            const unsigned background = 1u - focused;
            return lastRendered_ != background &&
                cadence_[background].shouldRender(now, true, false) ? background : focused;
        }
        void rendered(unsigned view, EditorViewCadence::TimePoint now) {
            cadence_[view].rendered(now);
            lastRendered_ = view;
        }
        void reset() {
            for (auto& cadence : cadence_) cadence.reset();
            lastRendered_.reset();
        }
    private:
        std::array<EditorViewCadence, 2> cadence_;
        std::optional<unsigned> lastRendered_;
    };
}
