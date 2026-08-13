#include "assets/runtime/AssetReimportScheduler.h"

#include <atomic>
#include <future>
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
            throw std::runtime_error(
                "Invalid test GUID.");
        }
        return *parsed;
    }

    PreparedRuntimeAsset prepared(
        std::string cookKey,
        std::function<RuntimeAssetPublishOutcome()>
            publish) {
        return {
            .cookKey = std::move(cookKey),
            .estimatedUploadBytes = 16,
            .publish = std::move(publish),
        };
    }

    bool preparesOffThreadAndPublishesOnDrain() {
        const AssetGuid asset = guid(
            "019f9bce-85b8-7400-8203-040506070809");
        const std::thread::id mainThread =
            std::this_thread::get_id();
        std::thread::id prepareThread;
        std::thread::id publishThread;
        AssetReimportScheduler scheduler;
        AssetRuntimePublisher publisher;
        CHECK(scheduler.enqueue({
            .assetGuid = asset,
            .requestKey = "source-hash-a",
            .prepare =
                [&](std::stop_token) {
                    prepareThread =
                        std::this_thread::get_id();
                    return prepared(
                        std::string(64, 'a'),
                        [&] {
                            publishThread =
                                std::this_thread::get_id();
                            return RuntimeAssetPublishOutcome{
                                .succeeded = true,
                                .cpuResidentBytes = 32,
                                .gpuResidentBytes = 64,
                            };
                        });
                },
        }));
        CHECK(scheduler.waitForCompletion(
            std::chrono::seconds(2)));
        const AssetReimportDrainResult drained =
            scheduler.drainTo(publisher);
        CHECK(drained.ready == 1);
        CHECK(prepareThread != mainThread);
        CHECK(publishThread == std::thread::id{});
        CHECK(publisher.tick(16).published == 1);
        CHECK(publishThread == mainThread);
        const auto snapshot =
            publisher.snapshot(asset);
        CHECK(snapshot.has_value());
        CHECK(snapshot->state ==
            RuntimeAssetState::Ready);
        CHECK(snapshot->revision == 1);
        return true;
    }

    bool editDuringCookSupersedesOldWork() {
        const AssetGuid asset = guid(
            "019f9bce-85b8-7410-8203-040506070809");
        std::promise<void> firstStarted;
        std::promise<void> releasePromise;
        const std::shared_future<void> releaseFirst =
            releasePromise.get_future().share();
        std::atomic<bool> cancellationSeen = false;
        int oldPublishes = 0;
        int newPublishes = 0;
        AssetReimportScheduler scheduler;
        AssetRuntimePublisher publisher;
        CHECK(scheduler.enqueue({
            .assetGuid = asset,
            .requestKey = "old",
            .prepare =
                [&](std::stop_token stopToken) {
                    firstStarted.set_value();
                    releaseFirst.wait();
                    cancellationSeen =
                        stopToken.stop_requested();
                    return prepared(
                        std::string(64, 'b'),
                        [&] {
                            ++oldPublishes;
                            return RuntimeAssetPublishOutcome{
                                .succeeded = true,
                            };
                        });
                },
        }));
        firstStarted.get_future().wait();
        CHECK(scheduler.enqueue({
            .assetGuid = asset,
            .requestKey = "new",
            .prepare =
                [&](std::stop_token) {
                    return prepared(
                        std::string(64, 'c'),
                        [&] {
                            ++newPublishes;
                            return RuntimeAssetPublishOutcome{
                                .succeeded = true,
                            };
                        });
                },
        }));
        releasePromise.set_value();
        CHECK(scheduler.waitForCompletion(
            std::chrono::seconds(2)));
        CHECK(cancellationSeen.load());
        const AssetReimportDrainResult drained =
            scheduler.drainTo(publisher);
        CHECK(drained.ready == 1);
        CHECK(publisher.tick(16).published == 1);
        CHECK(oldPublishes == 0);
        CHECK(newPublishes == 1);
        const AssetReimportSchedulerStats stats =
            scheduler.stats();
        CHECK(stats.cancellationRequests == 1);
        CHECK(stats.superseded == 1);
        return true;
    }

    bool failureRetainsLastKnownGood() {
        const AssetGuid asset = guid(
            "019f9bce-85b8-7420-8203-040506070809");
        AssetRuntimePublisher publisher;
        CHECK(publisher.enqueue({
            .assetGuid = asset,
            .cookKey = std::string(64, 'd'),
            .publish = [] {
                return RuntimeAssetPublishOutcome{
                    .succeeded = true,
                    .cpuResidentBytes = 10,
                    .gpuResidentBytes = 20,
                };
            },
        }));
        CHECK(publisher.tick(0).published == 1);

        AssetReimportScheduler scheduler;
        CHECK(scheduler.enqueue({
            .assetGuid = asset,
            .requestKey = "broken-source",
            .prepare = [](std::stop_token)
                -> PreparedRuntimeAsset {
                throw std::runtime_error(
                    "texture decode failed");
            },
        }));
        CHECK(scheduler.waitForCompletion(
            std::chrono::seconds(2)));
        const AssetReimportDrainResult drained =
            scheduler.drainTo(publisher);
        CHECK(drained.failed == 1);
        const auto snapshot =
            publisher.snapshot(asset);
        CHECK(snapshot.has_value());
        CHECK(snapshot->state ==
            RuntimeAssetState::ReadyWithError);
        CHECK(snapshot->revision == 1);
        CHECK(snapshot->cpuResidentBytes == 10);
        CHECK(snapshot->gpuResidentBytes == 20);
        CHECK(snapshot->diagnostic ==
            "texture decode failed");
        return true;
    }

    bool coalescesQueuedAndRejectsInvalidRequests() {
        const AssetGuid blocker = guid(
            "019f9bce-85b8-7430-8203-040506070809");
        const AssetGuid asset = guid(
            "019f9bce-85b8-7431-8203-040506070809");
        std::promise<void> blockerStarted;
        std::promise<void> releasePromise;
        const std::shared_future<void> release =
            releasePromise.get_future().share();
        AssetReimportScheduler scheduler;
        CHECK(scheduler.enqueue({
            .assetGuid = blocker,
            .requestKey = "blocker",
            .prepare =
                [&](std::stop_token) {
                    blockerStarted.set_value();
                    release.wait();
                    return prepared(
                        std::string(64, 'e'),
                        [] {
                            return RuntimeAssetPublishOutcome{
                                .succeeded = true,
                            };
                        });
                },
        }));
        blockerStarted.get_future().wait();
        CHECK(scheduler.enqueue({
            .assetGuid = asset,
            .requestKey = "first",
            .prepare = [](std::stop_token) {
                return prepared(
                    std::string(64, 'f'),
                    [] {
                        return RuntimeAssetPublishOutcome{
                            .succeeded = true,
                        };
                    });
            },
        }));
        CHECK(scheduler.enqueue({
            .assetGuid = asset,
            .requestKey = "second",
            .prepare = [](std::stop_token) {
                return prepared(
                    std::string(64, '1'),
                    [] {
                        return RuntimeAssetPublishOutcome{
                            .succeeded = true,
                        };
                    });
            },
        }));
        CHECK(!scheduler.enqueue({
            .assetGuid = asset,
            .requestKey = "second",
            .prepare = [](std::stop_token) {
                return PreparedRuntimeAsset{};
            },
        }));
        bool rejected = false;
        try {
            scheduler.enqueue({});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        CHECK(rejected);
        releasePromise.set_value();
        CHECK(scheduler.waitForCompletion(
            std::chrono::seconds(2)));
        AssetRuntimePublisher publisher;
        AssetReimportDrainResult aggregate;
        for (int attempt = 0; attempt < 2; ++attempt) {
            const AssetReimportDrainResult drained =
                scheduler.drainTo(publisher);
            aggregate.ready += drained.ready;
            if (aggregate.ready == 2) break;
            CHECK(scheduler.waitForCompletion(
                std::chrono::seconds(2)));
        }
        CHECK(aggregate.ready == 2);
        CHECK(scheduler.stats().coalesced == 2);
        return true;
    }

    bool shutdownRequestsActiveCancellation() {
        const AssetGuid asset = guid(
            "019f9bce-85b8-7440-8203-040506070809");
        std::promise<void> started;
        std::promise<void> cancelled;
        AssetReimportScheduler scheduler;
        CHECK(scheduler.enqueue({
            .assetGuid = asset,
            .requestKey = "shutdown",
            .prepare =
                [&](std::stop_token stopToken)
                    -> PreparedRuntimeAsset {
                    started.set_value();
                    while (!stopToken.stop_requested()) {
                        std::this_thread::yield();
                    }
                    cancelled.set_value();
                    throw std::runtime_error(
                        "cancelled");
                },
        }));
        started.get_future().wait();
        scheduler.shutdown();
        CHECK(cancelled.get_future().wait_for(
            std::chrono::seconds(1)) ==
            std::future_status::ready);
        CHECK(scheduler.stats().active == 0);
        return true;
    }

    bool cancelDropsQueuedAndCompletedWork() {
        const AssetGuid asset = guid(
            "019f9bce-85b8-7450-8203-040506070809");
        AssetReimportScheduler scheduler;
        CHECK(scheduler.enqueue({
            .assetGuid = asset,
            .requestKey = "cancel",
            .prepare = [](std::stop_token) {
                return prepared(
                    std::string(64, '2'),
                    [] {
                        return RuntimeAssetPublishOutcome{
                            .succeeded = true,
                        };
                    });
            },
        }));
        CHECK(scheduler.waitForCompletion(
            std::chrono::seconds(2)));
        scheduler.cancel(asset);
        CHECK(scheduler.takeCompletions().empty());
        CHECK(scheduler.stats().completed == 0);
        return true;
    }

    bool rapidReimportKeepsOnlyNewestRevision() {
        constexpr uint32_t RevisionCount = 1'024;
        const AssetGuid blocker = guid(
            "019f9bce-85b8-7460-8203-040506070809");
        const AssetGuid asset = guid(
            "019f9bce-85b8-7461-8203-040506070809");
        std::promise<void> blockerStarted;
        std::promise<void> releasePromise;
        const std::shared_future<void> release =
            releasePromise.get_future().share();
        AssetReimportScheduler scheduler;
        CHECK(scheduler.enqueue({
            .assetGuid = blocker,
            .requestKey = "blocker",
            .prepare =
                [&](std::stop_token) {
                    blockerStarted.set_value();
                    release.wait();
                    return prepared(
                        "blocker-cook",
                        [] {
                            return RuntimeAssetPublishOutcome{
                                .succeeded = true,
                            };
                        });
                },
        }));
        blockerStarted.get_future().wait();

        uint32_t publishedRevision = UINT32_MAX;
        for (uint32_t revision = 0;
            revision < RevisionCount; ++revision) {
            CHECK(scheduler.enqueue({
                .assetGuid = asset,
                .requestKey =
                    "revision-" +
                    std::to_string(revision),
                .prepare =
                    [revision,
                        &publishedRevision](
                        std::stop_token) {
                        return prepared(
                            "cook-" +
                                std::to_string(
                                    revision),
                            [revision,
                                &publishedRevision] {
                                publishedRevision =
                                    revision;
                                return RuntimeAssetPublishOutcome{
                                    .succeeded = true,
                                };
                            });
                    },
            }));
        }
        CHECK(scheduler.stats().queued == 1);
        CHECK(scheduler.stats().coalesced ==
            RevisionCount - 1);
        releasePromise.set_value();

        AssetRuntimePublisher publisher;
        uint32_t ready = 0;
        for (int attempt = 0;
            attempt < 3 && ready < 2; ++attempt) {
            CHECK(scheduler.waitForCompletion(
                std::chrono::seconds(2)));
            ready +=
                scheduler.drainTo(publisher).ready;
        }
        CHECK(ready == 2);
        CHECK(publisher.tick(32).published == 2);
        CHECK(publishedRevision ==
            RevisionCount - 1);
        CHECK(publisher.snapshot(asset)
            ->cookKey ==
            "cook-" +
                std::to_string(
                    RevisionCount - 1));
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*run)();
    };
    constexpr TestCase tests[]{
        { "background prepare and main-thread publish",
            preparesOffThreadAndPublishesOnDrain },
        { "edit during cook supersedes old work",
            editDuringCookSupersedesOldWork },
        { "failure retains last known good",
            failureRetainsLastKnownGood },
        { "queued coalescing and invalid input",
            coalescesQueuedAndRejectsInvalidRequests },
        { "shutdown cancellation",
            shutdownRequestsActiveCancellation },
        { "asset cancellation",
            cancelDropsQueuedAndCompletedWork },
        { "rapid reimport keeps newest revision",
            rapidReimportKeepsOnlyNewestRevision },
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
