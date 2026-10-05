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
        };
    }

} // namespace Iridium
