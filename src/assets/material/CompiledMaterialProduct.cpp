#include "assets/material/CompiledMaterialProduct.h"

#include "utils/Sha256.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace Iridium {

    namespace {

        constexpr std::array<std::byte, 8> kMagic{
            std::byte{ 'I' }, std::byte{ 'R' }, std::byte{ 'C' },
            std::byte{ 'M' }, std::byte{ 'A' }, std::byte{ 'T' },
            std::byte{ '0' }, std::byte{ '1' },
        };
        constexpr uint32_t kContainerVersion = 1;
        constexpr size_t kChecksumSize = 64;
        constexpr size_t kHeaderSize =
            kMagic.size() + sizeof(uint32_t) + sizeof(uint64_t) +
            kChecksumSize;
        constexpr uint32_t kMaximumRecords = 1u << 20u;
        constexpr uint32_t kMaximumStringBytes = 16u << 20u;

        class Writer {
        public:
            template <typename Integer>
            void integer(Integer value) {
                static_assert(std::is_integral_v<Integer>);
                using Unsigned = std::make_unsigned_t<Integer>;
                const Unsigned bits = static_cast<Unsigned>(value);
                for (size_t index = 0; index < sizeof(Integer); ++index) {
                    bytes.push_back(static_cast<std::byte>(
                        (bits >> (index * 8)) &
                        static_cast<Unsigned>(0xff)));
                }
            }

            void floating(float value) {
                integer(std::bit_cast<uint32_t>(value));
            }

            void string(std::string_view value) {
                if (value.size() > std::numeric_limits<uint32_t>::max()) {
                    throw std::invalid_argument(
                        "Compiled material string exceeds schema limits.");
                }
                integer(static_cast<uint32_t>(value.size()));
                bytes.insert(bytes.end(),
                    reinterpret_cast<const std::byte*>(value.data()),
                    reinterpret_cast<const std::byte*>(
                        value.data() + value.size()));
            }

            void optionalU32(std::optional<uint32_t> value) {
                integer<uint8_t>(value.has_value() ? 1 : 0);
                integer<uint32_t>(value.value_or(0));
            }

            void optionalI32(std::optional<int32_t> value) {
                integer<uint8_t>(value.has_value() ? 1 : 0);
                integer<int32_t>(value.value_or(0));
            }

            std::vector<std::byte> bytes;
        };

        class Reader {
        public:
            explicit Reader(std::span<const std::byte> input) : m_input(input) {}

            template <typename Integer>
            bool integer(Integer& value) {
                static_assert(std::is_integral_v<Integer>);
                if (remaining() < sizeof(Integer)) return false;
                using Unsigned = std::make_unsigned_t<Integer>;
                Unsigned bits = 0;
                for (size_t index = 0; index < sizeof(Integer); ++index) {
                    bits |= static_cast<Unsigned>(
                        std::to_integer<uint8_t>(m_input[m_offset + index]))
                        << (index * 8);
                }
                value = static_cast<Integer>(bits);
                m_offset += sizeof(Integer);
                return true;
            }

            bool floating(float& value) {
                uint32_t bits = 0;
                if (!integer(bits)) return false;
                value = std::bit_cast<float>(bits);
                return true;
            }

            bool string(std::string& value) {
                uint32_t size = 0;
                if (!integer(size) || size > kMaximumStringBytes ||
                    remaining() < size) {
                    return false;
                }
                value.assign(reinterpret_cast<const char*>(
                    m_input.data() + m_offset), size);
                m_offset += size;
                return true;
            }

            bool optionalU32(std::optional<uint32_t>& value) {
                uint8_t present = 0;
                uint32_t payload = 0;
                if (!integer(present) || present > 1 ||
                    !integer(payload)) {
                    return false;
                }
                value = present != 0 ? std::optional(payload) : std::nullopt;
                return true;
            }

            bool optionalI32(std::optional<int32_t>& value) {
                uint8_t present = 0;
                int32_t payload = 0;
                if (!integer(present) || present > 1 ||
                    !integer(payload)) {
                    return false;
                }
                value = present != 0 ? std::optional(payload) : std::nullopt;
                return true;
            }

            [[nodiscard]] size_t remaining() const noexcept {
                return m_input.size() - m_offset;
            }

        private:
            std::span<const std::byte> m_input;
            size_t m_offset = 0;
        };

        void writeVec2(Writer& writer, glm::vec2 value) {
            writer.floating(value.x);
            writer.floating(value.y);
        }

        void writeVec3(Writer& writer, glm::vec3 value) {
            writer.floating(value.x);
            writer.floating(value.y);
            writer.floating(value.z);
        }

        void writeVec4(Writer& writer, glm::vec4 value) {
            writer.floating(value.x);
            writer.floating(value.y);
            writer.floating(value.z);
            writer.floating(value.w);
        }

        bool readVec2(Reader& reader, glm::vec2& value) {
            return reader.floating(value.x) && reader.floating(value.y);
        }

        bool readVec3(Reader& reader, glm::vec3& value) {
            return reader.floating(value.x) &&
                reader.floating(value.y) &&
                reader.floating(value.z);
        }

        bool readVec4(Reader& reader, glm::vec4& value) {
            return reader.floating(value.x) &&
                reader.floating(value.y) &&
                reader.floating(value.z) &&
                reader.floating(value.w);
        }

        template <typename T>
        void writeSourceValue(Writer& writer, const SourceValue<T>& value) {
            if constexpr (std::is_same_v<T, glm::vec2>) {
                writeVec2(writer, value.value);
            } else if constexpr (std::is_same_v<T, float>) {
                writer.floating(value.value);
            } else if constexpr (std::is_same_v<T, int32_t>) {
                writer.integer(value.value);
            }
            writer.integer(static_cast<uint8_t>(value.origin));
        }

        template <typename T>
        bool readSourceValue(Reader& reader, SourceValue<T>& value) {
            bool readable = false;
            if constexpr (std::is_same_v<T, glm::vec2>) {
                readable = readVec2(reader, value.value);
            } else if constexpr (std::is_same_v<T, float>) {
                readable = reader.floating(value.value);
            } else if constexpr (std::is_same_v<T, int32_t>) {
                readable = reader.integer(value.value);
            }
            uint8_t origin = 0;
            if (!readable || !reader.integer(origin) ||
                origin > static_cast<uint8_t>(
                    SourceValueOrigin::FormatDefault)) {
                return false;
            }
            value.origin = static_cast<SourceValueOrigin>(origin);
            return true;
        }

        void writeOptionalSourceValue(Writer& writer,
            const SourceValue<std::optional<int32_t>>& value) {
            writer.optionalI32(value.value);
            writer.integer(static_cast<uint8_t>(value.origin));
        }

        bool readOptionalSourceValue(Reader& reader,
            SourceValue<std::optional<int32_t>>& value) {
            uint8_t origin = 0;
            if (!reader.optionalI32(value.value) ||
                !reader.integer(origin) ||
                origin > static_cast<uint8_t>(
                    SourceValueOrigin::FormatDefault)) {
                return false;
            }
            value.origin = static_cast<SourceValueOrigin>(origin);
            return true;
        }

        void writeTexture(Writer& writer,
            const CompiledTextureOperation& texture) {
            writer.integer(static_cast<uint8_t>(texture.semantic));
            writer.integer(texture.sourceTextureIndex);
            writer.optionalU32(texture.sourceImageIndex);
            writer.string(texture.imageIdentity);
            writer.string(texture.channels);
            writer.integer(static_cast<uint8_t>(texture.transfer));
            writer.integer(texture.texCoord);
            writeSourceValue(writer, texture.transform.offset);
            writeSourceValue(writer, texture.transform.rotation);
            writeSourceValue(writer, texture.transform.scale);
            writer.optionalU32(texture.transform.texCoordOverride);
            writer.optionalU32(texture.sampler.sourceIndex);
            writeOptionalSourceValue(writer, texture.sampler.magFilter);
            writeOptionalSourceValue(writer, texture.sampler.minFilter);
            writeSourceValue(writer, texture.sampler.wrapS);
            writeSourceValue(writer, texture.sampler.wrapT);
            writeSourceValue(writer, texture.scalar);
        }

        bool readTexture(Reader& reader,
            CompiledTextureOperation& texture) {
            uint8_t semantic = 0;
            uint8_t transfer = 0;
            if (!reader.integer(semantic) ||
                semantic > static_cast<uint8_t>(
                    SourceTextureSemantic::DiffuseTransmissionColor) ||
                !reader.integer(texture.sourceTextureIndex) ||
                !reader.optionalU32(texture.sourceImageIndex) ||
                !reader.string(texture.imageIdentity) ||
                !reader.string(texture.channels) ||
                !reader.integer(transfer) ||
                transfer > static_cast<uint8_t>(
                    SourceTextureTransfer::Linear) ||
                !reader.integer(texture.texCoord) ||
                !readSourceValue(reader, texture.transform.offset) ||
                !readSourceValue(reader, texture.transform.rotation) ||
                !readSourceValue(reader, texture.transform.scale) ||
                !reader.optionalU32(texture.transform.texCoordOverride) ||
                !reader.optionalU32(texture.sampler.sourceIndex) ||
                !readOptionalSourceValue(
                    reader, texture.sampler.magFilter) ||
                !readOptionalSourceValue(
                    reader, texture.sampler.minFilter) ||
                !readSourceValue(reader, texture.sampler.wrapS) ||
                !readSourceValue(reader, texture.sampler.wrapT) ||
                !readSourceValue(reader, texture.scalar)) {
                return false;
            }
            texture.semantic =
                static_cast<SourceTextureSemantic>(semantic);
            texture.transfer =
                static_cast<SourceTextureTransfer>(transfer);
            return true;
        }

        template <typename Lobe>
        const Lobe& requireLobe(const ComplexLobeRecord& record) {
            const Lobe* data = std::get_if<Lobe>(&record.data);
            if (!data) {
                throw std::invalid_argument(
                    "Complex lobe type and payload variant disagree.");
            }
            return *data;
        }

        void writeLobe(Writer& writer,
            const ComplexLobeRecord& record) {
            writer.integer(static_cast<uint8_t>(record.type));
            writer.string(record.sourceExtension);
            switch (record.type) {
            case ComplexLobeType::Clearcoat: {
                const auto& data = requireLobe<ClearcoatLobe>(record);
                writer.floating(data.factor);
                writer.floating(data.roughnessFactor);
                writer.floating(data.normalScale);
                writer.integer(data.textureMask);
                break;
            }
            case ComplexLobeType::Sheen: {
                const auto& data = requireLobe<SheenLobe>(record);
                writeVec3(writer, data.color);
                writer.floating(data.roughnessFactor);
                writer.integer(data.textureMask);
                break;
            }
            case ComplexLobeType::Anisotropy: {
                const auto& data = requireLobe<AnisotropyLobe>(record);
                writer.floating(data.strength);
                writer.floating(data.rotation);
                writer.integer(data.textureMask);
                break;
            }
            case ComplexLobeType::Iridescence: {
                const auto& data = requireLobe<IridescenceLobe>(record);
                writer.floating(data.factor);
                writer.floating(data.ior);
                writer.floating(data.thicknessMinimumNm);
                writer.floating(data.thicknessMaximumNm);
                writer.integer(data.textureMask);
                break;
            }
            case ComplexLobeType::ThinTransmission: {
                const auto& data =
                    requireLobe<ThinTransmissionLobe>(record);
                writer.floating(data.factor);
                writer.floating(data.ior);
                writer.floating(data.specularFactor);
                writeVec3(writer, data.specularColor);
                writer.integer(data.textureMask);
                break;
            }
            case ComplexLobeType::VolumeTransmission: {
                const auto& data =
                    requireLobe<VolumeTransmissionLobe>(record);
                writer.floating(data.thicknessFactor);
                writer.floating(data.attenuationDistance);
                writeVec3(writer, data.attenuationColor);
                writer.integer(data.textureMask);
                break;
            }
            case ComplexLobeType::Dispersion:
                writer.floating(
                    requireLobe<DispersionLobe>(record).dispersion);
                break;
            case ComplexLobeType::DiffuseTransmission: {
                const auto& data =
                    requireLobe<DiffuseTransmissionLobe>(record);
                writer.floating(data.factor);
                writeVec3(writer, data.color);
                writer.integer(data.textureMask);
                break;
            }
            }
        }

        bool readLobe(Reader& reader, ComplexLobeRecord& record) {
            uint8_t type = 0;
            if (!reader.integer(type) ||
                type > static_cast<uint8_t>(
                    ComplexLobeType::DiffuseTransmission) ||
                !reader.string(record.sourceExtension)) {
                return false;
            }
            record.type = static_cast<ComplexLobeType>(type);
            switch (record.type) {
            case ComplexLobeType::Clearcoat: {
                ClearcoatLobe data;
                if (!reader.floating(data.factor) ||
                    !reader.floating(data.roughnessFactor) ||
                    !reader.floating(data.normalScale) ||
                    !reader.integer(data.textureMask)) return false;
                record.data = data;
                break;
            }
            case ComplexLobeType::Sheen: {
                SheenLobe data;
                if (!readVec3(reader, data.color) ||
                    !reader.floating(data.roughnessFactor) ||
                    !reader.integer(data.textureMask)) return false;
                record.data = data;
                break;
            }
            case ComplexLobeType::Anisotropy: {
                AnisotropyLobe data;
                if (!reader.floating(data.strength) ||
                    !reader.floating(data.rotation) ||
                    !reader.integer(data.textureMask)) return false;
                record.data = data;
                break;
            }
            case ComplexLobeType::Iridescence: {
                IridescenceLobe data;
                if (!reader.floating(data.factor) ||
                    !reader.floating(data.ior) ||
                    !reader.floating(data.thicknessMinimumNm) ||
                    !reader.floating(data.thicknessMaximumNm) ||
                    !reader.integer(data.textureMask)) return false;
                record.data = data;
                break;
            }
            case ComplexLobeType::ThinTransmission: {
                ThinTransmissionLobe data;
                if (!reader.floating(data.factor) ||
                    !reader.floating(data.ior) ||
                    !reader.floating(data.specularFactor) ||
                    !readVec3(reader, data.specularColor) ||
                    !reader.integer(data.textureMask)) return false;
                record.data = data;
                break;
            }
            case ComplexLobeType::VolumeTransmission: {
                VolumeTransmissionLobe data;
                if (!reader.floating(data.thicknessFactor) ||
                    !reader.floating(data.attenuationDistance) ||
                    !readVec3(reader, data.attenuationColor) ||
                    !reader.integer(data.textureMask)) return false;
                record.data = data;
                break;
            }
            case ComplexLobeType::Dispersion: {
                DispersionLobe data;
                if (!reader.floating(data.dispersion)) return false;
                record.data = data;
                break;
            }
            case ComplexLobeType::DiffuseTransmission: {
                DiffuseTransmissionLobe data;
                if (!reader.floating(data.factor) ||
                    !readVec3(reader, data.color) ||
                    !reader.integer(data.textureMask)) return false;
                record.data = data;
                break;
            }
            }
            return true;
        }

        void writeMaterialPayload(Writer& writer,
            const CompiledMaterial& material) {
            writer.integer(material.schemaVersion);
            writer.integer(material.sourceMaterialIndex);
            writer.string(material.sourceName);
            writer.integer(static_cast<uint8_t>(material.workflow));
            writer.integer(static_cast<uint8_t>(material.closureClass));
            writer.integer(material.featureFlags);

            const StandardClosureRecipe& recipe = material.standard;
            writeVec4(writer, recipe.baseColorFactor);
            writer.floating(recipe.metallicFactor);
            writer.floating(recipe.roughnessFactor);
            writer.floating(recipe.ior);
            writer.floating(recipe.specularFactor);
            writeVec3(writer, recipe.specularColorFactor);
            writeVec4(writer, recipe.diffuseFactor);
            writeVec3(writer, recipe.specularGlossinessFactor);
            writer.floating(recipe.glossinessFactor);
            writeVec3(writer, recipe.emissiveFactor);
            writer.floating(recipe.emissiveStrength);
            writer.floating(recipe.normalScale);
            writer.floating(recipe.occlusionStrength);
            writer.integer(static_cast<uint8_t>(recipe.alphaMode));
            writer.floating(recipe.alphaCutoff);
            writer.integer<uint8_t>(recipe.doubleSided ? 1 : 0);
            writer.integer(recipe.textureMask);

            if (material.textureOperations.size() >
                    std::numeric_limits<uint32_t>::max() ||
                material.complexLobes.size() >
                    std::numeric_limits<uint32_t>::max()) {
                throw std::invalid_argument(
                    "Compiled material record count exceeds schema limits.");
            }
            writer.integer(static_cast<uint32_t>(
                material.textureOperations.size()));
            for (const auto& texture : material.textureOperations) {
                writeTexture(writer, texture);
            }
            writer.integer(static_cast<uint32_t>(
                material.complexLobes.size()));
            for (const auto& lobe : material.complexLobes) {
                writeLobe(writer, lobe);
            }
            writer.string(material.contentHash);
        }

        bool readMaterialPayload(Reader& reader,
            CompiledMaterial& material) {
            uint8_t workflow = 0;
            uint8_t closure = 0;
            uint8_t alphaMode = 0;
            uint8_t doubleSided = 0;
            if (!reader.integer(material.schemaVersion) ||
                material.schemaVersion != CompiledMaterial::SchemaVersion ||
                !reader.integer(material.sourceMaterialIndex) ||
                !reader.string(material.sourceName) ||
                !reader.integer(workflow) ||
                workflow > static_cast<uint8_t>(MaterialWorkflow::Unlit) ||
                !reader.integer(closure) ||
                closure > static_cast<uint8_t>(
                    MaterialClosureClass::Invalid) ||
                !reader.integer(material.featureFlags) ||
                !readVec4(reader, material.standard.baseColorFactor) ||
                !reader.floating(material.standard.metallicFactor) ||
                !reader.floating(material.standard.roughnessFactor) ||
                !reader.floating(material.standard.ior) ||
                !reader.floating(material.standard.specularFactor) ||
                !readVec3(reader,
                    material.standard.specularColorFactor) ||
                !readVec4(reader, material.standard.diffuseFactor) ||
                !readVec3(reader,
                    material.standard.specularGlossinessFactor) ||
                !reader.floating(
                    material.standard.glossinessFactor) ||
                !readVec3(reader, material.standard.emissiveFactor) ||
                !reader.floating(
                    material.standard.emissiveStrength) ||
                !reader.floating(material.standard.normalScale) ||
                !reader.floating(material.standard.occlusionStrength) ||
                !reader.integer(alphaMode) ||
                alphaMode > static_cast<uint8_t>(SourceAlphaMode::Blend) ||
                !reader.floating(material.standard.alphaCutoff) ||
                !reader.integer(doubleSided) || doubleSided > 1 ||
                !reader.integer(material.standard.textureMask)) {
                return false;
            }
            material.workflow = static_cast<MaterialWorkflow>(workflow);
            material.closureClass =
                static_cast<MaterialClosureClass>(closure);
            material.standard.alphaMode =
                static_cast<SourceAlphaMode>(alphaMode);
            material.standard.doubleSided = doubleSided != 0;

            uint32_t textureCount = 0;
            if (!reader.integer(textureCount) ||
                textureCount > kMaximumRecords) return false;
            material.textureOperations.resize(textureCount);
            for (auto& texture : material.textureOperations) {
                if (!readTexture(reader, texture)) return false;
            }
            uint32_t lobeCount = 0;
            if (!reader.integer(lobeCount) ||
                lobeCount > kMaximumRecords) return false;
            material.complexLobes.resize(lobeCount);
            for (auto& lobe : material.complexLobes) {
                if (!readLobe(reader, lobe)) return false;
            }
            return reader.string(material.contentHash);
        }

        void addError(std::vector<CookDiagnostic>& diagnostics,
            std::string code, std::string field, std::string message) {
            diagnostics.push_back({
                .severity = CookDiagnosticSeverity::Error,
                .code = std::move(code),
                .field = std::move(field),
                .message = std::move(message),
            });
        }

        bool canonicalHash(std::string_view hash) {
            return hash.size() == 64 &&
                std::ranges::all_of(hash, [](unsigned char value) {
                    return std::isdigit(value) ||
                        (value >= 'a' && value <= 'f');
                });
        }

    } // namespace

    std::vector<std::byte> serializeCompiledMaterial(
        const CompiledMaterial& material) {
        if (material.schemaVersion != CompiledMaterial::SchemaVersion ||
            !canonicalHash(material.contentHash) ||
            material.contentHash !=
                calculateCompiledMaterialHash(material)) {
            throw std::invalid_argument(
                "Compiled material has an invalid schema or canonical hash.");
        }

        Writer payload;
        writeMaterialPayload(payload, material);
        const std::string payloadHash = sha256(payload.bytes);

        Writer output;
        output.bytes.insert(output.bytes.end(),
            kMagic.begin(), kMagic.end());
        output.integer(kContainerVersion);
        output.integer(static_cast<uint64_t>(payload.bytes.size()));
        output.bytes.insert(output.bytes.end(),
            reinterpret_cast<const std::byte*>(payloadHash.data()),
            reinterpret_cast<const std::byte*>(
                payloadHash.data() + payloadHash.size()));
        output.bytes.insert(output.bytes.end(),
            payload.bytes.begin(), payload.bytes.end());
        return output.bytes;
    }

    CompiledMaterialReadResult readCompiledMaterial(
        std::span<const std::byte> bytes) {
        CompiledMaterialReadResult result;
        if (bytes.size() < kHeaderSize ||
            !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
            addError(result.diagnostics, "MATERIAL_PRODUCT_HEADER", "/",
                "Compiled material product header is invalid.");
            return result;
        }

        Reader header(bytes.subspan(kMagic.size()));
        uint32_t containerVersion = 0;
        uint64_t payloadSize = 0;
        if (!header.integer(containerVersion) ||
            !header.integer(payloadSize) ||
            containerVersion != kContainerVersion ||
            payloadSize != bytes.size() - kHeaderSize) {
            addError(result.diagnostics, "MATERIAL_PRODUCT_SCHEMA", "/",
                "Compiled material product schema or payload size is invalid.");
            return result;
        }
        const std::string_view expectedHash(
            reinterpret_cast<const char*>(
                bytes.data() + kMagic.size() + sizeof(uint32_t) +
                sizeof(uint64_t)),
            kChecksumSize);
        const std::span<const std::byte> payload =
            bytes.subspan(kHeaderSize);
        if (!canonicalHash(expectedHash) ||
            sha256(payload) != expectedHash) {
            addError(result.diagnostics, "MATERIAL_PRODUCT_CHECKSUM", "/",
                "Compiled material product payload checksum does not match.");
            return result;
        }

        Reader reader(payload);
        CompiledMaterial material;
        if (!readMaterialPayload(reader, material) ||
            reader.remaining() != 0) {
            addError(result.diagnostics, "MATERIAL_PRODUCT_PAYLOAD", "/",
                "Compiled material payload is malformed or has trailing data.");
            return result;
        }
        if (!canonicalHash(material.contentHash) ||
            calculateCompiledMaterialHash(material) !=
                material.contentHash) {
            addError(result.diagnostics, "MATERIAL_PRODUCT_CONTENT_HASH",
                "/content_hash",
                "Compiled material closure hash does not match its payload.");
            return result;
        }
        result.material = std::move(material);
        return result;
    }

} // namespace Iridium
