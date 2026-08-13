#include "assets/cooker/CookedArtifact.h"

#include "utils/Sha256.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

namespace Iridium {

    namespace {

        constexpr std::array<std::byte, 8> kMagic{
            std::byte{ 'I' }, std::byte{ 'R' }, std::byte{ 'A' }, std::byte{ 'R' },
            std::byte{ 'T' }, std::byte{ '0' }, std::byte{ '1' }, std::byte{ 0 },
        };
        constexpr size_t kSectionRecordSize = 72;

        template <typename Integer>
        void appendInteger(std::vector<std::byte>& output, Integer value) {
            for (size_t index = 0; index < sizeof(Integer); ++index) {
                output.push_back(static_cast<std::byte>(
                    static_cast<uint64_t>(value) >> (index * 8)));
            }
        }

        template <typename Integer>
        void writeInteger(std::vector<std::byte>& output, size_t offset, Integer value) {
            if (offset + sizeof(Integer) > output.size()) {
                throw std::logic_error("Artifact integer write exceeded buffer.");
            }
            for (size_t index = 0; index < sizeof(Integer); ++index) {
                output[offset + index] = static_cast<std::byte>(
                    static_cast<uint64_t>(value) >> (index * 8));
            }
        }

        template <typename Integer>
        bool readInteger(std::span<const std::byte> bytes, size_t offset,
            Integer& value) {
            if (offset + sizeof(Integer) > bytes.size()) return false;
            uint64_t result = 0;
            for (size_t index = 0; index < sizeof(Integer); ++index) {
                result |= static_cast<uint64_t>(
                    std::to_integer<uint8_t>(bytes[offset + index])) << (index * 8);
            }
            value = static_cast<Integer>(result);
            return true;
        }

        void appendFixedString(std::vector<std::byte>& output,
            std::string_view value, size_t capacity) {
            if (value.size() >= capacity) {
                throw std::invalid_argument("Artifact fixed string exceeds its capacity.");
            }
            for (const char character : value) {
                output.push_back(static_cast<std::byte>(
                    static_cast<unsigned char>(character)));
            }
            output.resize(output.size() + capacity - value.size(), std::byte{ 0 });
        }

        std::string readFixedString(std::span<const std::byte> bytes,
            size_t offset, size_t capacity) {
            if (offset + capacity > bytes.size()) return {};
            size_t length = 0;
            while (length < capacity &&
                bytes[offset + length] != std::byte{ 0 }) {
                ++length;
            }
            if (length == capacity) return {};
            return std::string(reinterpret_cast<const char*>(bytes.data() + offset),
                length);
        }

        int hexValue(char character) {
            if (character >= '0' && character <= '9') return character - '0';
            if (character >= 'a' && character <= 'f') return character - 'a' + 10;
            if (character >= 'A' && character <= 'F') return character - 'A' + 10;
            return -1;
        }

        std::array<std::byte, 32> hashBytes(std::string_view hash,
            bool allowEmpty = false) {
            std::array<std::byte, 32> result{};
            if (hash.empty() && allowEmpty) return result;
            if (hash.size() != 64) {
                throw std::invalid_argument("SHA-256 text must contain 64 hex characters.");
            }
            for (size_t index = 0; index < result.size(); ++index) {
                const int high = hexValue(hash[index * 2]);
                const int low = hexValue(hash[index * 2 + 1]);
                if (high < 0 || low < 0) {
                    throw std::invalid_argument("SHA-256 text contains a non-hex character.");
                }
                result[index] = static_cast<std::byte>((high << 4) | low);
            }
            return result;
        }

        std::string hashText(std::span<const std::byte> bytes) {
            constexpr char hex[] = "0123456789abcdef";
            std::string result;
            result.resize(bytes.size() * 2);
            for (size_t index = 0; index < bytes.size(); ++index) {
                const uint8_t value = std::to_integer<uint8_t>(bytes[index]);
                result[index * 2] = hex[value >> 4];
                result[index * 2 + 1] = hex[value & 0x0f];
            }
            return result;
        }

        void appendHash(std::vector<std::byte>& output,
            std::string_view hash, bool allowEmpty = false) {
            const auto bytes = hashBytes(hash, allowEmpty);
            output.insert(output.end(), bytes.begin(), bytes.end());
        }

        uint64_t alignUp(uint64_t value, uint32_t alignment) {
            if (alignment == 0 || !std::has_single_bit(alignment)) {
                throw std::invalid_argument("Artifact section alignment must be a power of two.");
            }
            const uint64_t mask = alignment - 1;
            if (value > std::numeric_limits<uint64_t>::max() - mask) {
                throw std::overflow_error("Artifact alignment overflow.");
            }
            return (value + mask) & ~mask;
        }

        void addReadError(CookedArtifactReadResult& result,
            std::string code, std::string message) {
            result.diagnostics.push_back({
                .code = std::move(code),
                .message = std::move(message),
            });
        }

        std::vector<std::byte> dependencyTable(
            std::vector<AssetDependency> dependencies) {
            std::sort(dependencies.begin(), dependencies.end());
            std::vector<std::byte> output;
            for (const AssetDependency& dependency : dependencies) {
                if (dependency.location.size() > UINT32_MAX) {
                    throw std::invalid_argument(
                        "Artifact dependency location exceeds schema limits.");
                }
                appendInteger<uint8_t>(output, static_cast<uint8_t>(dependency.type));
                appendInteger<uint8_t>(output, dependency.assetGuid ? 1 : 0);
                const uint8_t hashFlags =
                    (dependency.contentHash.empty() ? 0u : 1u) |
                    (dependency.artifactHash.empty() ? 0u : 2u);
                appendInteger<uint8_t>(output, hashFlags);
                appendInteger<uint8_t>(output, 0);
                if (dependency.assetGuid) {
                    output.insert(output.end(),
                        reinterpret_cast<const std::byte*>(
                            dependency.assetGuid->bytes().data()),
                        reinterpret_cast<const std::byte*>(
                            dependency.assetGuid->bytes().data() +
                            dependency.assetGuid->bytes().size()));
                } else {
                    output.resize(output.size() + 16, std::byte{ 0 });
                }
                appendInteger<uint32_t>(output,
                    static_cast<uint32_t>(dependency.location.size()));
                appendHash(output, dependency.contentHash, true);
                appendHash(output, dependency.artifactHash, true);
                for (const char character : dependency.location) {
                    output.push_back(static_cast<std::byte>(
                        static_cast<unsigned char>(character)));
                }
            }
            return output;
        }

    } // namespace

    CookedArtifactBlob readCookedArtifactBlobFile(
        const std::filesystem::path& path) {
        std::ifstream input(
            path, std::ios::binary |
                std::ios::ate);
        if (!input) {
            throw std::runtime_error(
                "Could not open cooked artifact: " +
                    path.string());
        }
        const std::streamsize byteCount =
            input.tellg();
        if (byteCount < 0) {
            throw std::runtime_error(
                "Could not size cooked artifact: " +
                    path.string());
        }
        input.seekg(0, std::ios::beg);
        std::vector<std::byte> bytes(
            static_cast<size_t>(byteCount));
        if (byteCount != 0 &&
            !input.read(
                reinterpret_cast<char*>(
                    bytes.data()),
                byteCount)) {
            throw std::runtime_error(
                "Could not read cooked artifact: " +
                    path.string());
        }
        const std::string artifactHash =
            sha256(bytes);
        return {
            .bytes = std::move(bytes),
            .artifactHash = artifactHash,
        };
    }

    CookedArtifactBlob serializeCookedArtifact(const CookedArtifact& input) {
        if (input.cookKey.size() != 64) {
            throw std::invalid_argument("Artifact cook key must be SHA-256 text.");
        }
        if (input.target.artifactContainerVersion !=
            kCookedArtifactContainerVersion) {
            throw std::invalid_argument("Unsupported cooked artifact container version.");
        }
        if (input.dependencies.size() > UINT32_MAX ||
            input.sections.size() > UINT32_MAX) {
            throw std::invalid_argument("Artifact table count exceeds schema limits.");
        }

        CookedArtifact artifact = input;
        std::sort(artifact.dependencies.begin(), artifact.dependencies.end());
        std::sort(artifact.sections.begin(), artifact.sections.end(),
            [](const CookSection& lhs, const CookSection& rhs) {
                return lhs.id < rhs.id;
            });
        for (size_t index = 1; index < artifact.sections.size(); ++index) {
            if (artifact.sections[index - 1].id == artifact.sections[index].id) {
                throw std::invalid_argument("Artifact section IDs must be unique.");
            }
        }

        const std::vector<std::byte> dependencies =
            dependencyTable(artifact.dependencies);
        const uint64_t dependencyOffset = kCookedArtifactHeaderSize;
        const uint64_t sectionTableOffset = dependencyOffset + dependencies.size();
        const uint64_t sectionTableSize =
            artifact.sections.size() * kSectionRecordSize;
        const uint64_t dataOffset = alignUp(sectionTableOffset + sectionTableSize, 8);

        std::vector<std::byte> bytes(kCookedArtifactHeaderSize, std::byte{ 0 });
        bytes.insert(bytes.end(), dependencies.begin(), dependencies.end());
        bytes.resize(static_cast<size_t>(sectionTableOffset + sectionTableSize),
            std::byte{ 0 });
        bytes.resize(static_cast<size_t>(dataOffset), std::byte{ 0 });

        size_t recordOffset = static_cast<size_t>(sectionTableOffset);
        for (const CookSection& section : artifact.sections) {
            const uint64_t offset = alignUp(bytes.size(), section.alignment);
            bytes.resize(static_cast<size_t>(offset), std::byte{ 0 });
            bytes.insert(bytes.end(), section.bytes.begin(), section.bytes.end());
            const std::string checksum = sha256(section.bytes);

            writeInteger<uint32_t>(bytes, recordOffset, section.id);
            writeInteger<uint32_t>(bytes, recordOffset + 4, section.schemaVersion);
            writeInteger<uint64_t>(bytes, recordOffset + 8, offset);
            writeInteger<uint64_t>(bytes, recordOffset + 16, section.bytes.size());
            writeInteger<uint64_t>(bytes, recordOffset + 24, section.bytes.size());
            writeInteger<uint32_t>(bytes, recordOffset + 32, section.alignment);
            writeInteger<uint32_t>(bytes, recordOffset + 36, 0);
            const auto checksumBytes = hashBytes(checksum);
            std::copy(checksumBytes.begin(), checksumBytes.end(),
                bytes.begin() + static_cast<std::ptrdiff_t>(recordOffset + 40));
            recordOffset += kSectionRecordSize;
        }

        const std::string payloadHash = sha256(
            std::span<const std::byte>(bytes).subspan(kCookedArtifactHeaderSize));
        std::vector<std::byte> header;
        header.insert(header.end(), kMagic.begin(), kMagic.end());
        appendInteger<uint32_t>(header, kCookedArtifactContainerVersion);
        appendInteger<uint32_t>(header, kCookedArtifactHeaderSize);
        appendInteger<uint32_t>(header, artifact.artifactSchemaVersion);
        appendInteger<uint32_t>(header,
            static_cast<uint32_t>(artifact.dependencies.size()));
        appendInteger<uint32_t>(header,
            static_cast<uint32_t>(artifact.sections.size()));
        appendInteger<uint32_t>(header, 0);
        header.insert(header.end(),
            reinterpret_cast<const std::byte*>(artifact.assetGuid.bytes().data()),
            reinterpret_cast<const std::byte*>(
                artifact.assetGuid.bytes().data() + artifact.assetGuid.bytes().size()));
        appendFixedString(header, artifact.artifactType, 32);
        appendFixedString(header, artifact.target.platform, 16);
        appendFixedString(header, artifact.target.profile, 16);
        appendHash(header, artifact.cookKey);
        appendHash(header, payloadHash);
        appendInteger<uint64_t>(header, dependencyOffset);
        appendInteger<uint64_t>(header, sectionTableOffset);
        appendInteger<uint64_t>(header, dataOffset);
        appendInteger<uint64_t>(header, bytes.size());
        appendHash(header, sha256(header));
        if (header.size() != kCookedArtifactHeaderSize) {
            throw std::logic_error("Cooked artifact header size mismatch.");
        }
        std::copy(header.begin(), header.end(), bytes.begin());
        const std::string artifactHash = sha256(bytes);
        return {
            .bytes = std::move(bytes),
            .artifactHash = artifactHash,
        };
    }

    CookedArtifactHeaderProbe probeCookedArtifactHeader(
        std::span<const std::byte> headerBytes, uint64_t fileSize,
        std::optional<std::string_view> expectedCookKey) {
        CookedArtifactHeaderProbe result;
        if (headerBytes.size() < kCookedArtifactHeaderSize ||
            !std::equal(kMagic.begin(), kMagic.end(), headerBytes.begin())) {
            result.diagnostics.push_back({
                .code = "ARTIFACT_HEADER_MAGIC",
                .message = "Cooked artifact magic or header size is invalid.",
            });
            return result;
        }
        uint32_t containerVersion = 0;
        uint32_t headerSize = 0;
        uint64_t totalSize = 0;
        readInteger(headerBytes, 8, containerVersion);
        readInteger(headerBytes, 12, headerSize);
        readInteger(headerBytes, 200, totalSize);
        result.cookKey = hashText(headerBytes.subspan(112, 32));
        result.totalSize = totalSize;
        const std::string expectedHeaderHash =
            hashText(headerBytes.subspan(208, 32));
        const std::string actualHeaderHash = sha256(headerBytes.first(208));
        if (containerVersion != kCookedArtifactContainerVersion ||
            headerSize != kCookedArtifactHeaderSize || totalSize != fileSize ||
            expectedHeaderHash != actualHeaderHash) {
            result.diagnostics.push_back({
                .code = "ARTIFACT_HEADER_VERSION_SIZE",
                .message = "Cooked artifact header checksum, version, or size is invalid.",
            });
            return result;
        }
        if (expectedCookKey && result.cookKey != *expectedCookKey) {
            result.diagnostics.push_back({
                .code = "ARTIFACT_COOK_KEY_MISMATCH",
                .message = "Cooked artifact does not match the requested cook key.",
            });
            return result;
        }
        result.valid = true;
        return result;
    }

    CookedArtifactReadResult readCookedArtifact(
        std::span<const std::byte> bytes,
        std::optional<std::string_view> expectedArtifactHash) {
        CookedArtifactReadResult result;
        result.artifactHash = sha256(bytes);
        if (expectedArtifactHash && result.artifactHash != *expectedArtifactHash) {
            addReadError(result, "ARTIFACT_HASH_MISMATCH",
                "Cooked artifact whole-file hash does not match.");
            return result;
        }
        const CookedArtifactHeaderProbe probe =
            probeCookedArtifactHeader(bytes.first(
                std::min(bytes.size(), kCookedArtifactHeaderSize)), bytes.size());
        if (!probe.valid) {
            result.diagnostics = probe.diagnostics;
            return result;
        }

        uint32_t artifactSchema = 0;
        uint32_t dependencyCount = 0;
        uint32_t sectionCount = 0;
        uint64_t dependencyOffset = 0;
        uint64_t sectionTableOffset = 0;
        uint64_t dataOffset = 0;
        readInteger(bytes, 16, artifactSchema);
        readInteger(bytes, 20, dependencyCount);
        readInteger(bytes, 24, sectionCount);
        readInteger(bytes, 176, dependencyOffset);
        readInteger(bytes, 184, sectionTableOffset);
        readInteger(bytes, 192, dataOffset);
        const uint64_t sectionTableSize =
            static_cast<uint64_t>(sectionCount) * kSectionRecordSize;
        if (dependencyOffset != kCookedArtifactHeaderSize ||
            sectionTableOffset < dependencyOffset ||
            dataOffset < sectionTableOffset ||
            sectionTableSize > dataOffset - sectionTableOffset ||
            dataOffset > bytes.size()) {
            addReadError(result, "ARTIFACT_TABLE_BOUNDS",
                "Cooked artifact table offsets are invalid.");
            return result;
        }
        const std::string expectedPayloadHash = hashText(bytes.subspan(144, 32));
        const std::string actualPayloadHash = sha256(
            bytes.subspan(kCookedArtifactHeaderSize));
        if (expectedPayloadHash != actualPayloadHash) {
            addReadError(result, "ARTIFACT_PAYLOAD_HASH",
                "Cooked artifact payload hash does not match.");
            return result;
        }

        CookedArtifact artifact;
        AssetGuid::Bytes guidBytes{};
        std::memcpy(guidBytes.data(), bytes.data() + 32, guidBytes.size());
        artifact.assetGuid = AssetGuid(guidBytes);
        artifact.artifactType = readFixedString(bytes, 48, 32);
        artifact.artifactSchemaVersion = artifactSchema;
        artifact.target.platform = readFixedString(bytes, 80, 16);
        artifact.target.profile = readFixedString(bytes, 96, 16);
        artifact.target.artifactContainerVersion = kCookedArtifactContainerVersion;
        artifact.cookKey = probe.cookKey;
        if (artifact.assetGuid.isNil() || artifact.artifactType.empty() ||
            artifact.target.platform.empty() || artifact.target.profile.empty()) {
            addReadError(result, "ARTIFACT_HEADER_FIELDS",
                "Cooked artifact required header fields are invalid.");
            return result;
        }

        size_t cursor = static_cast<size_t>(dependencyOffset);
        for (uint32_t index = 0; index < dependencyCount; ++index) {
            if (cursor + 88 > sectionTableOffset) {
                addReadError(result, "ARTIFACT_DEPENDENCY_BOUNDS",
                    "Cooked artifact dependency record is truncated.");
                return result;
            }
            uint8_t type = 0;
            uint8_t hasGuid = 0;
            uint8_t hashFlags = 0;
            uint32_t locationLength = 0;
            readInteger(bytes, cursor, type);
            readInteger(bytes, cursor + 1, hasGuid);
            readInteger(bytes, cursor + 2, hashFlags);
            readInteger(bytes, cursor + 20, locationLength);
            if (type > static_cast<uint8_t>(AssetDependencyType::OptionalAsset) ||
                hasGuid > 1 || (hashFlags & ~3u) != 0 ||
                cursor + 88ull + locationLength > sectionTableOffset) {
                addReadError(result, "ARTIFACT_DEPENDENCY_INVALID",
                    "Cooked artifact dependency record is invalid.");
                return result;
            }
            AssetDependency dependency;
            dependency.type = static_cast<AssetDependencyType>(type);
            if (hasGuid != 0) {
                AssetGuid::Bytes dependencyGuid{};
                std::memcpy(dependencyGuid.data(), bytes.data() + cursor + 4,
                    dependencyGuid.size());
                dependency.assetGuid = AssetGuid(dependencyGuid);
            }
            if ((hashFlags & 1u) != 0) {
                dependency.contentHash = hashText(bytes.subspan(cursor + 24, 32));
            }
            if ((hashFlags & 2u) != 0) {
                dependency.artifactHash = hashText(bytes.subspan(cursor + 56, 32));
            }
            dependency.location.assign(
                reinterpret_cast<const char*>(bytes.data() + cursor + 88),
                locationLength);
            artifact.dependencies.push_back(std::move(dependency));
            cursor += 88 + locationLength;
        }
        if (cursor != sectionTableOffset) {
            addReadError(result, "ARTIFACT_DEPENDENCY_TABLE_SIZE",
                "Cooked artifact dependency table has trailing bytes.");
            return result;
        }

        std::set<uint32_t> sectionIds;
        cursor = static_cast<size_t>(sectionTableOffset);
        for (uint32_t index = 0; index < sectionCount; ++index) {
            uint32_t id = 0;
            uint32_t schema = 0;
            uint64_t offset = 0;
            uint64_t storedSize = 0;
            uint64_t decodedSize = 0;
            uint32_t alignment = 0;
            readInteger(bytes, cursor, id);
            readInteger(bytes, cursor + 4, schema);
            readInteger(bytes, cursor + 8, offset);
            readInteger(bytes, cursor + 16, storedSize);
            readInteger(bytes, cursor + 24, decodedSize);
            readInteger(bytes, cursor + 32, alignment);
            if (!sectionIds.insert(id).second || alignment == 0 ||
                !std::has_single_bit(alignment) || offset % alignment != 0 ||
                offset < dataOffset || storedSize != decodedSize ||
                offset > bytes.size() || storedSize > bytes.size() - offset) {
                addReadError(result, "ARTIFACT_SECTION_RECORD",
                    "Cooked artifact section record is invalid.");
                return result;
            }
            const auto sectionBytes = bytes.subspan(
                static_cast<size_t>(offset), static_cast<size_t>(storedSize));
            const std::string expectedChecksum =
                hashText(bytes.subspan(cursor + 40, 32));
            if (sha256(sectionBytes) != expectedChecksum) {
                addReadError(result, "ARTIFACT_SECTION_CHECKSUM",
                    "Cooked artifact section checksum does not match.");
                return result;
            }
            artifact.sections.push_back({
                .id = id,
                .schemaVersion = schema,
                .alignment = alignment,
                .bytes = std::vector<std::byte>(
                    sectionBytes.begin(), sectionBytes.end()),
            });
            cursor += kSectionRecordSize;
        }
        result.artifact = std::move(artifact);
        return result;
    }

} // namespace Iridium
