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
    // M9 G5b/G6c: jitter sequence lengths (phases before it repeats).
    inline constexpr uint32_t DefaultTemporalJitterSequenceLength = 8;
    inline constexpr uint32_t MaximumTemporalJitterSequenceLength = 4096;

    struct ViewMotionInput {
        uint32_t historySet = 0;
        uint64_t identity = 0;
        uint64_t requestedResetRevision = 0;
        bool explicitCut = false;
        glm::mat4 view{ 1.0f };
        glm::mat4 projection{ 1.0f };   // unjittered
        uint32_t projectionKind = 0;
        glm::uvec2 extent{ 0u };
        float metresPerWorldUnit = 1.0f;
        // M9 G5b: produce this turn's sub-pixel jitter.
        bool jitter = false;
        // Phases before the sequence repeats (M9 G6c: long sequences build
        // supersampled references; TAA compares 8 and 16).
        uint32_t jitterSequenceLength = DefaultTemporalJitterSequenceLength;
        // M9.5: the frame's clock (seconds, monotonic; the caller decides
        // whether it is wall or simulated time). Only differences are used.
        double timeSeconds = 0.0;
    };

    // M9 G5b: the jitter sequence. Halton(2,3) phases, indexed by the view's
    // turns since its last cut (never wall time), as an NDC offset.
    [[nodiscard]] glm::vec2 temporalJitterPixels(uint32_t sequenceIndex,
        uint32_t sequenceLength = DefaultTemporalJitterSequenceLength) noexcept;
    [[nodiscard]] glm::vec2 temporalJitterNdc(uint32_t sequenceIndex,
        glm::uvec2 extent,
        uint32_t sequenceLength = DefaultTemporalJitterSequenceLength) noexcept;
    // The projection with an NDC translation `jitterNdc` applied after it:
    // clip.xy += jitterNdc * clip.w. Returns `projection` itself for zero jitter.
    [[nodiscard]] glm::mat4 jitterProjection(const glm::mat4& projection,
        glm::vec2 jitterNdc) noexcept;

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
        // The previous turn's unjittered view-projection (this turn's on a cut).
        glm::mat4 previousViewProjection{ 1.0f };
        uint32_t jitterIndex = 0;
        glm::vec2 jitterNdc{ 0.0f };
        glm::vec2 previousJitterNdc{ 0.0f };
        // M9.5: clock time since this view's previous turn, also across cuts
        // (adapted exposure survives them); 0 on a first turn.
        float deltaSeconds = 0.0f;
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
            glm::mat4 viewProjection{ 1.0f };
            uint32_t projectionKind = 0;
            glm::uvec2 extent{ 0u };
            uint64_t resetRevision = 0;
            uint64_t turnsSinceCut = 0;
            glm::vec2 jitterNdc{ 0.0f };
            double timeSeconds = 0.0;
        };

        ViewMotionThresholds thresholds_{};
        std::array<State, RenderGraph::HistoryViewSetCount> states_{};
    };

} // namespace Iridium
