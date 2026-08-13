#include "assets/runtime/AssetRuntimeService.h"

#include "assets/AssetMetadata.h"
#include "assets/cooker/AssetCooker.h"
#include "assets/cooker/LocalDerivedDataCache.h"
#include "assets/model/GltfModelImporter.h"
#include "assets/model/ModelProduct.h"
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
                ("iridium-runtime-service-" +
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
                "Could not write runtime-service fixture.");
        }
    }

    AssetDependency assetDependency(
        AssetGuid asset) {
        return {
            .type = AssetDependencyType::Asset,
            .assetGuid = asset,
        };
    }

    PreparedRuntimeAsset prepared(
        char cookKeyCharacter,
        std::vector<AssetGuid>& publishes,
        AssetGuid asset) {
        return {
            .cookKey =
                std::string(
                    64, cookKeyCharacter),
            .estimatedUploadBytes = 8,
            .publish =
                [&publishes, asset] {
                    publishes.push_back(asset);
                    return RuntimeAssetPublishOutcome{
                        .succeeded = true,
                        .cpuResidentBytes = 4,
                        .gpuResidentBytes = 8,
                    };
                },
        };
    }

    bool endToEndDependencyRebuildAndPublish() {
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
            "019f9bce-85b8-7700-8203-040506070809");
        const AssetGuid material = guid(
            "019f9bce-85b8-7701-8203-040506070809");
        const AssetGuid model = guid(
            "019f9bce-85b8-7702-8203-040506070809");
        std::vector<AssetGuid> prepares;
        std::vector<AssetGuid> publishes;
        AssetRuntimeService service({
            .debounceNanoseconds = 0,
            .scanInterval =
                std::chrono::seconds(1),
            .uploadBudgetBytes = 24,
            .startSourceWorkers = false,
        });
        const auto tracked =
            [&](AssetGuid asset,
                const std::filesystem::path& path,
                std::vector<AssetDependency>
                    dependencies,
                char cookKey) {
                service.track({
                    .assetGuid = asset,
                    .sources = {{
                        path, sha256File(path),
                    }},
                    .dependencies =
                        std::move(dependencies),
                    .prepare =
                        [&, asset, cookKey](
                            const AssetReimportCause&
                                cause,
                            std::stop_token) {
                            if (cause.assetGuid !=
                                asset) {
                                throw std::runtime_error(
                                    "Reimport cause GUID mismatch.");
                            }
                            prepares.push_back(asset);
                            return prepared(
                                cookKey,
                                publishes, asset);
                        },
                });
            };
        tracked(texture, texturePath, {}, 'a');
        tracked(material, materialPath,
            { assetDependency(texture) }, 'b');
        tracked(model, modelPath,
            { assetDependency(material) }, 'c');

        write(texturePath, "texture-b-longer");
        service.processSourcesOnce(UINT64_MAX);
        AssetRuntimeServiceTick tick =
            service.tick();
        CHECK(tick.changeBatches == 1);
        CHECK(tick.rebuildsRequested == 3);

        for (int attempt = 0;
            attempt < 10'000 &&
                publishes.size() != 3;
            ++attempt) {
            std::this_thread::yield();
            tick = service.tick();
        }
        CHECK(prepares ==
            (std::vector{
                texture, material, model }));
        CHECK(publishes ==
            (std::vector{
                texture, material, model }));
        CHECK(service.snapshot(model)->state ==
            RuntimeAssetState::Ready);
        CHECK(service.stats().publisher
            .gpuResidentBytes == 24);
        return true;
    }

    bool uploadBudgetDefersPreparedPublication() {
        TemporaryDirectory temporary;
        const auto path =
            temporary.path / "asset.bin";
        write(path, "a");
        const AssetGuid asset = guid(
            "019f9bce-85b8-7710-8203-040506070809");
        int publishes = 0;
        AssetRuntimeService service({
            .debounceNanoseconds = 0,
            .scanInterval =
                std::chrono::seconds(1),
            .uploadBudgetBytes = 7,
            .startSourceWorkers = false,
        });
        service.track({
            .assetGuid = asset,
            .sources = {{
                path, sha256File(path),
            }},
            .prepare =
                [&](const AssetReimportCause&,
                    std::stop_token) {
                    return PreparedRuntimeAsset{
                        .cookKey =
                            std::string(64, 'd'),
                        .estimatedUploadBytes = 8,
                        .publish = [&] {
                            ++publishes;
                            return RuntimeAssetPublishOutcome{
                                .succeeded = true,
                            };
                        },
                    };
                },
        });
        write(path, "larger");
        service.processSourcesOnce(UINT64_MAX);
        (void)service.tick();
        for (int attempt = 0;
            attempt < 100 &&
                service.stats().reimport
                    .completed == 0 &&
                service.stats().publisher
                    .queued == 0;
            ++attempt) {
            std::this_thread::yield();
        }
        const AssetRuntimeServiceTick tick =
            service.tick();
        CHECK(tick.publication.published == 0);
        CHECK(tick.publication
            .deferredByBudget == 1);
        CHECK(publishes == 0);
        return true;
    }

    bool manualReimportUsesTrackedPreparer() {
        TemporaryDirectory temporary;
        const auto sourcePath = temporary.path / "manual.bin";
        write(sourcePath, "manual");
        const AssetGuid asset = guid(
            "019f9bce-85b8-7720-8203-040506070809");
        uint32_t preparations = 0;
        std::vector<AssetGuid> publishes;
        AssetRuntimeService service({
            .debounceNanoseconds = 0,
            .uploadBudgetBytes = 64,
            .startSourceWorkers = false,
        });
        service.track({
            .assetGuid = asset,
            .sources = {{
                sourcePath, sha256File(sourcePath),
            }},
            .prepare =
                [&](const AssetReimportCause& cause,
                    std::stop_token) {
                    if (cause.assetGuid != asset ||
                        !cause.changedSources.empty()) {
                        throw std::runtime_error(
                            "Manual reimport cause was not GUID-only.");
                    }
                    ++preparations;
                    return prepared('m', publishes, asset);
                },
        });
        CHECK(service.requestReimport(asset));
        CHECK(!service.requestReimport(guid(
            "019f9bce-85b8-7721-8203-040506070809")));
        for (int attempt = 0; attempt < 10'000 &&
            publishes.empty(); ++attempt) {
            std::this_thread::yield();
            (void)service.tick();
        }
        CHECK(preparations == 1);
        CHECK(publishes == std::vector<AssetGuid>{ asset });
        CHECK(service.snapshot(asset)->state ==
            RuntimeAssetState::Ready);
        return true;
    }

    bool externallyPreparedAssetUsesBudgetedPublisher() {
        const AssetGuid asset = guid(
            "019f9bce-85b8-7722-8203-040506070809");
        uint32_t publications = 0;
        AssetRuntimeService service({
            .uploadBudgetBytes = 64,
            .startSourceWorkers = false,
        });
        CHECK(service.enqueuePrepared(
            asset,
            PreparedRuntimeAsset{
                .cookKey = std::string(64, 'x'),
                .estimatedUploadBytes = 32,
                .publish = [&] {
                    ++publications;
                    return RuntimeAssetPublishOutcome{
                        .succeeded = true,
                        .cpuResidentBytes = 16,
                        .gpuResidentBytes = 32,
                    };
                },
            }));
        const AssetRuntimeServiceTick tick =
            service.tick();
        CHECK(tick.publication.published == 1);
        CHECK(publications == 1);
        CHECK(service.snapshot(asset)->state ==
            RuntimeAssetState::Ready);
        CHECK(service.snapshot(asset)->gpuResidentBytes == 32);
        CHECK(!service.enqueuePrepared(
            asset,
            PreparedRuntimeAsset{
                .cookKey = std::string(64, 'x'),
                .estimatedUploadBytes = 32,
                .publish = [] {
                    return RuntimeAssetPublishOutcome{};
                },
            }));
        service.reportFailure(
            asset, "later preparation failed");
        CHECK(service.snapshot(asset)->state ==
            RuntimeAssetState::ReadyWithError);
        CHECK(service.snapshot(asset)->diagnostic ==
            "later preparation failed");
        return true;
    }

    bool cycleAndMissingPreparerBecomeErrors() {
        TemporaryDirectory temporary;
        const auto firstPath =
            temporary.path / "first.bin";
        const auto secondPath =
            temporary.path / "second.bin";
        write(firstPath, "first");
        write(secondPath, "second");
        const AssetGuid first = guid(
            "019f9bce-85b8-7720-8203-040506070809");
        const AssetGuid second = guid(
            "019f9bce-85b8-7721-8203-040506070809");
        AssetRuntimeService service({
            .debounceNanoseconds = 0,
            .scanInterval =
                std::chrono::seconds(1),
            .startSourceWorkers = false,
        });
        const auto noOp =
            [](const AssetReimportCause&,
                std::stop_token) {
                return PreparedRuntimeAsset{
                    .cookKey =
                        std::string(64, 'e'),
                    .publish = [] {
                        return RuntimeAssetPublishOutcome{
                            .succeeded = true,
                        };
                    },
                };
            };
        service.track({
            .assetGuid = first,
            .sources = {{
                firstPath, sha256File(firstPath),
            }},
            .dependencies = {
                assetDependency(second),
            },
            .prepare = noOp,
        });
        service.track({
            .assetGuid = second,
            .sources = {{
                secondPath, sha256File(secondPath),
            }},
            .dependencies = {
                assetDependency(first),
            },
            .prepare = noOp,
        });
        write(firstPath, "changed-first");
        service.processSourcesOnce(UINT64_MAX);
        const AssetRuntimeServiceTick tick =
            service.tick();
        CHECK(tick.blockedBatches == 1);
        CHECK(service.snapshot(first)->state ==
            RuntimeAssetState::Failed);
        CHECK(service.snapshot(second)->state ==
            RuntimeAssetState::Failed);
        return true;
    }

    bool realSourceEditCooksAndPublishes() {
        TemporaryDirectory temporary;
        const std::filesystem::path source =
            temporary.path /
                "fixture.gltf";
        const std::filesystem::path metadataPath =
            temporary.path /
                "fixture.gltf.iridium.meta";
        const std::filesystem::path fixtureRoot =
            std::filesystem::path(
                PROJECT_ROOT_DIR) /
                "tests" / "assets";
        std::filesystem::copy_file(
            fixtureRoot /
                "gltf_model_cooker_fixture.gltf",
            source);
        std::filesystem::copy_file(
            fixtureRoot /
                "gltf_model_cooker_fixture.gltf.iridium.meta",
            metadataPath);
        const std::filesystem::path texturePath =
            temporary.path / "texture.png";
        const std::filesystem::path carTextures =
            std::filesystem::path(
                PROJECT_ROOT_DIR) /
                "assets" / "models" /
                "alfa_romeo" / "textures";
        std::filesystem::copy_file(
            carTextures /
                "ID04_plastic_textured_001_rtint_colors_001_diff_6_54_baseColor.png",
            texturePath);
        {
            nlohmann::json document;
            {
                std::ifstream input(source);
                input >> document;
            }
            document["images"][0]["uri"] =
                "texture.png";
            std::ofstream output(
                source,
                std::ios::binary |
                    std::ios::trunc);
            output << document.dump(2) <<
                '\n';
        }
        const AssetMetadataReadResult metadata =
            readAssetMetadata(metadataPath);
        CHECK(metadata.metadata.has_value());
        CHECK(!metadata.hasErrors());

        ImporterRegistry importers;
        importers.registerImporter(
            std::make_shared<
                GltfModelImporter>());
        LocalDerivedDataCache cache(
            temporary.path / "ddc");
        const CookTarget target{
            .platform = "windows-x64",
            .profile = "editor",
            .qualityPolicy = "reference",
            .artifactContainerVersion =
                kCookedArtifactContainerVersion,
            .materialSchemaVersion = 2,
        };
        PreparedAssetCook baseline =
            prepareAssetCook(
                importers, temporary.path,
                source.filename(),
                *metadata.metadata,
                target,
                "m3.2-framework-v1");
        CHECK(baseline.valid());
        DdcRequestResult baselineCook =
            requestPreparedCook(
                cache, baseline).get();
        CHECK(baselineCook.blob.has_value());
        const auto textureDependency =
            std::ranges::find_if(
                baseline.resolvedDependencies,
                [](const AssetDependency&
                    dependency) {
                    return dependency.type ==
                            AssetDependencyType::SourceFile &&
                        dependency.location ==
                            "texture.png";
                });
        CHECK(textureDependency !=
            baseline.resolvedDependencies.end());

        int publications = 0;
        std::string publishedCookKey;
        AssetRuntimeService service({
            .debounceNanoseconds = 0,
            .scanInterval =
                std::chrono::seconds(1),
            .uploadBudgetBytes =
                4ull * 1024ull * 1024ull,
            .startSourceWorkers = false,
        });
        const AssetGuid assetGuid =
            metadata.metadata->assetGuid;
        service.track({
            .assetGuid = assetGuid,
            .sources = {
                {
                    source,
                    baseline.sourceContentHash,
                },
                {
                    metadataPath,
                    sha256File(metadataPath),
                },
                {
                    texturePath,
                    textureDependency->contentHash,
                },
            },
            .prepare =
                [&](const AssetReimportCause&,
                    std::stop_token stopToken) {
                    const AssetMetadataReadResult
                        currentMetadata =
                            readAssetMetadata(
                                metadataPath);
                    if (!currentMetadata.metadata ||
                        currentMetadata.hasErrors()) {
                        throw std::runtime_error(
                            "Fixture metadata became invalid.");
                    }
                    PreparedAssetCook preparedCook =
                        prepareAssetCook(
                            importers,
                            temporary.path,
                            source.filename(),
                            *currentMetadata.metadata,
                            target,
                            "m3.2-framework-v1");
                    if (!preparedCook.valid()) {
                        throw std::runtime_error(
                            "Fixture source preparation failed.");
                    }
                    DdcRequestResult result =
                        requestPreparedCook(
                            cache,
                            preparedCook,
                            stopToken).get();
                    if (!result.blob) {
                        throw std::runtime_error(
                            "Fixture source cook failed.");
                    }
                    CookedArtifactReadResult
                        artifact =
                            readCookedArtifact(
                                result.blob->bytes,
                                result.blob
                                    ->artifactHash);
                    if (!artifact.valid() ||
                        !readCookedModelProduct(
                            *artifact.artifact)
                            .valid()) {
                        throw std::runtime_error(
                            "Cooked fixture product is invalid.");
                    }
                    const std::string cookKey =
                        preparedCook.cookKey;
                    return PreparedRuntimeAsset{
                        .cookKey = cookKey,
                        .estimatedUploadBytes =
                            result.blob->bytes.size(),
                        .publish =
                            [&, cookKey] {
                                ++publications;
                                publishedCookKey =
                                    cookKey;
                                return RuntimeAssetPublishOutcome{
                                    .succeeded = true,
                                };
                            },
                    };
                },
            .pinned = true,
        });
        service.adoptPublished(
            assetGuid, baseline.cookKey,
            0, 0);
        std::filesystem::copy_file(
            carTextures /
                "ID04_plastic_textured_001_rtint_colors_001_diff_6_73_metallicRoughness.png",
            texturePath,
            std::filesystem::copy_options::
                overwrite_existing);
        service.processSourcesOnce(
            UINT64_MAX);
        AssetRuntimeServiceTick tick =
            service.tick();
        CHECK(tick.rebuildsRequested == 1);
        for (int attempt = 0;
            attempt < 2'000 &&
                publications == 0;
            ++attempt) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
            (void)service.tick();
        }
        CHECK(publications == 1);
        CHECK(publishedCookKey !=
            baseline.cookKey);
        CHECK(service.snapshot(assetGuid)
            ->revision == 2);
        CHECK(service.snapshot(assetGuid)
            ->cookKey == publishedCookKey);
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*run)();
    };
    constexpr TestCase tests[]{
        { "dependency rebuild and publish",
            endToEndDependencyRebuildAndPublish },
        { "upload budget deferral",
            uploadBudgetDefersPreparedPublication },
        { "manual reimport",
            manualReimportUsesTrackedPreparer },
        { "external prepared publication",
            externallyPreparedAssetUsesBudgetedPublisher },
        { "cycle error propagation",
            cycleAndMissingPreparerBecomeErrors },
        { "real texture edit cook and publish",
            realSourceEditCooksAndPublishes },
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
