#pragma once

#include "assets/AssetGuid.h"
#include "assets/cooker/CookTypes.h"
#include "assets/cooker/CookedArtifact.h"
#include "renderer/rhi/TextureTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Iridium {

    inline constexpr uint32_t kCookedEnvironmentSchemaVersion = 1;
    inline constexpr uint32_t kCookedEnvironmentManifestSection = 0x454e4d31; // ENM1
    inline constexpr uint32_t kCookedEnvironmentRadianceSection = 0x454e5231; // ENR1
    inline constexpr uint32_t kCookedEnvironmentIrradianceSection = 0x454e4931; // ENI1
    inline constexpr uint32_t kCookedEnvironmentPrefilterSection = 0x454e5031; // ENP1
    inline constexpr uint32_t kCookedEnvironmentBrdfSection = 0x454e4231; // ENB1
    inline constexpr const char* kEnvironmentCubeOrientation =
        "vulkan_rhs_+x_-x_+y_-y_+z_-z_v1";
    inline constexpr const char* kEnvironmentRoughnessMipConvention =
        "roughness=mip/(mip_count-1)";

    struct EnvironmentImageProductDesc {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mipLevels = 1;
        uint32_t arrayLayers = 1;
        TextureFormat format = TextureFormat::RGBA16_SFloat;

        auto operator<=>(const EnvironmentImageProductDesc&) const = default;
    };

    struct CookedEnvironmentManifest {
        uint32_t schemaVersion = kCookedEnvironmentSchemaVersion;
        AssetGuid sourceTextureGuid;
        std::string sourcePrimaries = "linear_rec709_d65";
        float sourceRadianceScale = 1.0f;
        std::string orientation = kEnvironmentCubeOrientation;
        std::string convolutionImplementation;
        std::string sampleSequence;
        std::string roughnessMipConvention =
            kEnvironmentRoughnessMipConvention;
        std::string toolVersion;
        EnvironmentImageProductDesc radiance;
        EnvironmentImageProductDesc irradiance;
        EnvironmentImageProductDesc prefilteredSpecular;
        EnvironmentImageProductDesc brdfLut;

        auto operator<=>(const CookedEnvironmentManifest&) const = default;
    };

    struct EnvironmentPayloads {
        std::span<const std::byte> radiance;
        std::span<const std::byte> irradiance;
        std::span<const std::byte> prefilteredSpecular;
        std::span<const std::byte> brdfLut;
    };

    struct CookedEnvironmentProductData {
        CookedEnvironmentManifest manifest;
        std::vector<std::byte> radiance;
        std::vector<std::byte> irradiance;
        std::vector<std::byte> prefilteredSpecular;
        std::vector<std::byte> brdfLut;
    };

    struct CookedEnvironmentReadResult {
        std::optional<CookedEnvironmentProductData> data;
        std::vector<CookDiagnostic> diagnostics;

        [[nodiscard]] bool valid() const noexcept {
            return data.has_value() && !hasCookErrors(diagnostics);
        }
    };

    [[nodiscard]] uint64_t environmentProductByteSize(
        const EnvironmentImageProductDesc& desc) noexcept;
    [[nodiscard]] std::vector<std::byte> serializeEnvironmentManifest(
        const CookedEnvironmentManifest& manifest);
    [[nodiscard]] std::optional<CookedEnvironmentManifest>
        readEnvironmentManifest(std::span<const std::byte> bytes,
            std::vector<CookDiagnostic>& diagnostics);
    [[nodiscard]] std::vector<CookDiagnostic> validateEnvironmentProduct(
        const CookedEnvironmentManifest& manifest,
        const EnvironmentPayloads& payloads);
    [[nodiscard]] CookProduct makeCookedEnvironmentProduct(
        const CookedEnvironmentManifest& manifest,
        const EnvironmentPayloads& payloads);
    [[nodiscard]] CookedEnvironmentReadResult readCookedEnvironmentProduct(
        const CookedArtifact& artifact);

} // namespace Iridium
