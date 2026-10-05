#include "renderer/rhi/ViewMotion.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Iridium {

    namespace {
        // World-space camera position of a view (world-to-view) matrix.
        glm::vec3 cameraPosition(const glm::mat4& view) noexcept {
            const glm::mat3 rotation(view);
            return -(glm::transpose(rotation) * glm::vec3(view[3]));
        }

        // Angle of the relative rotation between two view matrices, degrees.
        float rotationDegrees(const glm::mat4& previous, const glm::mat4& current) noexcept {
            const glm::mat3 relative = glm::mat3(current) * glm::transpose(glm::mat3(previous));
            const float trace = relative[0][0] + relative[1][1] + relative[2][2];
            const float cosine = std::clamp((trace - 1.0f) * 0.5f, -1.0f, 1.0f);
            return glm::degrees(std::acos(cosine));
        }
    }

    namespace {
        float radicalInverse(uint32_t index, uint32_t base) noexcept {
            float result = 0.0f;
            float fraction = 1.0f / static_cast<float>(base);
            while (index > 0) {
                result += static_cast<float>(index % base) * fraction;
                index /= base;
                fraction /= static_cast<float>(base);
            }
            return result;
        }
    }

    glm::vec2 temporalJitterPixels(uint32_t sequenceIndex, uint32_t sequenceLength) noexcept {
        // Halton index 1-based (index 0 would be the pixel corner); centred
        // on the pixel, so offsets lie in (-0.5, 0.5).
        const uint32_t length = std::clamp(sequenceLength, 1u,
            MaximumTemporalJitterSequenceLength);
        const uint32_t index = sequenceIndex % length + 1u;
        return { radicalInverse(index, 2) - 0.5f, radicalInverse(index, 3) - 0.5f };
    }

    glm::vec2 temporalJitterNdc(uint32_t sequenceIndex, glm::uvec2 extent,
        uint32_t sequenceLength) noexcept {
        if (extent.x == 0 || extent.y == 0) return glm::vec2(0.0f);
        const glm::vec2 pixels = temporalJitterPixels(sequenceIndex, sequenceLength);
        return { 2.0f * pixels.x / static_cast<float>(extent.x),
            2.0f * pixels.y / static_cast<float>(extent.y) };
    }

    glm::mat4 jitterProjection(const glm::mat4& projection, glm::vec2 jitterNdc) noexcept {
        if (jitterNdc == glm::vec2(0.0f)) return projection;
        glm::mat4 translation(1.0f);
        translation[3][0] = jitterNdc.x;
        translation[3][1] = jitterNdc.y;
        return translation * projection;
    }

    ViewMotionResult ViewMotionTracker::update(const ViewMotionInput& input) {
        if (input.historySet >= states_.size())
            throw std::invalid_argument("View history set is out of range");
        State& state = states_[input.historySet];

        ViewCutReason cut = ViewCutReason::None;
        if (!state.valid || state.identity != input.identity) {
            cut = ViewCutReason::FirstTurn;
        }
        else if (state.requestedResetRevision != input.requestedResetRevision) {
            cut = ViewCutReason::RequestedRevision;
        }
        else if (input.explicitCut) {
            cut = ViewCutReason::Explicit;
        }
        else if (state.projectionKind != input.projectionKind || state.extent != input.extent) {
            cut = ViewCutReason::Projection;
        }
        else {
            const float metres = glm::length(cameraPosition(input.view) -
                cameraPosition(state.view)) * std::max(input.metresPerWorldUnit, 0.0f);
            if (metres > thresholds_.maxTranslationMetres) cut = ViewCutReason::Translation;
            else if (rotationDegrees(state.view, input.view) > thresholds_.maxRotationDegrees)
                cut = ViewCutReason::Rotation;
        }

        // A first turn needs no new revision (nothing is valid yet); every
        // other cut advances the view's revision.
        if (cut != ViewCutReason::None && cut != ViewCutReason::FirstTurn)
            ++state.resetRevision;
        state.turnsSinceCut = cut == ViewCutReason::None ? state.turnsSinceCut + 1 : 0;
        const glm::mat4 viewProjection = input.projection * input.view;
        const bool continuous = cut == ViewCutReason::None;
        const glm::mat4 previousViewProjection = continuous ? state.viewProjection : viewProjection;
        const uint32_t sequenceLength = std::clamp(input.jitterSequenceLength, 1u,
            MaximumTemporalJitterSequenceLength);
        const uint32_t jitterIndex = static_cast<uint32_t>(
            state.turnsSinceCut % sequenceLength);
        const glm::vec2 jitterNdc = input.jitter
            ? temporalJitterNdc(jitterIndex, input.extent, sequenceLength)
            : glm::vec2(0.0f);
        const glm::vec2 previousJitterNdc = continuous ? state.jitterNdc : jitterNdc;
        const double elapsed = cut == ViewCutReason::FirstTurn
            ? 0.0 : input.timeSeconds - state.timeSeconds;
        const float deltaSeconds = std::isfinite(elapsed) && elapsed > 0.0
            ? static_cast<float>(elapsed) : 0.0f;
        state.timeSeconds = input.timeSeconds;
        state.viewProjection = viewProjection;
        state.jitterNdc = jitterNdc;
        state.valid = true;
        state.identity = input.identity;
        state.requestedResetRevision = input.requestedResetRevision;
        state.view = input.view;
        state.projectionKind = input.projectionKind;
        state.extent = input.extent;

        return {
            .history = { input.identity, state.resetRevision, input.historySet },
            .cut = cut,
            .turnsSinceCut = state.turnsSinceCut,
            .previousViewProjection = previousViewProjection,
            .jitterIndex = jitterIndex,
            .jitterNdc = jitterNdc,
            .previousJitterNdc = previousJitterNdc,
            .deltaSeconds = deltaSeconds,
        };
    }

} // namespace Iridium
