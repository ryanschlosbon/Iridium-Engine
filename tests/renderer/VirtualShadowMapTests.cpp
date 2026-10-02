#include "renderer/rhi/VirtualShadowMap.h"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
#include <chrono>
#include <algorithm>

namespace {

    using namespace Iridium;

    #define CHECK(condition) do { if (!(condition)) { \
        std::cerr << "check failed: " #condition " at line " << __LINE__ \
            << '\n'; return false; } } while (false)

    SceneEntityUuid owner(uint32_t ordinal) {
        std::string text = "019fb73d-5a60-7000-8000-000000000000";
        text.back() = static_cast<char>('0' + ordinal);
        return *SceneEntityUuid::parse(text);
    }

    VirtualShadowPageAddress address(uint32_t ordinal, uint32_t x = 0) {
        return {
            .lightOwner = owner(ordinal),
            .projectionRevision = 7,
            .pageX = x,
        };
    }

    VirtualShadowPageRequest request(uint32_t ordinal, uint32_t x = 0,
        int32_t priority = 0, uint32_t receiverSamples = 1) {
        return {
            .address = address(ordinal, x),
            .staticCasterRevision = 11,
            .dynamicCasterRevision = 13,
            .receiverSamples = receiverSamples,
            .priority = priority,
        };
    }

    DirectionalVirtualShadowClipLevel clipLevel(uint8_t level,
        int32_t priority = 0) {
        return {
            .lightOwner = owner(1),
            .projectionRevision = 17,
            .staticCasterRevision = 23,
            .dynamicCasterRevision = 29,
            .virtualResolutionTexels = 1'024,
            .priority = priority,
            .level = level,
        };
    }

    const VirtualShadowPageMapping& mappingFor(
        const VirtualShadowPagePlan& plan,
        const VirtualShadowPageAddress& wanted) {
        for (const VirtualShadowPageMapping& mapping : plan.mappings)
            if (mapping.address == wanted) return mapping;
        throw std::runtime_error("mapping not found");
    }

    bool policyValidationFreezesBoundedPages() {
        CHECK(validateVirtualShadowPagePolicy({}).empty());
        VirtualShadowPagePolicy policy{};
        policy.pageSizeTexels = 96;
        CHECK(!validateVirtualShadowPagePolicy(policy).empty());
        policy.pageSizeTexels = 128;
        policy.borderTexels = 64;
        CHECK(!validateVirtualShadowPagePolicy(policy).empty());
        policy.borderTexels = 4;
        policy.maximumPhysicalPages = 2;
        policy.maximumPageUpdatesPerFrame = 3;
        CHECK(!validateVirtualShadowPagePolicy(policy).empty());
        return true;
    }

    bool resourceValidationFreezesQualificationStorage() {
        CHECK(validateVirtualShadowResourceConfig({}).empty());
        VirtualShadowResourceConfig config{};
        config.pageSizeTexels = 96;
        CHECK(!validateVirtualShadowResourceConfig(config).empty());
        config.pageSizeTexels = 128;
        config.borderTexels = 0;
        CHECK(!validateVirtualShadowResourceConfig(config).empty());
        config.borderTexels = 33;
        CHECK(!validateVirtualShadowResourceConfig(config).empty());
        config.borderTexels = 4;
        config.physicalPageCapacity = 0;
        CHECK(!validateVirtualShadowResourceConfig(config).empty());
        config.physicalPageCapacity = 1'024;
        config.pageTableEntryCapacity = 1'023;
        CHECK(!validateVirtualShadowResourceConfig(config).empty());
        config.pageTableEntryCapacity = 65'536;
        config.receiverMarkCapacity = 65'535;
        CHECK(!validateVirtualShadowResourceConfig(config).empty());
        config.receiverMarkCapacity = 65'536;
        config.compactedRequestCapacity = 65'537;
        CHECK(!validateVirtualShadowResourceConfig(config).empty());
        config.compactedRequestCapacity = 4'096;
        const auto layout = buildVirtualShadowGpuWorkingSetLayout(config, 256);
        CHECK(layout.scratchCapacity == 65'536);
        CHECK(layout.clipLevels.size ==
            16 * sizeof(PackedDirectionalVirtualShadowClipLevel));
        CHECK(layout.receivers.size ==
            65'536 * sizeof(PackedDirectionalVirtualShadowReceiver));
        CHECK(layout.rawMarks.size ==
            65'536 * sizeof(PackedDirectionalVirtualShadowGpuMark));
        CHECK(layout.alternateRequests.size ==
            65'536 * sizeof(PackedDirectionalVirtualShadowGpuRequest));
        CHECK(layout.denseRequests.size ==
            65'536 * sizeof(PackedDirectionalVirtualShadowGpuRequest));
        CHECK(layout.outputRequests.size ==
            4'096 * sizeof(PackedDirectionalVirtualShadowGpuRequest));
        CHECK(layout.telemetry.size == sizeof(
            PackedDirectionalVirtualShadowGpuCompactionTelemetry));
        CHECK(layout.totalBytes == 8'521'248);
        CHECK(layout.receivers.offset % 256 == 0);
        CHECK(layout.telemetry.offset % 256 == 0);
        return true;
    }

    bool receiverMarksDeduplicateAndCommitBeforeSampling() {
        VirtualShadowPagePolicy policy{};
        policy.maximumPhysicalPages = 4;
        policy.maximumPageUpdatesPerFrame = 2;
        auto first = request(1, 3, 2, 5);
        auto duplicate = first;
        duplicate.receiverSamples = 7;
        std::vector<VirtualShadowPageRequest> requests{ duplicate, first };
        VirtualShadowPagePlan plan = buildVirtualShadowPagePlan(
            policy, requests, {}, 20);
        CHECK(plan.uniqueRequests == 1);
        CHECK(plan.pagesAllocated == 1);
        CHECK(plan.pagesToRender == 1);
        CHECK(plan.cacheHits == 0);
        CHECK(plan.mappings.size() == 1);
        CHECK(plan.mappings[0].receiverSamples == 12);
        CHECK(plan.mappings[0].dynamicCasterRevision == 13);
        CHECK(plan.mappings[0].state ==
            VirtualShadowPageState::PendingRaster);
        CHECK(plan.nextResidency[0].validLayers ==
            VirtualShadowPageLayerNone);

        const uint32_t completed[] = { plan.mappings[0].physicalPage };
        commitVirtualShadowPageUpdates(plan, completed, 21);
        CHECK(plan.mappings[0].state ==
            VirtualShadowPageState::CachedSampleable);
        CHECK(plan.nextResidency[0].validLayers ==
            VirtualShadowPageLayerAll);
        CHECK(plan.nextResidency[0].lastRenderedFrame == 21);

        VirtualShadowPageRequest stable = duplicate;
        stable.receiverSamples = 1;
        VirtualShadowPagePlan reused = buildVirtualShadowPagePlan(
            policy, std::span(&stable, 1), plan.nextResidency, 22);
        CHECK(reused.cacheHits == 1);
        CHECK(reused.pagesToRender == 0);
        CHECK(reused.mappings[0].state ==
            VirtualShadowPageState::CachedSampleable);
        return true;
    }

    bool directionalReceiverMarksChooseFinestAndDeduplicate() {
        const std::vector<DirectionalVirtualShadowClipLevel> levels{
            clipLevel(1, 4), clipLevel(0, 10)
        };
        const std::vector<DirectionalVirtualShadowReceiverSample> receivers{
            { .worldPosition = { 0.0f, 0.0f, 0.5f },
                .receiverSamples = 2 },
            { .worldPosition = { 0.02f, 0.02f, 0.5f },
                .receiverSamples = 3 },
        };
        const auto marks = buildDirectionalVirtualShadowReceiverMarks(
            {}, levels, receivers);
        CHECK(marks.requests.size() == 1);
        CHECK(marks.uniquePagesBeforeCapacity == 1);
        CHECK(marks.inputReceiverSamples == 5);
        CHECK(marks.markedReceiverSamples == 5);
        CHECK(marks.unmappedReceiverSamples == 0);
        CHECK(marks.requests[0].address.levelOrFace == 0);
        CHECK(marks.requests[0].address.pageX == 4);
        CHECK(marks.requests[0].address.pageY == 4);
        CHECK(marks.requests[0].receiverSamples == 5);
        CHECK(marks.requests[0].priority == 10);

        VirtualShadowPagePolicy policy{};
        policy.maximumPhysicalPages = 2;
        policy.maximumPageUpdatesPerFrame = 2;
        const VirtualShadowPagePlan residency = buildVirtualShadowPagePlan(
            policy, marks.requests, {}, 1);
        CHECK(residency.pagesToRender == 1);
        CHECK(residency.mappings[0].state ==
            VirtualShadowPageState::PendingRaster);
        return true;
    }

    bool directionalGuardBandFallsBackWithoutEdgeGap() {
        const std::vector<DirectionalVirtualShadowClipLevel> levels{
            clipLevel(0, 10), clipLevel(1, 5)
        };
        const DirectionalVirtualShadowReceiverSample edge{
            .worldPosition = { 0.9f, 0.0f, 0.5f },
            .receiverSamples = 1,
        };
        DirectionalVirtualShadowMarkConfig config{};
        config.finerLevelGuardBandPages = 1;
        const auto marks = buildDirectionalVirtualShadowReceiverMarks(
            config, levels, std::span(&edge, 1));
        CHECK(marks.requests.size() == 1);
        CHECK(marks.requests[0].address.levelOrFace == 1);
        CHECK(marks.requests[0].address.pageX == 7);
        CHECK(marks.unmappedReceiverSamples == 0);
        return true;
    }

    bool directionalWorldPageIdentitySurvivesSnappedClipScroll() {
        DirectionalVirtualShadowClipLevel baseline = clipLevel(0, 10);
        DirectionalVirtualShadowClipLevel scrolled = baseline;
        scrolled.worldToShadowClip[3][0] = -0.25f;
        scrolled.worldPageOriginX = 1;
        const DirectionalVirtualShadowReceiverSample receiver{
            .worldPosition = { 0.25f, 0.0f, 0.5f },
            .receiverSamples = 1,
        };
        const auto baselineMarks =
            buildDirectionalVirtualShadowReceiverMarks(
                {}, std::span(&baseline, 1), std::span(&receiver, 1));
        const auto scrolledMarks =
            buildDirectionalVirtualShadowReceiverMarks(
                {}, std::span(&scrolled, 1), std::span(&receiver, 1));
        CHECK(baselineMarks.requests.size() == 1);
        CHECK(scrolledMarks.requests.size() == 1);
        CHECK(baselineMarks.requests[0].address ==
            scrolledMarks.requests[0].address);
        CHECK(baselineMarks.requests[0].address.pageX == 5);
        return true;
    }

    bool directionalMarksReportInvalidOutsideAndCapacityOverflow() {
        const std::vector<DirectionalVirtualShadowClipLevel> levels{
            clipLevel(0, 3)
        };
        const std::vector<DirectionalVirtualShadowReceiverSample> receivers{
            { .worldPosition = { -0.75f, 0.0f, 0.5f },
                .receiverSamples = 1 },
            { .worldPosition = { 0.0f, 0.0f, 0.5f },
                .receiverSamples = 5 },
            { .worldPosition = { 0.75f, 0.0f, 0.5f },
                .receiverSamples = 3 },
            { .worldPosition = { 0.0f, 0.0f, -0.1f },
                .receiverSamples = 7 },
            { .worldPosition = {
                    std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.5f },
                .receiverSamples = 11 },
            { .worldPosition = { 0.0f, 0.0f, 0.5f },
                .receiverSamples = 0 },
        };
        DirectionalVirtualShadowMarkConfig config{};
        config.maximumUniquePageRequests = 2;
        config.finerLevelGuardBandPages = 0;
        const auto marks = buildDirectionalVirtualShadowReceiverMarks(
            config, levels, receivers);
        CHECK(marks.inputReceiverSamples == 27);
        CHECK(marks.markedReceiverSamples == 9);
        CHECK(marks.unmappedReceiverSamples == 18);
        CHECK(marks.invalidReceivers == 1);
        CHECK(marks.uniquePagesBeforeCapacity == 3);
        CHECK(marks.requestCapacityOverflow == 1);
        CHECK(marks.requestCapacityDroppedSamples == 1);
        CHECK(marks.requests.size() == 2);
        CHECK(marks.requests[0].receiverSamples == 5);
        CHECK(marks.requests[1].receiverSamples == 3);
        return true;
    }

    bool directionalMarkingRejectsIncoherentClipContracts() {
        auto fine = clipLevel(0);
        auto coarse = clipLevel(1);
        coarse.projectionRevision = 18;
        bool rejected = false;
        try {
            const std::vector<DirectionalVirtualShadowClipLevel> levels{
                fine, coarse
            };
            (void)buildDirectionalVirtualShadowReceiverMarks(
                {}, levels, {});
        }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
        return true;
    }

    bool conflictingMarksFailAndStaticCachingCanBeDisabled() {
        VirtualShadowPagePolicy policy{};
        policy.maximumPhysicalPages = 2;
        policy.maximumPageUpdatesPerFrame = 1;
        VirtualShadowPageRequest first = request(1);
        VirtualShadowPageRequest conflict = first;
        conflict.dynamicCasterRevision = 14;
        bool rejected = false;
        try {
            const std::vector<VirtualShadowPageRequest> requests{
                first, conflict
            };
            (void)buildVirtualShadowPagePlan(policy, requests, {}, 1);
        }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);

        VirtualShadowResidentPage resident{
            .address = first.address,
            .staticCasterRevision = first.staticCasterRevision,
            .dynamicCasterRevision = first.dynamicCasterRevision,
            .physicalPage = 0,
            .validLayers = VirtualShadowPageLayerAll,
        };
        policy.cacheStaticCasters = false;
        const VirtualShadowPagePlan plan = buildVirtualShadowPagePlan(
            policy, std::span(&first, 1), std::span(&resident, 1), 2);
        CHECK(plan.cacheHits == 0);
        CHECK(plan.mappings[0].updateLayers ==
            VirtualShadowPageLayerStatic);
        return true;
    }

    bool dynamicRevisionInvalidatesOnlyDynamicLayer() {
        VirtualShadowPagePolicy policy{};
        policy.maximumPhysicalPages = 2;
        policy.maximumPageUpdatesPerFrame = 1;
        const VirtualShadowPageRequest original = request(1);
        VirtualShadowResidentPage resident{
            .address = original.address,
            .staticCasterRevision = original.staticCasterRevision,
            .dynamicCasterRevision = original.dynamicCasterRevision,
            .lastUsedFrame = 9,
            .lastRenderedFrame = 9,
            .physicalPage = 0,
            .validLayers = VirtualShadowPageLayerAll,
        };
        VirtualShadowPageRequest moved = original;
        moved.dynamicCasterRevision = 99;
        VirtualShadowPagePlan plan = buildVirtualShadowPagePlan(
            policy, std::span(&moved, 1), std::span(&resident, 1), 10);
        CHECK(plan.mappings[0].updateLayers ==
            VirtualShadowPageLayerDynamic);
        CHECK(plan.mappings[0].state ==
            VirtualShadowPageState::PendingRaster);
        CHECK(plan.nextResidency[0].validLayers ==
            VirtualShadowPageLayerStatic);
        const uint32_t completed[] = { 0 };
        commitVirtualShadowPageUpdates(plan, completed, 10);
        CHECK(plan.nextResidency[0].validLayers ==
            VirtualShadowPageLayerAll);
        return true;
    }

    bool updateBudgetUsesPriorityAndFailsVisible() {
        VirtualShadowPagePolicy policy{};
        policy.maximumPhysicalPages = 2;
        policy.maximumPageUpdatesPerFrame = 1;
        const std::vector<VirtualShadowPageRequest> requests{
            request(1, 0, 1, 100), request(2, 0, 10, 1)
        };
        const VirtualShadowPagePlan plan = buildVirtualShadowPagePlan(
            policy, requests, {}, 1);
        CHECK(plan.pagesToRender == 1);
        CHECK(plan.missingPages == 1);
        CHECK(plan.updateBudgetOverflow == 1);
        CHECK(mappingFor(plan, address(2)).state ==
            VirtualShadowPageState::PendingRaster);
        const auto& missing = mappingFor(plan, address(1));
        CHECK(missing.state == VirtualShadowPageState::Missing);
        CHECK(missing.physicalPage == InvalidVirtualShadowPhysicalPage);
        CHECK(missing.missingAction ==
            VirtualShadowMissingPageAction::ConventionalShadowFallback);
        return true;
    }

    bool evictionIsDeterministicAndProtectsRequestedPages() {
        VirtualShadowPagePolicy policy{};
        policy.maximumPhysicalPages = 2;
        policy.maximumPageUpdatesPerFrame = 2;
        const VirtualShadowPageRequest a = request(1);
        const VirtualShadowPageRequest b = request(2);
        std::vector<VirtualShadowResidentPage> current{
            { .address = a.address, .staticCasterRevision = 11,
                .dynamicCasterRevision = 13, .lastUsedFrame = 1,
                .lastRenderedFrame = 1, .physicalPage = 0,
                .lastPriority = 1, .validLayers = VirtualShadowPageLayerAll },
            { .address = b.address, .staticCasterRevision = 11,
                .dynamicCasterRevision = 13, .lastUsedFrame = 2,
                .lastRenderedFrame = 2, .physicalPage = 1,
                .lastPriority = 5, .validLayers = VirtualShadowPageLayerAll },
        };
        const VirtualShadowPageRequest c = request(3, 0, 20);
        const std::vector<VirtualShadowPageRequest> requests{ b, c };
        const VirtualShadowPagePlan plan = buildVirtualShadowPagePlan(
            policy, requests, current, 3);
        CHECK(plan.cacheHits == 1);
        CHECK(plan.pagesEvicted == 1);
        CHECK(mappingFor(plan, c.address).physicalPage == 0);
        CHECK(mappingFor(plan, b.address).physicalPage == 1);
        return true;
    }

    bool fullRequestedPoolReportsOverflowWithoutStaleSampling() {
        VirtualShadowPagePolicy policy{};
        policy.maximumPhysicalPages = 2;
        policy.maximumPageUpdatesPerFrame = 2;
        const VirtualShadowPageRequest a = request(1);
        const VirtualShadowPageRequest b = request(2);
        const VirtualShadowPageRequest c = request(3);
        std::vector<VirtualShadowResidentPage> current{
            { .address = a.address, .staticCasterRevision = 11,
                .dynamicCasterRevision = 13, .physicalPage = 0,
                .validLayers = VirtualShadowPageLayerAll },
            { .address = b.address, .staticCasterRevision = 11,
                .dynamicCasterRevision = 13, .physicalPage = 1,
                .validLayers = VirtualShadowPageLayerAll },
        };
        const std::vector<VirtualShadowPageRequest> requests{ a, b, c };
        const VirtualShadowPagePlan plan = buildVirtualShadowPagePlan(
            policy, requests, current, 4);
        CHECK(plan.cacheHits == 2);
        CHECK(plan.physicalPoolOverflow == 1);
        CHECK(plan.missingPages == 1);
        CHECK(mappingFor(plan, c.address).state ==
            VirtualShadowPageState::Missing);
        return true;
    }

    bool fullViewPageGridIsBoundedAndStable() {
        DirectionalVirtualShadowClipLevel fine{};
        fine.lightOwner = owner(1); fine.projectionRevision = 1;
        fine.virtualResolutionTexels = 16'384; fine.worldPageOriginX = -128;
        fine.worldPageOriginY = -64;
        auto coarse = fine; coarse.level = 1;
        const std::vector levels{fine, coarse};
        auto grid = buildVirtualShadowFullViewPageGrid({}, levels, 65'536);
        CHECK(grid.cellCount == 32'768);
        CHECK(grid.levelOffsets == std::vector<uint32_t>({0, 16'384}));
        fine.worldPageOriginX += 1;
        grid = buildVirtualShadowFullViewPageGrid({}, std::span(&fine, 1), 65'536);
        CHECK(grid.cellCount == 16'384);
        auto rejects = [&](auto stack, uint32_t capacity) {
            try { (void)buildVirtualShadowFullViewPageGrid({}, stack, capacity); }
            catch (const std::invalid_argument&) { return true; }
            return false;
        };
        CHECK(rejects(std::span(levels), 32'767));
        fine.worldPageOriginX = std::numeric_limits<int32_t>::max();
        CHECK(rejects(std::span(&fine, 1), 65'536));
        fine.worldPageOriginX = int64_t{std::numeric_limits<int32_t>::min()} - 1;
        CHECK(rejects(std::span(&fine, 1), 65'536));
        fine.worldPageOriginX = 0; fine.virtualResolutionTexels = 65'536;
        CHECK(rejects(std::span(&fine, 1), 65'536));
        return true;
    }

    bool depthReceiversPreservePixelsAndRejectUnsafeRegions() {
        VirtualShadowDepthReceiverRegion region{3, 2, 1, 0, 2, 2, false};
        const std::vector<float> depth{0.25f, 0.5f, 1.0f, 0.5f, 0.0f,
            std::numeric_limits<float>::quiet_NaN()};
        auto output = buildVirtualShadowDepthReceivers(region, glm::mat4(1), depth);
        CHECK(output.size() == 4);
        CHECK(output[0].receiverSamples == 1 && output[0].worldPosition.x == 0.0f);
        CHECK(output[0].worldPosition.y == -0.5f && output[0].worldPosition.z == 0.5f);
        CHECK(output[1].receiverSamples == 0);
        CHECK(output[2].receiverSamples == 1 && output[2].worldPosition.z == 0.0f);
        CHECK(output[3].receiverSamples == 0);
        region.reverseDepth = true;
        output = buildVirtualShadowDepthReceivers(region, glm::mat4(1), depth);
        CHECK(output[1].receiverSamples == 1 && output[1].worldPosition.z == 1.0f);
        CHECK(output[2].receiverSamples == 0);
        output = buildVirtualShadowDepthReceivers(region, glm::mat4(0), depth);
        for (const auto& receiver : output) CHECK(receiver.receiverSamples == 0);
        auto rejects = [&](VirtualShadowDepthReceiverRegion bad, uint32_t cap) {
            try { (void)validateVirtualShadowDepthReceiverRegion(bad, cap); }
            catch (const std::invalid_argument&) { return true; }
            return false;
        };
        CHECK(rejects(region, 3));
        auto bad = region; bad.originX = 3; CHECK(rejects(bad, 65'536));
        bad = region; bad.width = 0; CHECK(rejects(bad, 65'536));
        bad = {3'840, 2'160, 0, 0, 3'840, 2'160, false};
        CHECK(rejects(bad, 65'536));
        return true;
    }

    bool cameraDrivenClipsPreserveWorldPages() {
        DirectionalVirtualShadowClipConfig config{};
        config.lightOwner = owner(1); config.projectionRevision = 17;
        config.staticCasterRevision = 23; config.dynamicCasterRevision = 29;
        config.lightForward = {0, 0, 1}; config.focusWorld = {0.125f, 0.125f, 0};
        config.lightDepthMinimum = -10; config.lightDepthMaximum = 10;
        const auto original = buildDirectionalVirtualShadowClips(config);
        CHECK(original.levelCount == 4);
        CHECK(original.clips[0].worldPageOriginX == -64);
        CHECK(original.worldUnitsPerPage[0] == 0.25);
        CHECK(original.worldUnitsPerTexel[0] == 32.0 / 16'384);
        CHECK(buildVirtualShadowFullViewPageGrid({}, original.levels(), 65'536).cellCount == 65'536);
        for (uint32_t i = 1; i < 4; ++i)
            CHECK(original.worldUnitsPerTexel[i] == original.worldUnitsPerTexel[i - 1] * 2);
        config.focusWorld.x += 0.05f;
        const auto subpage = buildDirectionalVirtualShadowClips(config);
        CHECK(subpage.clips[0].worldToShadowClip == original.clips[0].worldToShadowClip);
        config.focusWorld = {0.375f, 0.125f, 100};
        const auto scrolled = buildDirectionalVirtualShadowClips(config);
        CHECK(scrolled.clips[0].worldPageOriginX == original.clips[0].worldPageOriginX + 1);
        const std::array receivers{DirectionalVirtualShadowReceiverSample{{0.1f, 0.1f, 0}, 1}};
        const auto before = buildDirectionalVirtualShadowReceiverMarks({}, original.levels(), receivers);
        const auto after = buildDirectionalVirtualShadowReceiverMarks({}, scrolled.levels(), receivers);
        CHECK(before.requests.size() == 1 && after.requests.size() == 1);
        CHECK(before.requests[0].address == after.requests[0].address);
        CHECK(after.requests[0].staticCasterRevision == 23);
        CHECK(after.requests[0].dynamicCasterRevision == 29);
        CHECK(scrolled.clips[0].worldToShadowClip[3][2] == original.clips[0].worldToShadowClip[3][2]);
        config.focusWorld.x = -0.125f;
        CHECK(buildDirectionalVirtualShadowClips(config).clips[0].worldPageOriginX == -65);
        const auto rejects = [](DirectionalVirtualShadowClipConfig bad) {
            try { (void)buildDirectionalVirtualShadowClips(bad); }
            catch (const std::invalid_argument&) { return true; }
            return false;
        };
        auto bad = config; bad.lightForward = {}; CHECK(rejects(bad));
        bad = config; bad.levelCount = 17; CHECK(rejects(bad));
        bad = config; bad.lightDepthMaximum = bad.lightDepthMinimum; CHECK(rejects(bad));
        bad = config; bad.projectionRevision = 0; CHECK(rejects(bad));
        bad = config; bad.focusWorld.x = std::numeric_limits<float>::infinity(); CHECK(rejects(bad));
        bad = config; bad.finestWorldSpan = 1e-100; CHECK(rejects(bad));
        bad = config; bad.finestWorldSpan = 1e100; CHECK(rejects(bad));
        bad = config; bad.virtualResolutionTexels = 384; CHECK(rejects(bad));
        config.lightForward = {0, -1, 0};
        CHECK(buildDirectionalVirtualShadowClips(config).levelCount == 4);
        return true;
    }

    bool livePublisherOwnsConservativeDepthRevisions() {
        DirectionalVirtualShadowClipConfig config{};
        config.lightOwner = owner(1); config.lightForward = {0, 0, 1};
        config.staticCasterRevision = 11; config.dynamicCasterRevision = 13;
        DirectionalVirtualShadowClipPublisher publisher;
        std::array bounds{VirtualShadowCasterBounds{{0, 0, 10}, 5}};
        auto first = publisher.publish(config, bounds);
        CHECK(first && first->clips[0].projectionRevision == 1);
        config.focusWorld.x = 1;
        auto scrolled = publisher.publish(config, bounds);
        CHECK(scrolled && scrolled->clips[0].projectionRevision == 1);
        config.staticCasterRevision = 19; config.dynamicCasterRevision = 23;
        auto changedCaster = publisher.publish(config, bounds);
        CHECK(changedCaster && changedCaster->clips[0].projectionRevision == 1);
        CHECK(changedCaster->clips[0].staticCasterRevision == 19);
        CHECK(changedCaster->clips[0].dynamicCasterRevision == 23);
        bounds[0] = {{0, 0, 2'000}, 50};
        auto expanded = publisher.publish(config, bounds);
        CHECK(expanded && expanded->clips[0].projectionRevision == 2);
        for (float z : {1'950.0f, 2'050.0f}) {
            const auto point = expanded->clips[0].worldToShadowClip * glm::vec4(0, 0, z, 1);
            CHECK(point.z >= 0 && point.z <= 1);
        }
        bounds[0] = {{0, 0, 10}, 5};
        auto shrunk = publisher.publish(config, bounds);
        CHECK(shrunk && shrunk->clips[0].projectionRevision == 2);
        CHECK(shrunk->clips[0].worldToShadowClip[3][2] == expanded->clips[0].worldToShadowClip[3][2]);
        bounds[0].radiusWorld = -1;
        CHECK(!publisher.publish(config, bounds));
        bounds[0].radiusWorld = 5;
        CHECK(publisher.publish(config, bounds)->clips[0].projectionRevision == 2);
        config.lightForward = {0, 1, 0};
        CHECK(publisher.publish(config, bounds)->clips[0].projectionRevision == 3);
        config.lightOwner = owner(2);
        CHECK(publisher.publish(config, bounds)->clips[0].projectionRevision == 4);
        publisher.reset();
        CHECK(publisher.publish(config, bounds)->clips[0].projectionRevision == 5);
        bool rejected = false;
        try { (void)publisher.publish(config, bounds, 0); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
        CHECK(publisher.publish(config, bounds)->clips[0].projectionRevision == 5);
        return true;
    }

    bool clipPackingPreservesAbiAndRejectsOverflow() {
        auto source = clipLevel(3, -7);
        source.worldPageOriginX = -123; source.worldPageOriginY = 456;
        source.mip = 65'535; source.requiredLayers = VirtualShadowPageLayerStatic;
        const auto packed = packDirectionalVirtualShadowClipLevel(source);
        CHECK(packed.worldToShadowClip == source.worldToShadowClip);
        CHECK(packed.worldPageOriginX == -123 && packed.worldPageOriginY == 456);
        CHECK(packed.levelAndMip == (3u | (65'535u << 8u)));
        CHECK(packed.priority == -7 && packed.requiredLayers == VirtualShadowPageLayerStatic);
        CHECK(packed.reserved0 == 0 && packed.reserved1 == 0);
        source.worldPageOriginX = int64_t{INT32_MAX} + 1;
        bool rejected = false;
        try { (void)packDirectionalVirtualShadowClipLevel(source); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
        return true;
    }

    bool pageRasterRegionsPreserveBordersAndScroll() {
        const auto atlas = buildVirtualShadowAtlasLayout(128, 4, 10, 4096);
        CHECK(atlas.tilesPerRow == 4 && atlas.widthTexels == 544 && atlas.heightTexels == 544);
        CHECK(atlas.tileFootprintTexels == 136);
        auto clip = clipLevel(0);
        clip.worldPageOriginX = -4; clip.worldPageOriginY = -3;
        VirtualShadowPageMapping mapping{
            .address = {.lightOwner = clip.lightOwner, .projectionRevision = clip.projectionRevision},
            .staticCasterRevision = clip.staticCasterRevision,
            .dynamicCasterRevision = clip.dynamicCasterRevision,
            .physicalPage = 5,
            .state = VirtualShadowPageState::PendingRaster,
            .requiredLayers = VirtualShadowPageLayerAll,
            .updateLayers = VirtualShadowPageLayerDynamic};
        const auto region = buildDirectionalVirtualShadowPageRasterRegion(atlas, clip, mapping);
        CHECK(region.originXTexels == 136 && region.originYTexels == 136 && region.extentTexels == 136);
        CHECK(region.updateLayers == VirtualShadowPageLayerDynamic);
        const auto left = region.worldToPageClip * glm::vec4(0, -0.125f, 0.5f, 1);
        const auto right = region.worldToPageClip * glm::vec4(0.25f, -0.125f, 0.5f, 1);
        CHECK(std::abs(left.x + 128.0f / 136) < 1e-6f);
        CHECK(std::abs(right.x - 128.0f / 136) < 1e-6f);
        CHECK(left.z == 0.5f && right.z == 0.5f);
        const auto border = region.worldToPageClip * glm::vec4(-4.0f / 512, -0.125f, 0.5f, 1);
        CHECK(std::abs(border.x + 1.0f) < 1e-6f);
        CHECK(std::abs(region.pageUvToAtlasUv.z - 140.0f / 544) < 1e-6f);
        CHECK(std::abs(region.pageUvToAtlasUv.z + region.pageUvToAtlasUv.x - 268.0f / 544) < 1e-6f);
        --clip.worldPageOriginX;
        clip.worldToShadowClip[3][0] = 0.25f;
        const auto scrolled = buildDirectionalVirtualShadowPageRasterRegion(atlas, clip, mapping);
        for (uint32_t c = 0; c < 4; ++c) for (uint32_t r = 0; r < 4; ++r)
            CHECK(std::abs(scrolled.worldToPageClip[c][r] - region.worldToPageClip[c][r]) < 1e-6f);
        CHECK(virtualShadowPageMayContainCaster(region, {{0.125f, -0.125f, 0.5f}, 0.01f}));
        CHECK(virtualShadowPageMayContainCaster(region, {{-0.006f, -0.125f, 0.5f}, 0}));
        CHECK(!virtualShadowPageMayContainCaster(region, {{-0.01f, -0.125f, 0.5f}, 0}));
        CHECK(virtualShadowPageMayContainCaster(region, {{-0.01f, -0.125f, 0.5f}, 0.01f}));
        CHECK(!virtualShadowPageMayContainCaster(region, {{1, -0.125f, 0.5f}, 0.01f}));
        CHECK(!virtualShadowPageMayContainCaster(region, {{0.125f, -0.125f, -0.1f}, 0.01f}));
        CHECK(virtualShadowPageMayContainCaster(region, {{1, 0, 0}, -1}));
        auto invalid = region;
        invalid.worldToPageClip[3][2] = std::numeric_limits<float>::quiet_NaN();
        CHECK(virtualShadowPageMayContainCaster(invalid, {{100, 0, 0}, 0}));
        invalid.worldToPageClip = glm::mat4(0);
        CHECK(virtualShadowPageMayContainCaster(invalid, {{100, 0, 0}, 0}));
        return true;
    }

    bool pageRasterRejectsStaleAndInvalidMappings() {
        const auto atlas = buildVirtualShadowAtlasLayout(128, 4, 10, 4096);
        const auto clip = clipLevel(0);
        VirtualShadowPageMapping mapping{
            .address = {.lightOwner = clip.lightOwner, .projectionRevision = clip.projectionRevision},
            .staticCasterRevision = clip.staticCasterRevision,
            .dynamicCasterRevision = clip.dynamicCasterRevision,
            .physicalPage = 0,
            .state = VirtualShadowPageState::PendingRaster,
            .requiredLayers = VirtualShadowPageLayerAll,
            .updateLayers = VirtualShadowPageLayerAll};
        const auto rejects = [&](const VirtualShadowPageMapping& candidate) {
            try { (void)buildDirectionalVirtualShadowPageRasterRegion(atlas, clip, candidate); }
            catch (const std::invalid_argument&) { return true; }
            return false;
        };
        CHECK(!rejects(mapping));
        auto bad = mapping; ++bad.address.projectionRevision; CHECK(rejects(bad));
        bad = mapping; bad.address.lightOwner = owner(2); CHECK(rejects(bad));
        bad = mapping; ++bad.dynamicCasterRevision; CHECK(rejects(bad));
        bad = mapping; ++bad.staticCasterRevision; CHECK(rejects(bad));
        bad = mapping; bad.address.projection = VirtualShadowProjection::Spot; CHECK(rejects(bad));
        bad = mapping; ++bad.address.mip; CHECK(rejects(bad));
        bad = mapping; ++bad.address.levelOrFace; CHECK(rejects(bad));
        bad = mapping; bad.address.pageX = -1; CHECK(rejects(bad));
        bad = mapping; bad.address.pageY = 8; CHECK(rejects(bad));
        bad = mapping; bad.physicalPage = 10; CHECK(rejects(bad));
        bad = mapping; bad.state = VirtualShadowPageState::CachedSampleable; CHECK(rejects(bad));
        bad = mapping; bad.state = VirtualShadowPageState::Missing; CHECK(rejects(bad));
        bad = mapping; bad.updateLayers = VirtualShadowPageLayerNone; CHECK(rejects(bad));
        bad = mapping; bad.requiredLayers = VirtualShadowPageLayerStatic; CHECK(rejects(bad));
        bad.updateLayers = VirtualShadowPageLayerStatic;
        ++bad.dynamicCasterRevision;
        CHECK(!rejects(bad)); // Unrequested dynamic content cannot invalidate static work.
        bool rejected = false;
        try { (void)buildVirtualShadowAtlasLayout(128, 4, 10, 543); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
        auto brokenAtlas = atlas; ++brokenAtlas.tileFootprintTexels;
        rejected = false;
        try { (void)buildDirectionalVirtualShadowPageRasterRegion(brokenAtlas, clip, mapping); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
        auto overflowingClip = clip;
        overflowingClip.worldToShadowClip[0][0] = std::numeric_limits<float>::max();
        rejected = false;
        try { (void)buildDirectionalVirtualShadowPageRasterRegion(atlas, overflowingClip, mapping); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
        return true;
    }

    bool residencyPackingPreservesFullIdentityAndValidity() {
        VirtualShadowResidentPage page{
            .address = {.lightOwner = owner(1), .projectionRevision = 0xfedcba9876543210ull,
                .pageX = std::numeric_limits<int64_t>::min(), .pageY = std::numeric_limits<int64_t>::max(),
                .mip = 0xabcd, .levelOrFace = 5, .projection = VirtualShadowProjection::PointFace},
            .staticCasterRevision = 0x123456789abcdef0ull, .dynamicCasterRevision = 0xfedcba9876543210ull,
            .lastUsedFrame = 0x100000002ull, .lastRenderedFrame = 0x300000004ull,
            .physicalPage = 31, .lastPriority = std::numeric_limits<int32_t>::min(),
            .validLayers = VirtualShadowPageLayerStatic};
        const auto packed = packVirtualShadowResidentPage(page, 32);
        CHECK(packed.key.ownerWords[0] == 0x3db79f01u);
        CHECK(packed.key.signedPageWords[0] == 0 && packed.key.signedPageWords[1] == 0x80000000u);
        CHECK(packed.key.signedPageWords[2] == 0xffffffffu && packed.key.signedPageWords[3] == 0x7fffffffu);
        CHECK(packed.key.projectionDescriptor == 0x0205abcdu);
        const auto restored = unpackVirtualShadowResidentPage(packed, 32);
        CHECK(restored.address == page.address && restored.staticCasterRevision == page.staticCasterRevision);
        CHECK(restored.dynamicCasterRevision == page.dynamicCasterRevision && restored.lastUsedFrame == page.lastUsedFrame);
        CHECK(restored.lastRenderedFrame == page.lastRenderedFrame && restored.physicalPage == page.physicalPage);
        CHECK(restored.lastPriority == page.lastPriority && restored.validLayers == page.validLayers);
        for (int64_t coordinate : {int64_t{-1}, int64_t{0}, int64_t{1}, int64_t{1} << 40}) {
            page.address.pageX = coordinate;
            CHECK(unpackVirtualShadowPageKey(packVirtualShadowPageKey(page.address)) == page.address);
        }
        auto other = page.address; other.lightOwner = owner(2);
        CHECK(packVirtualShadowPageKey(other).ownerWords != packVirtualShadowPageKey(page.address).ownerWords);
        page.validLayers = VirtualShadowPageLayerNone;
        CHECK(unpackVirtualShadowResidentPage(packVirtualShadowResidentPage(page, 32), 32).validLayers == 0);
        return true;
    }

    bool residencyPackingRejectsMalformedAndUnusedRecords() {
        const auto rejectsKey = [](const PackedVirtualShadowPageKey& key) {
            try { (void)unpackVirtualShadowPageKey(key); } catch (const std::invalid_argument&) { return true; }
            return false;
        };
        CHECK(rejectsKey({}));
        auto key = packVirtualShadowPageKey(address(1));
        auto bad = key; ++bad.abiVersion; CHECK(rejectsKey(bad));
        bad = key; bad.ownerWords = {}; CHECK(rejectsKey(bad));
        bad = key; bad.projectionRevisionWords = {}; CHECK(rejectsKey(bad));
        bad = key; bad.projectionDescriptor = 0xff000000u; CHECK(rejectsKey(bad));
        bad = key; bad.projectionDescriptor = 0x02060000u; CHECK(rejectsKey(bad));
        bad = key; bad.projectionDescriptor = 0x01010000u; CHECK(rejectsKey(bad));
        PackedVirtualShadowResidentPage record{}; record.key = key; record.physicalPage = 0;
        const auto rejectsRecord = [](const PackedVirtualShadowResidentPage& candidate) {
            try { (void)unpackVirtualShadowResidentPage(candidate, 2); } catch (const std::invalid_argument&) { return true; }
            return false;
        };
        CHECK(!rejectsRecord(record));
        auto invalid = record; invalid.physicalPage = 2; CHECK(rejectsRecord(invalid));
        invalid = record; invalid.validLayers = 256; CHECK(rejectsRecord(invalid));
        invalid = record; invalid.reserved = 1; CHECK(rejectsRecord(invalid));
        return true;
    }

    bool gpuRequestsRecoverOnlyTheirOwningSnapshot() {
        auto clip = clipLevel(3, -17); clip.worldPageOriginX = -4; clip.worldPageOriginY = -3;
        const std::array clips{clip};
        PackedDirectionalVirtualShadowGpuRequest request{
            .pageX = -2, .pageY = 1, .level = 3, .mip = 0, .receiverSamples = 123,
            .priority = -17, .requiredLayers = VirtualShadowPageLayerAll, .selectedLevelIndex = 0};
        const auto owned = unpackDirectionalVirtualShadowGpuRequest(request, clips, 128);
        CHECK(owned.address.lightOwner == clip.lightOwner && owned.address.projectionRevision == clip.projectionRevision);
        CHECK(owned.address.pageX == -2 && owned.address.pageY == 1 && owned.address.levelOrFace == 3);
        CHECK(owned.staticCasterRevision == clip.staticCasterRevision && owned.dynamicCasterRevision == clip.dynamicCasterRevision);
        CHECK(owned.receiverSamples == 123 && owned.priority == -17 && owned.requiredLayers == VirtualShadowPageLayerAll);
        const auto rejects = [&](const PackedDirectionalVirtualShadowGpuRequest& candidate) {
            try { (void)unpackDirectionalVirtualShadowGpuRequest(candidate, clips, 128); }
            catch (const std::invalid_argument&) { return true; } return false;
        };
        auto bad = request; bad.selectedLevelIndex = 1; CHECK(rejects(bad));
        bad = request; bad.receiverSamples = 0; CHECK(rejects(bad));
        bad = request; bad.pageX = -5; CHECK(rejects(bad));
        bad = request; bad.pageY = 5; CHECK(rejects(bad));
        bad = request; ++bad.level; CHECK(rejects(bad));
        bad = request; ++bad.mip; CHECK(rejects(bad));
        bad = request; ++bad.priority; CHECK(rejects(bad));
        bad = request; bad.requiredLayers = VirtualShadowPageLayerStatic; CHECK(rejects(bad));
        return true;
    }

} // namespace

namespace {
    bool residencyRequestsAndMappingsPreserveAndValidate() {
        auto source=request(1);
        source.address.pageX=-(int64_t{1}<<40);source.staticCasterRevision=(uint64_t{1}<<51)+5;
        source.priority=-73;source.requiredLayers=VirtualShadowPageLayerDynamic;
        const auto packed=packVirtualShadowResidencyRequest(source);
        CHECK(unpackVirtualShadowPageKey(packed.key)==source.address);
        CHECK(packed.priority==-73 && packed.receiverSamples==1 && packed.requiredLayers==2 && packed.reserved==0);
        PackedVirtualShadowPageMapping mapping{};
        mapping.key=packed.key;mapping.staticRevisionWords=packed.staticRevisionWords;
        mapping.dynamicRevisionWords=packed.dynamicRevisionWords;mapping.receiverSamples=packed.receiverSamples;
        mapping.physicalPage=3;mapping.requiredLayers=2;mapping.updateLayers=2;mapping.state=2;mapping.missingAction=1;
        auto result=unpackVirtualShadowPageMapping(mapping,4);
        CHECK(result.address==source.address && result.staticCasterRevision==source.staticCasterRevision);
        CHECK(result.state==VirtualShadowPageState::PendingRaster && result.updateLayers==2);
        mapping.state=0;
        CHECK(unpackVirtualShadowPageMapping(mapping,4).state==VirtualShadowPageState::Missing);
        mapping.state=1;mapping.updateLayers=0;
        CHECK(unpackVirtualShadowPageMapping(mapping,4).state==VirtualShadowPageState::CachedSampleable);
        auto rejects=[](PackedVirtualShadowPageMapping value) {
            try {(void)unpackVirtualShadowPageMapping(value,4);return false;}
            catch(const std::invalid_argument&) {return true;}
        };
        auto invalid=mapping;invalid.physicalPage=4;CHECK(rejects(invalid));
        invalid=mapping;invalid.physicalPage=InvalidVirtualShadowPhysicalPage;CHECK(rejects(invalid));
        invalid=mapping;invalid.state=2;CHECK(rejects(invalid));
        invalid=mapping;invalid.updateLayers=2;CHECK(rejects(invalid));
        invalid=mapping;invalid.state=0;invalid.updateLayers=1;CHECK(rejects(invalid));
        invalid=mapping;invalid.requiredLayers=0;CHECK(rejects(invalid));
        invalid=mapping;invalid.reserved1=1;CHECK(rejects(invalid));
        invalid=mapping;invalid.key.abiVersion=0;CHECK(rejects(invalid));
        invalid=mapping;invalid.receiverSamples=0;CHECK(rejects(invalid));
        for(uint32_t bad: {0u,4u}) {
            source.requiredLayers=uint8_t(bad);
            try {(void)packVirtualShadowResidencyRequest(source);CHECK(false);} catch(const std::invalid_argument&) {}
        }
        source.requiredLayers=3;source.receiverSamples=0;
        try {(void)packVirtualShadowResidencyRequest(source);CHECK(false);} catch(const std::invalid_argument&) {}
        return true;
    }
}

int main() {
    const struct { const char* name; bool (*run)(); } tests[]{
        { "residency packing preserves full identity and validity", residencyPackingPreservesFullIdentityAndValidity },
        { "residency requests and mappings preserve and validate", residencyRequestsAndMappingsPreserveAndValidate },
        { "residency packing rejects malformed and unused records", residencyPackingRejectsMalformedAndUnusedRecords },
        { "GPU requests recover only their owning snapshot", gpuRequestsRecoverOnlyTheirOwningSnapshot },
        { "page raster regions preserve borders and scroll", pageRasterRegionsPreserveBordersAndScroll },
        { "page raster rejects stale and invalid mappings", pageRasterRejectsStaleAndInvalidMappings },
        { "clip packing preserves ABI and rejects overflow", clipPackingPreservesAbiAndRejectsOverflow },
        { "live publisher owns conservative depth revisions", livePublisherOwnsConservativeDepthRevisions },
        { "camera-driven clips preserve world pages", cameraDrivenClipsPreserveWorldPages },
        { "full-view page grid is bounded and stable", fullViewPageGridIsBoundedAndStable },
        { "depth receivers preserve pixels and reject unsafe regions",
            depthReceiversPreservePixelsAndRejectUnsafeRegions },
        { "policy validation freezes bounded pages",
            policyValidationFreezesBoundedPages },
        { "resource validation freezes qualification storage",
            resourceValidationFreezesQualificationStorage },
        { "receiver marks deduplicate and commit before sampling",
            receiverMarksDeduplicateAndCommitBeforeSampling },
        { "directional receiver marks choose finest and deduplicate",
            directionalReceiverMarksChooseFinestAndDeduplicate },
        { "directional guard band falls back without edge gap",
            directionalGuardBandFallsBackWithoutEdgeGap },
        { "directional world page identity survives snapped clip scroll",
            directionalWorldPageIdentitySurvivesSnappedClipScroll },
        { "directional marks report invalid outside and capacity overflow",
            directionalMarksReportInvalidOutsideAndCapacityOverflow },
        { "directional marking rejects incoherent clip contracts",
            directionalMarkingRejectsIncoherentClipContracts },
        { "dynamic revision invalidates only dynamic layer",
            dynamicRevisionInvalidatesOnlyDynamicLayer },
        { "conflicting marks fail and static caching can be disabled",
            conflictingMarksFailAndStaticCachingCanBeDisabled },
        { "update budget uses priority and fails visible",
            updateBudgetUsesPriorityAndFailsVisible },
        { "eviction is deterministic and protects requested pages",
            evictionIsDeterministicAndProtectsRequestedPages },
        { "full requested pool reports overflow without stale sampling",
            fullRequestedPoolReportsOverflowWithoutStaleSampling },
    };
    size_t failures = 0;
    for (const auto& test : tests) {
        if (test.run()) std::cout << "[PASS] " << test.name << '\n';
        else { ++failures; std::cerr << "[FAIL] " << test.name << '\n'; }
    }
    std::cout << std::size(tests) - failures << '/' << std::size(tests)
        << " tests passed\n";
    DirectionalVirtualShadowClipConfig benchmarkConfig{};
    benchmarkConfig.lightOwner = owner(1); benchmarkConfig.projectionRevision = 1;
    std::array<double, 5> timings{};
    int64_t checksum = 0;
    constexpr uint32_t iterations = 100'000;
    for (auto& elapsed : timings) {
        const auto start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < iterations; ++i) {
            benchmarkConfig.focusWorld.x = float(i % 1'024) * 0.03125f;
            const auto plan = buildDirectionalVirtualShadowClips(benchmarkConfig);
            checksum += plan.clips[0].worldPageOriginX;
        }
        elapsed = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start).count() / iterations;
    }
    std::ranges::sort(timings);
    std::cout << "Four-level clip builder CPU median: " << timings[2]
        << " us/plan (checksum=" << checksum << ")\n";
    return failures == 0 ? 0 : 1;
}
