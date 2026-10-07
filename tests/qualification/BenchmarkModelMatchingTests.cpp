// M7C P1 multi-model benchmark fixtures: cooked model artifacts are matched to
// fixture sources by asset identity (the sidecar GUID), independent of
// argument order, with clear failures for missing, extra, ambiguous and stale
// artifacts.
#include "qualification/harness/BenchmarkModelMatching.h"

#include "assets/AssetMetadata.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

    using namespace Iridium;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    AssetGuid guid(const char* text) { return *AssetGuid::parse(text); }

    const AssetGuid kCarA = guid("01a07f17-9455-7bc1-9d5f-d54165583727");
    const AssetGuid kCarB = guid("01a0306a-b1b6-755a-8ccb-0d73effd7897");
    const AssetGuid kCarC = guid("019fc681-2110-7000-8000-000000000001");

    BenchmarkModelSource source(const char* path, std::optional<AssetGuid> id) {
        return { .sourceAsset = path, .assetGuid = id };
    }

    BenchmarkModelArtifact artifact(const char* path, AssetGuid id,
        std::vector<AssetDependency> dependencies = {}) {
        return { .artifactPath = path, .assetGuid = id,
            .dependencies = std::move(dependencies) };
    }

    // Files beside the sources: path -> content hash.
    BenchmarkDependencyHasher hasher(
        std::map<std::filesystem::path, std::string> files) {
        return [files = std::move(files)](const std::filesystem::path& path)
            -> std::optional<std::string> {
            const auto found = files.find(path);
            if (found == files.end()) return std::nullopt;
            return found->second;
        };
    }

    // The diagnostic of a failed match, or empty when it succeeded.
    std::string failure(std::span<const BenchmarkModelSource> sources,
        std::span<const BenchmarkModelArtifact> artifacts,
        const BenchmarkDependencyHasher& hash = hasher({})) {
        try {
            (void)matchBenchmarkModelArtifacts(sources, artifacts, hash);
        }
        catch (const std::exception& exception) {
            return exception.what();
        }
        return {};
    }

    bool contains(const std::string& text, const char* part) {
        return text.find(part) != std::string::npos;
    }

    bool testMatchesByIdentityNotOrder() {
        const std::vector<BenchmarkModelSource> sources{
            source("lib/a/a.gltf", kCarA), source("lib/b/b.gltf", kCarB) };
        // Startup artifact first, then the rest in either order.
        const std::vector<BenchmarkModelArtifact> forward{
            artifact("ddc/a.irartifact", kCarA), artifact("ddc/b.irartifact", kCarB) };
        const std::vector<BenchmarkModelArtifact> reversed{
            artifact("ddc/b.irartifact", kCarB), artifact("ddc/a.irartifact", kCarA) };
        CHECK((matchBenchmarkModelArtifacts(sources, forward, hasher({})) ==
            std::vector<size_t>{ 0, 1 }));
        CHECK((matchBenchmarkModelArtifacts(sources, reversed, hasher({})) ==
            std::vector<size_t>{ 1, 0 }));
        // Three sources, artifacts in a rotated order.
        const std::vector<BenchmarkModelSource> three{ source("a.gltf", kCarA),
            source("b.gltf", kCarB), source("c.gltf", kCarC) };
        const std::vector<BenchmarkModelArtifact> rotated{
            artifact("c", kCarC), artifact("a", kCarA), artifact("b", kCarB) };
        CHECK((matchBenchmarkModelArtifacts(three, rotated, hasher({})) ==
            std::vector<size_t>{ 1, 2, 0 }));
        return true;
    }

    bool testMissingExtraAndAmbiguousArtifactsFail() {
        const std::vector<BenchmarkModelSource> sources{
            source("lib/a/a.gltf", kCarA), source("lib/b/b.gltf", kCarB) };
        const std::vector<BenchmarkModelArtifact> onlyA{ artifact("ddc/a", kCarA) };
        const std::string missing = failure(sources, onlyA);
        CHECK(contains(missing, "No cooked model artifact"));
        CHECK(contains(missing, "lib/b/b.gltf"));

        const std::vector<BenchmarkModelArtifact> extra{ artifact("ddc/a", kCarA),
            artifact("ddc/b", kCarB), artifact("ddc/c", kCarC) };
        const std::string unused = failure(sources, extra);
        CHECK(contains(unused, "ddc/c"));
        CHECK(contains(unused, "matches no source"));

        const std::vector<BenchmarkModelArtifact> twice{ artifact("ddc/a", kCarA),
            artifact("ddc/a-lod", kCarA), artifact("ddc/b", kCarB) };
        const std::string ambiguous = failure(sources, twice);
        CHECK(contains(ambiguous, "both match"));
        CHECK(contains(ambiguous, "ddc/a-lod"));
        return true;
    }

    bool testSourceIdentityRequired() {
        const std::vector<BenchmarkModelArtifact> artifacts{
            artifact("ddc/a", kCarA), artifact("ddc/b", kCarB) };
        const std::vector<BenchmarkModelSource> noSidecar{
            source("lib/a/a.gltf", kCarA), source("lib/b/b.gltf", std::nullopt) };
        const std::string unidentified = failure(noSidecar, artifacts);
        CHECK(contains(unidentified, "metadata sidecar"));
        CHECK(contains(unidentified, "lib/b/b.gltf"));
        const std::vector<BenchmarkModelSource> shared{
            source("lib/a/a.gltf", kCarA), source("lib/b/b.gltf", kCarA) };
        CHECK(contains(failure(shared, artifacts), "share asset"));
        return true;
    }

    bool testStaleDependenciesFail() {
        const std::vector<BenchmarkModelSource> sources{
            source("lib/a/a.gltf", kCarA), source("lib/b/b.gltf", kCarB) };
        const std::vector<AssetDependency> dependencies{
            { .type = AssetDependencyType::SourceFile, .location = "scene.bin",
                .contentHash = "aaaa" },
            { .type = AssetDependencyType::SourceFile,
                .location = "textures/paint.png", .contentHash = "bbbb" },
            // Not source files: never compared.
            { .type = AssetDependencyType::Tool, .location = "scene.bin",
                .contentHash = "ffff" },
        };
        const std::vector<BenchmarkModelArtifact> artifacts{
            artifact("ddc/a", kCarA), artifact("ddc/b", kCarB, dependencies) };
        // Dependencies resolve beside their own source.
        const std::string current = failure(sources, artifacts, hasher({
            { "lib/b/scene.bin", "aaaa" }, { "lib/b/textures/paint.png", "bbbb" },
            { "lib/a/scene.bin", "9999" } }));
        CHECK(current.empty());
        // A dependency absent beside the source (another asset root) is not
        // judged.
        CHECK(failure(sources, artifacts, hasher({ { "lib/b/scene.bin", "aaaa" } }))
            .empty());
        const std::string stale = failure(sources, artifacts, hasher({
            { "lib/b/scene.bin", "aaaa" }, { "lib/b/textures/paint.png", "cccc" } }));
        CHECK(contains(stale, "stale"));
        CHECK(contains(stale, "textures/paint.png"));
        CHECK(contains(stale, "ddc/b"));
        return true;
    }

    bool testSidecarIdentity() {
        const std::filesystem::path directory =
            std::filesystem::temp_directory_path() /
            ("iridium-benchmark-model-matching-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
        const std::filesystem::path withSidecar = directory / "car.gltf";
        const std::filesystem::path without = directory / "bare.gltf";
        AssetMetadata metadata{};
        metadata.assetGuid = kCarB;
        metadata.assetType = "iridium.model";
        metadata.importerId = "iridium.gltf-model";
        metadata.importerVersion = 1;
        std::string error;
        bool passed = writeAssetMetadataAtomic(
            assetMetadataSidecarPath(withSidecar), metadata, error);
        const BenchmarkModelSource identified = readBenchmarkModelSource(withSidecar);
        const BenchmarkModelSource bare = readBenchmarkModelSource(without);
        passed = passed && identified.sourceAsset == withSidecar &&
            identified.assetGuid == kCarB && !bare.assetGuid;
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        return passed;
    }

    struct TestCase {
        const char* name;
        bool (*run)();
    };

} // namespace

int main() {
    const TestCase tests[] = {
        { "matches by identity, not order", testMatchesByIdentityNotOrder },
        { "missing, extra and ambiguous artifacts fail",
            testMissingExtraAndAmbiguousArtifactsFail },
        { "source identity required", testSourceIdentityRequired },
        { "stale dependencies fail", testStaleDependenciesFail },
        { "sidecar identity", testSidecarIdentity },
    };
    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.run()) std::cout << "[PASS] " << test.name << '\n';
            else { ++failures; std::cerr << "[FAIL] " << test.name << '\n'; }
        }
        catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << exception.what() << '\n';
        }
    }
    std::cout << std::size(tests) - failures << '/' << std::size(tests)
        << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
