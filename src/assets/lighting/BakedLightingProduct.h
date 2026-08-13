#pragma once

#include "assets/AssetGuid.h"
#include "assets/cooker/CookTypes.h"
#include "assets/cooker/CookedArtifact.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Iridium {

    inline constexpr uint32_t kBakedLightingSchemaVersion = 1;
    inline constexpr uint32_t kBakedLightingManifestSection = 0x424c4d31; // BLM1
    inline constexpr uint32_t kBakedLightingLightmapSection = 0x424c4c31; // BLL1
    inline constexpr uint32_t kBakedLightingProbeVolumeSection = 0x424c5031; // BLP1
    inline constexpr uint32_t kBakedLightingVisibilitySection = 0x424c5631; // BLV1
    inline constexpr std::string_view kBakedLightingColorSpace =
        "scene_linear_acescg_ap1_d60";
    inline constexpr std::string_view kBakedLightingLengthUnit = "meter";

    using BakedSceneEntityId = std::array<uint8_t, 16>;

    enum class BakedLightmapEncoding : uint32_t {
        DirectionalBasisRgb16F = 1,
        DirectionalBasisBc6h = 2,
    };

    enum class BakedProbeVolumeEncoding : uint32_t {
        ShL2Rgb16F = 1,
        ShL2Rgb32F = 2,
    };

    enum class BakedVisibilityEncoding : uint32_t {
        BentNormalConeRgba16F = 1,
        DirectionalMomentsRgba16F = 2,
    };

    enum BakedLightingInvalidation : uint32_t {
        BakedLightingInvalidationNone = 0,
        BakedLightingInvalidationScene = 1u << 0u,
        BakedLightingInvalidationGeometry = 1u << 1u,
        BakedLightingInvalidationMaterials = 1u << 2u,
        BakedLightingInvalidationLights = 1u << 3u,
        BakedLightingInvalidationSettings = 1u << 4u,
        BakedLightingInvalidationTool = 1u << 5u,
    };

    struct BakedLightingInputFingerprint {
        std::string sceneCanonicalHash;
        std::string geometryHash;
        std::string materialHash;
        std::string lightingHash;
        std::string bakeSettingsHash;
        std::string toolHash;

        auto operator<=>(const BakedLightingInputFingerprint&) const = default;
    };

    struct BakedLightingManifest {
        uint32_t schemaVersion = kBakedLightingSchemaVersion;
        AssetGuid sceneAssetGuid;
        std::string colorSpace{ kBakedLightingColorSpace };
        std::string lengthUnit{ kBakedLightingLengthUnit };
        std::string bakerId;
        uint32_t bakerVersion = 0;
        std::string qualityProfile;
        BakedLightingInputFingerprint inputs;
        uint32_t lightmapAtlasCount = 0;
        uint32_t lightmapBindingCount = 0;
        uint32_t probeVolumeCount = 0;
        uint32_t visibilityVolumeCount = 0;

        auto operator<=>(const BakedLightingManifest&) const = default;
    };

    struct BakedLightmapAtlasDesc {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t layers = 0;
        uint32_t mipLevels = 0;
        BakedLightmapEncoding encoding =
            BakedLightmapEncoding::DirectionalBasisRgb16F;
        uint64_t payloadOffset = 0;
        uint64_t payloadSize = 0;

        auto operator<=>(const BakedLightmapAtlasDesc&) const = default;
    };

    struct BakedLightmapBinding {
        BakedSceneEntityId entity;
        AssetGuid meshPrimitiveGuid;
        uint32_t atlasIndex = 0;
        uint32_t uvSet = 1;
        std::array<float, 4> uvScaleBias{ 1.0f, 1.0f, 0.0f, 0.0f };

        auto operator<=>(const BakedLightmapBinding&) const = default;
    };

    struct BakedProbeVolumeDesc {
        BakedSceneEntityId owner;
        std::array<float, 3> boundsMin{};
        std::array<float, 3> boundsMax{};
        std::array<uint32_t, 3> probeCount{};
        BakedProbeVolumeEncoding encoding =
            BakedProbeVolumeEncoding::ShL2Rgb16F;
        uint64_t payloadOffset = 0;
        uint64_t payloadSize = 0;

        auto operator<=>(const BakedProbeVolumeDesc&) const = default;
    };

    struct BakedVisibilityVolumeDesc {
        BakedSceneEntityId owner;
        std::array<float, 3> boundsMin{};
        std::array<float, 3> boundsMax{};
        std::array<uint32_t, 3> cellCount{};
        BakedVisibilityEncoding encoding =
            BakedVisibilityEncoding::BentNormalConeRgba16F;
        uint64_t payloadOffset = 0;
        uint64_t payloadSize = 0;

        auto operator<=>(const BakedVisibilityVolumeDesc&) const = default;
    };

    struct BakedLightingProductData {
        BakedLightingManifest manifest;
        std::vector<BakedLightmapAtlasDesc> lightmapAtlases;
        std::vector<BakedLightmapBinding> lightmapBindings;
        std::vector<std::byte> lightmapPayload;
        std::vector<BakedProbeVolumeDesc> probeVolumes;
        std::vector<std::byte> probeVolumePayload;
        std::vector<BakedVisibilityVolumeDesc> visibilityVolumes;
        std::vector<std::byte> visibilityPayload;
    };

    struct BakedLightingReadResult {
        std::optional<BakedLightingProductData> data;
        std::vector<CookDiagnostic> diagnostics;

        [[nodiscard]] bool valid() const noexcept {
            return data.has_value() && !hasCookErrors(diagnostics);
        }
    };

    struct BakedLightingPublicationResult {
        bool published = false;
        bool retainedLastKnownGood = false;
        uint64_t generation = 0;
        std::vector<CookDiagnostic> diagnostics;
    };

    class BakedLightingPublication {
    public:
        [[nodiscard]] BakedLightingPublicationResult publish(
            const CookedArtifact& artifact);
        void clear() noexcept;

        [[nodiscard]] const BakedLightingProductData* active() const noexcept;
        [[nodiscard]] AssetGuid activeAssetGuid() const noexcept {
            return activeAssetGuid_;
        }
        [[nodiscard]] uint64_t generation() const noexcept { return generation_; }

    private:
        std::optional<BakedLightingProductData> active_;
        AssetGuid activeAssetGuid_;
        uint64_t generation_ = 0;
    };

    [[nodiscard]] uint32_t bakedLightingInvalidationMask(
        const BakedLightingManifest& baked,
        const BakedLightingInputFingerprint& current,
        std::string_view currentBakerId,
        uint32_t currentBakerVersion) noexcept;
    [[nodiscard]] std::vector<CookDiagnostic> validateBakedLightingProduct(
        const BakedLightingProductData& product);
    [[nodiscard]] CookProduct makeCookedBakedLightingProduct(
        BakedLightingProductData product);
    [[nodiscard]] BakedLightingReadResult readCookedBakedLightingProduct(
        const CookedArtifact& artifact);

} // namespace Iridium
