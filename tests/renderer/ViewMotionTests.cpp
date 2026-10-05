// M9 G2: per-retained-view cut detection (ViewMotionTracker).
#include "renderer/rhi/ViewMotion.h"

#include <cmath>
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

namespace {
    // M9 G5b: the Halton(2,3) jitter sequence and its projection.
    void jitterSequence() {
        constexpr uint32_t TemporalJitterSequenceLength = DefaultTemporalJitterSequenceLength;
        CHECK(TemporalJitterSequenceLength == 8);
        // A longer sequence (supersampled references) keeps distinct phases.
        CHECK(temporalJitterPixels(8, 16) != temporalJitterPixels(0, 16));
        CHECK(temporalJitterPixels(16, 16) == temporalJitterPixels(0, 16));
        CHECK(temporalJitterPixels(5, 0) == temporalJitterPixels(0, 1));   // clamped
        glm::vec2 sum(0.0f);
        for (uint32_t index = 0; index < TemporalJitterSequenceLength; ++index) {
            const glm::vec2 pixels = temporalJitterPixels(index);
            CHECK(pixels.x > -0.5f && pixels.x < 0.5f);
            CHECK(pixels.y > -0.5f && pixels.y < 0.5f);
            // Distinct phases.
            for (uint32_t other = 0; other < index; ++other)
                CHECK(temporalJitterPixels(other) != pixels);
            sum += pixels;
            // The sequence repeats.
            CHECK(temporalJitterPixels(index + TemporalJitterSequenceLength) == pixels);
        }
        // Halton(2,3) phase 1: (1/2, 1/3) - 0.5.
        CHECK(temporalJitterPixels(0) == glm::vec2(0.0f, 1.0f / 3.0f - 0.5f));
        // Well spread: the mean offset stays near the pixel centre.
        CHECK(std::abs(sum.x) / TemporalJitterSequenceLength < 0.07f);
        CHECK(std::abs(sum.y) / TemporalJitterSequenceLength < 0.07f);
        // NDC conversion and zero extent.
        const glm::vec2 ndc = temporalJitterNdc(0, { 3840, 2160 });
        CHECK(std::abs(ndc.x - 2.0f * temporalJitterPixels(0).x / 3840.0f) < 1e-9f);
        CHECK(temporalJitterNdc(3, { 0, 2160 }) == glm::vec2(0.0f));

        // The jittered projection shifts clip xy by jitter * w and nothing else.
        const glm::mat4 projection = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
        CHECK(jitterProjection(projection, glm::vec2(0.0f)) == projection);
        const glm::vec2 jitter{ 1e-3f, -2e-3f };
        const glm::mat4 jittered = jitterProjection(projection, jitter);
        const glm::vec4 point{ 0.3f, -0.2f, -5.0f, 1.0f };
        const glm::vec4 a = projection * point;
        const glm::vec4 b = jittered * point;
        CHECK(std::abs(b.x - (a.x + jitter.x * a.w)) < 1e-6f);
        CHECK(std::abs(b.y - (a.y + jitter.y * a.w)) < 1e-6f);
        CHECK(b.z == a.z && b.w == a.w);
    }

    // M9 G5b: the tracker reports the previous turn's view-projection and the
    // jitter phase per view; a cut restarts both.
    void previousViewProjectionAndJitter() {
        ViewMotionTracker tracker;
        const glm::mat4 projection = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
        ViewMotionInput input = sceneInput(lookFrom({ 0, 1, 5 }, { 0, 0, 0 }));
        input.projection = projection;
        input.jitter = true;
        ViewMotionResult first = tracker.update(input);
        CHECK(first.previousViewProjection == projection * input.view);   // first turn
        CHECK(first.jitterIndex == 0);
        CHECK(first.jitterNdc == temporalJitterNdc(0, input.extent));
        CHECK(first.previousJitterNdc == first.jitterNdc);
        const glm::mat4 firstViewProjection = projection * input.view;
        input.view = lookFrom({ 0.05f, 1, 5 }, { 0, 0, 0 });
        ViewMotionResult second = tracker.update(input);
        CHECK(second.previousViewProjection == firstViewProjection);
        CHECK(second.jitterIndex == 1);
        CHECK(second.previousJitterNdc == first.jitterNdc);
        // Without jitter the offsets are exactly zero.
        input.jitter = false;
        const ViewMotionResult off = tracker.update(input);
        CHECK(off.jitterNdc == glm::vec2(0.0f));
        // A cut restarts the sequence and drops the previous matrix.
        input.jitter = true;
        input.explicitCut = true;
        const ViewMotionResult cut = tracker.update(input);
        CHECK(cut.jitterIndex == 0);
        CHECK(cut.previousViewProjection == projection * input.view);
        CHECK(cut.previousJitterNdc == cut.jitterNdc);
    }
}

int main() {
    continuousMotionKeepsHistory();
    discontinuitiesCut();
    viewsAreIndependent();
    jitterSequence();
    previousViewProjectionAndJitter();
    if (failures == 0) std::cout << "ViewMotionTests passed\n";
    return failures == 0 ? 0 : 1;
}
