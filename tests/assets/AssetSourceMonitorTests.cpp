#include "assets/runtime/AssetSourceMonitor.h"

#include "utils/Sha256.h"

#include <filesystem>
#include <fstream>
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

    class TemporaryDirectory {
    public:
        TemporaryDirectory()
            : path(std::filesystem::temp_directory_path() /
                ("iridium-source-monitor-" +
                    createAssetGuidV7().toString())) {
            std::filesystem::create_directories(path);
        }
        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
        std::filesystem::path path;
    };

    void write(
        const std::filesystem::path& path,
        std::string_view text) {
        std::ofstream output(
            path, std::ios::binary |
                std::ios::trunc);
        output << text;
        output.flush();
        if (!output) {
            throw std::runtime_error(
                "Could not write monitor fixture.");
        }
    }

    AssetDependency assetDependency(
        AssetGuid asset) {
        return {
            .type = AssetDependencyType::Asset,
            .assetGuid = asset,
        };
    }

    bool composesWatchHashAndDependencyOrder() {
        TemporaryDirectory temporary;
        const auto texturePath =
            temporary.path / "texture.bin";
        const auto materialPath =
            temporary.path / "material.bin";
        const auto modelPath =
            temporary.path / "model.bin";
        write(texturePath, "texture-a");
        write(materialPath, "material");
        write(modelPath, "model");

        const AssetGuid texture = guid(
            "019f9bce-85b8-7600-8203-040506070809");
        const AssetGuid material = guid(
            "019f9bce-85b8-7601-8203-040506070809");
        const AssetGuid model = guid(
            "019f9bce-85b8-7602-8203-040506070809");
        AssetSourceMonitor monitor(
            0, std::chrono::seconds(1),
            {}, false);
        const TrackedSourceFile textureSource{
            texturePath, sha256File(texturePath),
        };
        const TrackedSourceFile materialSource{
            materialPath, sha256File(materialPath),
        };
        const TrackedSourceFile modelSource{
            modelPath, sha256File(modelPath),
        };
        monitor.trackAsset(
            texture,
            std::span(&textureSource, 1), {});
        monitor.trackAsset(
            material,
            std::span(&materialSource, 1),
            { assetDependency(texture) });
        monitor.trackAsset(
            model,
            std::span(&modelSource, 1),
            { assetDependency(material) });

        write(texturePath, "texture-b-longer");
        monitor.processOnce(UINT64_MAX);
        const auto batches =
            monitor.drainBatches();
        CHECK(batches.size() == 1);
        CHECK(batches[0].changedAssets ==
            std::vector{ texture });
        CHECK(batches[0].rebuildOrder ==
            (std::vector{
                texture, material, model }));
        CHECK(batches[0].changedSources.size() ==
            1);
        CHECK(batches[0].changedSources[0]
            .contentHash == sha256File(texturePath));
        CHECK(monitor.stats().emittedBatches == 1);
        monitor.untrackAsset(texture);
        write(texturePath, "texture-c-even-longer");
        monitor.processOnce(UINT64_MAX);
        CHECK(monitor.drainBatches().empty());
        return true;
    }

    bool sameContentTimestampIsSuppressed() {
        TemporaryDirectory temporary;
        const auto path =
            temporary.path / "asset.bin";
        write(path, "same-content");
        const AssetGuid asset = guid(
            "019f9bce-85b8-7610-8203-040506070809");
        AssetSourceMonitor monitor(
            0, std::chrono::seconds(1),
            {}, false);
        const TrackedSourceFile source{
            path, sha256File(path),
        };
        monitor.trackAsset(
            asset, std::span(&source, 1), {});
        std::error_code error;
        const auto timestamp =
            std::filesystem::last_write_time(path);
        std::filesystem::last_write_time(
            path,
            timestamp +
                std::chrono::seconds(1),
            error);
        CHECK(!error);
        monitor.processOnce(UINT64_MAX);
        const auto batches =
            monitor.drainBatches();
        CHECK(batches.size() == 1);
        CHECK(batches[0].sameContentEvents == 1);
        CHECK(batches[0].changedAssets.empty());
        return true;
    }

    bool cycleBlocksComposedBatch() {
        TemporaryDirectory temporary;
        const auto firstPath =
            temporary.path / "first.bin";
        const auto secondPath =
            temporary.path / "second.bin";
        write(firstPath, "first");
        write(secondPath, "second");
        const AssetGuid first = guid(
            "019f9bce-85b8-7620-8203-040506070809");
        const AssetGuid second = guid(
            "019f9bce-85b8-7621-8203-040506070809");
        AssetSourceMonitor monitor(
            0, std::chrono::seconds(1),
            {}, false);
        const TrackedSourceFile firstSource{
            firstPath, sha256File(firstPath),
        };
        const TrackedSourceFile secondSource{
            secondPath, sha256File(secondPath),
        };
        monitor.trackAsset(
            first, std::span(&firstSource, 1),
            { assetDependency(second) });
        monitor.trackAsset(
            second, std::span(&secondSource, 1),
            { assetDependency(first) });
        write(firstPath, "changed-first");
        monitor.processOnce(UINT64_MAX);
        const auto batches =
            monitor.drainBatches();
        CHECK(batches.size() == 1);
        CHECK(batches[0].blocked());
        CHECK(batches[0].rebuildOrder.empty());
        CHECK(batches[0].diagnostics[0].code ==
            "ASSET_DEPENDENCY_CYCLE");
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*run)();
    };
    constexpr TestCase tests[]{
        { "watch, hash, and dependency composition",
            composesWatchHashAndDependencyOrder },
        { "same-content suppression",
            sameContentTimestampIsSuppressed },
        { "cycle blocks composed batch",
            cycleBlocksComposedBatch },
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
