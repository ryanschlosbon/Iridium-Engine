#include "assets/runtime/SourceChangeTracker.h"

#include <array>
#include <chrono>
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
        if (!parsed) throw std::runtime_error(
            "Invalid test GUID.");
        return *parsed;
    }

    AssetGuid scaleGuid(uint64_t index) {
        std::array<uint8_t, 10> random{};
        for (size_t byte = 0; byte < 8; ++byte) {
            random[9 - byte] = static_cast<uint8_t>(
                index >> (byte * 8));
        }
        return AssetGuid::fromUuidV7Fields(
            1'900'000'000'000ull + index,
            random);
    }

    AssetDependency assetDependency(AssetGuid asset) {
        return {
            .type = AssetDependencyType::Asset,
            .assetGuid = asset,
        };
    }

    bool coalescesDebouncesAndIgnoresTimestamps() {
        const AssetGuid texture = guid(
            "019f9bce-85b8-7300-8203-040506070809");
        const AssetGuid material = guid(
            "019f9bce-85b8-7301-8203-040506070809");
        const AssetGuid model = guid(
            "019f9bce-85b8-7302-8203-040506070809");
        AssetDependencyGraph graph;
        graph.setDependencies(texture, {});
        graph.setDependencies(material,
            { assetDependency(texture) });
        graph.setDependencies(model,
            { assetDependency(material) });

        SourceChangeTracker tracker(100);
        tracker.seedContentHash(
            texture, "texture.png",
            std::string(64, 'a'));
        tracker.notify(texture, "texture.png", 100);
        tracker.notify(texture, "texture.png", 150);
        int hashCalls = 0;
        auto hasher =
            [&hashCalls](const std::filesystem::path&) {
                ++hashCalls;
                return std::string(64, 'b');
            };
        CHECK(tracker.poll(249, graph, hasher)
            .changedAssets.empty());
        CHECK(hashCalls == 0);
        const SourceChangeBatch changed =
            tracker.poll(250, graph, hasher);
        CHECK(hashCalls == 1);
        CHECK(changed.changedAssets ==
            std::vector{ texture });
        CHECK(changed.changedSources.size() == 1);
        CHECK(changed.changedSources[0].sourcePath ==
            std::filesystem::path("texture.png"));
        CHECK(changed.changedSources[0].previousHash ==
            std::string(64, 'a'));
        CHECK(changed.changedSources[0].contentHash ==
            std::string(64, 'b'));
        CHECK(changed.invalidatedAssets ==
            (std::vector{ texture, material, model }));
        CHECK(changed.rebuildOrder ==
            (std::vector{ texture, material, model }));
        CHECK(!changed.blocked());

        tracker.notify(texture, "texture.png", 300);
        const SourceChangeBatch unchanged =
            tracker.poll(400, graph, hasher);
        CHECK(unchanged.changedAssets.empty());
        CHECK(unchanged.sameContentEvents == 1);
        CHECK(tracker.stats().notifications == 3);
        CHECK(tracker.stats().coalesced == 1);
        CHECK(tracker.stats().contentChanges == 1);
        CHECK(tracker.stats().sameContent == 1);
        CHECK(tracker.stats().pending == 0);
        return true;
    }

    bool dependencyCyclesBlockScheduling() {
        const AssetGuid first = guid(
            "019f9bce-85b8-7310-8203-040506070809");
        const AssetGuid second = guid(
            "019f9bce-85b8-7311-8203-040506070809");
        AssetDependencyGraph graph;
        graph.setDependencies(first,
            { assetDependency(second) });
        graph.setDependencies(second,
            { assetDependency(first) });
        SourceChangeTracker tracker(0);
        tracker.notify(first, "first.asset", 1);
        const SourceChangeBatch batch =
            tracker.poll(1, graph,
                [](const std::filesystem::path&) {
                    return std::string(64, 'c');
                });
        CHECK(batch.changedAssets ==
            std::vector{ first });
        CHECK(batch.invalidatedAssets ==
            (std::vector{ first, second }));
        CHECK(batch.blocked());
        CHECK(batch.blockingCycles.size() == 1);
        CHECK(batch.rebuildOrder.empty());
        CHECK(batch.diagnostics.size() == 1);
        CHECK(batch.diagnostics[0].code ==
            "ASSET_DEPENDENCY_CYCLE");
        return true;
    }

    bool hashFailuresCanRetry() {
        const AssetGuid asset = guid(
            "019f9bce-85b8-7320-8203-040506070809");
        AssetDependencyGraph graph;
        graph.setDependencies(asset, {});
        SourceChangeTracker tracker(10);
        tracker.seedContentHash(
            asset, "asset.bin",
            std::string(64, 'd'));
        tracker.notify(asset, "asset.bin", 10);
        const SourceChangeBatch failed =
            tracker.poll(20, graph,
                [](const std::filesystem::path&)
                    -> std::string {
                    throw std::runtime_error(
                        "sharing violation");
                });
        CHECK(failed.changedAssets.empty());
        CHECK(failed.diagnostics.size() == 1);
        CHECK(failed.diagnostics[0].code ==
            "ASSET_SOURCE_HASH_FAILED");
        CHECK(tracker.stats().hashFailures == 1);
        CHECK(tracker.stats().pending == 0);

        tracker.notify(asset, "asset.bin", 30);
        const SourceChangeBatch retried =
            tracker.poll(40, graph,
                [](const std::filesystem::path&) {
                    return std::string(64, 'e');
                });
        CHECK(retried.changedAssets ==
            std::vector{ asset });
        CHECK(retried.rebuildOrder ==
            std::vector{ asset });
        return true;
    }

    bool tracksMultiplePathsPerOwner() {
        const AssetGuid asset = guid(
            "019f9bce-85b8-7330-8203-040506070809");
        AssetDependencyGraph graph;
        graph.setDependencies(asset, {});
        SourceChangeTracker tracker(0);
        tracker.seedContentHash(
            asset, "albedo.png",
            std::string(64, '1'));
        tracker.seedContentHash(
            asset, "normal.png",
            std::string(64, '2'));
        tracker.notify(
            asset, "albedo.png", 1);
        tracker.notify(
            asset, "normal.png", 1);
        int calls = 0;
        const SourceChangeBatch batch =
            tracker.poll(1, graph,
                [&calls](
                    const std::filesystem::path& path) {
                    ++calls;
                    return path.filename() ==
                            "albedo.png"
                        ? std::string(64, '1')
                        : std::string(64, '3');
                });
        CHECK(calls == 2);
        CHECK(batch.sameContentEvents == 1);
        CHECK(batch.changedSources.size() == 1);
        CHECK(batch.changedSources[0].sourcePath ==
            std::filesystem::path("normal.png"));
        CHECK(batch.changedAssets ==
            std::vector{ asset });
        CHECK(batch.rebuildOrder ==
            std::vector{ asset });

        tracker.notify(
            asset, "normal.png", 2);
        tracker.removeAsset(asset);
        CHECK(tracker.stats().pending == 0);
        CHECK(tracker.poll(2, graph,
            [](const std::filesystem::path&) {
                return std::string(64, '4');
            }).changedAssets.empty());
        return true;
    }

    bool invalidatesLargeDependencyFanout() {
        constexpr uint64_t DependentCount = 10'000;
        const AssetGuid root = scaleGuid(0);
        AssetDependencyGraph graph;
        graph.setDependencies(root, {});
        for (uint64_t index = 1;
            index <= DependentCount; ++index) {
            graph.setDependencies(
                scaleGuid(index),
                { assetDependency(root) });
        }
        CHECK(graph.reverseDependents(root).size() ==
            DependentCount);

        SourceChangeTracker tracker(0);
        tracker.seedContentHash(
            root, "shared.texture",
            std::string(64, 'a'));
        tracker.notify(root, "shared.texture", 1);
        const auto begin =
            std::chrono::steady_clock::now();
        const SourceChangeBatch batch =
            tracker.poll(1, graph,
                [](const std::filesystem::path&) {
                    return std::string(64, 'b');
                });
        const auto elapsed =
            std::chrono::steady_clock::now() -
            begin;
        CHECK(!batch.blocked());
        CHECK(batch.invalidatedAssets.size() ==
            DependentCount + 1);
        CHECK(batch.rebuildOrder.size() ==
            DependentCount + 1);
        CHECK(batch.rebuildOrder.front() == root);
        CHECK(std::chrono::duration_cast<
            std::chrono::seconds>(elapsed).count() <
            5);
        return true;
    }

    bool rejectsInvalidInputs() {
        SourceChangeTracker tracker(0);
        bool rejectedSeed = false;
        try {
            tracker.seedContentHash(
                AssetGuid{}, "source.bin",
                std::string(64, 'a'));
        } catch (const std::invalid_argument&) {
            rejectedSeed = true;
        }
        CHECK(rejectedSeed);
        bool rejectedNotify = false;
        try {
            tracker.notify(AssetGuid{}, {}, 0);
        } catch (const std::invalid_argument&) {
            rejectedNotify = true;
        }
        CHECK(rejectedNotify);
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*run)();
    };
    constexpr TestCase tests[]{
        { "debounce, hashes, and dependency order",
            coalescesDebouncesAndIgnoresTimestamps },
        { "cycle blocks scheduling",
            dependencyCyclesBlockScheduling },
        { "hash failure retry",
            hashFailuresCanRetry },
        { "multiple source paths per owner",
            tracksMultiplePathsPerOwner },
        { "10k dependency fanout",
            invalidatesLargeDependencyFanout },
        { "invalid inputs", rejectsInvalidInputs },
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
