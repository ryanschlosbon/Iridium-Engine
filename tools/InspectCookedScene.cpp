#include "assets/cooker/CookedArtifact.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

    using Json = nlohmann::ordered_json;

    constexpr uint32_t sectionId(char a, char b, char c, char d) {
        return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
            (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
            (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
            (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
    }

    std::string sectionName(uint32_t value) {
        std::string result(4, '\0');
        for (size_t index = 0; index < result.size(); ++index) {
            result[index] = static_cast<char>(value >> (index * 8));
        }
        return result;
    }

    template <typename Integer>
    Integer integer(std::span<const std::byte> bytes, size_t offset) {
        if (offset + sizeof(Integer) > bytes.size()) {
            throw std::runtime_error("Cooked scene field is out of range");
        }
        uint64_t result = 0;
        for (size_t index = 0; index < sizeof(Integer); ++index) {
            result |= static_cast<uint64_t>(std::to_integer<uint8_t>(
                bytes[offset + index])) << (index * 8);
        }
        return static_cast<Integer>(result);
    }

    std::string hashText(std::span<const std::byte> bytes) {
        constexpr char hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(bytes.size() * 2);
        for (std::byte value : bytes) {
            const uint8_t byte = std::to_integer<uint8_t>(value);
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 0x0f]);
        }
        return result;
    }

    std::vector<std::byte> readFile(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) throw std::runtime_error("Could not open cooked scene artifact");
        const std::streamsize size = input.tellg();
        if (size < 0) throw std::runtime_error("Could not size cooked scene artifact");
        input.seekg(0, std::ios::beg);
        std::vector<std::byte> bytes(static_cast<size_t>(size));
        if (size != 0 && !input.read(reinterpret_cast<char*>(bytes.data()), size)) {
            throw std::runtime_error("Could not read cooked scene artifact");
        }
        return bytes;
    }

    const Iridium::CookSection& requiredSection(
        const Iridium::CookedArtifact& artifact, uint32_t id) {
        const auto found = std::ranges::find_if(artifact.sections,
            [id](const Iridium::CookSection& section) {
                return section.id == id;
            });
        if (found == artifact.sections.end()) {
            throw std::runtime_error("Cooked scene required section is missing");
        }
        return *found;
    }

    std::vector<std::string> strings(const Iridium::CookSection& section) {
        const std::span<const std::byte> bytes(section.bytes);
        size_t offset = 0;
        const uint32_t count = integer<uint32_t>(bytes, offset);
        offset += 4;
        std::vector<std::string> result;
        result.reserve(count);
        for (uint32_t index = 0; index < count; ++index) {
            const uint32_t length = integer<uint32_t>(bytes, offset);
            offset += 4;
            if (length > bytes.size() - (std::min)(offset, bytes.size())) {
                throw std::runtime_error("Cooked scene string table is truncated");
            }
            result.emplace_back(reinterpret_cast<const char*>(bytes.data() + offset),
                length);
            offset += length;
        }
        if (offset != bytes.size()) {
            throw std::runtime_error("Cooked scene string table has trailing bytes");
        }
        return result;
    }

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: IridiumInspectCookedScene <artifact>\n";
        return 1;
    }
    try {
        const std::vector<std::byte> bytes = readFile(argv[1]);
        const Iridium::CookedArtifactReadResult read =
            Iridium::readCookedArtifact(bytes);
        if (!read.valid()) {
            throw std::runtime_error("Cooked artifact container validation failed");
        }
        const Iridium::CookedArtifact& artifact = *read.artifact;
        if (artifact.artifactType != "iridium.scene.runtime" ||
            artifact.artifactSchemaVersion != 1) {
            throw std::runtime_error("Artifact is not a runtime scene schema 1 product");
        }
        const Iridium::CookSection& scene = requiredSection(artifact,
            sectionId('S', 'C', 'N', '1'));
        const Iridium::CookSection& stringSection = requiredSection(artifact,
            sectionId('S', 'T', 'R', '1'));
        (void)requiredSection(artifact, sectionId('E', 'N', 'T', '1'));
        const std::span<const std::byte> header(scene.bytes);
        if (scene.schemaVersion != 1 || scene.alignment != 8 ||
            header.size() < 112 || integer<uint32_t>(header, 0) !=
                sectionId('S', 'C', 'N', '1') ||
            integer<uint32_t>(header, 4) != 1 ||
            integer<uint32_t>(header, 8) != 0x01020304u ||
            integer<uint32_t>(header, 12) != 112) {
            throw std::runtime_error("SCN1 header contract is invalid");
        }
        Iridium::AssetGuid::Bytes sceneGuidBytes{};
        for (size_t index = 0; index < sceneGuidBytes.size(); ++index) {
            sceneGuidBytes[index] = std::to_integer<uint8_t>(header[16 + index]);
        }
        const Iridium::AssetGuid sceneGuid(sceneGuidBytes);
        const uint32_t entityCount = integer<uint32_t>(header, 40);
        const uint32_t typeCount = integer<uint32_t>(header, 44);
        const uint32_t stringCount = integer<uint32_t>(header, 48);
        const uint32_t dependencyCount = integer<uint32_t>(header, 52);
        const uint64_t directoryOffset = integer<uint64_t>(header, 56);
        const uint64_t directorySize = integer<uint64_t>(header, 64);
        const std::vector<std::string> stringValues = strings(stringSection);
        if (sceneGuid != artifact.assetGuid || stringValues.size() != stringCount ||
            directoryOffset != 112 || directorySize !=
                static_cast<uint64_t>(typeCount) * 16 ||
            directoryOffset + directorySize != header.size() ||
            dependencyCount != artifact.dependencies.size()) {
            throw std::runtime_error("SCN1 counts or directory range are invalid");
        }

        Json types = Json::array();
        for (uint32_t index = 0; index < typeCount; ++index) {
            const size_t offset = static_cast<size_t>(directoryOffset) + index * 16;
            const uint32_t stringIndex = integer<uint32_t>(header, offset);
            if (stringIndex >= stringValues.size()) {
                throw std::runtime_error("SCN1 type string index is invalid");
            }
            types.push_back({
                { "id", stringValues[stringIndex] },
                { "section", sectionName(integer<uint32_t>(header, offset + 4)) },
                { "version", integer<uint32_t>(header, offset + 8) },
                { "records", integer<uint32_t>(header, offset + 12) },
            });
        }
        Json sections = Json::array();
        for (const Iridium::CookSection& section : artifact.sections) {
            sections.push_back({ { "id", sectionName(section.id) },
                { "schema", section.schemaVersion },
                { "alignment", section.alignment },
                { "bytes", section.bytes.size() } });
        }
        Json dependencies = Json::array();
        for (const Iridium::AssetDependency& dependency : artifact.dependencies) {
            dependencies.push_back({
                { "type", static_cast<uint32_t>(dependency.type) },
                { "assetGuid", dependency.assetGuid
                    ? Json(dependency.assetGuid->toString()) : Json(nullptr) },
                { "artifactHash", dependency.artifactHash },
            });
        }
        Json output{
            { "status", "ok" },
            { "artifactHash", read.artifactHash },
            { "cookKey", artifact.cookKey },
            { "sceneAssetGuid", sceneGuid.toString() },
            { "platform", artifact.target.platform },
            { "profile", artifact.target.profile },
            { "decodedBytes", integer<uint64_t>(header, 32) },
            { "entityCount", entityCount },
            { "typeCount", typeCount },
            { "stringCount", stringCount },
            { "dependencyCount", dependencyCount },
            { "registryManifestHash", hashText(header.subspan(72, 32)) },
            { "types", std::move(types) },
            { "sections", std::move(sections) },
            { "dependencies", std::move(dependencies) },
        };
        std::cout << output.dump(2) << '\n';
        return 0;
    }
    catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 2;
    }
}
