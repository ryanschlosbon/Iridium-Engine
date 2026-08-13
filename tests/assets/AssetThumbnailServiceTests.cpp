#include "assets/thumbnail/AssetThumbnailService.h"
#include "assets/thumbnail/AssetThumbnailUploadQueue.h"
#include "assets/model/AssetModelPreparationService.h"
#include "assets/texture/TextureImporter.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

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

    struct TemporaryDirectory {
        std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("iridium-thumbnail-service-" +
                createAssetGuidV7().toString());
        TemporaryDirectory() {
            std::filesystem::create_directories(path);
        }
        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };

    CookTarget target() {
        return {
            .platform = "windows-x64",
            .profile = "editor",
            .qualityPolicy = "reference",
            .artifactContainerVersion =
                kCookedArtifactContainerVersion,
            .materialSchemaVersion = 2,
        };
    }

    AssetCatalogRecord rootRecord() {
        return {
            .guid = *AssetGuid::parse(
                "019f9bce-85b8-7100-8203-040506070809"),
            .assetType = "iridium.model",
            .assetRoot = "project",
            .sourcePath = "fixture.gltf",
            .metadataPath =
                "fixture.gltf.iridium.meta",
            .status = AssetCatalogStatus::Ready,
        };
    }

    void copyFixture(
        const std::filesystem::path& destination) {
        const std::filesystem::path fixtureRoot =
            std::filesystem::path(PROJECT_ROOT_DIR) /
            "tests" / "assets";
        std::filesystem::copy_file(
            fixtureRoot /
                "gltf_model_cooker_fixture.gltf",
            destination / "fixture.gltf");
        std::filesystem::copy_file(
            fixtureRoot /
                "gltf_model_cooker_fixture.gltf.iridium.meta",
            destination /
                "fixture.gltf.iridium.meta");
    }

    std::vector<PreparedAssetThumbnailBatch>
        waitForResults(
            AssetThumbnailService& service) {
        for (int attempt = 0;
            attempt < 10'000; ++attempt) {
            auto results =
                service.takeResults();
            if (!results.empty()) {
                return results;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
        }
        return {};
    }

    bool waitForStatus(
        AssetThumbnailService& service,
        AssetGuid guid,
        AssetThumbnailStatus status) {
        for (int attempt = 0;
            attempt < 10'000; ++attempt) {
            if (service.info(guid).status ==
                status) {
                return true;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
        }
        return false;
    }

    bool prepareModel(
        AssetModelPreparationService& service,
        const AssetCatalogRecord& record) {
        if (!service.request(record)) {
            return false;
        }
        for (int attempt = 0;
            attempt < 10'000; ++attempt) {
            auto results = service.takeResults();
            if (!results.empty()) {
                return results.size() == 1 &&
                    results[0].succeeded;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
        }
        return false;
    }

    bool producesVisibleCookedThumbnails() {
        TemporaryDirectory temporary;
        copyFixture(temporary.path);
        const auto cache =
            std::make_shared<
                LocalDerivedDataCache>(
                    temporary.path / "ddc");
        AssetModelPreparationService models(
            temporary.path, cache, target());
        AssetThumbnailService service(
            temporary.path, cache, target());
        const AssetCatalogRecord root =
            rootRecord();
        CHECK(prepareModel(models, root));
        const AssetCatalogRecord material{
            .guid = *AssetGuid::parse(
                "019f9bce-85b8-7101-8304-05060708090a"),
            .parentGuid = root.guid,
            .assetType = "iridium.material",
            .assetRoot = "project",
            .sourcePath = root.sourcePath,
            .metadataPath = root.metadataPath,
            .status = AssetCatalogStatus::Ready,
        };
        const AssetCatalogRecord texture{
            .guid = *AssetGuid::parse(
                "019f9bce-85b8-7120-890a-0b0c0d0e0f10"),
            .parentGuid = root.guid,
            .assetType = "iridium.texture",
            .assetRoot = "project",
            .sourcePath = root.sourcePath,
            .metadataPath = root.metadataPath,
            .status = AssetCatalogStatus::Ready,
        };
        const std::array visible{
            root, material, texture,
        };
        service.setDemand(visible);
        const auto results =
            waitForResults(service);
        CHECK(results.size() == 1);
        CHECK(results[0].diagnostic.empty());
        CHECK(results[0].thumbnails.size() == 3);
        for (const AssetThumbnailPixels& thumbnail :
            results[0].thumbnails) {
            CHECK(thumbnail.valid());
            CHECK(service.info(thumbnail.assetGuid)
                .status ==
                AssetThumbnailStatus::Prepared);
            service.markPublished(
                thumbnail.assetGuid);
            CHECK(service.info(thumbnail.assetGuid)
                .status ==
                AssetThumbnailStatus::Ready);
        }
        const AssetThumbnailSourceDetail detail =
            service.sourceDetail(texture.guid);
        CHECK(detail.available);
        CHECK(!detail.settingsJson.empty());
        CHECK(!detail.dependencies.empty());
        const auto rootToMaterial =
            std::ranges::find_if(
                detail.associations,
                [&](const AssetThumbnailAssociation&
                        association) {
                    return association.parentGuid ==
                            root.guid &&
                        association.childGuid ==
                            material.guid;
                });
        CHECK(rootToMaterial !=
            detail.associations.end());
        const auto materialToTexture =
            std::ranges::find_if(
                detail.associations,
                [&](const AssetThumbnailAssociation&
                        association) {
                    return association.parentGuid ==
                            material.guid &&
                        association.childGuid ==
                            texture.guid;
                });
        CHECK(materialToTexture !=
            detail.associations.end());
        service.setDemand(visible);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(10));
        CHECK(service.takeResults().empty());
        CHECK(service.stats().queuedRoots == 0);

        service.markEvicted(texture.guid);
        const auto regenerated =
            waitForResults(service);
        CHECK(regenerated.size() == 1);
        CHECK(regenerated[0].thumbnails.size() == 1);
        CHECK(regenerated[0].thumbnails[0]
            .assetGuid == texture.guid);
        return true;
    }

    bool cancelsNoLongerVisibleResults() {
        TemporaryDirectory temporary;
        copyFixture(temporary.path);
        AssetThumbnailService service(
            temporary.path,
            temporary.path / "ddc",
            target());
        const AssetCatalogRecord root =
            rootRecord();
        service.setDemand(
            std::span(&root, 1));
        service.setDemand(
            std::span<const
                AssetCatalogRecord>{});
        for (int attempt = 0;
            attempt < 10'000 &&
                service.stats().active;
            ++attempt) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
        }
        CHECK(!service.isDemanded(root.guid));
        CHECK(service.takeResults().empty());
        CHECK(service.stats().queuedRoots == 0);
        return true;
    }

    bool pinnedDemandSurvivesBrowserPaging() {
        TemporaryDirectory temporary;
        copyFixture(temporary.path);
        AssetThumbnailService service(
            temporary.path,
            temporary.path / "ddc",
            target());
        const AssetCatalogRecord root =
            rootRecord();
        service.setPinnedDemand(
            std::span(&root, 1));
        service.setDemand(
            std::span<const
                AssetCatalogRecord>{});
        CHECK(service.isDemanded(root.guid));
        CHECK(service.stats().demandedAssets == 1);
        service.setPinnedDemand(
            std::span<const
                AssetCatalogRecord>{});
        CHECK(!service.isDemanded(root.guid));
        return true;
    }

    bool selectedDetailUsesBoundedHighResolutionLane() {
        TemporaryDirectory temporary;
        copyFixture(temporary.path);
        const auto cache =
            std::make_shared<
                LocalDerivedDataCache>(
                    temporary.path / "ddc");
        AssetModelPreparationService models(
            temporary.path, cache, target());
        AssetThumbnailService service(
            temporary.path, cache, target());
        const AssetCatalogRecord root =
            rootRecord();
        CHECK(prepareModel(models, root));
        service.setDemand(
            std::span(&root, 1));
        const auto browser =
            waitForResults(service);
        CHECK(browser.size() == 1);
        CHECK(browser[0].thumbnails.size() == 1);
        CHECK(browser[0].thumbnails[0].width ==
            kAssetThumbnailExtent);
        CHECK(browser[0].thumbnails[0].purpose ==
            AssetThumbnailPurpose::Browser);

        service.setDetailDemand(
            std::span(&root, 1),
            root.guid);
        CHECK(service.isDetailDemanded(
            root.guid));
        const auto detail =
            waitForResults(service);
        CHECK(detail.size() == 1);
        CHECK(detail[0].thumbnails.size() == 1);
        CHECK(detail[0].thumbnails[0].width ==
            kAssetDetailThumbnailExtent);
        CHECK(detail[0].thumbnails[0].height ==
            kAssetDetailThumbnailExtent);
        CHECK(detail[0].thumbnails[0].purpose ==
            AssetThumbnailPurpose::Detail);

        service.setDetailDemand(
            std::span<const
                AssetCatalogRecord>{},
            std::nullopt);
        CHECK(!service.isDetailDemanded(
            root.guid));
        return true;
    }

    bool producesStandaloneTextureThumbnail() {
        TemporaryDirectory temporary;
        const std::filesystem::path source =
            temporary.path / "texture.png";
        std::filesystem::copy_file(
            std::filesystem::path(
                PROJECT_ROOT_DIR) /
                "assets" / "models" /
                "alfa_romeo" / "textures" /
                "ID04_plastic_textured_001_rtint_colors_001_diff_6_1_baseColor.png",
            source);
        const AssetGuid textureGuid =
            createAssetGuidV7();
        std::string writeError;
        CHECK(writeAssetMetadataAtomic(
            assetMetadataSidecarPath(source),
            AssetMetadata{
                .assetGuid = textureGuid,
                .assetType =
                    "iridium.texture",
                .importerId =
                    "iridium.texture.directxtex",
                .importerVersion =
                    kDirectXTexCodecVersion,
                .settingsSchemaVersion = 1,
            },
            writeError));
        const AssetCatalogRecord record{
            .guid = textureGuid,
            .assetType = "iridium.texture",
            .assetRoot = "project",
            .sourcePath = "texture.png",
            .metadataPath =
                "texture.png.iridium.meta",
            .status =
                AssetCatalogStatus::Ready,
        };
        AssetThumbnailService service(
            temporary.path,
            temporary.path / "ddc",
            target());
        service.setDemand(
            std::span(&record, 1));
        const auto results =
            waitForResults(service);
        CHECK(results.size() == 1);
        CHECK(results[0].diagnostic.empty());
        CHECK(results[0].thumbnails.size() == 1);
        CHECK(results[0].thumbnails[0].valid());
        CHECK(results[0].thumbnails[0]
            .assetGuid == textureGuid);
        return true;
    }

    bool enforcesUploadBudgetAndCancellation() {
        AssetThumbnailUploadQueue queue;
        const AssetGuid first =
            createAssetGuidV7();
        const AssetGuid second =
            createAssetGuidV7();
        const auto thumbnail =
            [](AssetGuid guid) {
                return AssetThumbnailPixels{
                    .assetGuid = guid,
                    .width = 4,
                    .height = 4,
                    .rgba8 =
                        std::vector<std::byte>(
                            4 * 4 * 4,
                            std::byte{ 0x7f }),
                };
            };
        queue.enqueue(thumbnail(first));
        queue.enqueue(thumbnail(second));
        std::vector<AssetGuid> published;
        AssetThumbnailUploadDrain drain =
            queue.drain(
                64,
                [](AssetGuid) { return true; },
                [&published](
                    const AssetThumbnailPixels&
                        pixels) {
                    published.push_back(
                        pixels.assetGuid);
                });
        CHECK(drain.uploaded == 1);
        CHECK(drain.uploadedBytes == 64);
        CHECK(drain.deferredByBudget == 1);
        CHECK(queue.size() == 1);
        CHECK(published ==
            std::vector<AssetGuid>{ first });

        queue.enqueue(thumbnail(first));
        drain = queue.drain(
            64,
            [first](AssetGuid guid) {
                return guid == first;
            },
            [&published](
                const AssetThumbnailPixels&
                    pixels) {
                published.push_back(
                    pixels.assetGuid);
            });
        CHECK(drain.cancelled == 1);
        CHECK(drain.uploaded == 1);
        CHECK(queue.size() == 0);
        CHECK(published.back() == first);

        queue.enqueue(thumbnail(second));
        drain = queue.drain(
            63,
            [](AssetGuid) { return true; },
            [](const AssetThumbnailPixels&) {});
        CHECK(drain.uploaded == 0);
        CHECK(drain.deferredByBudget == 1);
        CHECK(queue.size() == 1);
        return true;
    }

    bool sharedDdcCoalescesAssignmentAndThumbnail() {
        TemporaryDirectory temporary;
        copyFixture(temporary.path);
        const auto cache =
            std::make_shared<
                LocalDerivedDataCache>(
                    temporary.path / "ddc");
        AssetModelPreparationService models(
            temporary.path, cache, target());
        AssetThumbnailService thumbnails(
            temporary.path, cache, target());
        const AssetCatalogRecord root =
            rootRecord();
        thumbnails.setDemand(
            std::span(&root, 1));
        CHECK(waitForStatus(
            thumbnails, root.guid,
            AssetThumbnailStatus::Unavailable));
        CHECK(thumbnails.takeResults().empty());
        const AssetThumbnailSourceDetail
            deferredDetail =
                thumbnails.sourceDetail(
                    root.guid);
        CHECK(deferredDetail.available);
        CHECK(!deferredDetail
            .settingsJson.empty());
        CHECK(deferredDetail.diagnostic.find(
            "first editor cook") !=
            std::string::npos);
        CHECK(models.request(root));

        std::vector<PreparedCatalogModel>
            modelResults;
        for (int attempt = 0;
            attempt < 10'000 &&
                modelResults.empty();
            ++attempt) {
            modelResults =
                models.takeResults();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
        }
        CHECK(modelResults.size() == 1);
        CHECK(modelResults[0].succeeded);
        thumbnails.invalidate(root.guid);
        const std::vector<
            PreparedAssetThumbnailBatch>
            thumbnailResults =
                waitForResults(thumbnails);
        CHECK(thumbnailResults.size() == 1);
        CHECK(thumbnailResults[0]
            .diagnostic.empty());
        size_t artifacts = 0;
        for (const auto& entry :
            std::filesystem::recursive_directory_iterator(
                cache->root())) {
            if (entry.is_regular_file() &&
                entry.path().extension() ==
                    ".irartifact") {
                ++artifacts;
            }
        }
        CHECK(artifacts == 1);
        return true;
    }

} // namespace

int main() {
    struct Test {
        const char* name;
        bool (*function)();
    };
    const std::vector<Test> tests{
        { "visible cooked thumbnails",
            producesVisibleCookedThumbnails },
        { "visibility cancellation",
            cancelsNoLongerVisibleResults },
        { "pinned demand survives browser paging",
            pinnedDemandSurvivesBrowserPaging },
        { "selected detail high-resolution lane",
            selectedDetailUsesBoundedHighResolutionLane },
        { "standalone texture thumbnail",
            producesStandaloneTextureThumbnail },
        { "upload budget and cancellation",
            enforcesUploadBudgetAndCancellation },
        { "shared DDC model-to-thumbnail handoff",
            sharedDdcCoalescesAssignmentAndThumbnail },
    };
    size_t failures = 0;
    for (const Test& test : tests) {
        try {
            const bool passed =
                test.function();
            std::cout << (passed
                ? "[PASS] " : "[FAIL] ")
                << test.name << '\n';
            if (!passed) ++failures;
        }
        catch (const std::exception&
            exception) {
            std::cerr << "[FAIL] "
                << test.name << ": "
                << exception.what() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
