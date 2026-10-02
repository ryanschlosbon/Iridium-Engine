#include "assets/AssetMetadata.h"
#include "assets/cooker/AssetCooker.h"
#include "assets/cooker/CanonicalSettings.h"
#include "assets/cooker/DependencyGraph.h"
#include "assets/cooker/TextFixtureImporter.h"
#include "assets/texture/TextureImporter.h"
#include "assets/texture/TextureProduct.h"
#include "utils/Sha256.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace {

    using namespace Iridium;
    using namespace std::chrono_literals;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    AssetGuid guid(uint64_t timestamp, uint8_t seed) {
        std::array<uint8_t, 10> random{};
        for (size_t index = 0; index < random.size(); ++index) {
            random[index] = static_cast<uint8_t>(seed + index);
        }
        return AssetGuid::fromUuidV7Fields(timestamp, random);
    }

    std::vector<std::byte> bytes(std::string_view text) {
        return {
            reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size()),
        };
    }

    std::vector<std::byte> bmp2x2() {
        std::vector<std::byte> result(70);
        const auto put16 = [&result](size_t offset, uint16_t value) {
            result[offset] = static_cast<std::byte>(value & 0xffu);
            result[offset + 1] = static_cast<std::byte>(value >> 8u);
        };
        const auto put32 = [&result](size_t offset, uint32_t value) {
            for (uint32_t shift = 0; shift < 32; shift += 8) {
                result[offset++] =
                    static_cast<std::byte>((value >> shift) & 0xffu);
            }
        };
        result[0] = std::byte{ 'B' };
        result[1] = std::byte{ 'M' };
        put32(2, static_cast<uint32_t>(result.size()));
        put32(10, 54);
        put32(14, 40);
        put32(18, 2);
        put32(22, 2);
        put16(26, 1);
        put16(28, 32);
        put32(34, 16);
        const std::array<uint8_t, 16> pixels{
            0, 0, 255, 255, 0, 255, 0, 255,
            255, 0, 0, 255, 255, 255, 255, 255,
        };
        for (size_t index = 0; index < pixels.size(); ++index) {
            result[54 + index] = static_cast<std::byte>(pixels[index]);
        }
        return result;
    }

    void writeBytes(const std::filesystem::path& path,
        std::span<const std::byte> data) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    }

    std::vector<std::byte> readBytes(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        const std::streamsize size = input.tellg();
        input.seekg(0, std::ios::beg);
        std::vector<std::byte> result(static_cast<size_t>(size));
        input.read(reinterpret_cast<char*>(result.data()), size);
        return result;
    }

    struct TemporaryDirectory {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("iridium-cooker-tests-" + createAssetGuidV7().toString());

        TemporaryDirectory() { std::filesystem::create_directories(path); }
        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };

    CookedArtifactBlob deterministicArtifact(std::string cookKey = {}) {
        if (cookKey.empty()) cookKey = sha256(bytes("deterministic-cook-key"));
        const std::string dependencyHash = sha256(bytes("dependency"));
        return serializeCookedArtifact({
            .assetGuid = guid(1'800'000'000'000ull, 3),
            .artifactType = "iridium.fixture-text",
            .artifactSchemaVersion = 1,
            .target = {
                .platform = "windows-x64",
                .profile = "release",
                .qualityPolicy = "reference",
            },
            .cookKey = std::move(cookKey),
            .dependencies = {
                {
                    .type = AssetDependencyType::SourceFile,
                    .location = "dependency.txt",
                    .contentHash = dependencyHash,
                },
            },
            .sections = {
                { 7, 2, 32, bytes("secondary") },
                { 1, 1, 16, bytes("primary payload") },
            },
        });
    }

    class DelegatingImporter final : public AssetImporter {
    public:
        explicit DelegatingImporter(std::string id, bool accepts)
            : m_descriptor{
                .id = std::move(id),
                .implementationVersion = 1,
                .currentSettingsSchemaVersion = 2,
                .assetTypes = { "iridium.fixture-text" },
                .extensions = { ".irtest" },
            },
              m_accepts(accepts) {}

        const ImporterDescriptor& descriptor() const noexcept override {
            return m_descriptor;
        }
        ImportProbeResult probe(const std::filesystem::path&,
            std::span<const std::byte>) const override {
            return m_accepts ? ImportProbeResult::Supported
                : ImportProbeResult::Unsupported;
        }
        NormalizedImportSettings normalizeSettings(uint32_t version,
            const nlohmann::json& settings, bool strict) const override {
            return m_delegate.normalizeSettings(version, settings, strict);
        }
        ParsedSourceAsset parse(const ImportSource& source,
            const NormalizedImportSettings& settings) const override {
            return m_delegate.parse(source, settings);
        }
        CookProduct cook(const ParsedSourceAsset& source,
            const NormalizedImportSettings& settings,
            const CookTarget& target,
            const AssetCookContext& context,
            std::stop_token stopToken) const override {
            return m_delegate.cook(
                source, settings, target,
                context, stopToken);
        }

    private:
        ImporterDescriptor m_descriptor;
        bool m_accepts = false;
        TextFixtureImporter m_delegate;
    };

    bool testCanonicalSettingsAndMigration() {
        const nlohmann::json first{
            { "z", 3u },
            { "a", { { "float", 1.5 }, { "signed", -2 } } },
        };
        const nlohmann::json reordered{
            { "a", { { "signed", -2 }, { "float", 1.5 } } },
            { "z", 3u },
        };
        const auto canonicalFirst = canonicalizeSettings(first);
        const auto canonicalSecond = canonicalizeSettings(reordered);
        CHECK(canonicalFirst.valid());
        CHECK(canonicalFirst.bytes == canonicalSecond.bytes);

        nlohmann::json nonfinite = nlohmann::json::object();
        nonfinite["value"] = std::numeric_limits<double>::infinity();
        CHECK(!canonicalizeSettings(nonfinite).valid());

        TextFixtureImporter importer;
        const auto migrated = importer.normalizeSettings(
            1, { { "uppercase", true } }, true);
        CHECK(migrated.valid());
        CHECK(migrated.schemaVersion == 2);
        CHECK(migrated.values.at("transform") == "uppercase");
        CHECK(migrated.values.at("repeat") == 1);
        const auto unknownStrict = importer.normalizeSettings(2, {
            { "transform", "identity" }, { "repeat", 1u }, { "future", true },
        }, true);
        CHECK(!unknownStrict.valid());
        const auto unknownRecovery = importer.normalizeSettings(2, {
            { "transform", "identity" }, { "repeat", 1u }, { "future", true },
        }, false);
        CHECK(unknownRecovery.valid());
        CHECK(unknownRecovery.values.contains("future"));
        CHECK(!importer.normalizeSettings(99, nlohmann::json::object(), true).valid());
        return true;
    }

    bool testImporterSelectionIsOrderIndependent() {
        const auto text = std::make_shared<TextFixtureImporter>();
        const auto reject = std::make_shared<DelegatingImporter>("aaa.reject", false);
        const auto source = bytes("IRIDIUM_TEXT\n\npayload");

        ImporterRegistry first;
        first.registerImporter(text);
        first.registerImporter(reject);
        ImporterRegistry second;
        second.registerImporter(reject);
        second.registerImporter(text);
        CHECK(first.descriptors() == second.descriptors());
        CHECK(first.selectAutomatic("fixture.irtest", source).importer
            ->descriptor().id == "iridium.fixture-text");
        CHECK(second.selectAutomatic("fixture.irtest", source).importer
            ->descriptor().id == "iridium.fixture-text");
        CHECK(first.selectExplicit("iridium.fixture-text", 1).valid());
        CHECK(!first.selectExplicit("iridium.fixture-text", 2).valid());

        ImporterRegistry ambiguous;
        ambiguous.registerImporter(text);
        ambiguous.registerImporter(
            std::make_shared<DelegatingImporter>("zzz.also-accepts", true));
        const ImporterSelection selection =
            ambiguous.selectAutomatic("fixture.irtest", source);
        CHECK(!selection.valid());
        CHECK(selection.diagnostics.front().code == "IMPORTER_PROBE_AMBIGUOUS");

        bool duplicateRejected = false;
        try {
            first.registerImporter(std::make_shared<TextFixtureImporter>());
        } catch (const std::invalid_argument&) {
            duplicateRejected = true;
        }
        CHECK(duplicateRejected);
        return true;
    }

    AssetDependency assetDependency(AssetGuid dependency) {
        return {
            .type = AssetDependencyType::Asset,
            .assetGuid = dependency,
            .artifactHash = sha256(bytes(dependency.toString())),
        };
    }

    bool testDependencyGraphInvalidationAndCycles() {
        const AssetGuid a = guid(100, 1);
        const AssetGuid b = guid(101, 2);
        const AssetGuid c = guid(102, 3);
        const AssetGuid d = guid(103, 4);
        AssetDependencyGraph graph;
        graph.setDependencies(a, { assetDependency(b) });
        graph.setDependencies(c, { assetDependency(b) });
        graph.setDependencies(d, { assetDependency(a), {
            .type = AssetDependencyType::SourceFile,
            .location = "not-an-asset-edge",
            .contentHash = sha256(bytes("source")),
        } });
        CHECK(graph.reverseDependents(b) == std::vector<AssetGuid>({ a, c }));
        CHECK(graph.invalidationClosure(std::array{ b }) ==
            std::vector<AssetGuid>({ a, b, c, d }));
        CHECK(graph.cycles().empty());

        graph.setDependencies(b, { assetDependency(c) });
        graph.setDependencies(c, { assetDependency(a) });
        const auto cycles = graph.cycles();
        CHECK(cycles.size() == 1);
        CHECK(cycles.front().chain.front() == a);
        CHECK(cycles.front().chain.back() == a);
        CHECK(cycles.front().chain.size() == 4);
        graph.removeAsset(b);
        CHECK(graph.directDependencies(b).empty());
        CHECK(graph.cycles().empty());
        CHECK(graph.reverseDependents(b) ==
            std::vector<AssetGuid>({ a }));
        return true;
    }

    bool testCookKeyInvalidationAndOrdering() {
        const AssetGuid asset = guid(200, 1);
        const AssetGuid dependencyGuid = guid(201, 2);
        const auto canonical = canonicalizeSettings({
            { "repeat", 1u }, { "transform", "identity" },
        });
        const AssetDependency first{
            .type = AssetDependencyType::SourceFile,
            .location = "b.txt",
            .contentHash = sha256(bytes("b")),
        };
        const AssetDependency second{
            .type = AssetDependencyType::Asset,
            .assetGuid = dependencyGuid,
            .artifactHash = sha256(bytes("asset")),
        };
        const CookTarget target{
            .platform = "windows-x64",
            .profile = "release",
            .qualityPolicy = "reference",
        };
        const std::array dependenciesA{ first, second };
        const std::array dependenciesB{ second, first };
        const auto key = [&](std::string sourceHash,
            std::span<const AssetDependency> dependencies) {
            return calculateCookKey({
                .assetGuid = asset,
                .importerId = "iridium.fixture-text",
                .importerImplementationVersion = 1,
                .settingsSchemaVersion = 2,
                .canonicalSettings = canonical.bytes,
                .sourceContentHash = std::move(sourceHash),
                .dependencies = dependencies,
                .target = target,
                .cookerFeatureVersion = "m3.2-framework-v1",
            });
        };
        CHECK(key(sha256(bytes("source")), dependenciesA) ==
            key(sha256(bytes("source")), dependenciesB));
        CHECK(key(sha256(bytes("source")), dependenciesA) !=
            key(sha256(bytes("changed")), dependenciesA));
        auto changedDependency = dependenciesA;
        changedDependency[0].contentHash = sha256(bytes("changed-dependency"));
        CHECK(key(sha256(bytes("source")), dependenciesA) !=
            key(sha256(bytes("source")), changedDependency));
        return true;
    }

    bool testArtifactValidationAndThreadDeterminism() {
        const CookedArtifactBlob first = deterministicArtifact();
        const CookedArtifactBlob second = deterministicArtifact();
        CHECK(first.bytes == second.bytes);
        CHECK(first.artifactHash == second.artifactHash);
        const auto decoded = readCookedArtifact(first.bytes, first.artifactHash);
        CHECK(decoded.valid());
        CHECK(decoded.artifact->sections.size() == 2);
        CHECK(decoded.artifact->sections[0].id == 1);

        std::vector<std::future<CookedArtifactBlob>> futures;
        for (size_t index = 0; index < 8; ++index) {
            futures.push_back(std::async(std::launch::async,
                [] { return deterministicArtifact(); }));
        }
        for (auto& future : futures) {
            CHECK(future.get().bytes == first.bytes);
        }

        auto corrupt = first.bytes;
        corrupt.back() ^= std::byte{ 0x01 };
        CHECK(!readCookedArtifact(corrupt).valid());
        corrupt = first.bytes;
        corrupt[50] ^= std::byte{ 0x01 };
        CHECK(!readCookedArtifact(corrupt).valid());
        corrupt.resize(kCookedArtifactHeaderSize - 1);
        CHECK(!readCookedArtifact(corrupt).valid());
        return true;
    }

    bool testTrackedImporterCookAndDependencyInvalidation() {
        const std::filesystem::path root =
            std::filesystem::path(PROJECT_ROOT_DIR) / "tests" / "assets";
        const auto metadataRead = readAssetMetadata(
            root / "cooker_fixture.irtest.iridium.meta");
        CHECK(metadataRead.metadata.has_value());
        ImporterRegistry registry;
        registry.registerImporter(std::make_shared<TextFixtureImporter>());
        const CookTarget target{
            .platform = "windows-x64",
            .profile = "release",
            .qualityPolicy = "reference",
        };
        const PreparedAssetCook first = prepareAssetCook(registry, root,
            "cooker_fixture.irtest", *metadataRead.metadata, target,
            "m3.2-framework-v1");
        CHECK(first.valid());
        CHECK(first.resolvedDependencies.size() == 1);
        const auto firstArtifact = buildPreparedArtifact(first);
        const auto secondArtifact = buildPreparedArtifact(first);
        CHECK(firstArtifact.bytes == secondArtifact.bytes);
        std::vector<std::future<CookedArtifactBlob>> threadedCooks;
        for (size_t index = 0; index < 8; ++index) {
            threadedCooks.push_back(std::async(std::launch::async,
                [&first] { return buildPreparedArtifact(first); }));
        }
        for (auto& cook : threadedCooks) {
            CHECK(cook.get().bytes == firstArtifact.bytes);
        }

        TemporaryDirectory temporary;
        std::filesystem::copy_file(root / "cooker_fixture.irtest",
            temporary.path / "cooker_fixture.irtest");
        std::filesystem::copy_file(root / "cooker_fixture_dependency.txt",
            temporary.path / "cooker_fixture_dependency.txt");
        const PreparedAssetCook copied = prepareAssetCook(registry, temporary.path,
            "cooker_fixture.irtest", *metadataRead.metadata, target,
            "m3.2-framework-v1");
        CHECK(copied.cookKey == first.cookKey);
        writeBytes(temporary.path / "cooker_fixture_dependency.txt",
            bytes("dependency revision two\n"));
        const PreparedAssetCook invalidated = prepareAssetCook(registry,
            temporary.path, "cooker_fixture.irtest", *metadataRead.metadata,
            target, "m3.2-framework-v1");
        CHECK(invalidated.valid());
        CHECK(invalidated.sourceContentHash == copied.sourceContentHash);
        CHECK(invalidated.cookKey != copied.cookKey);
        return true;
    }

    bool testDdcCoalescingCorruptionAndCancellation() {
        TemporaryDirectory temporary;
        LocalDerivedDataCache cache(temporary.path / "ddc");
        const std::string key = sha256(bytes("coalesced-key"));
        const CookedArtifactBlob artifact = deterministicArtifact(key);
        std::atomic<uint32_t> buildCount{ 0 };
        std::vector<std::shared_future<DdcRequestResult>> requests;
        for (size_t index = 0; index < 16; ++index) {
            requests.push_back(cache.request(key, {},
                [&buildCount, &artifact](std::stop_token) {
                    ++buildCount;
                    std::this_thread::sleep_for(20ms);
                    return artifact;
                }));
        }
        for (auto& request : requests) {
            CHECK(request.get().status == DdcRequestStatus::Built);
        }
        CHECK(buildCount == 1);
        CHECK(cache.probe(key).status == DdcLookupStatus::Hit);

        const auto hit = cache.request(key, {},
            [&buildCount](std::stop_token) {
                ++buildCount;
                throw std::runtime_error("cache hit must not build");
                return CookedArtifactBlob{};
            }).get();
        CHECK(hit.status == DdcRequestStatus::CacheHit);
        CHECK(buildCount == 1);

        const std::filesystem::path entry = cache.entryPath(key);
        {
            std::fstream file(entry, std::ios::binary | std::ios::in | std::ios::out);
            file.seekp(-1, std::ios::end);
            const char changed = '\xff';
            file.write(&changed, 1);
        }
        const auto rebuilt = cache.request(key, {},
            [&buildCount, &artifact](std::stop_token) {
                ++buildCount;
                return artifact;
            }).get();
        CHECK(rebuilt.status == DdcRequestStatus::Built);
        CHECK(buildCount == 2);
        CHECK(cache.read(key).status == DdcLookupStatus::Hit);
        CHECK(std::filesystem::is_directory(cache.root() / "quarantine"));
        CHECK(std::filesystem::directory_iterator(cache.root() / "quarantine") !=
            std::filesystem::directory_iterator{});

        writeBytes(entry, std::span(artifact.bytes).first(32));
        const auto rebuiltTruncated = cache.request(key, {},
            [&buildCount, &artifact](std::stop_token) {
                ++buildCount;
                return artifact;
            }).get();
        CHECK(rebuiltTruncated.status == DdcRequestStatus::Built);
        CHECK(buildCount == 3);
        CHECK(cache.read(key).status == DdcLookupStatus::Hit);

        const std::string cancelledKey = sha256(bytes("cancelled-key"));
        std::stop_source cancellation;
        cancellation.request_stop();
        const auto cancelled = cache.request(cancelledKey,
            cancellation.get_token(), [&buildCount](std::stop_token) {
                ++buildCount;
                return deterministicArtifact();
            }).get();
        CHECK(cancelled.status == DdcRequestStatus::Cancelled);
        CHECK(cache.probe(cancelledKey).status == DdcLookupStatus::Miss);
        CHECK(std::none_of(
            std::filesystem::recursive_directory_iterator(cache.root()),
            std::filesystem::recursive_directory_iterator{},
            [](const std::filesystem::directory_entry& entry) {
                return entry.path().extension() == ".tmp";
            }));
        return true;
    }

    bool testCleanCacheRebuildEquality() {
        TemporaryDirectory firstDirectory;
        TemporaryDirectory secondDirectory;
        const std::string key = sha256(bytes("clean-cache-key"));
        const CookedArtifactBlob artifact = deterministicArtifact(key);
        LocalDerivedDataCache first(firstDirectory.path / "ddc");
        LocalDerivedDataCache second(secondDirectory.path / "ddc");
        const auto firstResult = first.request(key, {},
            [&artifact](std::stop_token) { return artifact; }).get();
        const auto secondResult = second.request(key, {},
            [&artifact](std::stop_token) { return artifact; }).get();
        CHECK(firstResult.status == DdcRequestStatus::Built);
        CHECK(secondResult.status == DdcRequestStatus::Built);
        CHECK(firstResult.blob->bytes == secondResult.blob->bytes);
        CHECK(firstResult.blob->artifactHash == secondResult.blob->artifactHash);
        return true;
    }

    bool testProductionTextureCookAndDdc() {
        TemporaryDirectory temporary;
        writeBytes(temporary.path / "fixture.bmp", bmp2x2());
        AssetMetadata metadata{
            .assetGuid = guid(1'900'000'000'000ull, 11),
            .assetType = "iridium.texture",
            .importerId = "iridium.texture.directxtex",
            .importerVersion = kDirectXTexCodecVersion,
            .settingsSchemaVersion = 1,
            .settings = {
                { "semantic", "color" },
                { "quality", "iteration" },
                { "mip_policy", "full_chain" },
                { "alpha_mode", "opaque" },
                { "view_color_space", "srgb" },
            },
        };
        ImporterRegistry registry;
        registry.registerImporter(std::make_shared<TextureImporter>());
        const PreparedAssetCook prepared = prepareAssetCook(
            registry, temporary.path, "fixture.bmp", metadata, {
                .platform = "windows-x64",
                .profile = "release",
                .qualityPolicy = "reference",
            }, "m3.3-texture-v1");
        CHECK(prepared.valid());
        CHECK(prepared.resolvedDependencies.size() == 1);
        CHECK(prepared.resolvedDependencies.front().type ==
            AssetDependencyType::Tool);
        CHECK(!prepared.resolvedDependencies.front().contentHash.empty());

        const CookedArtifactBlob first = buildPreparedArtifact(prepared);
        const CookedArtifactBlob second = buildPreparedArtifact(prepared);
        CHECK(first.bytes == second.bytes);
        CHECK(first.artifactHash == second.artifactHash);
        const CookedArtifactReadResult decoded =
            readCookedArtifact(first.bytes, first.artifactHash);
        CHECK(decoded.valid());
        CHECK(decoded.artifact->artifactType == "iridium.texture");
        CHECK(decoded.artifact->sections.size() == 2);
        CHECK(decoded.artifact->sections[0].id ==
            kCookedTextureManifestSection);
        CHECK(decoded.artifact->sections[1].id ==
            kCookedTexturePayloadSection);

        LocalDerivedDataCache cache(temporary.path / "ddc");
        const DdcRequestResult built =
            requestPreparedCook(cache, prepared).get();
        CHECK(built.status == DdcRequestStatus::Built);
        CHECK(built.blob->bytes == first.bytes);
        const DdcRequestResult hit =
            requestPreparedCook(cache, prepared).get();
        CHECK(hit.status == DdcRequestStatus::CacheHit);
        CHECK(hit.blob->artifactHash == first.artifactHash);
        return true;
    }

    bool emitArtifact(const std::filesystem::path& path) {
        const std::filesystem::path root =
            std::filesystem::path(PROJECT_ROOT_DIR) / "tests" / "assets";
        const auto metadata = readAssetMetadata(
            root / "cooker_fixture.irtest.iridium.meta");
        if (!metadata.metadata) return false;
        ImporterRegistry registry;
        registry.registerImporter(std::make_shared<TextFixtureImporter>());
        const PreparedAssetCook prepared = prepareAssetCook(
            registry, root, "cooker_fixture.irtest", *metadata.metadata, {
                .platform = "windows-x64",
                .profile = "release",
                .qualityPolicy = "reference",
            }, "m3.2-framework-v1");
        if (!prepared.valid()) return false;
        const CookedArtifactBlob cooked = buildPreparedArtifact(prepared);
        writeBytes(path, cooked.bytes);
        return std::filesystem::file_size(path) == cooked.bytes.size();
    }

    bool testIndependentProcessDeterminism(
        const std::filesystem::path& executable) {
        TemporaryDirectory temporary;
        const std::filesystem::path first = temporary.path / "first.irartifact";
        const std::filesystem::path second = temporary.path / "second.irartifact";
        const auto run = [&executable](const std::filesystem::path& output) {
#if defined(_WIN32)
            std::wstring command = L"\"" + executable.wstring() +
                L"\" --emit-artifact \"" + output.wstring() + L"\"";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) == 0) {
                return -1;
            }
            WaitForSingleObject(process.hProcess, INFINITE);
            DWORD exitCode = 1;
            GetExitCodeProcess(process.hProcess, &exitCode);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return static_cast<int>(exitCode);
#else
            const std::string command = "\"" + executable.string() +
                "\" --emit-artifact \"" + output.string() + "\"";
            return std::system(command.c_str());
#endif
        };
        CHECK(run(first) == 0);
        CHECK(run(second) == 0);
        CHECK(readBytes(first) == readBytes(second));
        CHECK(sha256File(first) == sha256File(second));
        return true;
    }

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--emit-artifact") {
        return emitArtifact(argv[2]) ? 0 : 2;
    }

    const std::filesystem::path executable =
        std::filesystem::absolute(argv[0]);
    struct Test {
        const char* name;
        std::function<bool()> function;
    };
    const std::vector<Test> tests{
        { "canonical settings and migration", testCanonicalSettingsAndMigration },
        { "importer selection order", testImporterSelectionIsOrderIndependent },
        { "dependency invalidation and cycles", testDependencyGraphInvalidationAndCycles },
        { "cook key invalidation", testCookKeyInvalidationAndOrdering },
        { "artifact validation and threads", testArtifactValidationAndThreadDeterminism },
        { "tracked importer and dependency", testTrackedImporterCookAndDependencyInvalidation },
        { "DDC coalescing/corruption/cancellation", testDdcCoalescingCorruptionAndCancellation },
        { "clean-cache rebuild equality", testCleanCacheRebuildEquality },
        { "production texture cook and DDC", testProductionTextureCookAndDdc },
        { "independent-process determinism",
            [executable] { return testIndependentProcessDeterminism(executable); } },
    };

    size_t failures = 0;
    for (const Test& test : tests) {
        try {
            const bool passed = test.function();
            std::cout << (passed ? "[PASS] " : "[FAIL] ") << test.name << '\n';
            if (!passed) ++failures;
        } catch (const std::exception& exception) {
            std::cerr << "[FAIL] " << test.name << ": " << exception.what() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
