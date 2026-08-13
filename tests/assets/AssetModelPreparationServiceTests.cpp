#include "assets/model/AssetModelPreparationService.h"
#include "assets/thumbnail/AssetThumbnail.h"
#include "utils/Sha256.h"

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
            ("iridium-model-preparation-" +
                createAssetGuidV7().toString());

        TemporaryDirectory() {
            std::filesystem::create_directories(path);
        }

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };

    std::vector<PreparedCatalogModel> waitForResults(
        AssetModelPreparationService& service) {
        for (int attempt = 0; attempt < 10'000; ++attempt) {
            std::vector<PreparedCatalogModel> results =
                service.takeResults();
            if (!results.empty()) return results;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
        }
        return {};
    }

    CookTarget editorTarget() {
        return {
            .platform = "windows-x64",
            .profile = "editor",
            .qualityPolicy = "reference",
            .artifactContainerVersion =
                kCookedArtifactContainerVersion,
            .materialSchemaVersion = 2,
        };
    }

    bool preparesCatalogModelAndCoalescesRequests() {
        TemporaryDirectory temporary;
        const std::filesystem::path fixtureRoot =
            std::filesystem::path(PROJECT_ROOT_DIR) /
            "tests" / "assets";
        const std::filesystem::path source =
            temporary.path / "fixture.gltf";
        const std::filesystem::path metadata =
            temporary.path /
                "fixture.gltf.iridium.meta";
        std::filesystem::copy_file(
            fixtureRoot /
                "gltf_model_cooker_fixture.gltf",
            source);
        std::filesystem::copy_file(
            fixtureRoot /
                "gltf_model_cooker_fixture.gltf.iridium.meta",
            metadata);

        const auto guid = AssetGuid::parse(
            "019f9bce-85b8-7100-8203-040506070809");
        CHECK(guid.has_value());
        const AssetCatalogRecord record{
            .guid = *guid,
            .assetType = "iridium.model",
            .assetRoot = "project",
            .sourcePath = "fixture.gltf",
            .metadataPath =
                "fixture.gltf.iridium.meta",
            .status = AssetCatalogStatus::Ready,
        };
        AssetModelPreparationService service(
            temporary.path,
            temporary.path / "ddc",
            editorTarget());
        CHECK(service.request(record));
        CHECK(!service.request(record));
        CHECK(service.pending(record.guid));

        std::vector<PreparedCatalogModel> results =
            waitForResults(service);
        CHECK(results.size() == 1);
        CHECK(results[0].succeeded);
        CHECK(results[0].artifact);
        CHECK(results[0].product);
        CHECK(results[0].artifact->assetGuid ==
            record.guid);
        CHECK(results[0].product->manifest
            .primitives.size() == 4);
        CHECK(!results[0].product->
            textureViews.empty());
        CHECK(results[0].product->
            textureViews[0].manifest.quality ==
            TextureCompressionQuality::Iteration);
        CHECK(results[0].product->
            textureViews[0].manifest.storageFormat ==
            TextureFormat::BC7_sRGB);
        CHECK(results[0].cpuResidentBytes > 0);
        CHECK(results[0].gpuResidentBytes > 0);
        CHECK(!service.pending(record.guid));

        const AssetThumbnailPixels modelThumbnail =
            makeAssetThumbnail(
                *results[0].product, record);
        CHECK(modelThumbnail.valid());
        const AssetCatalogRecord materialRecord{
            .guid = *AssetGuid::parse(
                "019f9bce-85b8-7101-8304-05060708090a"),
            .parentGuid = record.guid,
            .assetType = "iridium.material",
            .status = AssetCatalogStatus::Ready,
        };
        const AssetThumbnailPixels materialThumbnail =
            makeAssetThumbnail(
                *results[0].product,
                materialRecord);
        CHECK(materialThumbnail.valid());
        const AssetCatalogRecord textureRecord{
            .guid = *AssetGuid::parse(
                "019f9bce-85b8-7120-890a-0b0c0d0e0f10"),
            .parentGuid = record.guid,
            .assetType = "iridium.texture",
            .status = AssetCatalogStatus::Ready,
        };
        const AssetThumbnailPixels textureThumbnail =
            makeAssetThumbnail(
                *results[0].product,
                textureRecord);
        CHECK(textureThumbnail.valid());
        CHECK(textureThumbnail.rgba8 ==
            makeAssetThumbnail(
                *results[0].product,
                textureRecord).rgba8);
        CHECK(sha256(modelThumbnail.rgba8) ==
            "d4d04d586f264b245f763ff564779d109679392efabbf2abb0f2515b347f096a");
        CHECK(sha256(materialThumbnail.rgba8) ==
            "a52bbff3b2816ae1f6d5c22a1e9fa4d9733312ad8397907f18544b57f809b56f");
        CHECK(sha256(textureThumbnail.rgba8) ==
            "80e4abf28564eef01917a5427f5ecd1b71905e2fc2726f4b109a9675c208f7c7");

        const std::string firstCookKey =
            results[0].artifact->cookKey;
        CHECK(service.request(record));
        results = waitForResults(service);
        CHECK(results.size() == 1);
        CHECK(results[0].succeeded);
        CHECK(results[0].artifact->cookKey ==
            firstCookKey);
        return true;
    }

    bool rejectsChangedCatalogIdentity() {
        TemporaryDirectory temporary;
        const std::filesystem::path fixtureRoot =
            std::filesystem::path(PROJECT_ROOT_DIR) /
            "tests" / "assets";
        std::filesystem::copy_file(
            fixtureRoot /
                "gltf_model_cooker_fixture.gltf",
            temporary.path / "fixture.gltf");
        std::filesystem::copy_file(
            fixtureRoot /
                "gltf_model_cooker_fixture.gltf.iridium.meta",
            temporary.path /
                "fixture.gltf.iridium.meta");
        const AssetGuid differentGuid =
            createAssetGuidV7();
        AssetModelPreparationService service(
            temporary.path,
            temporary.path / "ddc",
            editorTarget());
        CHECK(service.request({
            .guid = differentGuid,
            .assetType = "iridium.model",
            .assetRoot = "project",
            .sourcePath = "fixture.gltf",
            .metadataPath =
                "fixture.gltf.iridium.meta",
            .status = AssetCatalogStatus::Ready,
        }));
        const std::vector<PreparedCatalogModel> results =
            waitForResults(service);
        CHECK(results.size() == 1);
        CHECK(!results[0].succeeded);
        CHECK(results[0].assetGuid == differentGuid);
        CHECK(results[0].diagnostic.find(
            "changed identity") != std::string::npos);
        return true;
    }

} // namespace

int main() {
    struct Test {
        const char* name;
        bool (*function)();
    };
    const std::vector<Test> tests{
        { "prepare and coalesce",
            preparesCatalogModelAndCoalescesRequests },
        { "identity mismatch rejection",
            rejectsChangedCatalogIdentity },
    };
    size_t failures = 0;
    for (const Test& test : tests) {
        try {
            const bool passed =
                test.function();
            std::cout << (passed ? "[PASS] " : "[FAIL] ")
                << test.name << '\n';
            if (!passed) ++failures;
        }
        catch (const std::exception& exception) {
            std::cerr << "[FAIL] " << test.name << ": "
                << exception.what() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
