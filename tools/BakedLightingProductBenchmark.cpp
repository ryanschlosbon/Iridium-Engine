#include "assets/lighting/BakedLightingProduct.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <vector>

namespace {

    using Clock = std::chrono::steady_clock;

    Iridium::AssetGuid assetId(uint64_t value) {
        Iridium::AssetGuid::Bytes bytes{};
        bytes[0] = 0x01;
        bytes[6] = 0x70;
        bytes[8] = 0x80;
        for (size_t index = 0; index < 7; ++index)
            bytes[15 - index] = static_cast<uint8_t>(value >> (index * 8u));
        return Iridium::AssetGuid(bytes);
    }

    Iridium::BakedSceneEntityId entityId(uint64_t value) {
        auto result = assetId(value).bytes();
        result[6] = 0x50;
        return result;
    }

    Iridium::BakedLightingProductData fixture() {
        constexpr size_t lightmapBytes = 32ull * 1024ull * 1024ull;
        constexpr size_t volumeBytes = 2ull * 1024ull * 1024ull;
        Iridium::BakedLightingProductData product;
        product.manifest.sceneAssetGuid = assetId(1);
        product.manifest.bakerId = "iridium.contract-benchmark";
        product.manifest.bakerVersion = 1;
        product.manifest.qualityProfile = "cinematic";
        product.manifest.inputs = {
            std::string(64, 'a'), std::string(64, 'b'),
            std::string(64, 'c'), std::string(64, 'd'),
            std::string(64, 'e'), std::string(64, 'f'),
        };
        product.lightmapAtlases.push_back({
            .width = 8192, .height = 8192, .layers = 3, .mipLevels = 14,
            .encoding = Iridium::BakedLightmapEncoding::DirectionalBasisBc6h,
            .payloadOffset = 0, .payloadSize = lightmapBytes,
        });
        product.lightmapPayload.resize(lightmapBytes, std::byte{ 7 });
        product.lightmapBindings.reserve(100'000);
        for (uint64_t index = 0; index < 100'000; ++index) {
            product.lightmapBindings.push_back({
                .entity = entityId(index + 10),
                .meshPrimitiveGuid = assetId(index + 200'000),
                .atlasIndex = 0,
                .uvSet = 1,
                .uvScaleBias = { 0.00390625f, 0.00390625f,
                    static_cast<float>(index % 256) / 256.0f,
                    static_cast<float>(index / 256) / 512.0f },
            });
        }
        for (uint64_t index = 0; index < 4; ++index) {
            product.probeVolumes.push_back({
                .owner = entityId(120'000 + index),
                .boundsMin = { -50.0f, 0.0f, -50.0f },
                .boundsMax = { 50.0f, 30.0f, 50.0f },
                .probeCount = { 32, 16, 32 },
                .encoding = Iridium::BakedProbeVolumeEncoding::ShL2Rgb16F,
                .payloadOffset = index * volumeBytes,
                .payloadSize = volumeBytes,
            });
            product.visibilityVolumes.push_back({
                .owner = entityId(130'000 + index),
                .boundsMin = { -50.0f, 0.0f, -50.0f },
                .boundsMax = { 50.0f, 30.0f, 50.0f },
                .cellCount = { 64, 32, 64 },
                .encoding = Iridium::BakedVisibilityEncoding::BentNormalConeRgba16F,
                .payloadOffset = index * volumeBytes,
                .payloadSize = volumeBytes,
            });
        }
        product.probeVolumePayload.resize(4 * volumeBytes, std::byte{ 11 });
        product.visibilityPayload.resize(4 * volumeBytes, std::byte{ 17 });
        return product;
    }

    double percentile(std::vector<double> values, double fraction) {
        std::ranges::sort(values);
        const size_t index = static_cast<size_t>(fraction * (values.size() - 1));
        return values[index];
    }

} // namespace

int main() {
    Iridium::CookProduct product =
        Iridium::makeCookedBakedLightingProduct(fixture());
    if (Iridium::hasCookErrors(product.diagnostics)) return 1;
    Iridium::CookedArtifact artifact{
        .assetGuid = assetId(2),
        .artifactType = product.artifactType,
        .artifactSchemaVersion = product.artifactSchemaVersion,
        .target = { .platform = "windows-x64", .profile = "release",
            .qualityPolicy = "cinematic" },
        .cookKey = std::string(64, '1'),
        .dependencies = {
            { .type = Iridium::AssetDependencyType::Asset,
              .assetGuid = assetId(1), .artifactHash = std::string(64, '2') },
        },
        .sections = std::move(product.sections),
    };
    const auto blob = Iridium::serializeCookedArtifact(artifact);
    const auto decoded = Iridium::readCookedArtifact(blob.bytes);
    if (!decoded.valid()) return 2;

    constexpr size_t warmup = 5;
    constexpr size_t measured = 25;
    std::vector<double> loadMilliseconds;
    std::vector<double> publishMilliseconds;
    loadMilliseconds.reserve(measured);
    publishMilliseconds.reserve(measured);
    uint64_t checksum = 0;
    for (size_t iteration = 0; iteration < warmup + measured; ++iteration) {
        const auto loadStart = Clock::now();
        const auto read = Iridium::readCookedBakedLightingProduct(
            *decoded.artifact);
        const auto loadEnd = Clock::now();
        if (!read.valid()) return 3;
        checksum += read.data->lightmapBindings.size();

        Iridium::BakedLightingPublication publication;
        const auto publishStart = Clock::now();
        const auto published = publication.publish(*decoded.artifact);
        const auto publishEnd = Clock::now();
        if (!published.published) return 4;
        checksum += publication.active()->probeVolumes.size();
        if (iteration >= warmup) {
            loadMilliseconds.push_back(std::chrono::duration<double, std::milli>(
                loadEnd - loadStart).count());
            publishMilliseconds.push_back(std::chrono::duration<double, std::milli>(
                publishEnd - publishStart).count());
        }
    }
    const uint64_t payloadBytes = 48ull * 1024ull * 1024ull;
    const uint64_t descriptorBytes =
        sizeof(Iridium::BakedLightmapAtlasDesc) +
        100'000ull * sizeof(Iridium::BakedLightmapBinding) +
        4ull * sizeof(Iridium::BakedProbeVolumeDesc) +
        4ull * sizeof(Iridium::BakedVisibilityVolumeDesc);
    std::cout << std::fixed << std::setprecision(6)
        << "{\n"
        << "  \"artifact_bytes\": " << blob.bytes.size() << ",\n"
        << "  \"payload_bytes\": " << payloadBytes << ",\n"
        << "  \"descriptor_bytes\": " << descriptorBytes << ",\n"
        << "  \"lightmap_bindings\": 100000,\n"
        << "  \"load_median_ms\": " << percentile(loadMilliseconds, 0.5) << ",\n"
        << "  \"load_p95_ms\": " << percentile(loadMilliseconds, 0.95) << ",\n"
        << "  \"publish_median_ms\": " << percentile(publishMilliseconds, 0.5) << ",\n"
        << "  \"publish_p95_ms\": " << percentile(publishMilliseconds, 0.95) << ",\n"
        << "  \"checksum\": " << checksum << "\n"
        << "}\n";
    return 0;
}
