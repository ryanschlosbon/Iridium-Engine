#include "assets/AssetBrowserModel.h"
#include "assets/SqliteAssetCatalog.h"

#include <array>
#include <cstring>
#include <iostream>
#include <memory>
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

    AssetGuid guid(uint64_t timestamp, uint8_t seed) {
        std::array<uint8_t, 10> random{};
        for (size_t index = 0; index < random.size(); ++index) {
            random[index] = static_cast<uint8_t>(seed + index);
        }
        return AssetGuid::fromUuidV7Fields(timestamp, random);
    }

    std::vector<AssetCatalogRecord> records() {
        const AssetGuid model = guid(100, 1);
        return {
            {
                .guid = model,
                .assetType = "iridium.model",
                .assetRoot = "project",
                .sourcePath = "vehicles/car.gltf",
                .metadataPath = "vehicles/car.gltf.iridium.meta",
                .displayName = "Car",
                .importerId = "iridium.gltf-model",
                .importerVersion = 3,
                .tags = { "vehicle", "hero" },
            },
            {
                .guid = guid(101, 2),
                .parentGuid = model,
                .assetType = "iridium.material",
                .assetRoot = "project",
                .sourcePath = "vehicles/car.gltf",
                .metadataPath = "vehicles/car.gltf.iridium.meta",
                .sourceKey = "materials/0",
                .displayName = "Car : materials/0",
                .importerId = "iridium.gltf-model",
                .importerVersion = 3,
            },
            {
                .guid = guid(102, 3),
                .assetType = "iridium.texture",
                .assetRoot = "project",
                .sourcePath = "textures/albedo.png",
                .metadataPath = "textures/albedo.png.iridium.meta",
                .displayName = "Albedo",
                .importerId = "iridium.texture",
                .importerVersion = 2,
                .status = AssetCatalogStatus::MissingSource,
                .diagnosticSummary = "Source is missing.",
            },
        };
    }

    class CountingCatalog final :
        public AssetCatalog {
    public:
        CountingCatalog()
            : inner_(
                createSqliteAssetCatalog(
                    ":memory:")) {}

        void rebuild(
            std::span<const
                AssetCatalogRecord> records,
            std::span<const std::string>
                directories = {}) override {
            inner_->rebuild(
                records, directories);
        }
        std::vector<AssetCatalogRecord>
            recordsForGuid(
                const AssetGuid& value)
                const override {
            return inner_->recordsForGuid(
                value);
        }
        std::vector<AssetCatalogRecord>
            recordsForSourceRoot(
                const AssetGuid& value)
                const override {
            return inner_->
                recordsForSourceRoot(
                    value);
        }
        AssetCatalogQueryPage query(
            const AssetCatalogQuery& query)
                const override {
            ++queryCount;
            return inner_->query(query);
        }
        uint64_t recordCount()
                const override {
            return inner_->recordCount();
        }
        std::vector<std::string>
            sourceDirectories()
                const override {
            return inner_->
                sourceDirectories();
        }

        mutable uint64_t queryCount = 0;

    private:
        std::unique_ptr<AssetCatalog>
            inner_;
    };

    bool testRefreshCachesUntilInvalidated() {
        CountingCatalog catalog;
        const auto source = records();
        catalog.rebuild(source);
        AssetBrowserModel model(&catalog);

        (void)model.refresh();
        CHECK(catalog.queryCount == 1);
        (void)model.refresh();
        CHECK(catalog.queryCount == 1);
        model.setLayout(
            AssetBrowserLayout::List);
        (void)model.refresh();
        CHECK(catalog.queryCount == 1);

        const AssetBrowserDecoration
            decoration{
                .thumbnailState =
                    AssetThumbnailState::Ready,
            };
        model.setDecoration(
            source[0].guid,
            decoration);
        (void)model.refresh();
        CHECK(catalog.queryCount == 2);
        model.setDecoration(
            source[0].guid,
            decoration);
        (void)model.refresh();
        CHECK(catalog.queryCount == 2);

        model.invalidate();
        (void)model.refresh();
        CHECK(catalog.queryCount == 3);
        return true;
    }

    bool testQuerySelectionAndDecoration() {
        const auto catalog = createSqliteAssetCatalog(":memory:");
        const auto source = records();
        catalog->rebuild(source);

        AssetBrowserModel model(catalog.get());
        model.setPageSize(64);
        model.setText("vehicle");
        model.setAssetType("iridium.model");
        model.setDecoration(source[0].guid, {
            .runtimeState = RuntimeAssetState::Ready,
            .thumbnailState = AssetThumbnailState::Ready,
            .diagnostic = "Published revision 2.",
        });
        const AssetBrowserPage page = model.refresh();
        CHECK(page.totalMatches == 1);
        CHECK(page.items.size() == 1);
        CHECK(page.items[0].record.guid == source[0].guid);
        CHECK(page.items[0].assignable());
        CHECK(page.items[0].decoration.runtimeState == RuntimeAssetState::Ready);
        CHECK(page.items[0].diagnosticSummary() == "Published revision 2.");
        CHECK(model.select(source[0].guid));
        CHECK(model.selectedItem() != nullptr);

        model.setText("");
        model.setAssetType(std::nullopt);
        model.setStatus(AssetCatalogStatus::MissingSource);
        CHECK(model.offset() == 0);
        const AssetBrowserPage missing = model.refresh();
        CHECK(missing.items.size() == 1);
        CHECK(!missing.items[0].assignable());
        CHECK(missing.items[0].diagnosticSummary() == "Source is missing.");
        CHECK(!model.selectedGuid());
        return true;
    }

    bool testPagingAndLayout() {
        const auto catalog = createSqliteAssetCatalog(":memory:");
        auto source = records();
        for (uint8_t index = 0; index < 10; ++index) {
            source.push_back({
                .guid = guid(200 + index, 20 + index),
                .assetType = "iridium.model",
                .assetRoot = "project",
                .sourcePath = "model-" + std::to_string(index) + ".gltf",
                .metadataPath = "model-" + std::to_string(index) +
                    ".gltf.iridium.meta",
                .displayName = "Model " + std::to_string(index),
                .importerId = "iridium.gltf-model",
                .importerVersion = 3,
            });
        }
        catalog->rebuild(source);

        AssetBrowserModel model(catalog.get());
        model.setAssetType("iridium.model");
        model.setPageSize(3);
        model.setOffset(3);
        model.setLayout(AssetBrowserLayout::List);
        const AssetBrowserPage page = model.refresh();
        CHECK(page.totalMatches == 11);
        CHECK(page.items.size() == 3);
        CHECK(page.offset == 3);
        CHECK(page.limit == 3);
        CHECK(model.layout() == AssetBrowserLayout::List);
        model.setPageSize(0);
        CHECK(model.pageSize() == 1);
        return true;
    }

    bool testTypedGuidPayload() {
        const AssetDragPayload expected{
            .guid = guid(300, 7),
            .kind = AssetDragKind::Model,
        };
        AssetDragPayloadBytes bytes = encodeAssetDragPayload(expected);
        const auto span = std::as_bytes(std::span(&bytes, 1));
        CHECK(decodeAssetDragPayload(
            kAssetBrowserDragPayloadType, span) == expected);
        CHECK(decodeAssetDragPayload(
            kAssetBrowserDragPayloadType, span,
            AssetDragKind::Model) == expected);
        CHECK(!decodeAssetDragPayload(
            kAssetBrowserDragPayloadType, span,
            AssetDragKind::Texture));

        const AssetDragPayload environment{
            .guid = guid(301, 7),
            .kind = AssetDragKind::Environment,
        };
        const AssetDragPayloadBytes environmentBytes =
            encodeAssetDragPayload(environment);
        CHECK(decodeAssetDragPayload(
            kAssetBrowserDragPayloadType,
            std::as_bytes(std::span(&environmentBytes, 1)),
            AssetDragKind::Environment) == environment);
        CHECK(assetDragKindForType("iridium.environment") ==
            AssetDragKind::Environment);
        const AssetDragPayload bakedLighting{
            .guid = guid(302, 7),
            .kind = AssetDragKind::BakedLighting,
        };
        const AssetDragPayloadBytes bakedLightingBytes =
            encodeAssetDragPayload(bakedLighting);
        CHECK(decodeAssetDragPayload(
            kAssetBrowserDragPayloadType,
            std::as_bytes(std::span(&bakedLightingBytes, 1)),
            AssetDragKind::BakedLighting) == bakedLighting);
        CHECK(assetDragKindForType("iridium.baked-lighting") ==
            AssetDragKind::BakedLighting);
        CHECK(!decodeAssetDragPayload("WRONG", span));
        CHECK(!decodeAssetDragPayload(
            kAssetBrowserDragPayloadType, span.first(span.size() - 1)));

        bytes.schemaVersion = 2;
        CHECK(!decodeAssetDragPayload(
            kAssetBrowserDragPayloadType,
            std::as_bytes(std::span(&bytes, 1))));
        bytes = encodeAssetDragPayload({
            .guid = {},
            .kind = AssetDragKind::Model,
        });
        CHECK(!decodeAssetDragPayload(
            kAssetBrowserDragPayloadType,
            std::as_bytes(std::span(&bytes, 1))));
        return true;
    }

    bool testFoldersAndDirectoryFiltering() {
        const auto catalog =
            createSqliteAssetCatalog(":memory:");
        auto source = records();
        source.push_back({
            .guid = guid(103, 4),
            .assetType = "iridium.model",
            .assetRoot = "project",
            .sourcePath =
                "vehicles/sports/coupe.gltf",
            .metadataPath =
                "vehicles/sports/coupe.gltf.iridium.meta",
            .displayName = "Coupe",
            .importerId = "iridium.gltf-model",
            .importerVersion = 3,
        });
        catalog->rebuild(source);
        const std::vector<std::string>
            directories =
                catalog->sourceDirectories();
        CHECK(directories ==
            std::vector<std::string>({
                "textures", "vehicles",
                "vehicles/sports",
            }));
        const auto folders =
            buildAssetBrowserFolders(
                directories);
        CHECK(folders.size() == 2);
        CHECK(folders[0].name == "textures");
        CHECK(folders[1].name == "vehicles");
        CHECK(folders[1].children.size() ==
            1);
        CHECK(folders[1].children[0].name ==
            "sports");

        AssetBrowserModel model(
            catalog.get());
        model.setDirectory("vehicles");
        const AssetBrowserPage vehicles =
            model.refresh();
        CHECK(vehicles.totalMatches == 1);
        CHECK(std::ranges::all_of(
            vehicles.items,
            [](const AssetBrowserItem& item) {
                return item.record.sourcePath ==
                    "vehicles/car.gltf";
            }));
        model.setDirectory(
            "vehicles/sports");
        const AssetBrowserPage sports =
            model.refresh();
        CHECK(sports.totalMatches == 1);
        CHECK(sports.items[0].record.sourcePath ==
            "vehicles/sports/coupe.gltf");
        model.setDirectory("textures");
        const AssetBrowserPage textures =
            model.refresh();
        CHECK(textures.totalMatches == 1);
        CHECK(textures.items[0].record.assetType ==
            "iridium.texture");
        model.setDirectory(std::nullopt);
        CHECK(model.refresh().totalMatches == 3);
        const auto sourceRecords =
            catalog->recordsForSourceRoot(
                source[0].guid);
        CHECK(sourceRecords.size() == 2);
        CHECK(sourceRecords[1].parentGuid ==
            source[0].guid);
        const bool childIsAssignable = AssetBrowserItem{
            .record = sourceRecords[1],
        }.assignable();
        CHECK(childIsAssignable);
        return true;
    }

} // namespace

int main() {
    struct Test {
        const char* name;
        bool (*function)();
    };
    const std::vector<Test> tests{
        { "query, selection, and decoration",
            testQuerySelectionAndDecoration },
        { "refresh cache invalidation",
            testRefreshCachesUntilInvalidated },
        { "paging and layout", testPagingAndLayout },
        { "typed GUID payload", testTypedGuidPayload },
        { "folders and directory filtering",
            testFoldersAndDirectoryFiltering },
    };
    size_t failures = 0;
    for (const Test& test : tests) {
        try {
            const bool passed = test.function();
            std::cout << (passed ? "[PASS] " : "[FAIL] ")
                << test.name << '\n';
            if (!passed) ++failures;
        } catch (const std::exception& exception) {
            std::cerr << "[FAIL] " << test.name << ": "
                << exception.what() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
