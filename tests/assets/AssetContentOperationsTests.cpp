#include "assets/AssetContentOperations.h"
#include "assets/AssetDiscovery.h"
#include "assets/AssetMetadata.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition \
                    " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    struct TemporaryRoot {
        std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("iridium-content-operations-" +
                std::to_string(std::chrono::steady_clock::now()
                    .time_since_epoch().count()));
        TemporaryRoot() {
            std::filesystem::create_directories(path);
        }
        ~TemporaryRoot() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };

    Iridium::AssetGuid guid() {
        constexpr std::array<uint8_t, 10> random{
            2, 4, 6, 8, 10, 12, 14, 16, 18, 20,
        };
        return Iridium::AssetGuid::fromUuidV7Fields(
            987654, random);
    }

    void write(const std::filesystem::path& path,
        std::string_view text) {
        std::filesystem::create_directories(
            path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output << text;
    }

    Iridium::AssetCatalogRecord makeAsset(
        const std::filesystem::path& relative) {
        return {
            .guid = guid(),
            .assetType = "iridium.model",
            .assetRoot = "project",
            .sourcePath = relative.generic_string(),
            .metadataPath =
                (relative.generic_string() +
                    ".iridium.meta"),
            .displayName =
                relative.filename().string(),
            .importerId = "iridium.gltf-model",
            .importerVersion = 3,
        };
    }

    void writeSidecar(
        const std::filesystem::path& source) {
        Iridium::AssetMetadata metadata{
            .assetGuid = guid(),
            .assetType = "iridium.model",
            .importerId = "iridium.gltf-model",
            .importerVersion = 3,
            .settings = {
                { "bake_node_transforms", true },
                { "generate_missing_tangents", true },
                { "import_scale", 1.0 },
                { "preserve_rt_geometry", true },
            },
        };
        std::string error;
        if (!Iridium::writeAssetMetadataAtomic(
                Iridium::assetMetadataSidecarPath(source),
                metadata, error)) {
            throw std::runtime_error(error);
        }
    }

    bool testPhysicalFoldersAndDiscovery() {
        TemporaryRoot temporary;
        const std::array roots{
            Iridium::AssetRoot{
                "project", temporary.path },
        };
        const Iridium::AssetContentOperations
            operations(roots);
        const auto created = operations.createFolder(
            "project", {}, "Models");
        if (!created.succeeded()) {
            std::cerr << created.diagnostic << '\n';
        }
        CHECK(created.succeeded());
        CHECK(std::filesystem::is_regular_file(
            created.path / ".iridium-folder"));
        auto discovery =
            Iridium::discoverAssetRoots(roots);
        CHECK(std::ranges::find(
            discovery.sourceDirectories,
            "Models") !=
            discovery.sourceDirectories.end());

        const auto renamed = operations.renameFolder(
            "project", "Models", "Vehicles");
        CHECK(renamed.succeeded());
        CHECK(std::filesystem::is_directory(
            temporary.path / "Vehicles"));
        discovery = Iridium::discoverAssetRoots(roots);
        CHECK(std::ranges::find(
            discovery.sourceDirectories,
            "Vehicles") !=
            discovery.sourceDirectories.end());

        const auto deleted = operations.deleteFolder(
            "project", "Vehicles");
        CHECK(deleted.succeeded());
        CHECK(!std::filesystem::exists(
            temporary.path / "Vehicles"));
        return true;
    }

    bool testAssetMoveRewritesGltfAndPreservesGuid() {
        TemporaryRoot temporary;
        const std::array roots{
            Iridium::AssetRoot{
                "project", temporary.path },
        };
        const Iridium::AssetContentOperations
            operations(roots);
        const std::filesystem::path source =
            temporary.path / "Incoming" /
            "vehicle.gltf";
        write(source, R"({
  "asset": { "version": "2.0" },
  "buffers": [{ "uri": "vehicle.bin", "byteLength": 4 }],
  "images": [{ "uri": "textures/albedo.png" }]
})");
        write(source.parent_path() / "vehicle.bin",
            "data");
        write(source.parent_path() / "textures" /
            "albedo.png", "image");
        writeSidecar(source);
        std::filesystem::create_directories(
            temporary.path / "Organized");

        const auto moved = operations.moveAsset(
            makeAsset("Incoming/vehicle.gltf"),
            "Organized");
        if (!moved.succeeded()) {
            std::cerr << moved.diagnostic << '\n';
        }
        CHECK(moved.succeeded());
        const std::filesystem::path destination =
            temporary.path / "Organized" /
            "vehicle.gltf";
        CHECK(std::filesystem::is_regular_file(
            destination));
        CHECK(std::filesystem::is_regular_file(
            Iridium::assetMetadataSidecarPath(
                destination)));
        const auto metadata = Iridium::readAssetMetadata(
            Iridium::assetMetadataSidecarPath(
                destination));
        CHECK(metadata.metadata);
        CHECK(metadata.metadata->assetGuid == guid());

        std::ifstream input(destination);
        nlohmann::json document;
        input >> document;
        input.close();
        CHECK(document["buffers"][0]["uri"] ==
            "../Incoming/vehicle.bin");
        CHECK(document["images"][0]["uri"] ==
            "../Incoming/textures/albedo.png");

        const auto renamed = operations.renameAsset(
            makeAsset("Organized/vehicle.gltf"),
            "sports_car");
        if (!renamed.succeeded()) {
            std::cerr << renamed.diagnostic << '\n';
        }
        CHECK(renamed.succeeded());
        CHECK(std::filesystem::is_regular_file(
            temporary.path / "Organized" /
            "sports_car.gltf"));
        CHECK(std::filesystem::is_regular_file(
            temporary.path / "Organized" /
            "sports_car.gltf.iridium.meta"));
        return true;
    }

    bool testSubassetAndEscapeRejection() {
        TemporaryRoot temporary;
        const std::array roots{
            Iridium::AssetRoot{
                "project", temporary.path },
        };
        const Iridium::AssetContentOperations
            operations(roots);
        Iridium::AssetCatalogRecord child =
            makeAsset("model.gltf");
        child.parentGuid = guid();
        CHECK(!operations.moveAsset(
            child, {}).succeeded());
        CHECK(!operations.createFolder(
            "project", "..", "Escape")
            .succeeded());
        CHECK(!operations.deleteFolder(
            "project", {}).succeeded());
        return true;
    }

    bool testExternalImportDeletionPreservesOriginal() {
        TemporaryRoot temporary;
        const std::filesystem::path project =
            temporary.path / "Project";
        const std::filesystem::path external =
            temporary.path / "External";
        std::filesystem::create_directories(
            project);
        const std::filesystem::path original =
            external / "sample.gltf";
        write(original, R"({
  "asset": { "version": "2.0" }
})");
        const std::array roots{
            Iridium::AssetRoot{
                "project", project },
        };
        const Iridium::AssetContentOperations
            operations(roots);
        const auto imported =
            operations.importAsset(
                "project", {},
                original);
        CHECK(imported.succeeded());
        CHECK(imported.path ==
            project / "sample" /
                "sample.gltf");
        CHECK(std::filesystem::is_regular_file(
            imported.path));
        writeSidecar(imported.path);

        const auto deleted =
            operations.deleteAsset(
                makeAsset(
                    "sample/sample.gltf"));
        CHECK(deleted.succeeded());
        CHECK(!std::filesystem::exists(
            imported.path));
        CHECK(!std::filesystem::exists(
            Iridium::assetMetadataSidecarPath(
                imported.path)));
        CHECK(std::filesystem::is_regular_file(
            original));
        return true;
    }

} // namespace

int main() {
    struct Test {
        const char* name;
        bool (*function)();
    };
    const std::vector<Test> tests{
        { "physical folders and discovery",
            testPhysicalFoldersAndDiscovery },
        { "asset move preserves GUID and glTF dependencies",
            testAssetMoveRewritesGltfAndPreservesGuid },
        { "subasset and escape rejection",
            testSubassetAndEscapeRejection },
        { "external import deletion preserves original",
            testExternalImportDeletionPreservesOriginal },
    };
    size_t failures = 0;
    for (const Test& test : tests) {
        try {
            const bool passed = test.function();
            std::cout << (passed ? "[PASS] " : "[FAIL] ")
                << test.name << '\n';
            if (!passed) ++failures;
        }
        catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": "
                << exception.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
