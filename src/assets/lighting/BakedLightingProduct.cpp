#include "assets/lighting/BakedLightingProduct.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>

#include <nlohmann/json.hpp>

namespace Iridium {
namespace {

    using Json = nlohmann::ordered_json;

    void error(std::vector<CookDiagnostic>& diagnostics, std::string code,
        std::string field, std::string message) {
        diagnostics.push_back({ CookDiagnosticSeverity::Error,
            std::move(code), std::move(field), std::move(message) });
    }

    [[nodiscard]] bool hash64(std::string_view value) noexcept {
        if (value.size() != 64) return false;
        return std::ranges::all_of(value, [](char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f');
        });
    }

    [[nodiscard]] bool validEntityId(const BakedSceneEntityId& value) noexcept {
        const bool nil = std::ranges::all_of(value,
            [](uint8_t byte) { return byte == 0; });
        const uint8_t version = static_cast<uint8_t>(value[6] >> 4u);
        return !nil && (value[8] & 0xc0u) == 0x80u &&
            (version == 5u || version == 7u);
    }

    [[nodiscard]] bool finite(std::span<const float> values) noexcept {
        return std::ranges::all_of(values,
            [](float value) { return std::isfinite(value); });
    }

    template <typename Integer>
    void appendInteger(std::vector<std::byte>& bytes, Integer value) {
        using Unsigned = std::make_unsigned_t<Integer>;
        const Unsigned bits = static_cast<Unsigned>(value);
        for (size_t index = 0; index < sizeof(Integer); ++index) {
            bytes.push_back(static_cast<std::byte>(bits >> (index * 8u)));
        }
    }

    void appendFloat(std::vector<std::byte>& bytes, float value) {
        appendInteger(bytes, std::bit_cast<uint32_t>(value));
    }

    void appendGuid(std::vector<std::byte>& bytes, AssetGuid value) {
        for (uint8_t byte : value.bytes()) bytes.push_back(static_cast<std::byte>(byte));
    }

    void appendEntity(std::vector<std::byte>& bytes,
        const BakedSceneEntityId& value) {
        for (uint8_t byte : value) bytes.push_back(static_cast<std::byte>(byte));
    }

    class Reader {
    public:
        explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

        template <typename Integer>
        bool integer(Integer& value) {
            static_assert(std::is_integral_v<Integer>);
            if (remaining() < sizeof(Integer)) return false;
            using Unsigned = std::make_unsigned_t<Integer>;
            Unsigned bits = 0;
            for (size_t index = 0; index < sizeof(Integer); ++index) {
                bits |= static_cast<Unsigned>(std::to_integer<uint8_t>(
                    bytes_[offset_ + index])) << (index * 8u);
            }
            offset_ += sizeof(Integer);
            value = static_cast<Integer>(bits);
            return true;
        }

        bool floating(float& value) {
            uint32_t bits = 0;
            if (!integer(bits)) return false;
            value = std::bit_cast<float>(bits);
            return std::isfinite(value);
        }

        bool guid(AssetGuid& value) {
            if (remaining() < 16) return false;
            AssetGuid::Bytes bytes{};
            for (size_t index = 0; index < bytes.size(); ++index)
                bytes[index] = std::to_integer<uint8_t>(bytes_[offset_ + index]);
            offset_ += bytes.size();
            value = AssetGuid(bytes);
            return true;
        }

        bool entity(BakedSceneEntityId& value) {
            if (remaining() < value.size()) return false;
            for (size_t index = 0; index < value.size(); ++index)
                value[index] = std::to_integer<uint8_t>(bytes_[offset_ + index]);
            offset_ += value.size();
            return true;
        }

        bool payload(uint64_t size, std::vector<std::byte>& value) {
            if (size > remaining() ||
                size > static_cast<uint64_t>((std::numeric_limits<size_t>::max)()))
                return false;
            value.assign(bytes_.begin() + static_cast<ptrdiff_t>(offset_),
                bytes_.begin() + static_cast<ptrdiff_t>(offset_ + size));
            offset_ += static_cast<size_t>(size);
            return true;
        }

        [[nodiscard]] size_t remaining() const noexcept {
            return offset_ <= bytes_.size() ? bytes_.size() - offset_ : 0;
        }

    private:
        std::span<const std::byte> bytes_;
        size_t offset_ = 0;
    };

    [[nodiscard]] Json fingerprintJson(
        const BakedLightingInputFingerprint& value) {
        return {
            { "bake_settings", value.bakeSettingsHash },
            { "geometry", value.geometryHash },
            { "lighting", value.lightingHash },
            { "materials", value.materialHash },
            { "scene_canonical", value.sceneCanonicalHash },
            { "tool", value.toolHash },
        };
    }

    [[nodiscard]] std::vector<std::byte> serializeManifest(
        const BakedLightingManifest& value) {
        const Json root{
            { "baker", {
                { "id", value.bakerId },
                { "version", value.bakerVersion },
            } },
            { "color_space", value.colorSpace },
            { "counts", {
                { "lightmap_atlases", value.lightmapAtlasCount },
                { "lightmap_bindings", value.lightmapBindingCount },
                { "probe_volumes", value.probeVolumeCount },
                { "visibility_volumes", value.visibilityVolumeCount },
            } },
            { "inputs", fingerprintJson(value.inputs) },
            { "length_unit", value.lengthUnit },
            { "quality_profile", value.qualityProfile },
            { "scene_asset_guid", value.sceneAssetGuid.toString() },
            { "schema", value.schemaVersion },
        };
        const std::string text = root.dump();
        return { reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size()) };
    }

    [[nodiscard]] std::optional<BakedLightingManifest> readManifest(
        std::span<const std::byte> bytes,
        std::vector<CookDiagnostic>& diagnostics) {
        try {
            const Json root = Json::parse(
                reinterpret_cast<const char*>(bytes.data()),
                reinterpret_cast<const char*>(bytes.data() + bytes.size()));
            const auto scene = AssetGuid::parse(
                root.at("scene_asset_guid").get<std::string>());
            if (!scene) throw std::runtime_error("scene asset GUID is invalid");
            const Json& inputs = root.at("inputs");
            const Json& counts = root.at("counts");
            BakedLightingManifest result;
            result.schemaVersion = root.at("schema").get<uint32_t>();
            result.sceneAssetGuid = *scene;
            result.colorSpace = root.at("color_space").get<std::string>();
            result.lengthUnit = root.at("length_unit").get<std::string>();
            result.bakerId = root.at("baker").at("id").get<std::string>();
            result.bakerVersion = root.at("baker").at("version").get<uint32_t>();
            result.qualityProfile = root.at("quality_profile").get<std::string>();
            result.inputs = {
                .sceneCanonicalHash = inputs.at("scene_canonical").get<std::string>(),
                .geometryHash = inputs.at("geometry").get<std::string>(),
                .materialHash = inputs.at("materials").get<std::string>(),
                .lightingHash = inputs.at("lighting").get<std::string>(),
                .bakeSettingsHash = inputs.at("bake_settings").get<std::string>(),
                .toolHash = inputs.at("tool").get<std::string>(),
            };
            result.lightmapAtlasCount = counts.at("lightmap_atlases").get<uint32_t>();
            result.lightmapBindingCount = counts.at("lightmap_bindings").get<uint32_t>();
            result.probeVolumeCount = counts.at("probe_volumes").get<uint32_t>();
            result.visibilityVolumeCount =
                counts.at("visibility_volumes").get<uint32_t>();
            return result;
        } catch (const std::exception& exception) {
            error(diagnostics, "BAKED_LIGHTING_MANIFEST_PARSE", "/",
                std::string("Invalid baked-lighting manifest: ") + exception.what());
            return std::nullopt;
        }
    }

    [[nodiscard]] bool addSize(size_t& total, size_t count, size_t element) {
        if (count > ((std::numeric_limits<size_t>::max)() - total) / element)
            return false;
        total += count * element;
        return true;
    }

    [[nodiscard]] std::vector<std::byte> serializeLightmaps(
        const BakedLightingProductData& product) {
        std::vector<std::byte> bytes;
        appendInteger<uint32_t>(bytes,
            static_cast<uint32_t>(product.lightmapAtlases.size()));
        appendInteger<uint32_t>(bytes,
            static_cast<uint32_t>(product.lightmapBindings.size()));
        appendInteger<uint64_t>(bytes, product.lightmapPayload.size());
        for (const BakedLightmapAtlasDesc& value : product.lightmapAtlases) {
            appendInteger(bytes, value.width);
            appendInteger(bytes, value.height);
            appendInteger(bytes, value.layers);
            appendInteger(bytes, value.mipLevels);
            appendInteger(bytes, static_cast<uint32_t>(value.encoding));
            appendInteger(bytes, value.payloadOffset);
            appendInteger(bytes, value.payloadSize);
        }
        for (const BakedLightmapBinding& value : product.lightmapBindings) {
            appendEntity(bytes, value.entity);
            appendGuid(bytes, value.meshPrimitiveGuid);
            appendInteger(bytes, value.atlasIndex);
            appendInteger(bytes, value.uvSet);
            for (float field : value.uvScaleBias) appendFloat(bytes, field);
        }
        bytes.insert(bytes.end(), product.lightmapPayload.begin(),
            product.lightmapPayload.end());
        return bytes;
    }

    [[nodiscard]] bool readLightmaps(std::span<const std::byte> bytes,
        BakedLightingProductData& product) {
        Reader reader(bytes);
        uint32_t atlasCount = 0;
        uint32_t bindingCount = 0;
        uint64_t payloadSize = 0;
        size_t minimum = 16;
        if (!reader.integer(atlasCount) || !reader.integer(bindingCount) ||
            !reader.integer(payloadSize) ||
            !addSize(minimum, atlasCount, 36) ||
            !addSize(minimum, bindingCount, 56) ||
            minimum > bytes.size() || payloadSize != bytes.size() - minimum)
            return false;
        product.lightmapAtlases.resize(atlasCount);
        for (BakedLightmapAtlasDesc& value : product.lightmapAtlases) {
            uint32_t encoding = 0;
            if (!reader.integer(value.width) || !reader.integer(value.height) ||
                !reader.integer(value.layers) || !reader.integer(value.mipLevels) ||
                !reader.integer(encoding) || !reader.integer(value.payloadOffset) ||
                !reader.integer(value.payloadSize)) return false;
            value.encoding = static_cast<BakedLightmapEncoding>(encoding);
        }
        product.lightmapBindings.resize(bindingCount);
        for (BakedLightmapBinding& value : product.lightmapBindings) {
            if (!reader.entity(value.entity) || !reader.guid(value.meshPrimitiveGuid) ||
                !reader.integer(value.atlasIndex) || !reader.integer(value.uvSet))
                return false;
            for (float& field : value.uvScaleBias)
                if (!reader.floating(field)) return false;
        }
        return reader.payload(payloadSize, product.lightmapPayload) &&
            reader.remaining() == 0;
    }

    template <typename Desc>
    void appendVolume(std::vector<std::byte>& bytes, const Desc& value) {
        appendEntity(bytes, value.owner);
        for (float field : value.boundsMin) appendFloat(bytes, field);
        for (float field : value.boundsMax) appendFloat(bytes, field);
        const auto& counts = [&]() -> const auto& {
            if constexpr (std::is_same_v<Desc, BakedProbeVolumeDesc>)
                return value.probeCount;
            else return value.cellCount;
        }();
        for (uint32_t field : counts) appendInteger(bytes, field);
        appendInteger(bytes, static_cast<uint32_t>(value.encoding));
        appendInteger(bytes, value.payloadOffset);
        appendInteger(bytes, value.payloadSize);
    }

    template <typename Desc>
    [[nodiscard]] bool readVolume(Reader& reader, Desc& value) {
        if (!reader.entity(value.owner)) return false;
        for (float& field : value.boundsMin) if (!reader.floating(field)) return false;
        for (float& field : value.boundsMax) if (!reader.floating(field)) return false;
        auto& counts = [&]() -> auto& {
            if constexpr (std::is_same_v<Desc, BakedProbeVolumeDesc>)
                return value.probeCount;
            else return value.cellCount;
        }();
        for (uint32_t& field : counts) if (!reader.integer(field)) return false;
        uint32_t encoding = 0;
        if (!reader.integer(encoding) || !reader.integer(value.payloadOffset) ||
            !reader.integer(value.payloadSize)) return false;
        value.encoding = static_cast<decltype(value.encoding)>(encoding);
        return true;
    }

    template <typename Desc>
    [[nodiscard]] std::vector<std::byte> serializeVolumes(
        const std::vector<Desc>& volumes, std::span<const std::byte> payload) {
        std::vector<std::byte> bytes;
        appendInteger<uint32_t>(bytes, static_cast<uint32_t>(volumes.size()));
        appendInteger<uint64_t>(bytes, payload.size());
        for (const Desc& value : volumes) appendVolume(bytes, value);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        return bytes;
    }

    template <typename Desc>
    [[nodiscard]] bool readVolumes(std::span<const std::byte> bytes,
        std::vector<Desc>& volumes, std::vector<std::byte>& payload) {
        Reader reader(bytes);
        uint32_t count = 0;
        uint64_t payloadSize = 0;
        size_t minimum = 12;
        if (!reader.integer(count) || !reader.integer(payloadSize) ||
            !addSize(minimum, count, 72) || minimum > bytes.size() ||
            payloadSize != bytes.size() - minimum) return false;
        volumes.resize(count);
        for (Desc& value : volumes) if (!readVolume(reader, value)) return false;
        return reader.payload(payloadSize, payload) && reader.remaining() == 0;
    }

    template <typename Desc>
    void validateRanges(std::vector<CookDiagnostic>& diagnostics,
        std::string_view field, const std::vector<Desc>& values,
        size_t payloadSize) {
        uint64_t expectedOffset = 0;
        for (size_t index = 0; index < values.size(); ++index) {
            const Desc& value = values[index];
            if (value.payloadSize == 0 || value.payloadOffset != expectedOffset ||
                value.payloadOffset > payloadSize ||
                value.payloadSize > payloadSize - value.payloadOffset) {
                error(diagnostics, "BAKED_LIGHTING_PAYLOAD_RANGE",
                    std::string(field) + "/" + std::to_string(index),
                    "Payload ranges must be nonempty, tightly packed, and in bounds.");
                return;
            }
            expectedOffset += value.payloadSize;
        }
        if (expectedOffset != payloadSize) {
            error(diagnostics, "BAKED_LIGHTING_PAYLOAD_COVERAGE",
                std::string(field),
                "Typed descriptors must cover their payload exactly.");
        }
    }

    template <typename Desc>
    void validateVolumes(std::vector<CookDiagnostic>& diagnostics,
        std::string_view field, const std::vector<Desc>& values,
        size_t payloadSize) {
        BakedSceneEntityId previous{};
        bool havePrevious = false;
        for (size_t index = 0; index < values.size(); ++index) {
            const Desc& value = values[index];
            const auto& counts = [&]() -> const auto& {
                if constexpr (std::is_same_v<Desc, BakedProbeVolumeDesc>)
                    return value.probeCount;
                else return value.cellCount;
            }();
            const bool encodingValid = [&]() {
                if constexpr (std::is_same_v<Desc, BakedProbeVolumeDesc>)
                    return value.encoding == BakedProbeVolumeEncoding::ShL2Rgb16F ||
                        value.encoding == BakedProbeVolumeEncoding::ShL2Rgb32F;
                else return value.encoding ==
                        BakedVisibilityEncoding::BentNormalConeRgba16F ||
                    value.encoding ==
                        BakedVisibilityEncoding::DirectionalMomentsRgba16F;
            }();
            if (!validEntityId(value.owner) ||
                !finite(value.boundsMin) || !finite(value.boundsMax) ||
                !std::ranges::all_of(counts,
                    [](uint32_t count) { return count > 0; }) ||
                !encodingValid || (havePrevious && !(previous < value.owner))) {
                error(diagnostics, "BAKED_LIGHTING_VOLUME_DESCRIPTOR",
                    std::string(field) + "/" + std::to_string(index),
                    "Volume identity, bounds, grid, encoding, or canonical order is invalid.");
                return;
            }
            for (size_t axis = 0; axis < 3; ++axis) {
                if (!(value.boundsMax[axis] > value.boundsMin[axis])) {
                    error(diagnostics, "BAKED_LIGHTING_VOLUME_BOUNDS",
                        std::string(field) + "/" + std::to_string(index),
                        "Volume maximum bounds must be greater than minimum bounds.");
                    return;
                }
            }
            previous = value.owner;
            havePrevious = true;
        }
        validateRanges(diagnostics, field, values, payloadSize);
    }

} // namespace

uint32_t bakedLightingInvalidationMask(
    const BakedLightingManifest& baked,
    const BakedLightingInputFingerprint& current,
    std::string_view currentBakerId,
    uint32_t currentBakerVersion) noexcept {
    uint32_t result = BakedLightingInvalidationNone;
    if (baked.inputs.sceneCanonicalHash != current.sceneCanonicalHash)
        result |= BakedLightingInvalidationScene;
    if (baked.inputs.geometryHash != current.geometryHash)
        result |= BakedLightingInvalidationGeometry;
    if (baked.inputs.materialHash != current.materialHash)
        result |= BakedLightingInvalidationMaterials;
    if (baked.inputs.lightingHash != current.lightingHash)
        result |= BakedLightingInvalidationLights;
    if (baked.inputs.bakeSettingsHash != current.bakeSettingsHash)
        result |= BakedLightingInvalidationSettings;
    if (baked.inputs.toolHash != current.toolHash ||
        baked.bakerId != currentBakerId || baked.bakerVersion != currentBakerVersion)
        result |= BakedLightingInvalidationTool;
    return result;
}

std::vector<CookDiagnostic> validateBakedLightingProduct(
    const BakedLightingProductData& product) {
    std::vector<CookDiagnostic> diagnostics;
    const BakedLightingManifest& manifest = product.manifest;
    if (manifest.schemaVersion != kBakedLightingSchemaVersion)
        error(diagnostics, "BAKED_LIGHTING_SCHEMA", "/schema",
            "Unsupported baked-lighting schema.");
    if (manifest.sceneAssetGuid.isNil())
        error(diagnostics, "BAKED_LIGHTING_SCENE", "/scene_asset_guid",
            "A stable scene asset GUID is required.");
    if (manifest.colorSpace != kBakedLightingColorSpace ||
        manifest.lengthUnit != kBakedLightingLengthUnit)
        error(diagnostics, "BAKED_LIGHTING_SEMANTICS", "/",
            "Baked lighting must use scene-linear ACEScg and metre units.");
    if (manifest.bakerId.empty() || manifest.bakerVersion == 0 ||
        manifest.qualityProfile.empty())
        error(diagnostics, "BAKED_LIGHTING_PROVENANCE", "/baker",
            "Baker identity, version, and quality profile are required.");
    const std::array hashes{
        manifest.inputs.sceneCanonicalHash, manifest.inputs.geometryHash,
        manifest.inputs.materialHash, manifest.inputs.lightingHash,
        manifest.inputs.bakeSettingsHash, manifest.inputs.toolHash,
    };
    if (!std::ranges::all_of(hashes, hash64))
        error(diagnostics, "BAKED_LIGHTING_INPUT_HASH", "/inputs",
            "Every invalidation input must be a lowercase SHA-256 hash.");
    if (manifest.lightmapAtlasCount != product.lightmapAtlases.size() ||
        manifest.lightmapBindingCount != product.lightmapBindings.size() ||
        manifest.probeVolumeCount != product.probeVolumes.size() ||
        manifest.visibilityVolumeCount != product.visibilityVolumes.size())
        error(diagnostics, "BAKED_LIGHTING_COUNTS", "/counts",
            "Manifest counts do not match typed sections.");

    for (size_t index = 0; index < product.lightmapAtlases.size(); ++index) {
        const auto& value = product.lightmapAtlases[index];
        const bool encoding =
            value.encoding == BakedLightmapEncoding::DirectionalBasisRgb16F ||
            value.encoding == BakedLightmapEncoding::DirectionalBasisBc6h;
        if (value.width == 0 || value.height == 0 || value.layers == 0 ||
            value.mipLevels == 0 || !encoding) {
            error(diagnostics, "BAKED_LIGHTING_LIGHTMAP_DESCRIPTOR",
                "/lightmaps/atlases/" + std::to_string(index),
                "Lightmap dimensions, layers, mips, or encoding are invalid.");
            break;
        }
    }
    validateRanges(diagnostics, "/lightmaps/atlases", product.lightmapAtlases,
        product.lightmapPayload.size());

    std::pair<BakedSceneEntityId, AssetGuid> previous{};
    bool havePrevious = false;
    for (size_t index = 0; index < product.lightmapBindings.size(); ++index) {
        const auto& value = product.lightmapBindings[index];
        const auto key = std::pair{ value.entity, value.meshPrimitiveGuid };
        if (!validEntityId(value.entity) || value.meshPrimitiveGuid.isNil() ||
            value.atlasIndex >= product.lightmapAtlases.size() ||
            value.uvSet > 7 || !finite(value.uvScaleBias) ||
            (havePrevious && !(previous < key))) {
            error(diagnostics, "BAKED_LIGHTING_LIGHTMAP_BINDING",
                "/lightmaps/bindings/" + std::to_string(index),
                "Lightmap stable identity, atlas, UV transform, or canonical order is invalid.");
            break;
        }
        previous = key;
        havePrevious = true;
    }
    validateVolumes(diagnostics, "/probe_volumes", product.probeVolumes,
        product.probeVolumePayload.size());
    validateVolumes(diagnostics, "/visibility_volumes", product.visibilityVolumes,
        product.visibilityPayload.size());
    return diagnostics;
}

CookProduct makeCookedBakedLightingProduct(BakedLightingProductData product) {
    product.manifest.lightmapAtlasCount =
        static_cast<uint32_t>(product.lightmapAtlases.size());
    product.manifest.lightmapBindingCount =
        static_cast<uint32_t>(product.lightmapBindings.size());
    product.manifest.probeVolumeCount =
        static_cast<uint32_t>(product.probeVolumes.size());
    product.manifest.visibilityVolumeCount =
        static_cast<uint32_t>(product.visibilityVolumes.size());
    std::ranges::sort(product.lightmapBindings, [](const auto& lhs, const auto& rhs) {
        return std::pair{ lhs.entity, lhs.meshPrimitiveGuid } <
            std::pair{ rhs.entity, rhs.meshPrimitiveGuid };
    });
    std::ranges::sort(product.probeVolumes, {}, &BakedProbeVolumeDesc::owner);
    std::ranges::sort(product.visibilityVolumes, {},
        &BakedVisibilityVolumeDesc::owner);

    CookProduct result{
        .artifactType = "iridium.baked-lighting",
        .artifactSchemaVersion = kBakedLightingSchemaVersion,
    };
    result.diagnostics = validateBakedLightingProduct(product);
    if (hasCookErrors(result.diagnostics)) return result;
    result.sections.push_back({ kBakedLightingManifestSection,
        kBakedLightingSchemaVersion, 16, serializeManifest(product.manifest) });
    if (!product.lightmapAtlases.empty())
        result.sections.push_back({ kBakedLightingLightmapSection,
            kBakedLightingSchemaVersion, 256, serializeLightmaps(product) });
    if (!product.probeVolumes.empty())
        result.sections.push_back({ kBakedLightingProbeVolumeSection,
            kBakedLightingSchemaVersion, 256,
            serializeVolumes(product.probeVolumes, product.probeVolumePayload) });
    if (!product.visibilityVolumes.empty())
        result.sections.push_back({ kBakedLightingVisibilitySection,
            kBakedLightingSchemaVersion, 256,
            serializeVolumes(product.visibilityVolumes,
                product.visibilityPayload) });
    return result;
}

BakedLightingReadResult readCookedBakedLightingProduct(
    const CookedArtifact& artifact) {
    BakedLightingReadResult result;
    if (artifact.artifactType != "iridium.baked-lighting" ||
        artifact.artifactSchemaVersion != kBakedLightingSchemaVersion) {
        error(result.diagnostics, "BAKED_LIGHTING_ARTIFACT_TYPE", "/",
            "Cooked artifact is not a supported baked-lighting product.");
        return result;
    }
    std::map<uint32_t, const CookSection*> sections;
    const std::set<uint32_t> known{ kBakedLightingManifestSection,
        kBakedLightingLightmapSection, kBakedLightingProbeVolumeSection,
        kBakedLightingVisibilitySection };
    for (const CookSection& section : artifact.sections) {
        if (!known.contains(section.id))
            error(result.diagnostics, "BAKED_LIGHTING_SECTION_UNKNOWN", "/sections",
                "Baked-lighting artifact contains an unknown section.");
        else if (!sections.emplace(section.id, &section).second)
            error(result.diagnostics, "BAKED_LIGHTING_SECTION_DUPLICATE", "/sections",
                "Baked-lighting section IDs must be unique.");
        if (section.schemaVersion != kBakedLightingSchemaVersion)
            error(result.diagnostics, "BAKED_LIGHTING_SECTION_SCHEMA", "/sections",
                "Baked-lighting section schema is unsupported.");
    }
    if (!sections.contains(kBakedLightingManifestSection))
        error(result.diagnostics, "BAKED_LIGHTING_SECTION_MISSING", "/sections",
            "Baked-lighting manifest section is required.");
    if (hasCookErrors(result.diagnostics)) return result;
    const auto manifest = readManifest(
        sections.at(kBakedLightingManifestSection)->bytes, result.diagnostics);
    if (!manifest) return result;
    const auto sceneDependency = std::ranges::find_if(artifact.dependencies,
        [&](const AssetDependency& dependency) {
            return dependency.type == AssetDependencyType::Asset &&
                dependency.assetGuid &&
                *dependency.assetGuid == manifest->sceneAssetGuid;
        });
    if (sceneDependency == artifact.dependencies.end() ||
        (sceneDependency->contentHash.empty() &&
            sceneDependency->artifactHash.empty())) {
        error(result.diagnostics, "BAKED_LIGHTING_SCENE_DEPENDENCY",
            "/dependencies",
            "The manifest scene must be a hashed stable asset dependency.");
        return result;
    }
    BakedLightingProductData product;
    product.manifest = *manifest;
    if (const auto found = sections.find(kBakedLightingLightmapSection);
        found != sections.end() && !readLightmaps(found->second->bytes, product))
        error(result.diagnostics, "BAKED_LIGHTING_LIGHTMAP_PARSE", "/lightmaps",
            "Baked lightmap section is truncated or malformed.");
    if (const auto found = sections.find(kBakedLightingProbeVolumeSection);
        found != sections.end() && !readVolumes(found->second->bytes,
            product.probeVolumes, product.probeVolumePayload))
        error(result.diagnostics, "BAKED_LIGHTING_PROBE_PARSE", "/probe_volumes",
            "Baked irradiance-probe section is truncated or malformed.");
    if (const auto found = sections.find(kBakedLightingVisibilitySection);
        found != sections.end() && !readVolumes(found->second->bytes,
            product.visibilityVolumes, product.visibilityPayload))
        error(result.diagnostics, "BAKED_LIGHTING_VISIBILITY_PARSE",
            "/visibility_volumes",
            "Baked visibility section is truncated or malformed.");
    if (hasCookErrors(result.diagnostics)) return result;
    std::vector<CookDiagnostic> validation = validateBakedLightingProduct(product);
    result.diagnostics.insert(result.diagnostics.end(), validation.begin(),
        validation.end());
    if (!hasCookErrors(result.diagnostics)) result.data = std::move(product);
    return result;
}

BakedLightingPublicationResult BakedLightingPublication::publish(
    const CookedArtifact& artifact) {
    BakedLightingReadResult read = readCookedBakedLightingProduct(artifact);
    BakedLightingPublicationResult result{
        .published = read.valid(),
        .retainedLastKnownGood = !read.valid() && active_.has_value(),
        .generation = generation_,
        .diagnostics = std::move(read.diagnostics),
    };
    if (!read.valid()) return result;
    active_ = std::move(*read.data);
    activeAssetGuid_ = artifact.assetGuid;
    ++generation_;
    if (generation_ == 0) ++generation_;
    result.generation = generation_;
    return result;
}

void BakedLightingPublication::clear() noexcept {
    if (!active_) return;
    active_.reset();
    activeAssetGuid_ = {};
    ++generation_;
    if (generation_ == 0) ++generation_;
}

const BakedLightingProductData* BakedLightingPublication::active() const noexcept {
    return active_ ? &*active_ : nullptr;
}

} // namespace Iridium
