#include "renderer/rhi/DepthPyramid.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

using namespace Iridium;

namespace {
    int failures = 0;
#define CHECK(expression) do { if (!(expression)) { \
    std::cerr << "check failed: " #expression " (line " << __LINE__ << ")\n"; \
    ++failures; } } while (false)

    void extentAndOddEdgeCoverage() {
        const DepthPyramidExtent base{ 5, 3 };
        CHECK(depthPyramidMipCount(base) == 3u);
        CHECK(depthPyramidMipExtent(base, 0) == base);
        CHECK(depthPyramidMipExtent(base, 1) == (DepthPyramidExtent{ 2, 1 }));
        CHECK(depthPyramidMipExtent(base, 2) == (DepthPyramidExtent{ 1, 1 }));

        std::vector<float> depths(15, 0.2f);
        depths[14] = 0.9f; // Bottom-right odd edge must reach every later mip.
        DepthPyramidReference pyramid;
        pyramid.build(base, DeviceDepthConvention::ForwardZeroToOne, depths);
        CHECK(pyramid.mipCount() == 3u);
        CHECK(pyramid.mip(0).size() == 15u);
        CHECK(pyramid.mip(1).size() == 2u);
        CHECK(pyramid.mip(2).size() == 1u);
        CHECK(std::abs(pyramid.mip(2)[0] - 0.9f) < 1.0e-6f);
    }

    void conservativeForwardOcclusion() {
        std::array<float, 16> depths{};
        depths.fill(0.25f);
        DepthPyramidReference pyramid;
        pyramid.build({ 4, 4 }, DeviceDepthConvention::ForwardZeroToOne,
            depths);
        const DepthPyramidOcclusionResult hidden = pyramid.test({
            .minimumX = 0, .minimumY = 0, .maximumX = 4, .maximumY = 4,
            .nearestDepth = 0.7f, .depthBias = 0.01f });
        CHECK(hidden.tested);
        CHECK(hidden.occluded);
        CHECK(hidden.mipLevel == 1u);
        CHECK(hidden.sampledTexels == 4u);
        CHECK(std::abs(hidden.farthestOccluderDepth - 0.25f) < 1.0e-6f);

        depths[15] = 1.0f; // A far-depth hole must force the whole query visible.
        pyramid.build({ 4, 4 }, DeviceDepthConvention::ForwardZeroToOne,
            depths);
        const auto visible = pyramid.test({ 0, 0, 4, 4, 0.7f, 0.01f });
        CHECK(visible.tested);
        CHECK(!visible.occluded);
        CHECK(std::abs(visible.farthestOccluderDepth - 1.0f) < 1.0e-6f);

        const auto equalDepth = pyramid.test({ 0, 0, 1, 1, 0.25f, 0.0f });
        CHECK(equalDepth.tested);
        CHECK(!equalDepth.occluded); // Avoid self-occlusion at equal depth.

        const auto packed = packDepthPyramidDeviceQuery(
            {3, 4, 17, 19, 0.625f, 0.0002f});
        CHECK(packed.minimumX == 3u && packed.minimumY == 4u);
        CHECK(packed.maximumX == 17u && packed.maximumY == 19u);
        CHECK(packed.nearestDepth == 0.625f);
        CHECK(packed.depthBias == 0.0002f);
        CHECK(packed.reserved0 == 0u && packed.reserved1 == 0u);
    }

    void reverseDepthAndInvalidSourceFailVisible() {
        std::array<float, 4> reverseDepths{ 0.8f, 0.8f, 0.8f, 0.8f };
        DepthPyramidReference pyramid;
        pyramid.build({ 2, 2 }, DeviceDepthConvention::ReverseZeroToOne,
            reverseDepths);
        CHECK(pyramid.test({ 0, 0, 2, 2, 0.3f, 0.01f }).occluded);

        reverseDepths[3] = std::numeric_limits<float>::quiet_NaN();
        pyramid.build({ 2, 2 }, DeviceDepthConvention::ReverseZeroToOne,
            reverseDepths);
        const auto visible = pyramid.test({ 0, 0, 2, 2, 0.3f, 0.01f });
        CHECK(visible.tested);
        CHECK(!visible.occluded); // Invalid source becomes far plane, never a hole.

        const auto invalidQuery = pyramid.test({ 0, 0, 3, 2, 0.3f, 0.0f });
        CHECK(!invalidQuery.tested);
        CHECK(!invalidQuery.occluded);
    }

    void oddMipQueriesPreserveHoles() {
        std::vector<float> depths(15, 0.2f);
        depths[4] = 1.0f;
        DepthPyramidReference pyramid;
        pyramid.build({ 15, 1 }, DeviceDepthConvention::ForwardZeroToOne, depths);
        CHECK(!pyramid.test({ 0, 0, 5, 1, 0.7f, 0.0f }).occluded);
        CHECK(depthPyramidMipExtent({ 15, 3 }, UINT32_MAX) ==
            (DepthPyramidExtent{ 1, 1 }));
    }

    void outOfRangeSourceFailsVisible() {
        DepthPyramidReference pyramid;
        for (float invalid : { -0.1f, 1.1f,
                std::numeric_limits<float>::infinity() }) {
            const std::array depths{ invalid };
            for (const auto convention : { DeviceDepthConvention::ForwardZeroToOne,
                    DeviceDepthConvention::ReverseZeroToOne }) {
                pyramid.build({ 1, 1 }, convention, depths);
                CHECK(!pyramid.test({ 0, 0, 1, 1, 0.5f, 0.0f }).occluded);
            }
        }
    }

    // A hole anywhere inside a rectangle must prevent rejection, regardless of
    // the selected mip or non-power-of-two footprint. Check against base pixels.
    void exhaustiveRectangleCoverage() {
        for (const auto convention : { DeviceDepthConvention::ForwardZeroToOne,
                DeviceDepthConvention::ReverseZeroToOne }) {
            for (const auto extent : { DepthPyramidExtent{ 15, 3 },
                    DepthPyramidExtent{ 7, 5 }, DepthPyramidExtent{ 1, 15 } }) {
                const bool forward = convention == DeviceDepthConvention::ForwardZeroToOne;
                std::vector<float> depths(extent.width * extent.height);
                DepthPyramidReference pyramid;
                for (uint32_t hole = 0; hole < depths.size(); ++hole) {
                    std::fill(depths.begin(), depths.end(), forward ? 0.2f : 0.8f);
                    depths[hole] = forward ? 1.0f : 0.0f;
                    pyramid.build(extent, convention, depths);
                    for (uint32_t y0 = 0; y0 < extent.height; ++y0)
                    for (uint32_t y1 = y0 + 1; y1 <= extent.height; ++y1)
                    for (uint32_t x0 = 0; x0 < extent.width; ++x0)
                    for (uint32_t x1 = x0 + 1; x1 <= extent.width; ++x1) {
                        const auto result = pyramid.test({ x0, y0, x1, y1, 0.5f, 0.0f });
                        CHECK(result.tested);
                        CHECK(result.sampledTexels <= 4u);
                        const bool containsHole = hole % extent.width >= x0 &&
                            hole % extent.width < x1 && hole / extent.width >= y0 &&
                            hole / extent.width < y1;
                        if (containsHole && result.occluded) {
                            CHECK(false);
                            return;
                        }
                    }
                }
            }
        }
    }

    void historyEligibilityFailsVisible() {
        DepthPyramidHistoryInput input{
            .currentExtent = { 3840, 2160 },
            .historyExtent = { 3840, 2160 },
            .currentOwner = {1, 1, 1},
            .historyOwner = {1, 1, 1},
            .currentFrameSerial = 2,
            .historyFrameSerial = 1,
            .enabled = true,
            .historyAvailable = true,
            .projectionValid = true,
        };
        CHECK(evaluateDepthPyramidHistory(input).eligible);
        CHECK(evaluateDepthPyramidHistory(input).rejection ==
            DepthPyramidHistoryRejection::None);

        input.enabled = false;
        CHECK(evaluateDepthPyramidHistory(input).rejection ==
            DepthPyramidHistoryRejection::Disabled);
        input.enabled = true;
        input.historyAvailable = false;
        CHECK(evaluateDepthPyramidHistory(input).rejection ==
            DepthPyramidHistoryRejection::HistoryUnavailable);
        input.historyAvailable = true;
        input.projectionValid = false;
        CHECK(evaluateDepthPyramidHistory(input).rejection ==
            DepthPyramidHistoryRejection::InvalidProjection);
        input.projectionValid = true;

        input.cameraCut = true;
        CHECK(evaluateDepthPyramidHistory(input).rejection ==
            DepthPyramidHistoryRejection::CameraCut);
        input.cameraCut = false;
        input.rapidMotion = true;
        CHECK(evaluateDepthPyramidHistory(input).rejection ==
            DepthPyramidHistoryRejection::RapidMotion);
        input.rapidMotion = false;
        input.nearPlaneIntersection = true;
        CHECK(evaluateDepthPyramidHistory(input).rejection ==
            DepthPyramidHistoryRejection::NearPlaneIntersection);
        input.nearPlaneIntersection = false;
        input.newlyResident = true;
        CHECK(evaluateDepthPyramidHistory(input).rejection ==
            DepthPyramidHistoryRejection::NewlyResident);
        input.newlyResident = false;
        input.largeTransformChange = true;
        CHECK(evaluateDepthPyramidHistory(input).rejection ==
            DepthPyramidHistoryRejection::LargeTransformChange);
        input.largeTransformChange = false;
        input.historyExtent.width = 1920;
        CHECK(evaluateDepthPyramidHistory(input).rejection ==
            DepthPyramidHistoryRejection::ExtentMismatch);
        input.historyExtent = input.currentExtent;
        input.currentFrameSerial = 3;
        CHECK(evaluateDepthPyramidHistory(input).rejection ==
            DepthPyramidHistoryRejection::StaleHistory);
        input.currentFrameSerial = 2;
        input.currentExtent = {};
        CHECK(evaluateDepthPyramidHistory(input).rejection ==
            DepthPyramidHistoryRejection::InvalidExtent);
    }

    void historyOwnershipAndSerials() {
        DepthPyramidHistoryInput input{
            .currentExtent = {127,73}, .historyExtent = {127,73},
            .currentOwner = {42,7,100,3,2}, .historyOwner = {42,7,100,3,2},
            .currentFrameSerial = 11, .historyFrameSerial = 10,
            .enabled = true, .historyAvailable = true, .projectionValid = true};
        const auto valid = input;
        CHECK(evaluateDepthPyramidHistory(input).eligible);
        CHECK(evaluateDepthPyramidHistory(input).abiVersion == 2);
        input.currentOwner = {};
        CHECK(evaluateDepthPyramidHistory(input).rejection == DepthPyramidHistoryRejection::InvalidOwnership);
        input = valid; input.historyOwner = {};
        CHECK(evaluateDepthPyramidHistory(input).rejection == DepthPyramidHistoryRejection::InvalidOwnership);
        input = valid; ++input.currentOwner.viewIdentity; // Scene/viewer switch or reopened viewer.
        CHECK(evaluateDepthPyramidHistory(input).rejection == DepthPyramidHistoryRejection::ViewMismatch);
        input = valid; ++input.currentOwner.sceneEpoch; // Replaced world, same view and extent.
        CHECK(evaluateDepthPyramidHistory(input).rejection == DepthPyramidHistoryRejection::SceneMismatch);
        input = valid; ++input.currentOwner.depthContentRevision; // A different occluder moved/vanished.
        CHECK(evaluateDepthPyramidHistory(input).rejection == DepthPyramidHistoryRejection::DepthContentChanged);
        input = valid; ++input.currentOwner.projectionRevision;
        CHECK(evaluateDepthPyramidHistory(input).rejection == DepthPyramidHistoryRejection::ProjectionChanged);
        input = valid; ++input.currentOwner.resetRevision;
        CHECK(evaluateDepthPyramidHistory(input).rejection == DepthPyramidHistoryRejection::CameraCut);
        input = valid; input.currentConvention = DeviceDepthConvention::ReverseZeroToOne;
        CHECK(evaluateDepthPyramidHistory(input).rejection == DepthPyramidHistoryRejection::DepthConventionMismatch);
        input.historyConvention = input.currentConvention;
        CHECK(evaluateDepthPyramidHistory(input).eligible);
        input.currentConvention = input.historyConvention = static_cast<DeviceDepthConvention>(99);
        CHECK(evaluateDepthPyramidHistory(input).rejection == DepthPyramidHistoryRejection::DepthConventionMismatch);
        for (auto serials : {std::array<uint64_t,2>{0,0}, {10,10}, {9,10}, {12,10}, {0,UINT64_MAX}}) {
            input = valid; input.currentFrameSerial = serials[0]; input.historyFrameSerial = serials[1];
            CHECK(evaluateDepthPyramidHistory(input).rejection == DepthPyramidHistoryRejection::StaleHistory);
        }
        input = valid; input.currentFrameSerial = UINT64_MAX; input.historyFrameSerial = UINT64_MAX-1;
        CHECK(evaluateDepthPyramidHistory(input).eligible);
        // Alternating retained views cannot reuse two-submission-old depth even
        // when neither camera moved and each considers this its next local frame.
        input = valid; input.currentFrameSerial += 1;
        CHECK(evaluateDepthPyramidHistory(input).rejection == DepthPyramidHistoryRejection::StaleHistory);
        input.historyFrameSerial = input.currentFrameSerial - 1;
        CHECK(evaluateDepthPyramidHistory(input).eligible);
    }

    void completionSafeViewPublication() {
        DepthPyramidHistoryPublicationTracker tracker;
        tracker.reset(2, 2, 7);
        CHECK(!tracker.published(0).available);
        CHECK(!tracker.published(1).available);
        const DepthPyramidHistoryOwner scene{1, 20, 30, 40, 2};
        const DepthPyramidHistoryOwner asset{9, 9, 90, 80, 3};

        tracker.schedule(0, 0, scene, {127, 73},
            DeviceDepthConvention::ForwardZeroToOne, 10);
        CHECK(tracker.queued(0).available);
        CHECK(tracker.queued(0).submissionSerial == 10);
        CHECK(!tracker.published(0).available);
        tracker.schedule(1, 0, scene, {127, 73},
            DeviceDepthConvention::ForwardZeroToOne, 11);
        CHECK(tracker.queued(0).submissionSerial == 11);
        tracker.complete(0, 10);
        CHECK(!tracker.published(0).available);
        CHECK(tracker.queued(0).submissionSerial == 11);
        tracker.complete(1, 11);
        CHECK(tracker.published(0).available);
        CHECK(tracker.published(0).submissionSerial == 11);
        CHECK(tracker.published(0).imageGeneration == 7);
        CHECK(tracker.published(0).owner.viewIdentity == 1);

        tracker.schedule(0, 1, asset, {127, 73},
            DeviceDepthConvention::ForwardZeroToOne, 12);
        tracker.complete(0, 11);
        CHECK(!tracker.published(1).available);
        tracker.complete(0, 12);
        CHECK(tracker.published(1).available);
        CHECK(tracker.published(1).submissionSerial == 12);
        CHECK(tracker.published(1).owner.viewIdentity == 9);
        CHECK(tracker.published(0).submissionSerial == 11);

        tracker.reset(2, 2, 8);
        CHECK(!tracker.queued(0).available);
        CHECK(!tracker.queued(1).available);
        CHECK(!tracker.published(0).available);
        CHECK(!tracker.published(1).available);
    }

    void projectedBoundsAreConservative() {
        DepthPyramidProjectionInput input;
        input.extent = {128, 128};
        input.worldToClip = {1,0,0,0, 0,1,0,0, 0,0,-100.0f/99,-100.0f/99, 0,0,-1,0};
        input.minimumWorld = {-1,-1,-6};
        input.maximumWorld = {1,1,-4};
        auto projected = projectDepthPyramidBounds(input);
        CHECK(projected.eligible);
        CHECK(projected.query.minimumX == 47);
        CHECK(projected.query.minimumY == 47);
        CHECK(projected.query.maximumX == 81);
        CHECK(projected.query.maximumY == 81);
        CHECK(projected.query.nearestDepth <= 100.0f/99 - 100.0f/396);
        DepthPyramidReference pyramid;
        std::vector<float> depth(128 * 128, .2f);
        pyramid.build(input.extent, input.convention, depth);
        CHECK(pyramid.test(projected.query).occluded);
        depth[64 * 128 + 64] = 1.0f;
        pyramid.build(input.extent, input.convention, depth);
        CHECK(!pyramid.test(projected.query).occluded);

        input.convention = DeviceDepthConvention::ReverseZeroToOne;
        input.worldToClip[10] = 1.0f/99;
        input.worldToClip[11] = 100.0f/99;
        projected = projectDepthPyramidBounds(input);
        CHECK(projected.eligible);
        CHECK(projected.query.nearestDepth >= 24.0f/99);
        std::fill(depth.begin(), depth.end(), .8f);
        pyramid.build(input.extent, input.convention, depth);
        CHECK(pyramid.test(projected.query).occluded);
        depth[64 * 128 + 64] = 0;
        pyramid.build(input.extent, input.convention, depth);
        CHECK(!pyramid.test(projected.query).occluded);

        const auto original = input;
        input.maximumWorld[2] = -.5f;
        CHECK(projectDepthPyramidBounds(input).rejection == DepthPyramidProjectionRejection::ClipPlaneIntersection);
        input = original;
        input.minimumWorld[0] = 10;
        input.maximumWorld[0] = 11;
        CHECK(projectDepthPyramidBounds(input).rejection == DepthPyramidProjectionRejection::OutsideView);
        input = original;
        input.minimumWorld[0] = -.001f;
        input.maximumWorld[0] = .001f;
        CHECK(projectDepthPyramidBounds(input).rejection == DepthPyramidProjectionRejection::SmallBounds);
        input = original;
        input.extent = {};
        CHECK(projectDepthPyramidBounds(input).rejection == DepthPyramidProjectionRejection::InvalidExtent);
        input = original;
        input.minimumWorld[0] = 2;
        CHECK(projectDepthPyramidBounds(input).rejection == DepthPyramidProjectionRejection::InvalidBounds);
        input = original;
        input.worldToClip[0] = std::numeric_limits<float>::quiet_NaN();
        CHECK(projectDepthPyramidBounds(input).rejection == DepthPyramidProjectionRejection::InvalidProjection);
        input = original;
        input.guardPixels = -1;
        CHECK(projectDepthPyramidBounds(input).rejection == DepthPyramidProjectionRejection::InvalidSettings);
    }

    void projectionContainsInteriorSamples() {
        for (const auto extent : {DepthPyramidExtent{127, 73}, DepthPyramidExtent{3840, 2160}})
        for (const bool reverse : {false, true})
        for (const bool orthographic : {false, true})
        for (const float yScale : {-1.0f, 1.0f}) {
            DepthPyramidProjectionInput input;
            input.extent = extent;
            input.convention = reverse ? DeviceDepthConvention::ReverseZeroToOne : DeviceDepthConvention::ForwardZeroToOne;
            input.worldToClip = orthographic
                ? std::array<float,16>{.2f,0,0,.001f, 0,yScale*.2f,0,-.002f,
                    0,0,reverse ? .01f : -.01f,reverse ? 1.0f : 0.0f, 0,0,0,1}
                : std::array<float,16>{1,0,.001f,0, 0,yScale,-.002f,0,
                    0,0,reverse ? 1.0f/99 : -100.0f/99,reverse ? 100.0f/99 : -100.0f/99, 0,0,-1,0};
            input.minimumWorld = {-2,-1,-7};
            input.maximumWorld = {1,2,-3};
            const auto result = projectDepthPyramidBounds(input);
            CHECK(result.eligible);
            for (unsigned x = 0; x <= 4; ++x)
            for (unsigned y = 0; y <= 4; ++y)
            for (unsigned z = 0; z <= 4; ++z) {
                const std::array<double,4> world{-2 + x*.75, -1 + y*.75, -7 + double(z), 1};
                std::array<double,4> clip{};
                for (unsigned row = 0; row < 4; ++row)
                for (unsigned column = 0; column < 4; ++column)
                    clip[row] += input.worldToClip[row*4+column] * world[column];
                const double px = (clip[0]/clip[3]*.5+.5)*extent.width;
                const double py = (clip[1]/clip[3]*.5+.5)*extent.height;
                const double depth = clip[2]/clip[3];
                CHECK(px >= result.query.minimumX && px < result.query.maximumX);
                CHECK(py >= result.query.minimumY && py < result.query.maximumY);
                CHECK(reverse ? depth <= result.query.nearestDepth : depth >= result.query.nearestDepth);
            }
        }
    }
}

int main() {
    extentAndOddEdgeCoverage();
    conservativeForwardOcclusion();
    reverseDepthAndInvalidSourceFailVisible();
    historyEligibilityFailsVisible();
    historyOwnershipAndSerials();
    completionSafeViewPublication();
    oddMipQueriesPreserveHoles();
    outOfRangeSourceFailsVisible();
    exhaustiveRectangleCoverage();
    projectedBoundsAreConservative();
    projectionContainsInteriorSamples();
    if (failures == 0) std::cout << "DepthPyramidTests passed\n";
    return failures == 0 ? 0 : 1;
}
