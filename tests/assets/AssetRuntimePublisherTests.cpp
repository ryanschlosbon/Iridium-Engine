#include "assets/runtime/AssetRuntimePublisher.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

    using namespace Iridium;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition \
                    " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    AssetGuid guid(std::string_view value) {
        const auto parsed = AssetGuid::parse(value);
        if (!parsed) {
            throw std::runtime_error("Invalid test GUID.");
        }
        return *parsed;
    }

    RuntimeAssetPublishRequest request(
        AssetGuid assetGuid, std::string cookKey,
        uint64_t uploadBytes, uint64_t cpuBytes,
        uint64_t gpuBytes, int& publications,
        int& retirements, bool succeed = true,
        std::string diagnostic = {}) {
        return {
            .assetGuid = assetGuid,
            .cookKey = std::move(cookKey),
            .estimatedUploadBytes = uploadBytes,
            .publish = [=, &publications,
                &retirements]() mutable {
                ++publications;
                std::function<void()> retire;
                if (succeed) {
                    retire = [&retirements]() {
                        ++retirements;
                    };
                }
                return RuntimeAssetPublishOutcome{
                    .succeeded = succeed,
                    .cpuResidentBytes = cpuBytes,
                    .gpuResidentBytes = gpuBytes,
                    .retire = std::move(retire),
                    .diagnostic =
                        std::move(diagnostic),
                };
            },
        };
    }

    bool publishesAndRetainsLastKnownGood() {
        AssetRuntimePublisher publisher;
        const AssetGuid asset = guid(
            "019f9bce-85b8-7200-8203-040506070809");
        int publications = 0;
        int retirements = 0;
        CHECK(publisher.enqueue(request(asset, "cook-a",
            64, 100, 200, publications, retirements)));
        const RuntimePublishTickResult first =
            publisher.tick(64);
        CHECK(first.published == 1);
        CHECK(first.failed == 0);
        CHECK(first.scheduledUploadBytes == 64);
        auto snapshot = publisher.snapshot(asset);
        CHECK(snapshot.has_value());
        CHECK(snapshot->state == RuntimeAssetState::Ready);
        CHECK(snapshot->revision == 1);
        CHECK(snapshot->cookKey == "cook-a");
        CHECK(snapshot->hasPublishedRevision);

        CHECK(publisher.enqueue(request(asset, "cook-b",
            32, 500, 600, publications, retirements,
            false, "compile failed")));
        const RuntimePublishTickResult failed =
            publisher.tick(32);
        CHECK(failed.failed == 1);
        snapshot = publisher.snapshot(asset);
        CHECK(snapshot->state ==
            RuntimeAssetState::ReadyWithError);
        CHECK(snapshot->revision == 1);
        CHECK(snapshot->cookKey == "cook-a");
        CHECK(snapshot->diagnostic == "compile failed");
        CHECK(retirements == 0);

        CHECK(publisher.enqueue(request(asset, "cook-c",
            48, 110, 210, publications, retirements)));
        CHECK(publisher.tick(48).published == 1);
        snapshot = publisher.snapshot(asset);
        CHECK(snapshot->state == RuntimeAssetState::Ready);
        CHECK(snapshot->revision == 2);
        CHECK(snapshot->cookKey == "cook-c");
        CHECK(snapshot->diagnostic.empty());
        CHECK(retirements == 1);
        CHECK(publications == 3);
        return true;
    }

    bool coalescesAndHonorsUploadBudget() {
        AssetRuntimePublisher publisher;
        const AssetGuid asset = guid(
            "019f9bce-85b8-7201-8203-040506070809");
        int oldPublications = 0;
        int newPublications = 0;
        int retirements = 0;
        CHECK(publisher.enqueue(request(asset, "old",
            100, 1, 1, oldPublications, retirements)));
        CHECK(publisher.enqueue(request(asset, "new",
            80, 2, 2, newPublications, retirements)));
        CHECK(!publisher.enqueue(request(asset, "new",
            80, 2, 2, newPublications, retirements)));
        CHECK(publisher.stats().coalesced == 1);
        CHECK(publisher.stats().unchanged == 1);

        const RuntimePublishTickResult deferred =
            publisher.tick(79);
        CHECK(deferred.published == 0);
        CHECK(deferred.deferredByBudget == 1);
        CHECK(deferred.queuedAfterTick == 1);
        CHECK(oldPublications == 0);
        CHECK(newPublications == 0);

        const RuntimePublishTickResult published =
            publisher.tick(80);
        CHECK(published.published == 1);
        CHECK(published.scheduledUploadBytes == 80);
        CHECK(oldPublications == 0);
        CHECK(newPublications == 1);
        CHECK(publisher.snapshot(asset)->cookKey == "new");
        return true;
    }

    bool cancelsQueuedWithoutRetiringPublished() {
        AssetRuntimePublisher publisher;
        const AssetGuid asset = guid(
            "019f9bce-85b8-7202-8203-040506070809");
        int publications = 0;
        int retirements = 0;
        CHECK(publisher.enqueue(request(
            asset, "ready", 0, 1, 1,
            publications, retirements)));
        CHECK(publisher.tick(0).published == 1);
        CHECK(publisher.enqueue(request(
            asset, "pending", 10, 2, 2,
            publications, retirements)));
        publisher.cancelQueued(asset);
        CHECK(publisher.stats().queued == 0);
        CHECK(publisher.snapshot(asset)->state ==
            RuntimeAssetState::Ready);
        CHECK(publisher.snapshot(asset)->cookKey ==
            "ready");
        CHECK(publisher.tick(10).published == 0);
        CHECK(publications == 1);
        CHECK(retirements == 0);
        return true;
    }

    bool permitsOneExplicitAtomicOversizedPublication() {
        AssetRuntimePublisher publisher;
        const AssetGuid asset = guid(
            "019f9bce-85b8-7203-8203-040506070809");
        uint32_t publications = 0;
        CHECK(publisher.enqueue({
            .assetGuid = asset,
            .cookKey = "large-environment",
            .estimatedUploadBytes = 512,
            .allowSingleOversizedUpload = true,
            .publish = [&] {
                ++publications;
                return RuntimeAssetPublishOutcome{ .succeeded = true };
            },
        }));
        const RuntimePublishTickResult tick = publisher.tick(128);
        CHECK(tick.published == 1);
        CHECK(tick.oversizedPublications == 1);
        CHECK(tick.scheduledUploadBytes == 512);
        CHECK(publications == 1);
        return true;
    }

    bool adoptsExistingLastKnownGood() {
        AssetRuntimePublisher publisher;
        const AssetGuid asset = guid(
            "019f9bce-85b8-7203-8203-040506070809");
        publisher.setPinned(asset, true);
        publisher.adoptPublished(
            asset, "baseline", 12, 24);
        const auto snapshot =
            publisher.snapshot(asset);
        CHECK(snapshot.has_value());
        CHECK(snapshot->state ==
            RuntimeAssetState::Ready);
        CHECK(snapshot->revision == 1);
        CHECK(snapshot->cookKey == "baseline");
        CHECK(snapshot->cpuResidentBytes == 12);
        CHECK(snapshot->gpuResidentBytes == 24);
        CHECK(snapshot->pinned);
        CHECK(publisher.stats().resident == 1);
        CHECK(publisher.stats().published == 0);
        publisher.reportFailure(
            asset, "new source is invalid");
        CHECK(publisher.snapshot(asset)->state ==
            RuntimeAssetState::ReadyWithError);
        CHECK(publisher.snapshot(asset)->cookKey ==
            "baseline");
        return true;
    }

    bool evictsByLruWhileRespectingPins() {
        AssetRuntimePublisher publisher;
        const std::array assets{
            guid("019f9bce-85b8-7210-8203-040506070809"),
            guid("019f9bce-85b8-7211-8203-040506070809"),
            guid("019f9bce-85b8-7212-8203-040506070809"),
        };
        std::array<int, 3> publications{};
        std::array<int, 3> retirements{};
        for (size_t index = 0; index < assets.size();
            ++index) {
            CHECK(publisher.enqueue(request(assets[index],
                "cook-" + std::to_string(index), 10,
                50, 100, publications[index],
                retirements[index])));
        }
        CHECK(publisher.tick(30).published == 3);
        CHECK(publisher.stats().gpuResidentBytes == 300);
        publisher.setPinned(assets[1], true);
        publisher.touch(assets[0], 10);
        publisher.touch(assets[1], 1);
        publisher.touch(assets[2], 5);

        const RuntimeResidencyResult first =
            publisher.evictToGpuBudget(200);
        CHECK(first.evicted == 1);
        CHECK(first.budgetSatisfied);
        CHECK(publisher.snapshot(assets[2])->state ==
            RuntimeAssetState::Evicted);
        CHECK(retirements[2] == 1);

        const RuntimeResidencyResult second =
            publisher.evictToGpuBudget(50);
        CHECK(second.evicted == 1);
        CHECK(!second.budgetSatisfied);
        CHECK(second.residentBytesAfter == 100);
        CHECK(publisher.snapshot(assets[1])
            ->hasPublishedRevision);
        CHECK(publisher.snapshot(assets[1])->pinned);

        CHECK(publisher.enqueue(request(assets[2],
            "cook-reloaded", 10, 50, 100,
            publications[2], retirements[2])));
        CHECK(publisher.tick(10).published == 1);
        CHECK(publisher.snapshot(assets[2])->revision == 2);
        publisher.shutdown();
        CHECK(retirements[1] == 1);
        CHECK(retirements[2] == 2);
        CHECK(publisher.snapshots().empty());
        CHECK(publisher.stats().resident == 0);
        return true;
    }

    bool rejectsInvalidAndConvertsExceptions() {
        AssetRuntimePublisher publisher;
        bool rejected = false;
        try {
            RuntimeAssetPublishRequest invalid;
            (void)publisher.enqueue(std::move(invalid));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        CHECK(rejected);

        const AssetGuid asset = guid(
            "019f9bce-85b8-7220-8203-040506070809");
        CHECK(publisher.enqueue({
            .assetGuid = asset,
            .cookKey = "throws",
            .estimatedUploadBytes = 0,
            .publish = []() -> RuntimeAssetPublishOutcome {
                throw std::runtime_error("publish exception");
            },
        }));
        CHECK(publisher.tick(0).failed == 1);
        const auto snapshot = publisher.snapshot(asset);
        CHECK(snapshot->state == RuntimeAssetState::Failed);
        CHECK(snapshot->diagnostic == "publish exception");
        CHECK(!snapshot->hasPublishedRevision);
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*run)();
    };
    constexpr TestCase tests[]{
        { "last-known-good publication",
            publishesAndRetainsLastKnownGood },
        { "coalescing and upload budget",
            coalescesAndHonorsUploadBudget },
        { "queued cancellation",
            cancelsQueuedWithoutRetiringPublished },
        { "existing revision adoption",
            adoptsExistingLastKnownGood },
        { "LRU eviction and pins",
            evictsByLruWhileRespectingPins },
        { "invalid requests and exceptions",
            rejectsInvalidAndConvertsExceptions },
        { "explicit atomic oversized publication",
            permitsOneExplicitAtomicOversizedPublication },
    };
    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.run()) {
                std::cout << "[PASS] " <<
                    test.name << '\n';
            } else {
                ++failures;
                std::cerr << "[FAIL] " <<
                    test.name << '\n';
            }
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name <<
                ": " << exception.what() << '\n';
        }
    }
    std::cout << (std::size(tests) - failures) <<
        '/' << std::size(tests) <<
        " tests passed\n";
    return failures == 0 ? 0 : 1;
}
