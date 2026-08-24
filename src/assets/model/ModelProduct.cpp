#include "assets/model/ModelProduct.h"

#include "utils/Sha256.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace Iridium {

    namespace {

        constexpr std::array<std::byte, 8> kManifestMagic{
            std::byte{ 'I' }, std::byte{ 'R' }, std::byte{ 'M' }, std::byte{ 'O' },
            std::byte{ 'D' }, std::byte{ 'E' }, std::byte{ 'L' }, std::byte{ '1' },
        };
        constexpr std::array<std::byte, 8> kMaterialMagic{
            std::byte{ 'I' }, std::byte{ 'R' }, std::byte{ 'M' }, std::byte{ 'M' },
            std::byte{ 'A' }, std::byte{ 'T' }, std::byte{ '0' }, std::byte{ '1' },
        };
        constexpr std::array<std::byte, 8> kTextureViewMagic{
            std::byte{ 'I' }, std::byte{ 'R' }, std::byte{ 'M' }, std::byte{ 'T' },
            std::byte{ 'E' }, std::byte{ 'X' }, std::byte{ '0' }, std::byte{ '1' },
        };
        constexpr uint32_t kManifestHeaderSize = 72;
        constexpr uint32_t kPrimitiveRecordSize = 208;
        constexpr uint32_t kMaterialSectionSchemaVersion = 3;
        constexpr uint32_t kTextureViewSectionSchemaVersion = 1;

        template <typename Integer>
        void appendInteger(std::vector<std::byte>& output, Integer value) {
            static_assert(std::is_integral_v<Integer>);
            using Unsigned = std::make_unsigned_t<Integer>;
            Unsigned bits = static_cast<Unsigned>(value);
            for (size_t index = 0; index < sizeof(Integer); ++index) {
                output.push_back(static_cast<std::byte>(
                    (bits >> (index * 8)) & static_cast<Unsigned>(0xff)));
            }
        }

        template <typename Integer>
        void writeInteger(std::vector<std::byte>& output, size_t offset,
            Integer value) {
            static_assert(std::is_integral_v<Integer>);
            if (offset > output.size() ||
                sizeof(Integer) > output.size() - offset) {
                throw std::out_of_range("Model product write exceeds output bounds.");
            }
            using Unsigned = std::make_unsigned_t<Integer>;
            Unsigned bits = static_cast<Unsigned>(value);
            for (size_t index = 0; index < sizeof(Integer); ++index) {
                output[offset + index] = static_cast<std::byte>(
                    (bits >> (index * 8)) & static_cast<Unsigned>(0xff));
            }
        }

        template <typename Integer>
        bool readInteger(std::span<const std::byte> input, size_t offset,
            Integer& value) {
            static_assert(std::is_integral_v<Integer>);
            if (offset > input.size() ||
                sizeof(Integer) > input.size() - offset) {
                return false;
            }
            using Unsigned = std::make_unsigned_t<Integer>;
            Unsigned bits = 0;
            for (size_t index = 0; index < sizeof(Integer); ++index) {
                bits |= static_cast<Unsigned>(
                    std::to_integer<uint8_t>(input[offset + index]))
                    << (index * 8);
            }
            value = static_cast<Integer>(bits);
            return true;
        }

        void appendFloat(std::vector<std::byte>& output, float value) {
            appendInteger<uint32_t>(output, std::bit_cast<uint32_t>(value));
        }

        void writeFloat(std::vector<std::byte>& output, size_t offset, float value) {
            writeInteger<uint32_t>(output, offset, std::bit_cast<uint32_t>(value));
        }

        bool readFloat(std::span<const std::byte> input, size_t offset,
            float& value) {
            uint32_t bits = 0;
            if (!readInteger(input, offset, bits)) return false;
            value = std::bit_cast<float>(bits);
            return true;
        }

        void writeGuid(std::vector<std::byte>& output, size_t offset,
            const AssetGuid& guid) {
            if (offset > output.size() ||
                guid.bytes().size() > output.size() - offset) {
                throw std::out_of_range("Model product GUID exceeds output bounds.");
            }
            std::memcpy(output.data() + offset, guid.bytes().data(),
                guid.bytes().size());
        }

        AssetGuid readGuid(std::span<const std::byte> input, size_t offset) {
            AssetGuid::Bytes bytes{};
            if (offset <= input.size() && bytes.size() <= input.size() - offset) {
                std::memcpy(bytes.data(), input.data() + offset, bytes.size());
            }
            return AssetGuid(bytes);
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

        bool validTopology(uint8_t value) {
            return value <= static_cast<uint8_t>(ModelPrimitiveTopology::Points);
        }

        bool validTransparencyPolicy(
            const CompiledTransparencyPolicy& policy) {
            return isAuthoredTransparencyClass(policy.requestedClass) &&
                policy.resolvedClass >= TransparencyClass::None &&
                policy.resolvedClass <= TransparencyClass::WeightedOit &&
                isTransparencyQuality(policy.quality) &&
                (policy.flags & 0xf0u) == 0 &&
                std::isfinite(policy.thinSheetThicknessMeters) &&
                policy.thinSheetThicknessMeters >= 0.0f &&
                policy.thinSheetThicknessMeters <= 1.0e6f;
        }

        bool validEnumValues(const CookedModelPrimitive& primitive) {
            return static_cast<uint8_t>(primitive.topology) <=
                    static_cast<uint8_t>(ModelPrimitiveTopology::Points) &&
                static_cast<uint8_t>(primitive.winding) <=
                    static_cast<uint8_t>(ModelWinding::Clockwise) &&
                static_cast<uint8_t>(primitive.coverage) <=
                    static_cast<uint8_t>(ModelCoverage::Transparent) &&
                static_cast<uint8_t>(primitive.indexFormat) <=
                    static_cast<uint8_t>(ModelIndexFormat::UInt32) &&
                validTransparencyPolicy(primitive.transparency);
        }

        bool validRange(uint64_t first, uint64_t count, uint64_t size) {
            return first <= size && count <= size - first;
        }

        bool finiteBounds(const CookedModelBounds& bounds) {
            return std::ranges::all_of(bounds.aabbMin,
                       [](float value) { return std::isfinite(value); }) &&
                std::ranges::all_of(bounds.aabbMax,
                    [](float value) { return std::isfinite(value); }) &&
                std::ranges::all_of(bounds.sphereCenter,
                    [](float value) { return std::isfinite(value); }) &&
                std::isfinite(bounds.sphereRadius);
        }

        bool sphereContainsAabb(const CookedModelBounds& bounds) {
            float maximumSquaredDistance = 0.0f;
            for (uint32_t corner = 0; corner < 8; ++corner) {
                float squaredDistance = 0.0f;
                for (uint32_t axis = 0; axis < 3; ++axis) {
                    const float value = (corner & (1u << axis)) != 0
                        ? bounds.aabbMax[axis] : bounds.aabbMin[axis];
                    const float delta = value - bounds.sphereCenter[axis];
                    squaredDistance += delta * delta;
                }
                maximumSquaredDistance =
                    std::max(maximumSquaredDistance, squaredDistance);
            }
            const float radiusSquared =
                bounds.sphereRadius * bounds.sphereRadius;
            const float tolerance =
                std::max(1.0f, maximumSquaredDistance) * 1.0e-5f;
            return radiusSquared + tolerance >= maximumSquaredDistance;
        }

        void appendVertex(std::vector<std::byte>& output,
            const CookedModelVertex& vertex) {
            for (float value : vertex.position) appendFloat(output, value);
            for (float value : vertex.color) appendFloat(output, value);
            for (float value : vertex.normal) appendFloat(output, value);
            for (float value : vertex.texCoord0) appendFloat(output, value);
            for (float value : vertex.tangent) appendFloat(output, value);
            for (float value : vertex.texCoord1) appendFloat(output, value);
        }

    } // namespace

    bool CookedModelMaterial::operator==(
        const CookedModelMaterial& other) const {
        return materialGuid == other.materialGuid &&
            sourceKey == other.sourceKey &&
            textureBindings == other.textureBindings &&
            serializeCompiledMaterial(compiled) ==
                serializeCompiledMaterial(other.compiled);
    }

    std::vector<std::byte> serializeModelManifest(
        const CookedModelManifest& manifest) {
        if (manifest.primitives.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::invalid_argument(
                "Cooked model primitive count exceeds schema limits.");
        }

        const uint64_t recordBytes =
            static_cast<uint64_t>(manifest.primitives.size()) *
            kPrimitiveRecordSize;
        const uint64_t stringTableOffset =
            static_cast<uint64_t>(kManifestHeaderSize) + recordBytes;
        if (stringTableOffset > std::numeric_limits<size_t>::max()) {
            throw std::invalid_argument(
                "Cooked model manifest exceeds addressable memory.");
        }

        std::vector<std::byte> output(
            static_cast<size_t>(stringTableOffset), std::byte{ 0 });
        std::copy(kManifestMagic.begin(), kManifestMagic.end(), output.begin());
        writeInteger<uint32_t>(output, 8, manifest.schemaVersion);
        writeInteger<uint32_t>(output, 12, kManifestHeaderSize);
        writeInteger<uint32_t>(output, 16, kPrimitiveRecordSize);
        writeInteger<uint32_t>(output, 20,
            static_cast<uint32_t>(manifest.primitives.size()));
        writeInteger<uint64_t>(output, 24, manifest.vertexCount);
        writeInteger<uint64_t>(output, 32, manifest.indexCount);
        writeInteger<uint64_t>(output, 40, manifest.rtPositionCount);
        writeInteger<uint64_t>(output, 48, manifest.rtIndexCount);
        writeInteger<uint32_t>(output, 56, manifest.vertexStride);
        writeInteger<uint32_t>(output, 60,
            static_cast<uint32_t>(
                manifest.transparencyExecutionMode));
        writeInteger<uint64_t>(output, 64, stringTableOffset);

        for (size_t index = 0; index < manifest.primitives.size(); ++index) {
            const CookedModelPrimitive& primitive = manifest.primitives[index];
            if (primitive.sourceKey.size() >
                std::numeric_limits<uint32_t>::max()) {
                throw std::invalid_argument(
                    "Cooked model primitive source key exceeds schema limits.");
            }
            const size_t record =
                kManifestHeaderSize + index * kPrimitiveRecordSize;
            writeGuid(output, record, primitive.primitiveGuid);
            writeGuid(output, record + 16, primitive.materialGuid);
            writeInteger<uint32_t>(output, record + 32, primitive.sourceNode);
            writeInteger<uint32_t>(output, record + 36, primitive.sourceMesh);
            writeInteger<uint32_t>(output, record + 40, primitive.sourcePrimitive);
            writeInteger<uint32_t>(output, record + 44, primitive.attributeMask);
            writeInteger<uint64_t>(output, record + 48, primitive.firstVertex);
            writeInteger<uint64_t>(output, record + 56, primitive.vertexCount);
            writeInteger<uint64_t>(output, record + 64, primitive.firstIndex);
            writeInteger<uint64_t>(output, record + 72, primitive.indexCount);
            writeInteger<uint64_t>(output, record + 80, primitive.rtFirstPosition);
            writeInteger<uint64_t>(output, record + 88, primitive.rtPositionCount);
            writeInteger<uint64_t>(output, record + 96, primitive.rtFirstIndex);
            writeInteger<uint64_t>(output, record + 104, primitive.rtIndexCount);
            writeInteger<uint8_t>(output, record + 112,
                static_cast<uint8_t>(primitive.topology));
            writeInteger<uint8_t>(output, record + 113,
                static_cast<uint8_t>(primitive.winding));
            writeInteger<uint8_t>(output, record + 114,
                static_cast<uint8_t>(primitive.coverage));
            writeInteger<uint8_t>(output, record + 115,
                static_cast<uint8_t>(primitive.indexFormat));
            writeInteger<uint32_t>(output, record + 116, primitive.flags);
            writeInteger<uint32_t>(output, record + 120, primitive.rtFlags);
            writeInteger<uint32_t>(output, record + 124, primitive.lodSection);
            writeInteger<uint32_t>(output, record + 128, primitive.meshletSection);
            const uint64_t sourceKeyOffset =
                output.size() - static_cast<size_t>(stringTableOffset);
            if (sourceKeyOffset > std::numeric_limits<uint32_t>::max()) {
                throw std::invalid_argument(
                    "Cooked model source-key table exceeds schema limits.");
            }
            writeInteger<uint32_t>(output, record + 132,
                static_cast<uint32_t>(sourceKeyOffset));
            writeInteger<uint32_t>(output, record + 136,
                static_cast<uint32_t>(primitive.sourceKey.size()));
            for (uint32_t axis = 0; axis < 3; ++axis) {
                writeFloat(output, record + 140 + axis * 4,
                    primitive.bounds.aabbMin[axis]);
                writeFloat(output, record + 152 + axis * 4,
                    primitive.bounds.aabbMax[axis]);
                writeFloat(output, record + 164 + axis * 4,
                    primitive.bounds.sphereCenter[axis]);
            }
            writeFloat(output, record + 176, primitive.bounds.sphereRadius);
            writeInteger<uint32_t>(output, record + 180,
                packTransparencyPolicyWord(
                    primitive.transparency));
            writeInteger<int32_t>(output, record + 184,
                primitive.transparency.priority);
            writeFloat(output, record + 188,
                primitive.transparency.thinSheetThicknessMeters);
            writeGuid(output, record + 192,
                primitive.sourcePrimitiveGuid);
            output.insert(output.end(),
                reinterpret_cast<const std::byte*>(primitive.sourceKey.data()),
                reinterpret_cast<const std::byte*>(
                    primitive.sourceKey.data() + primitive.sourceKey.size()));
        }
        return output;
    }

    std::optional<CookedModelManifest> readModelManifest(
        std::span<const std::byte> bytes,
        std::vector<CookDiagnostic>& diagnostics) {
        if (bytes.size() < kManifestHeaderSize ||
            !std::equal(kManifestMagic.begin(), kManifestMagic.end(),
                bytes.begin())) {
            addError(diagnostics, "MODEL_MANIFEST_HEADER", "/",
                "Cooked model manifest magic or header is invalid.");
            return std::nullopt;
        }

        uint32_t schema = 0;
        uint32_t headerSize = 0;
        uint32_t recordSize = 0;
        uint32_t primitiveCount = 0;
        uint32_t transparencyExecutionMode = 0;
        uint64_t stringTableOffset = 0;
        CookedModelManifest result;
        if (!readInteger(bytes, 8, schema) ||
            !readInteger(bytes, 12, headerSize) ||
            !readInteger(bytes, 16, recordSize) ||
            !readInteger(bytes, 20, primitiveCount) ||
            !readInteger(bytes, 24, result.vertexCount) ||
            !readInteger(bytes, 32, result.indexCount) ||
            !readInteger(bytes, 40, result.rtPositionCount) ||
            !readInteger(bytes, 48, result.rtIndexCount) ||
            !readInteger(bytes, 56, result.vertexStride) ||
            !readInteger(bytes, 60, transparencyExecutionMode) ||
            !readInteger(bytes, 64, stringTableOffset) ||
            schema != kCookedModelSchemaVersion ||
            headerSize != kManifestHeaderSize ||
            recordSize != kPrimitiveRecordSize ||
            transparencyExecutionMode > static_cast<uint32_t>(
                TransparencyExecutionMode::Classified)) {
            addError(diagnostics, "MODEL_MANIFEST_SCHEMA", "/schema",
                "Cooked model manifest schema or record layout is unsupported.");
            return std::nullopt;
        }
        result.schemaVersion = schema;
        result.transparencyExecutionMode =
            static_cast<TransparencyExecutionMode>(
                transparencyExecutionMode);

        const uint64_t expectedStringTableOffset =
            static_cast<uint64_t>(headerSize) +
            static_cast<uint64_t>(primitiveCount) * recordSize;
        if (stringTableOffset != expectedStringTableOffset ||
            stringTableOffset > bytes.size()) {
            addError(diagnostics, "MODEL_MANIFEST_TABLE_BOUNDS", "/primitives",
                "Cooked model primitive or source-key table is out of bounds.");
            return std::nullopt;
        }

        result.primitives.reserve(primitiveCount);
        for (uint32_t index = 0; index < primitiveCount; ++index) {
            const size_t record =
                kManifestHeaderSize + static_cast<size_t>(index) *
                kPrimitiveRecordSize;
            CookedModelPrimitive primitive;
            primitive.primitiveGuid = readGuid(bytes, record);
            primitive.materialGuid = readGuid(bytes, record + 16);
            primitive.sourcePrimitiveGuid = readGuid(bytes, record + 192);
            uint8_t topology = 0;
            uint8_t winding = 0;
            uint8_t coverage = 0;
            uint8_t indexFormat = 0;
            uint32_t sourceKeyOffset = 0;
            uint32_t sourceKeyLength = 0;
            uint32_t transparencyWord = 0;
            int32_t transparencyPriority = 0;
            float thinSheetThicknessMeters = 0.0f;
            bool readable =
                readInteger(bytes, record + 32, primitive.sourceNode) &&
                readInteger(bytes, record + 36, primitive.sourceMesh) &&
                readInteger(bytes, record + 40, primitive.sourcePrimitive) &&
                readInteger(bytes, record + 44, primitive.attributeMask) &&
                readInteger(bytes, record + 48, primitive.firstVertex) &&
                readInteger(bytes, record + 56, primitive.vertexCount) &&
                readInteger(bytes, record + 64, primitive.firstIndex) &&
                readInteger(bytes, record + 72, primitive.indexCount) &&
                readInteger(bytes, record + 80, primitive.rtFirstPosition) &&
                readInteger(bytes, record + 88, primitive.rtPositionCount) &&
                readInteger(bytes, record + 96, primitive.rtFirstIndex) &&
                readInteger(bytes, record + 104, primitive.rtIndexCount) &&
                readInteger(bytes, record + 112, topology) &&
                readInteger(bytes, record + 113, winding) &&
                readInteger(bytes, record + 114, coverage) &&
                readInteger(bytes, record + 115, indexFormat) &&
                readInteger(bytes, record + 116, primitive.flags) &&
                readInteger(bytes, record + 120, primitive.rtFlags) &&
                readInteger(bytes, record + 124, primitive.lodSection) &&
                readInteger(bytes, record + 128, primitive.meshletSection) &&
                readInteger(bytes, record + 132, sourceKeyOffset) &&
                readInteger(bytes, record + 136, sourceKeyLength) &&
                readInteger(bytes, record + 180,
                    transparencyWord) &&
                readInteger(bytes, record + 184,
                    transparencyPriority) &&
                readFloat(bytes, record + 188,
                    thinSheetThicknessMeters);
            for (uint32_t axis = 0; axis < 3 && readable; ++axis) {
                readable =
                    readFloat(bytes, record + 140 + axis * 4,
                        primitive.bounds.aabbMin[axis]) &&
                    readFloat(bytes, record + 152 + axis * 4,
                        primitive.bounds.aabbMax[axis]) &&
                    readFloat(bytes, record + 164 + axis * 4,
                        primitive.bounds.sphereCenter[axis]);
            }
            readable = readable &&
                readFloat(bytes, record + 176, primitive.bounds.sphereRadius);
            primitive.transparency = unpackTransparencyPolicyWord(
                transparencyWord, transparencyPriority,
                thinSheetThicknessMeters);
            const uint64_t keyStart = stringTableOffset + sourceKeyOffset;
            if (!readable || !validTopology(topology) ||
                winding > static_cast<uint8_t>(ModelWinding::Clockwise) ||
                coverage > static_cast<uint8_t>(ModelCoverage::Transparent) ||
                indexFormat > static_cast<uint8_t>(ModelIndexFormat::UInt32) ||
                !validTransparencyPolicy(primitive.transparency) ||
                keyStart > bytes.size() ||
                sourceKeyLength > bytes.size() - keyStart) {
                addError(diagnostics, "MODEL_PRIMITIVE_RECORD",
                    "/primitives/" + std::to_string(index),
                    "Cooked model primitive record is invalid.");
                return std::nullopt;
            }
            primitive.topology = static_cast<ModelPrimitiveTopology>(topology);
            primitive.winding = static_cast<ModelWinding>(winding);
            primitive.coverage = static_cast<ModelCoverage>(coverage);
            primitive.indexFormat = static_cast<ModelIndexFormat>(indexFormat);
            primitive.sourceKey.assign(
                reinterpret_cast<const char*>(bytes.data() + keyStart),
                sourceKeyLength);
            result.primitives.push_back(std::move(primitive));
        }
        return result;
    }

    std::vector<std::byte> serializeModelMaterials(
        std::span<const CookedModelMaterial> materials) {
        if (materials.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::invalid_argument(
                "Cooked model material count exceeds schema limits.");
        }
        std::vector<std::byte> output;
        output.insert(output.end(),
            kMaterialMagic.begin(), kMaterialMagic.end());
        appendInteger<uint32_t>(
            output, kMaterialSectionSchemaVersion);
        appendInteger<uint32_t>(
            output, static_cast<uint32_t>(materials.size()));
        for (const CookedModelMaterial& material : materials) {
            if (material.sourceKey.size() >
                    std::numeric_limits<uint32_t>::max() ||
                material.textureBindings.size() >
                    std::numeric_limits<uint32_t>::max()) {
                throw std::invalid_argument(
                    "Cooked model material record exceeds schema limits.");
            }
            const std::vector<std::byte> compiled =
                serializeCompiledMaterial(material.compiled);
            output.insert(output.end(),
                reinterpret_cast<const std::byte*>(
                    material.materialGuid.bytes().data()),
                reinterpret_cast<const std::byte*>(
                    material.materialGuid.bytes().data() +
                    material.materialGuid.bytes().size()));
            appendInteger<uint32_t>(output,
                static_cast<uint32_t>(material.sourceKey.size()));
            appendInteger<uint64_t>(output, compiled.size());
            appendInteger<uint32_t>(output,
                static_cast<uint32_t>(
                    material.textureBindings.size()));
            output.insert(output.end(),
                reinterpret_cast<const std::byte*>(
                    material.sourceKey.data()),
                reinterpret_cast<const std::byte*>(
                    material.sourceKey.data() +
                    material.sourceKey.size()));
            output.insert(output.end(),
                compiled.begin(), compiled.end());
            for (const CookedModelTextureBinding& binding :
                material.textureBindings) {
                appendInteger<uint32_t>(
                    output, binding.operationIndex);
                appendInteger<uint32_t>(
                    output, binding.sourceImageIndex);
                appendInteger<uint32_t>(
                    output, binding.textureViewIndex);
                output.insert(output.end(),
                    reinterpret_cast<const std::byte*>(
                        binding.textureGuid.bytes().data()),
                    reinterpret_cast<const std::byte*>(
                        binding.textureGuid.bytes().data() +
                        binding.textureGuid.bytes().size()));
            }
        }
        return output;
    }

    std::optional<std::vector<CookedModelMaterial>>
        readModelMaterials(
            std::span<const std::byte> bytes,
            std::vector<CookDiagnostic>& diagnostics) {
        constexpr size_t headerSize =
            kMaterialMagic.size() + sizeof(uint32_t) +
            sizeof(uint32_t);
        if (bytes.size() < headerSize ||
            !std::equal(kMaterialMagic.begin(),
                kMaterialMagic.end(), bytes.begin())) {
            addError(diagnostics, "MODEL_MATERIAL_HEADER",
                "/materials",
                "Cooked model material section header is invalid.");
            return std::nullopt;
        }
        uint32_t schema = 0;
        uint32_t materialCount = 0;
        if (!readInteger(bytes, 8, schema) ||
            !readInteger(bytes, 12, materialCount) ||
            schema != kMaterialSectionSchemaVersion) {
            addError(diagnostics, "MODEL_MATERIAL_SCHEMA",
                "/materials",
                "Cooked model material section schema is unsupported.");
            return std::nullopt;
        }

        size_t offset = headerSize;
        std::vector<CookedModelMaterial> materials;
        materials.reserve(materialCount);
        for (uint32_t materialIndex = 0;
            materialIndex < materialCount; ++materialIndex) {
            constexpr size_t fixedRecordSize =
                16 + sizeof(uint32_t) + sizeof(uint64_t) +
                sizeof(uint32_t);
            if (offset > bytes.size() ||
                fixedRecordSize > bytes.size() - offset) {
                addError(diagnostics, "MODEL_MATERIAL_RECORD",
                    "/materials/" +
                        std::to_string(materialIndex),
                    "Cooked model material record is truncated.");
                return std::nullopt;
            }
            CookedModelMaterial material;
            material.materialGuid = readGuid(bytes, offset);
            offset += 16;
            uint32_t sourceKeyLength = 0;
            uint64_t compiledLength = 0;
            uint32_t bindingCount = 0;
            if (!readInteger(bytes, offset, sourceKeyLength) ||
                !readInteger(bytes, offset + 4, compiledLength) ||
                !readInteger(bytes, offset + 12, bindingCount)) {
                addError(diagnostics, "MODEL_MATERIAL_RECORD",
                    "/materials/" +
                        std::to_string(materialIndex),
                    "Cooked model material record is unreadable.");
                return std::nullopt;
            }
            offset += sizeof(uint32_t) + sizeof(uint64_t) +
                sizeof(uint32_t);
            const uint64_t bindingBytes =
                static_cast<uint64_t>(bindingCount) * 28;
            const uint64_t variableBytes =
                static_cast<uint64_t>(sourceKeyLength) +
                compiledLength + bindingBytes;
            if (variableBytes > bytes.size() - offset ||
                compiledLength >
                    std::numeric_limits<size_t>::max()) {
                addError(diagnostics, "MODEL_MATERIAL_BOUNDS",
                    "/materials/" +
                        std::to_string(materialIndex),
                    "Cooked model material payload is out of bounds.");
                return std::nullopt;
            }
            material.sourceKey.assign(
                reinterpret_cast<const char*>(
                    bytes.data() + offset),
                sourceKeyLength);
            offset += sourceKeyLength;
            const CompiledMaterialReadResult compiled =
                readCompiledMaterial(bytes.subspan(offset,
                    static_cast<size_t>(compiledLength)));
            if (!compiled.valid()) {
                addError(diagnostics,
                    "MODEL_COMPILED_MATERIAL",
                    "/materials/" +
                        std::to_string(materialIndex) +
                        "/compiled",
                    "Embedded compiled M2 material is invalid.");
                return std::nullopt;
            }
            material.compiled = *compiled.material;
            offset += static_cast<size_t>(compiledLength);
            material.textureBindings.reserve(bindingCount);
            for (uint32_t bindingIndex = 0;
                bindingIndex < bindingCount; ++bindingIndex) {
                CookedModelTextureBinding binding;
                if (!readInteger(bytes, offset,
                        binding.operationIndex) ||
                    !readInteger(bytes, offset + 4,
                        binding.sourceImageIndex) ||
                    !readInteger(bytes, offset + 8,
                        binding.textureViewIndex)) {
                    addError(diagnostics,
                        "MODEL_TEXTURE_BINDING",
                        "/materials/" +
                            std::to_string(materialIndex) +
                            "/texture_bindings/" +
                            std::to_string(bindingIndex),
                        "Cooked texture binding is unreadable.");
                    return std::nullopt;
                }
                binding.textureGuid =
                    readGuid(bytes, offset + 12);
                offset += 28;
                material.textureBindings.push_back(binding);
            }
            materials.push_back(std::move(material));
        }
        if (offset != bytes.size()) {
            addError(diagnostics, "MODEL_MATERIAL_TRAILING_DATA",
                "/materials",
                "Cooked model material section has trailing data.");
            return std::nullopt;
        }
        return materials;
    }

    std::string calculateModelTextureViewKey(
        const CookedModelTextureView& view) {
        std::vector<std::byte> identity;
        identity.insert(identity.end(),
            reinterpret_cast<const std::byte*>(
                view.textureGuid.bytes().data()),
            reinterpret_cast<const std::byte*>(
                view.textureGuid.bytes().data() +
                view.textureGuid.bytes().size()));
        appendInteger<uint32_t>(
            identity, view.sourceImageIndex);
        const std::vector<std::byte> manifest =
            serializeTextureManifest(view.manifest);
        appendInteger<uint64_t>(identity, manifest.size());
        identity.insert(identity.end(),
            manifest.begin(), manifest.end());
        const std::string payloadHash =
            sha256(view.payload);
        identity.insert(identity.end(),
            reinterpret_cast<const std::byte*>(
                payloadHash.data()),
            reinterpret_cast<const std::byte*>(
                payloadHash.data() +
                payloadHash.size()));
        return sha256(identity);
    }

    std::vector<std::byte> serializeModelTextureViews(
        std::span<const CookedModelTextureView> views) {
        if (views.size() >
            std::numeric_limits<uint32_t>::max()) {
            throw std::invalid_argument(
                "Cooked texture-view count exceeds schema limits.");
        }
        std::vector<std::byte> output;
        output.insert(output.end(),
            kTextureViewMagic.begin(),
            kTextureViewMagic.end());
        appendInteger<uint32_t>(
            output, kTextureViewSectionSchemaVersion);
        appendInteger<uint32_t>(
            output, static_cast<uint32_t>(views.size()));
        for (const CookedModelTextureView& view : views) {
            const std::vector<std::byte> manifest =
                serializeTextureManifest(view.manifest);
            if (view.viewKey.size() != 64 ||
                manifest.size() >
                    std::numeric_limits<uint32_t>::max()) {
                throw std::invalid_argument(
                    "Cooked texture-view record exceeds schema limits.");
            }
            output.insert(output.end(),
                reinterpret_cast<const std::byte*>(
                    view.textureGuid.bytes().data()),
                reinterpret_cast<const std::byte*>(
                    view.textureGuid.bytes().data() +
                    view.textureGuid.bytes().size()));
            appendInteger<uint32_t>(
                output, view.sourceImageIndex);
            appendInteger<uint32_t>(
                output, static_cast<uint32_t>(
                    manifest.size()));
            appendInteger<uint64_t>(
                output, view.payload.size());
            output.insert(output.end(),
                reinterpret_cast<const std::byte*>(
                    view.viewKey.data()),
                reinterpret_cast<const std::byte*>(
                    view.viewKey.data() +
                    view.viewKey.size()));
            output.insert(output.end(),
                manifest.begin(), manifest.end());
            output.insert(output.end(),
                view.payload.begin(), view.payload.end());
        }
        return output;
    }

    std::optional<std::vector<CookedModelTextureView>>
        readModelTextureViews(
            std::span<const std::byte> bytes,
            std::vector<CookDiagnostic>& diagnostics) {
        constexpr size_t headerSize =
            kTextureViewMagic.size() + 8;
        if (bytes.size() < headerSize ||
            !std::equal(kTextureViewMagic.begin(),
                kTextureViewMagic.end(), bytes.begin())) {
            addError(diagnostics,
                "MODEL_TEXTURE_VIEW_HEADER",
                "/texture_views",
                "Cooked model texture-view section header is invalid.");
            return std::nullopt;
        }
        uint32_t schema = 0;
        uint32_t viewCount = 0;
        if (!readInteger(bytes, 8, schema) ||
            !readInteger(bytes, 12, viewCount) ||
            schema != kTextureViewSectionSchemaVersion) {
            addError(diagnostics,
                "MODEL_TEXTURE_VIEW_SCHEMA",
                "/texture_views",
                "Cooked model texture-view section schema is unsupported.");
            return std::nullopt;
        }
        size_t offset = headerSize;
        std::vector<CookedModelTextureView> result;
        result.reserve(viewCount);
        for (uint32_t viewIndex = 0;
            viewIndex < viewCount; ++viewIndex) {
            constexpr size_t fixedSize =
                16 + 4 + 4 + 8 + 64;
            if (offset > bytes.size() ||
                fixedSize > bytes.size() - offset) {
                addError(diagnostics,
                    "MODEL_TEXTURE_VIEW_RECORD",
                    "/texture_views/" +
                        std::to_string(viewIndex),
                    "Cooked texture-view record is truncated.");
                return std::nullopt;
            }
            CookedModelTextureView view;
            view.textureGuid = readGuid(bytes, offset);
            offset += 16;
            uint32_t manifestSize = 0;
            uint64_t payloadSize = 0;
            if (!readInteger(bytes, offset,
                    view.sourceImageIndex) ||
                !readInteger(bytes, offset + 4,
                    manifestSize) ||
                !readInteger(bytes, offset + 8,
                    payloadSize)) {
                addError(diagnostics,
                    "MODEL_TEXTURE_VIEW_RECORD",
                    "/texture_views/" +
                        std::to_string(viewIndex),
                    "Cooked texture-view record is unreadable.");
                return std::nullopt;
            }
            offset += 16;
            view.viewKey.assign(
                reinterpret_cast<const char*>(
                    bytes.data() + offset), 64);
            offset += 64;
            if (manifestSize > bytes.size() - offset) {
                addError(diagnostics,
                    "MODEL_TEXTURE_VIEW_BOUNDS",
                    "/texture_views/" +
                        std::to_string(viewIndex),
                    "Cooked texture-view manifest is out of bounds.");
                return std::nullopt;
            }
            std::vector<CookDiagnostic> textureDiagnostics;
            const auto manifest = readTextureManifest(
                bytes.subspan(offset, manifestSize),
                textureDiagnostics);
            if (!manifest ||
                hasCookErrors(textureDiagnostics)) {
                addError(diagnostics,
                    "MODEL_TEXTURE_VIEW_MANIFEST",
                    "/texture_views/" +
                        std::to_string(viewIndex),
                    "Embedded cooked texture manifest is invalid.");
                return std::nullopt;
            }
            view.manifest = *manifest;
            offset += manifestSize;
            if (payloadSize > bytes.size() - offset ||
                payloadSize >
                    std::numeric_limits<size_t>::max()) {
                addError(diagnostics,
                    "MODEL_TEXTURE_VIEW_BOUNDS",
                    "/texture_views/" +
                        std::to_string(viewIndex),
                    "Cooked texture-view payload is out of bounds.");
                return std::nullopt;
            }
            view.payload.assign(
                bytes.begin() + offset,
                bytes.begin() + offset +
                    static_cast<size_t>(payloadSize));
            offset += static_cast<size_t>(payloadSize);
            result.push_back(std::move(view));
        }
        if (offset != bytes.size()) {
            addError(diagnostics,
                "MODEL_TEXTURE_VIEW_TRAILING_DATA",
                "/texture_views",
                "Cooked texture-view section has trailing data.");
            return std::nullopt;
        }
        return result;
    }

    std::vector<std::byte> serializeModelVertices(
        std::span<const CookedModelVertex> vertices) {
        if (vertices.size() >
            std::numeric_limits<size_t>::max() / kCookedModelVertexStride) {
            throw std::invalid_argument(
                "Cooked model vertex stream exceeds addressable memory.");
        }
        std::vector<std::byte> output;
        output.reserve(vertices.size() * kCookedModelVertexStride);
        for (const CookedModelVertex& vertex : vertices) {
            appendVertex(output, vertex);
        }
        return output;
    }

    std::vector<std::byte> serializeModelIndices(
        std::span<const uint32_t> indices) {
        std::vector<std::byte> output;
        output.reserve(indices.size() * sizeof(uint32_t));
        for (uint32_t index : indices) appendInteger<uint32_t>(output, index);
        return output;
    }

    std::vector<std::byte> serializeModelRtPositions(
        std::span<const std::array<float, 3>> positions) {
        std::vector<std::byte> output;
        output.reserve(positions.size() * sizeof(float) * 3);
        for (const auto& position : positions) {
            for (float value : position) appendFloat(output, value);
        }
        return output;
    }

    std::optional<std::vector<CookedModelVertex>> readModelVertices(
        std::span<const std::byte> bytes,
        std::vector<CookDiagnostic>& diagnostics) {
        if (bytes.size() % kCookedModelVertexStride != 0) {
            addError(diagnostics, "MODEL_VERTEX_LAYOUT", "/vertices",
                "Cooked model vertex byte size is not a whole canonical vertex stream.");
            return std::nullopt;
        }
        std::vector<CookedModelVertex> result(
            bytes.size() / kCookedModelVertexStride);
        size_t offset = 0;
        for (CookedModelVertex& vertex : result) {
            const auto readArray = [&](auto& values) {
                for (float& value : values) {
                    if (!readFloat(bytes, offset, value)) return false;
                    offset += sizeof(float);
                }
                return true;
            };
            if (!readArray(vertex.position) || !readArray(vertex.color) ||
                !readArray(vertex.normal) || !readArray(vertex.texCoord0) ||
                !readArray(vertex.tangent) || !readArray(vertex.texCoord1)) {
                addError(diagnostics, "MODEL_VERTEX_BOUNDS", "/vertices",
                    "Cooked model vertex stream is truncated.");
                return std::nullopt;
            }
        }
        return result;
    }

    std::optional<std::vector<uint32_t>> readModelIndices(
        std::span<const std::byte> bytes,
        std::vector<CookDiagnostic>& diagnostics,
        std::string field) {
        if (bytes.size() % sizeof(uint32_t) != 0) {
            addError(diagnostics, "MODEL_INDEX_LAYOUT", std::move(field),
                "Cooked model index byte size is not a whole uint32 stream.");
            return std::nullopt;
        }
        std::vector<uint32_t> result(bytes.size() / sizeof(uint32_t));
        for (size_t index = 0; index < result.size(); ++index) {
            if (!readInteger(bytes, index * sizeof(uint32_t), result[index])) {
                addError(diagnostics, "MODEL_INDEX_BOUNDS", std::move(field),
                    "Cooked model index stream is truncated.");
                return std::nullopt;
            }
        }
        return result;
    }

    std::optional<std::vector<std::array<float, 3>>>
        readModelRtPositions(
            std::span<const std::byte> bytes,
            std::vector<CookDiagnostic>& diagnostics) {
        constexpr size_t stride = sizeof(float) * 3;
        if (bytes.size() % stride != 0) {
            addError(diagnostics, "MODEL_RT_POSITION_LAYOUT", "/rt_positions",
                "Cooked RT position byte size is not a whole float3 stream.");
            return std::nullopt;
        }
        std::vector<std::array<float, 3>> result(bytes.size() / stride);
        size_t offset = 0;
        for (auto& position : result) {
            for (float& value : position) {
                if (!readFloat(bytes, offset, value)) {
                    addError(diagnostics, "MODEL_RT_POSITION_BOUNDS",
                        "/rt_positions",
                        "Cooked RT position stream is truncated.");
                    return std::nullopt;
                }
                offset += sizeof(float);
            }
        }
        return result;
    }

    std::vector<CookDiagnostic> validateModelProduct(
        const CookedModelProductData& data) {
        std::vector<CookDiagnostic> diagnostics;
        const CookedModelManifest& manifest = data.manifest;
        if (manifest.schemaVersion != kCookedModelSchemaVersion ||
            manifest.vertexStride != kCookedModelVertexStride ||
            manifest.transparencyExecutionMode >
                TransparencyExecutionMode::Classified) {
            addError(diagnostics, "MODEL_SCHEMA", "/schema",
                "Cooked model schema or canonical vertex stride is unsupported.");
        }
        if (manifest.vertexCount != data.vertices.size() ||
            manifest.indexCount != data.indices.size() ||
            manifest.rtPositionCount != data.rtPositions.size() ||
            manifest.rtIndexCount != data.rtIndices.size()) {
            addError(diagnostics, "MODEL_STREAM_COUNTS", "/",
                "Cooked model manifest counts do not match stream payloads.");
        }
        if (manifest.primitives.empty()) {
            addError(diagnostics, "MODEL_PRIMITIVES_EMPTY", "/primitives",
                "Cooked models must retain at least one source primitive.");
            return diagnostics;
        }

        std::set<AssetGuid> materialGuids;
        std::map<AssetGuid, const CookedModelMaterial*> materialsByGuid;
        std::set<std::string> materialSourceKeys;
        std::set<std::string> textureViewKeys;
        for (size_t viewIndex = 0;
            viewIndex < data.textureViews.size(); ++viewIndex) {
            const CookedModelTextureView& view =
                data.textureViews[viewIndex];
            const std::string field =
                "/texture_views/" + std::to_string(viewIndex);
            if (view.textureGuid.isNil() ||
                view.viewKey.size() != 64 ||
                !textureViewKeys.insert(view.viewKey).second ||
                view.viewKey !=
                    calculateModelTextureViewKey(view)) {
                addError(diagnostics,
                    "MODEL_TEXTURE_VIEW_IDENTITY",
                    field,
                    "Texture view requires a non-nil asset GUID and unique canonical view key.");
            }
            std::vector<CookDiagnostic> textureDiagnostics =
                validateTextureProduct(
                    view.manifest, view.payload.size());
            if (hasCookErrors(textureDiagnostics)) {
                addError(diagnostics,
                    "MODEL_TEXTURE_VIEW_PRODUCT",
                    field,
                    "Embedded cooked texture product is invalid.");
            }
        }
        std::set<uint32_t> referencedTextureViews;
        if (data.materials.empty()) {
            addError(diagnostics, "MODEL_MATERIALS_EMPTY",
                "/materials",
                "Cooked models must publish every referenced compiled material.");
        }
        for (size_t materialIndex = 0;
            materialIndex < data.materials.size(); ++materialIndex) {
            const CookedModelMaterial& material =
                data.materials[materialIndex];
            const std::string field =
                "/materials/" + std::to_string(materialIndex);
            if (material.materialGuid.isNil() ||
                !materialGuids.insert(
                    material.materialGuid).second) {
                addError(diagnostics, "MODEL_MATERIAL_IDENTITY",
                    field + "/material_guid",
                    "Material GUID must be non-nil and unique.");
            } else {
                materialsByGuid.emplace(
                    material.materialGuid, &material);
            }
            if (material.sourceKey.empty() ||
                !materialSourceKeys.insert(
                    material.sourceKey).second) {
                addError(diagnostics, "MODEL_MATERIAL_SOURCE_KEY",
                    field + "/source_key",
                    "Material source key must be non-empty and unique.");
            }
            if (material.compiled.schemaVersion !=
                    CompiledMaterial::SchemaVersion ||
                material.compiled.closureClass ==
                    MaterialClosureClass::Invalid ||
                !validTransparencyPolicy(
                    material.compiled.transparency) ||
                material.compiled.contentHash !=
                    calculateCompiledMaterialHash(
                        material.compiled)) {
                addError(diagnostics,
                    "MODEL_COMPILED_MATERIAL_INVALID",
                    field + "/compiled",
                    "Material must contain a valid canonical M2 closure.");
            }
            std::set<uint32_t> boundOperations;
            for (size_t bindingIndex = 0;
                bindingIndex <
                    material.textureBindings.size();
                ++bindingIndex) {
                const CookedModelTextureBinding& binding =
                    material.textureBindings[bindingIndex];
                const std::string bindingField =
                    field + "/texture_bindings/" +
                    std::to_string(bindingIndex);
                if (binding.textureGuid.isNil() ||
                    binding.operationIndex >=
                        material.compiled.textureOperations.size() ||
                    !boundOperations.insert(
                        binding.operationIndex).second) {
                    addError(diagnostics,
                        "MODEL_TEXTURE_BINDING_IDENTITY",
                        bindingField,
                        "Texture operation binding must be unique, in range, and use a non-nil GUID.");
                    continue;
                }
                const CompiledTextureOperation& operation =
                    material.compiled.textureOperations[
                        binding.operationIndex];
                if (!operation.sourceImageIndex ||
                    *operation.sourceImageIndex !=
                        binding.sourceImageIndex) {
                    addError(diagnostics,
                        "MODEL_TEXTURE_BINDING_SOURCE",
                        bindingField,
                        "Texture binding must agree with the compiled operation's source image.");
                }
                if (binding.textureViewIndex >=
                    data.textureViews.size()) {
                    addError(diagnostics,
                        "MODEL_TEXTURE_VIEW_REFERENCE",
                        bindingField,
                        "Texture binding references an absent cooked texture view.");
                } else {
                    const CookedModelTextureView& view =
                        data.textureViews[
                            binding.textureViewIndex];
                    referencedTextureViews.insert(
                        binding.textureViewIndex);
                    if (view.textureGuid !=
                            binding.textureGuid ||
                        view.sourceImageIndex !=
                            binding.sourceImageIndex) {
                        addError(diagnostics,
                            "MODEL_TEXTURE_VIEW_REFERENCE",
                            bindingField,
                            "Texture binding GUID/source image does not match its cooked view.");
                    }
                }
            }
            size_t expectedBindings = 0;
            for (const CompiledTextureOperation& operation :
                material.compiled.textureOperations) {
                if (operation.sourceImageIndex) {
                    ++expectedBindings;
                }
            }
            if (material.textureBindings.size() !=
                expectedBindings) {
                addError(diagnostics,
                    "MODEL_TEXTURE_BINDINGS_INCOMPLETE",
                    field + "/texture_bindings",
                    "Every compiled image operation requires a stable texture GUID binding.");
            }
        }
        if (referencedTextureViews.size() !=
            data.textureViews.size()) {
            addError(diagnostics,
                "MODEL_TEXTURE_VIEW_UNUSED",
                "/texture_views",
                "Every embedded texture view must be referenced by a compiled material operation.");
        }

        std::set<AssetGuid> primitiveGuids;
        std::set<std::string> sourceKeys;
        constexpr uint32_t knownAttributes =
            ModelAttributePosition | ModelAttributeColor0 | ModelAttributeNormal |
            ModelAttributeTexCoord0 | ModelAttributeTangent |
            ModelAttributeTexCoord1;
        constexpr uint32_t knownFlags =
            ModelPrimitiveDoubleSided | ModelPrimitiveMirroredTransform |
            ModelPrimitiveGeneratedTangent;
        constexpr uint32_t knownRtFlags =
            ModelRtBuildInput | ModelRtOpaque | ModelRtAllowAnyHit;

        for (size_t index = 0; index < manifest.primitives.size(); ++index) {
            const CookedModelPrimitive& primitive = manifest.primitives[index];
            const std::string field = "/primitives/" + std::to_string(index);
            if (primitive.primitiveGuid.isNil() ||
                !primitiveGuids.insert(primitive.primitiveGuid).second) {
                addError(diagnostics, "MODEL_PRIMITIVE_GUID",
                    field + "/primitive_guid",
                    "Primitive GUID must be non-nil and unique.");
            }
            if (primitive.sourcePrimitiveGuid.isNil()) {
                addError(diagnostics, "MODEL_SOURCE_PRIMITIVE_GUID",
                    field + "/source_primitive_guid",
                    "Source primitive GUID must be non-nil.");
            }
            if (primitive.materialGuid.isNil()) {
                addError(diagnostics, "MODEL_MATERIAL_GUID",
                    field + "/material_guid",
                    "Primitive material GUID must be non-nil.");
            } else if (!materialGuids.contains(
                primitive.materialGuid)) {
                addError(diagnostics,
                    "MODEL_MATERIAL_REFERENCE",
                    field + "/material_guid",
                    "Primitive material GUID is absent from the compiled material section.");
            } else {
                const CookedModelMaterial& material =
                    *materialsByGuid.at(primitive.materialGuid);
                ModelCoverage expectedCoverage =
                    ModelCoverage::Opaque;
                switch (material.compiled.standard.alphaMode) {
                case SourceAlphaMode::Opaque:
                    expectedCoverage = ModelCoverage::Opaque;
                    break;
                case SourceAlphaMode::Mask:
                    expectedCoverage = ModelCoverage::Masked;
                    break;
                case SourceAlphaMode::Blend:
                    expectedCoverage = ModelCoverage::Transparent;
                    break;
                }
                const bool primitiveDoubleSided =
                    (primitive.flags &
                        ModelPrimitiveDoubleSided) != 0;
                if (primitive.coverage != expectedCoverage ||
                    primitiveDoubleSided !=
                        material.compiled.standard.doubleSided) {
                    addError(diagnostics,
                        "MODEL_MATERIAL_ROUTING",
                        field + "/material_guid",
                        "Primitive coverage and culling flags must match its compiled material closure.");
                }
            }
            if (primitive.sourceKey.empty() ||
                !sourceKeys.insert(primitive.sourceKey).second) {
                addError(diagnostics, "MODEL_SOURCE_KEY",
                    field + "/source_key",
                    "Primitive source key must be non-empty and unique.");
            }
            if (!validEnumValues(primitive) ||
                (primitive.attributeMask & ~knownAttributes) != 0 ||
                (primitive.flags & ~knownFlags) != 0 ||
                (primitive.rtFlags & ~knownRtFlags) != 0) {
                addError(diagnostics, "MODEL_PRIMITIVE_FLAGS", field,
                    "Primitive enum, attribute, or feature flags are invalid.");
            }
            if ((primitive.attributeMask & ModelAttributePosition) == 0) {
                addError(diagnostics, "MODEL_POSITION_REQUIRED",
                    field + "/attribute_mask",
                    "Every cooked primitive requires a position stream.");
            }
            if (primitive.vertexCount == 0 || primitive.indexCount == 0 ||
                !validRange(primitive.firstVertex, primitive.vertexCount,
                    manifest.vertexCount) ||
                !validRange(primitive.firstIndex, primitive.indexCount,
                    manifest.indexCount)) {
                addError(diagnostics, "MODEL_PRIMITIVE_RANGE", field,
                    "Primitive vertex or index range is empty or out of bounds.");
            }
            if (primitive.topology == ModelPrimitiveTopology::Triangles &&
                primitive.indexCount % 3 != 0) {
                addError(diagnostics, "MODEL_TRIANGLE_INDEX_COUNT",
                    field + "/index_count",
                    "Triangle primitive index count must be divisible by three.");
            }
            if (!finiteBounds(primitive.bounds) ||
                primitive.bounds.sphereRadius < 0.0f ||
                primitive.bounds.aabbMin[0] > primitive.bounds.aabbMax[0] ||
                primitive.bounds.aabbMin[1] > primitive.bounds.aabbMax[1] ||
                primitive.bounds.aabbMin[2] > primitive.bounds.aabbMax[2] ||
                !sphereContainsAabb(primitive.bounds)) {
                addError(diagnostics, "MODEL_PRIMITIVE_BOUNDS",
                    field + "/bounds",
                    "Primitive AABB and conservative sphere are invalid.");
            }

            const bool rtBuildInput =
                (primitive.rtFlags & ModelRtBuildInput) != 0;
            if (rtBuildInput) {
                if (primitive.topology != ModelPrimitiveTopology::Triangles ||
                    primitive.rtPositionCount == 0 ||
                    primitive.rtIndexCount != primitive.indexCount ||
                    !validRange(primitive.rtFirstPosition,
                        primitive.rtPositionCount, manifest.rtPositionCount) ||
                    !validRange(primitive.rtFirstIndex,
                        primitive.rtIndexCount, manifest.rtIndexCount)) {
                    addError(diagnostics, "MODEL_RT_RANGE", field + "/rt",
                        "RT build input must preserve a valid triangle position/index range.");
                }
            } else if (primitive.rtPositionCount != 0 ||
                primitive.rtIndexCount != 0) {
                addError(diagnostics, "MODEL_RT_FLAGS", field + "/rt_flags",
                    "RT ranges require the RT build-input flag.");
            }
            const bool opaqueRt = (primitive.rtFlags & ModelRtOpaque) != 0;
            const bool anyHitRt = (primitive.rtFlags & ModelRtAllowAnyHit) != 0;
            if ((opaqueRt && primitive.coverage != ModelCoverage::Opaque) ||
                (anyHitRt && primitive.coverage == ModelCoverage::Opaque) ||
                (opaqueRt && anyHitRt)) {
                addError(diagnostics, "MODEL_RT_COVERAGE", field + "/rt_flags",
                    "RT geometry flags must agree with primitive coverage.");
            }

            if (validRange(primitive.firstIndex, primitive.indexCount,
                    data.indices.size())) {
                for (uint64_t item = primitive.firstIndex;
                    item < primitive.firstIndex + primitive.indexCount; ++item) {
                    const uint64_t value =
                        data.indices[static_cast<size_t>(item)];
                    if (value < primitive.firstVertex ||
                        value >= primitive.firstVertex +
                            primitive.vertexCount) {
                        addError(diagnostics, "MODEL_INDEX_VALUE",
                            field + "/indices",
                            "Raster index does not address this primitive's vertex range.");
                        break;
                    }
                }
            }
            if (rtBuildInput &&
                validRange(primitive.rtFirstIndex, primitive.rtIndexCount,
                    data.rtIndices.size())) {
                for (uint64_t item = primitive.rtFirstIndex;
                    item < primitive.rtFirstIndex + primitive.rtIndexCount; ++item) {
                    if (data.rtIndices[static_cast<size_t>(item)] >=
                        primitive.rtPositionCount) {
                        addError(diagnostics, "MODEL_RT_INDEX_VALUE",
                            field + "/rt_indices",
                            "Primitive-local RT index exceeds its position range.");
                        break;
                    }
                }
            }
        }
        return diagnostics;
    }

    CookProduct makeCookedModelProduct(const CookedModelProductData& data) {
        CookProduct product{
            .artifactType = "iridium.model",
            .artifactSchemaVersion = kCookedModelSchemaVersion,
            .diagnostics = validateModelProduct(data),
        };
        if (hasCookErrors(product.diagnostics)) return product;
        product.sections = {
            {
                .id = kCookedModelManifestSection,
                .schemaVersion = kCookedModelSchemaVersion,
                .alignment = 8,
                .bytes = serializeModelManifest(data.manifest),
            },
            {
                .id = kCookedModelMaterialSection,
                .schemaVersion =
                    kMaterialSectionSchemaVersion,
                .alignment = 8,
                .bytes = serializeModelMaterials(data.materials),
            },
            {
                .id = kCookedModelTextureViewSection,
                .schemaVersion =
                    kTextureViewSectionSchemaVersion,
                .alignment = 16,
                .bytes = serializeModelTextureViews(
                    data.textureViews),
            },
            {
                .id = kCookedModelVertexSection,
                .schemaVersion = 1,
                .alignment = 16,
                .bytes = serializeModelVertices(data.vertices),
            },
            {
                .id = kCookedModelIndexSection,
                .schemaVersion = 1,
                .alignment = 4,
                .bytes = serializeModelIndices(data.indices),
            },
            {
                .id = kCookedModelRtPositionSection,
                .schemaVersion = 1,
                .alignment = 16,
                .bytes = serializeModelRtPositions(data.rtPositions),
            },
            {
                .id = kCookedModelRtIndexSection,
                .schemaVersion = 1,
                .alignment = 4,
                .bytes = serializeModelIndices(data.rtIndices),
            },
        };
        return product;
    }

    CookedModelReadResult readCookedModelProduct(
        const CookedArtifact& artifact) {
        CookedModelReadResult result;
        if (artifact.artifactType != "iridium.model" ||
            artifact.artifactSchemaVersion != kCookedModelSchemaVersion) {
            addError(result.diagnostics, "MODEL_ARTIFACT_TYPE", "/",
                "Cooked artifact is not a supported model product.");
            return result;
        }
        const auto section = [&artifact](uint32_t id) -> const CookSection* {
            const auto found = std::ranges::find_if(artifact.sections,
                [id](const CookSection& candidate) {
                    return candidate.id == id;
                });
            return found == artifact.sections.end() ? nullptr : &*found;
        };
        const CookSection* manifestSection =
            section(kCookedModelManifestSection);
        const CookSection* vertexSection =
            section(kCookedModelVertexSection);
        const CookSection* materialSection =
            section(kCookedModelMaterialSection);
        const CookSection* textureViewSection =
            section(kCookedModelTextureViewSection);
        const CookSection* indexSection =
            section(kCookedModelIndexSection);
        const CookSection* rtPositionSection =
            section(kCookedModelRtPositionSection);
        const CookSection* rtIndexSection =
            section(kCookedModelRtIndexSection);
        if (!manifestSection || !materialSection ||
            !textureViewSection ||
            !vertexSection || !indexSection ||
            !rtPositionSection || !rtIndexSection) {
            addError(result.diagnostics, "MODEL_ARTIFACT_SECTIONS", "/",
                "Cooked model artifact is missing a required typed section.");
            return result;
        }
        if (manifestSection->schemaVersion != kCookedModelSchemaVersion ||
            materialSection->schemaVersion !=
                kMaterialSectionSchemaVersion ||
            textureViewSection->schemaVersion !=
                kTextureViewSectionSchemaVersion ||
            vertexSection->schemaVersion != 1 ||
            indexSection->schemaVersion != 1 ||
            rtPositionSection->schemaVersion != 1 ||
            rtIndexSection->schemaVersion != 1) {
            addError(result.diagnostics, "MODEL_SECTION_SCHEMA", "/",
                "Cooked model section schema is unsupported.");
            return result;
        }

        auto manifest =
            readModelManifest(manifestSection->bytes, result.diagnostics);
        auto materials =
            readModelMaterials(materialSection->bytes,
                result.diagnostics);
        auto textureViews =
            readModelTextureViews(textureViewSection->bytes,
                result.diagnostics);
        auto vertices =
            readModelVertices(vertexSection->bytes, result.diagnostics);
        auto indices =
            readModelIndices(indexSection->bytes, result.diagnostics);
        auto rtPositions =
            readModelRtPositions(rtPositionSection->bytes, result.diagnostics);
        auto rtIndices = readModelIndices(
            rtIndexSection->bytes, result.diagnostics, "/rt_indices");
        if (!manifest || !materials || !textureViews ||
            !vertices || !indices || !rtPositions ||
            !rtIndices || hasCookErrors(result.diagnostics)) {
            return result;
        }
        CookedModelProductData data{
            .manifest = std::move(*manifest),
            .materials = std::move(*materials),
            .textureViews = std::move(*textureViews),
            .vertices = std::move(*vertices),
            .indices = std::move(*indices),
            .rtPositions = std::move(*rtPositions),
            .rtIndices = std::move(*rtIndices),
        };
        std::vector<CookDiagnostic> validation =
            validateModelProduct(data);
        result.diagnostics.insert(result.diagnostics.end(),
            validation.begin(), validation.end());
        if (!hasCookErrors(result.diagnostics)) {
            result.data = std::move(data);
        }
        return result;
    }

} // namespace Iridium
