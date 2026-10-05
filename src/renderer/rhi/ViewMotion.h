#pragma once

#include <array>
#include <cstdint>

#include <glm/glm.hpp>

#include "renderer/graph/ViewHistory.h"

namespace Iridium {

    // M9 G2: per-retained-view camera continuity. The extractor feeds every
    // rendered view through update(); the result is that view's History
    // context. A view's resetRevision advances on a cut:
    // - a changed requested revision (benchmark cut, asset-preview framing);
    // - an explicit cut request (editor teleport);
    // - a projection-kind or extent change;
    // - a discontinuity: camera translation or rotation in one turn above
    //   the thresholds (a safety net for teleports nobody declared).
    // A field-of-view change is not a cut: it is reprojectable motion.
    // Deterministic: a pure function of the inputs (never wall time).
    struct ViewMotionInput {
        uint32_t historySet = 0;
        uint64_t identity = 0;
        uint64_t requestedResetRevision = 0;
        bool explicitCut = false;
        glm::mat4 view{ 1.0f };
        uint32_t projectionKind = 0;
        glm::uvec2 extent{ 0u };
        float metresPerWorldUnit = 1.0f;
    };

    struct ViewMotionThresholds {
        float maxTranslationMetres = 10.0f;
        float maxRotationDegrees = 45.0f;
    };

    enum class ViewCutReason : uint8_t {
        None = 0,
        FirstTurn,           // no previous turn for this set (or a new identity)
        RequestedRevision,
        Explicit,
        Projection,          // kind or extent
        Translation,
        Rotation,
    };

    struct ViewMotionResult {
        RenderGraph::ViewHistoryContext history{};
        ViewCutReason cut = ViewCutReason::None;
        // Turns this view has rendered since its last cut (0 on a cut turn).
        uint64_t turnsSinceCut = 0;
    };

    class ViewMotionTracker {
    public:
        explicit ViewMotionTracker(ViewMotionThresholds thresholds = {}) noexcept
            : thresholds_(thresholds) {}

        [[nodiscard]] ViewMotionResult update(const ViewMotionInput& input);
        void reset() noexcept { states_ = {}; }

    private:
        struct State {
            bool valid = false;
            uint64_t identity = 0;
            uint64_t requestedResetRevision = 0;
            glm::mat4 view{ 1.0f };
            uint32_t projectionKind = 0;
            glm::uvec2 extent{ 0u };
            uint64_t resetRevision = 0;
            uint64_t turnsSinceCut = 0;
        };

        ViewMotionThresholds thresholds_{};
        std::array<State, RenderGraph::HistoryViewSetCount> states_{};
    };

} // namespace Iridium
