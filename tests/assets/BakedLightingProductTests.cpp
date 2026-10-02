#include "assets/lighting/BakedLightingProduct.h"
#include "assets/cooker/CookKey.h"

#include <algorithm>
#include <array>
#include <iostream>

namespace {

#define CHECK(value) do { if (!(value)) { std::cerr << "check failed at " \
    << __LINE__ << ": " #value "\n"; return false; } } while (false)

    Iridium::AssetGuid guid(std::string_view suffix) {
        return *Iridium::AssetGuid::parse(
            std::string("01890f4c-0000-7000-8000-") + std::string(suffix));
    }

    Iridium::BakedSceneEntityId entity(std::string_view suffix) {
        const Iridium::AssetGuid value = guid(suffix);
        return value.bytes();
    }

    std::vector<std::byte> bytes(size_t count, uint8_t seed) {
        std::vector<std::byte> result(count);
        for (size_t index = 0; index < count; ++index)
            result[index] = static_cast<std::byte>(seed + index);
        return result;
    }

    Iridium::BakedLightingProductData fixture(bool reverseBindings = false) {
        Iridium::BakedLightingProductData product;
        product.manifest.sceneAssetGuid = guid("000000000100");
        product.manifest.bakerId = "iridium.reference-baker";
        product.manifest.bakerVersion = 3;
        product.manifest.qualityProfile = "cinematic";
        product.manifest.inputs = {
            .sceneCanonicalHash = std::string(64, 'a'),
            .geometryHash = std::string(64, 'b'),
            .materialHash = std::string(64, 'c'),
            .lightingHash = std::string(64, 'd'),
            .bakeSettingsHash = std::string(64, 'e'),
            .toolHash = std::string(64, 'f'),
        };
        product.lightmapAtlases.push_back({
            .width = 4096,
            .height = 4096,
            .layers = 3,
            .mipLevels = 13,
            .encoding = Iridium::BakedLightmapEncoding::DirectionalBasisBc6h,
            .payloadOffset = 0,
            .payloadSize = 16,
        });
        product.lightmapBindings = {
            { .entity = entity("000000000102"),
              .meshPrimitiveGuid = guid("000000000202"),
              .atlasIndex = 0, .uvSet = 1,
              .uvScaleBias = { 0.5f, 0.5f, 0.5f, 0.0f } },
            { .entity = entity("000000000101"),
              .meshPrimitiveGuid = guid("000000000201"),
              .atlasIndex = 0, .uvSet = 1,
              .uvScaleBias = { 0.5f, 0.5f, 0.0f, 0.0f } },
        };
        if (!reverseBindings)
            std::ranges::reverse(product.lightmapBindings);
        product.lightmapPayload = bytes(16, 1);
        product.probeVolumes.push_back({
            .owner = entity("000000000103"),
            .boundsMin = { -10.0f, 0.0f, -10.0f },
            .boundsMax = { 10.0f, 8.0f, 10.0f },
            .probeCount = { 8, 4, 8 },
            .encoding = Iridium::BakedProbeVolumeEncoding::ShL2Rgb16F,
            .payloadOffset = 0,
            .payloadSize = 24,
        });
        product.probeVolumePayload = bytes(24, 21);
        product.visibilityVolumes.push_back({
            .owner = entity("000000000104"),
            .boundsMin = { -10.0f, 0.0f, -10.0f },
            .boundsMax = { 10.0f, 8.0f, 10.0f },
            .cellCount = { 16, 8, 16 },
            .encoding =
                Iridium::BakedVisibilityEncoding::BentNormalConeRgba16F,
            .payloadOffset = 0,
            .payloadSize = 32,
        });
        product.visibilityPayload = bytes(32, 51);
        return product;
    }

    Iridium::CookedArtifact artifact(Iridium::AssetGuid assetGuid,
        Iridium::CookProduct product) {
        return {
            .assetGuid = assetGuid,
            .artifactType = std::move(product.artifactType),
            .artifactSchemaVersion = product.artifactSchemaVersion,
            .target = { .platform = "windows-x64", .profile = "release",
                .qualityPolicy = "cinematic" },
            .cookKey = std::string(64, '1'),
            .dependencies = {
                { .type = Iridium::AssetDependencyType::Asset,
                  .assetGuid = guid("000000000100"),
                  .artifactHash = std::string(64, '2') },
            },
            .sections = std::move(product.sections),
        };
    }

    bool deterministicTypedRoundTrip() {
        auto firstProduct = Iridium::makeCookedBakedLightingProduct(fixture(false));
        auto secondProduct = Iridium::makeCookedBakedLightingProduct(fixture(true));
        CHECK(!Iridium::hasCookErrors(firstProduct.diagnostics));
        CHECK(!Iridium::hasCookErrors(secondProduct.diagnostics));
        const auto first = artifact(guid("000000000300"), std::move(firstProduct));
        const auto second = artifact(guid("000000000300"), std::move(secondProduct));
        const auto firstBlob = Iridium::serializeCookedArtifact(first);
        const auto secondBlob = Iridium::serializeCookedArtifact(second);
        CHECK(firstBlob.bytes == secondBlob.bytes);
        CHECK(firstBlob.artifactHash == secondBlob.artifactHash);

        const auto container = Iridium::readCookedArtifact(firstBlob.bytes);
        CHECK(container.valid());
        const auto read = Iridium::readCookedBakedLightingProduct(
            *container.artifact);
        CHECK(read.valid());
        CHECK(read.data->lightmapAtlases.size() == 1);
        CHECK(read.data->lightmapBindings.size() == 2);
        CHECK(read.data->probeVolumes.size() == 1);
        CHECK(read.data->visibilityVolumes.size() == 1);
        CHECK(read.data->lightmapBindings[0].entity ==
            entity("000000000101"));
        CHECK(read.data->manifest.sceneAssetGuid == guid("000000000100"));
        return true;
    }

    bool malformedSectionsFailClosed() {
        auto valid = artifact(guid("000000000300"),
            Iridium::makeCookedBakedLightingProduct(fixture()));
        auto unknown = valid;
        unknown.sections.push_back({ 0xdeadbeefu, 1, 16, bytes(4, 1) });
        CHECK(!Iridium::readCookedBakedLightingProduct(unknown).valid());

        auto truncated = valid;
        auto found = std::ranges::find(truncated.sections,
            Iridium::kBakedLightingProbeVolumeSection,
            &Iridium::CookSection::id);
        CHECK(found != truncated.sections.end());
        found->bytes.pop_back();
        CHECK(!Iridium::readCookedBakedLightingProduct(truncated).valid());

        auto missing = valid;
        std::erase_if(missing.sections, [](const Iridium::CookSection& section) {
            return section.id == Iridium::kBakedLightingVisibilitySection;
        });
        CHECK(!Iridium::readCookedBakedLightingProduct(missing).valid());
        return true;
    }

    bool publicationRetainsLastKnownGood() {
        const auto good = artifact(guid("000000000300"),
            Iridium::makeCookedBakedLightingProduct(fixture()));
        Iridium::BakedLightingPublication publication;
        const auto first = publication.publish(good);
        CHECK(first.published);
        CHECK(first.generation == 1);
        CHECK(publication.active() != nullptr);
        CHECK(publication.activeAssetGuid() == guid("000000000300"));

        auto bad = good;
        bad.assetGuid = guid("000000000301");
        bad.artifactSchemaVersion = 99;
        const auto rejected = publication.publish(bad);
        CHECK(!rejected.published);
        CHECK(rejected.retainedLastKnownGood);
        CHECK(publication.activeAssetGuid() == guid("000000000300"));
        CHECK(publication.generation() == 1);
        publication.clear();
        CHECK(publication.active() == nullptr);
        CHECK(publication.generation() == 2);
        return true;
    }

    bool invalidationDomainsAreIndependent() {
        const auto product = fixture();
        auto current = product.manifest.inputs;
        CHECK(Iridium::bakedLightingInvalidationMask(product.manifest, current,
            product.manifest.bakerId, product.manifest.bakerVersion) ==
            Iridium::BakedLightingInvalidationNone);
        current.materialHash = std::string(64, '0');
        current.lightingHash = std::string(64, '1');
        const uint32_t changed = Iridium::bakedLightingInvalidationMask(
            product.manifest, current, product.manifest.bakerId,
            product.manifest.bakerVersion);
        CHECK((changed & Iridium::BakedLightingInvalidationMaterials) != 0);
        CHECK((changed & Iridium::BakedLightingInvalidationLights) != 0);
        CHECK((changed & Iridium::BakedLightingInvalidationGeometry) == 0);
        return true;
    }

    bool dependencyIdentityAndCookKeyDriveInvalidation() {
        const auto cooked = Iridium::makeCookedBakedLightingProduct(fixture());
        auto stable = artifact(guid("000000000300"), cooked);
        auto moved = stable;
        moved.dependencies[0].location = "renamed/scenes/atrium.scene";
        const auto stableRead = Iridium::readCookedBakedLightingProduct(stable);
        const auto movedRead = Iridium::readCookedBakedLightingProduct(moved);
        CHECK(stableRead.valid());
        CHECK(movedRead.valid());
        CHECK(stableRead.data->lightmapBindings ==
            movedRead.data->lightmapBindings);

        const std::array<std::byte, 2> settings{
            std::byte{ 1 }, std::byte{ 2 } };
        const auto key = [&](const std::vector<Iridium::AssetDependency>& deps) {
            return Iridium::calculateCookKey({
                .assetGuid = guid("000000000300"),
                .importerId = "iridium.baked-lighting@1",
                .importerImplementationVersion = 1,
                .settingsSchemaVersion = 1,
                .canonicalSettings = settings,
                .sourceContentHash = std::string(64, '3'),
                .dependencies = deps,
                .target = stable.target,
                .cookerFeatureVersion = "m5.10",
            });
        };
        auto changed = stable.dependencies;
        changed[0].artifactHash = std::string(64, '4');
        CHECK(key(stable.dependencies) != key(changed));

        auto missing = stable;
        missing.dependencies.clear();
        CHECK(!Iridium::readCookedBakedLightingProduct(missing).valid());
        return true;
    }

} // namespace

int main() {
    const std::array tests{
        std::pair{ "deterministic typed round trip", deterministicTypedRoundTrip },
        std::pair{ "malformed sections fail closed", malformedSectionsFailClosed },
        std::pair{ "last known good publication", publicationRetainsLastKnownGood },
        std::pair{ "invalidation domains", invalidationDomainsAreIndependent },
        std::pair{ "dependency identity and cook key",
            dependencyIdentityAndCookKeyDriveInvalidation },
    };
    for (const auto& [name, run] : tests) {
        if (!run()) { std::cerr << "[FAIL] " << name << '\n'; return 1; }
        std::cout << "[PASS] " << name << '\n';
    }
    return 0;
}
