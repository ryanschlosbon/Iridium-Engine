// M9 G2: per-retained-view cut detection (ViewMotionTracker).
#include "renderer/rhi/ViewMotion.h"

#include <iostream>

#include <glm/gtc/matrix_transform.hpp>

using namespace Iridium;

namespace {
    int failures = 0;
#define CHECK(expression) do { if (!(expression)) { \
    std::cerr << "check failed: " #expression " (line " << __LINE__ << ")\n"; \
    ++failures; } } while (false)

    glm::mat4 lookFrom(glm::vec3 position, glm::vec3 target) {
        return glm::lookAt(position, target, glm::vec3(0.0f, 1.0f, 0.0f));
    }

    ViewMotionInput sceneInput(glm::mat4 view) {
        return { .historySet = 0, .identity = 1, .view = view, .extent = { 1920, 1080 } };
    }

    void continuousMotionKeepsHistory() {
        ViewMotionTracker tracker;
        ViewMotionResult result = tracker.update(sceneInput(lookFrom({ 0, 1, 5 }, { 0, 0, 0 })));
        CHECK(result.cut == ViewCutReason::FirstTurn);
        CHECK(result.history.identity == 1 && result.history.historySet == 0);
        const uint64_t revision = result.history.resetRevision;
        for (int step = 1; step <= 10; ++step) {
            // 5 cm and about 1 degree per turn: ordinary motion.
            result = tracker.update(sceneInput(lookFrom({ 0.05f * step, 1, 5 },
                { 0.09f * step, 0, 0 })));
            CHECK(result.cut == ViewCutReason::None);
            CHECK(result.turnsSinceCut == static_cast<uint64_t>(step));
            CHECK(result.history.resetRevision == revision);
        }
    }

    void discontinuitiesCut() {
        ViewMotionTracker tracker;
        (void)tracker.update(sceneInput(lookFrom({ 0, 1, 5 }, { 0, 0, 0 })));
        ViewMotionResult result = tracker.update(sceneInput(lookFrom({ 20, 1, 5 }, { 20, 0, 0 })));
        CHECK(result.cut == ViewCutReason::Translation);
        CHECK(result.history.resetRevision == 1);
        CHECK(result.turnsSinceCut == 0);
        // Metres per unit scale the translation threshold.
        ViewMotionInput scaled = sceneInput(lookFrom({ 25, 1, 5 }, { 25, 0, 0 }));
        scaled.metresPerWorldUnit = 0.01f;   // 5 units = 5 cm
        CHECK(tracker.update(scaled).cut == ViewCutReason::None);
        // A 90-degree turn in one turn.
        result = tracker.update(sceneInput(lookFrom({ 25, 1, 5 }, { 35, 1, 5 })));
        CHECK(result.cut == ViewCutReason::Rotation);
        CHECK(result.history.resetRevision == 2);
        // Requested revision (benchmark cut) and explicit cut.
        ViewMotionInput requested = sceneInput(lookFrom({ 25, 1, 5 }, { 35, 1, 5 }));
        requested.requestedResetRevision = 1;
        CHECK(tracker.update(requested).cut == ViewCutReason::RequestedRevision);
        CHECK(tracker.update(requested).cut == ViewCutReason::None);
        requested.explicitCut = true;
        result = tracker.update(requested);
        CHECK(result.cut == ViewCutReason::Explicit);
        CHECK(result.history.resetRevision == 4);
        // Extent or projection kind.
        requested.explicitCut = false;
        requested.extent = { 1280, 720 };
        CHECK(tracker.update(requested).cut == ViewCutReason::Projection);
        requested.projectionKind = 1;
        CHECK(tracker.update(requested).cut == ViewCutReason::Projection);
        CHECK(tracker.update(requested).cut == ViewCutReason::None);
    }

    void viewsAreIndependent() {
        ViewMotionTracker tracker;
        const glm::mat4 sceneView = lookFrom({ 0, 1, 5 }, { 0, 0, 0 });
        ViewMotionInput preview{ .historySet = 1, .identity = 7,
            .view = lookFrom({ 100, 50, 0 }, { 0, 0, 0 }), .extent = { 1920, 1080 } };
        (void)tracker.update(sceneInput(sceneView));
        CHECK(tracker.update(preview).cut == ViewCutReason::FirstTurn);
        // Alternating: neither view sees the other's camera as a jump.
        for (int turn = 0; turn < 4; ++turn) {
            CHECK(tracker.update(sceneInput(sceneView)).cut == ViewCutReason::None);
            const ViewMotionResult result = tracker.update(preview);
            CHECK(result.cut == ViewCutReason::None);
            CHECK(result.history.historySet == 1 && result.history.identity == 7);
        }
        // A new preview session is a first turn for set 1 only.
        preview.identity = 8;
        CHECK(tracker.update(preview).cut == ViewCutReason::FirstTurn);
        CHECK(tracker.update(sceneInput(sceneView)).cut == ViewCutReason::None);
        bool threw = false;
        try { (void)tracker.update({ .historySet = RenderGraph::HistoryViewSetCount, .identity = 1 }); }
        catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
    }
}

int main() {
    continuousMotionKeepsHistory();
    discontinuitiesCut();
    viewsAreIndependent();
    if (failures == 0) std::cout << "ViewMotionTests passed\n";
    return failures == 0 ? 0 : 1;
}
