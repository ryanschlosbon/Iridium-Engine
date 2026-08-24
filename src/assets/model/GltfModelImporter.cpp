#include "assets/model/GltfModelImporter.h"

#include "assets/AssetSourceValidation.h"
#include "assets/cooker/CookKey.h"
#include "assets/cooker/LocalDerivedDataCache.h"
#include "assets/model/ModelProduct.h"
#include "assets/texture/TextureImporter.h"
#include "assets/texture/TextureProduct.h"
#include "material/MaterialCompiler.h"
#include "material/MaterialTangentGeneration.h"
#include "material/SourceMaterial.h"
#include "utils/Sha256.h"

#include <fastgltf/core.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Iridium {

    namespace {

        using Json = nlohmann::ordered_json;

        constexpr uint32_t kParsedDocumentSchema = 2;
        constexpr uint32_t kGlbMagic = 0x46546c67;
        constexpr uint32_t kGlbJsonChunk = 0x4e4f534a;

        struct WorkingVertex {
            glm::vec3 pos{ 0.0f };
            glm::vec4 color{ 1.0f };
            glm::vec3 normal{ 0.0f, 1.0f, 0.0f };
            glm::vec2 uv0{ 0.0f };
            glm::vec4 tangent{ 1.0f, 0.0f, 0.0f, 1.0f };
            glm::vec2 uv1{ 0.0f };
        };

        bool usableDirection(
            const glm::vec3& value) {
            const float lengthSquared =
                glm::dot(value, value);
            return std::isfinite(lengthSquared) &&
                lengthSquared >
                    MaterialNormalEpsilon;
        }

        void repairPrimitiveNormals(
            std::span<WorkingVertex> vertices,
            std::span<const uint32_t> indices,
            bool replaceAll) {
            std::vector<glm::vec3> accumulated(
                vertices.size(), glm::vec3(0.0f));
            for (size_t triangle = 0;
                triangle + 2 < indices.size();
                triangle += 3) {
                const uint32_t a =
                    indices[triangle];
                const uint32_t b =
                    indices[triangle + 1];
                const uint32_t c =
                    indices[triangle + 2];
                const glm::vec3 face =
                    glm::cross(
                        vertices[b].pos -
                            vertices[a].pos,
                        vertices[c].pos -
                            vertices[a].pos);
                if (!usableDirection(face)) {
                    continue;
                }
                accumulated[a] += face;
                accumulated[b] += face;
                accumulated[c] += face;
            }
            for (size_t index = 0;
                index < vertices.size();
                ++index) {
                if (!replaceAll &&
                    usableDirection(
                        vertices[index].normal)) {
                    continue;
                }
                vertices[index].normal =
                    usableDirection(accumulated[index])
                    ? glm::normalize(
                        accumulated[index])
                    : glm::vec3(
                        0.0f, 1.0f, 0.0f);
            }
        }

        void addDiagnostic(std::vector<CookDiagnostic>& diagnostics,
            CookDiagnosticSeverity severity, std::string code,
            std::string field, std::string message) {
            diagnostics.push_back({
                .severity = severity,
                .code = std::move(code),
                .field = std::move(field),
                .message = std::move(message),
            });
        }

        void addError(std::vector<CookDiagnostic>& diagnostics,
            std::string code, std::string field, std::string message) {
            addDiagnostic(diagnostics, CookDiagnosticSeverity::Error,
                std::move(code), std::move(field), std::move(message));
        }

        uint32_t readLe32(std::span<const std::byte> bytes, size_t offset) {
            if (offset > bytes.size() || 4 > bytes.size() - offset) {
                throw std::runtime_error("GLB header is truncated.");
            }
            return static_cast<uint32_t>(
                std::to_integer<uint8_t>(bytes[offset])) |
                (static_cast<uint32_t>(
                    std::to_integer<uint8_t>(bytes[offset + 1])) << 8) |
                (static_cast<uint32_t>(
                    std::to_integer<uint8_t>(bytes[offset + 2])) << 16) |
                (static_cast<uint32_t>(
                    std::to_integer<uint8_t>(bytes[offset + 3])) << 24);
        }

        std::string hexEncode(std::span<const std::byte> bytes) {
            constexpr std::string_view digits = "0123456789abcdef";
            std::string result;
            result.resize(bytes.size() * 2);
            for (size_t index = 0; index < bytes.size(); ++index) {
                const uint8_t value =
                    std::to_integer<uint8_t>(bytes[index]);
                result[index * 2] = digits[value >> 4];
                result[index * 2 + 1] = digits[value & 0x0f];
            }
            return result;
        }

        std::vector<std::byte> hexDecode(std::string_view text) {
            if (text.size() % 2 != 0) {
                throw std::runtime_error(
                    "Hex-encoded compiled material has odd length.");
            }
            const auto nibble = [](char value) -> uint8_t {
                if (value >= '0' && value <= '9') {
                    return static_cast<uint8_t>(value - '0');
                }
                if (value >= 'a' && value <= 'f') {
                    return static_cast<uint8_t>(
                        value - 'a' + 10);
                }
                throw std::runtime_error(
                    "Hex-encoded compiled material is invalid.");
            };
            std::vector<std::byte> result(text.size() / 2);
            for (size_t index = 0; index < result.size(); ++index) {
                result[index] = static_cast<std::byte>(
                    (nibble(text[index * 2]) << 4) |
                    nibble(text[index * 2 + 1]));
            }
            return result;
        }

        AssetGuid connectedPrimitiveGuid(const AssetGuid& sourcePrimitiveGuid,
            uint32_t sourceTriangleSeed) {
            std::array<std::byte, 20> identity{};
            std::memcpy(identity.data(), sourcePrimitiveGuid.bytes().data(), 16);
            for (uint32_t byte = 0; byte < 4; ++byte) {
                identity[16 + byte] = static_cast<std::byte>(
                    (sourceTriangleSeed >> (byte * 8)) & 0xffu);
            }
            const std::vector<std::byte> digest = hexDecode(sha256(identity));
            AssetGuid::Bytes bytes = sourcePrimitiveGuid.bytes();
            for (size_t index = 6; index < bytes.size(); ++index) {
                bytes[index] = std::to_integer<uint8_t>(digest[index - 6]);
            }
            bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0fu) | 0x70u);
            bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3fu) | 0x80u);
            return AssetGuid(bytes);
        }

        bool transparentConnectedWork(ModelCoverage coverage,
            TransparencyClass resolvedClass) {
            return coverage == ModelCoverage::Transparent ||
                resolvedClass == TransparencyClass::SortedSurface ||
                resolvedClass == TransparencyClass::ThinGlass ||
                resolvedClass == TransparencyClass::LayeredGlass ||
                resolvedClass == TransparencyClass::WeightedOit;
        }

        nlohmann::json sourceJson(
            std::span<const std::byte> bytes,
            std::stop_token stopToken = {}) {
            const nlohmann::json::parser_callback_t
                callback =
                    [stopToken](
                        int,
                        nlohmann::json::parse_event_t,
                        nlohmann::json&) {
                        if (stopToken.stop_requested()) {
                            throw std::runtime_error(
                                "Asset import cancelled.");
                        }
                        return true;
                    };
            if (bytes.size() >= 4 && readLe32(bytes, 0) == kGlbMagic) {
                if (bytes.size() < 20 || readLe32(bytes, 16) != kGlbJsonChunk) {
                    throw std::runtime_error(
                        "GLB does not begin with a JSON chunk.");
                }
                const uint32_t length = readLe32(bytes, 12);
                if (20ull + length > bytes.size()) {
                    throw std::runtime_error("GLB JSON chunk is truncated.");
                }
                return nlohmann::json::parse(
                    reinterpret_cast<const char*>(bytes.data() + 20),
                    reinterpret_cast<const char*>(bytes.data() + 20 + length),
                    callback);
            }
            return nlohmann::json::parse(
                reinterpret_cast<const char*>(bytes.data()),
                reinterpret_cast<const char*>(bytes.data() + bytes.size()),
                callback);
        }

        bool externalUri(std::string_view value) {
            return !value.empty() && !value.starts_with("data:") &&
                value.find("://") == std::string_view::npos;
        }

        std::string normalizedDependency(
            const std::filesystem::path& sourceRelativePath,
            std::string_view uri) {
            const std::filesystem::path combined =
                (sourceRelativePath.parent_path() /
                    std::filesystem::path(uri)).lexically_normal();
            return combined.generic_string();
        }

        void discoverExternalDependencies(
            const nlohmann::json& root,
            const std::filesystem::path& sourceRelativePath,
            ParsedSourceAsset& result) {
            std::set<std::string> unique;
            const auto scan = [&](const char* collection) {
                const auto found = root.find(collection);
                if (found == root.end() || !found->is_array()) return;
                for (const auto& item : *found) {
                    const auto uri = item.find("uri");
                    if (uri == item.end() || !uri->is_string()) continue;
                    const std::string text = uri->get<std::string>();
                    if (!externalUri(text)) continue;
                    const std::string location =
                        normalizedDependency(sourceRelativePath, text);
                    if (location.empty() ||
                        std::filesystem::path(location).is_absolute() ||
                        location == ".." || location.starts_with("../")) {
                        addError(result.diagnostics,
                            "GLTF_DEPENDENCY_PATH", text,
                            "External glTF dependencies must remain inside the asset root.");
                        continue;
                    }
                    unique.insert(location);
                }
            };
            scan("buffers");
            scan("images");
            for (const std::string& location : unique) {
                result.dependencies.push_back({
                    .type = AssetDependencyType::SourceFile,
                    .location = location,
                });
            }
        }

        std::span<const std::byte> encodedImageBytes(
            const fastgltf::Asset& asset,
            const fastgltf::Image& image) {
            return std::visit(fastgltf::visitor{
                [](const fastgltf::sources::Array& source)
                    -> std::span<const std::byte> {
                    return { source.bytes.data(), source.bytes.size() };
                },
                [](const fastgltf::sources::Vector& source)
                    -> std::span<const std::byte> {
                    return { source.bytes.data(), source.bytes.size() };
                },
                [](const fastgltf::sources::ByteView& source)
                    -> std::span<const std::byte> {
                    return { source.bytes.data(), source.bytes.size() };
                },
                [&asset](const fastgltf::sources::BufferView& source)
                    -> std::span<const std::byte> {
                    const auto bytes =
                        fastgltf::DefaultBufferDataAdapter{}(
                            asset, source.bufferViewIndex);
                    return { bytes.data(), bytes.size() };
                },
                [](const auto&) -> std::span<const std::byte> {
                    return {};
                },
            }, image.data);
        }

        std::filesystem::path suggestedImagePath(
            const nlohmann::json& root, size_t imageIndex) {
            const auto images = root.find("images");
            if (images == root.end() || !images->is_array() ||
                imageIndex >= images->size()) {
                return "image-" + std::to_string(imageIndex) +
                    ".png";
            }
            const nlohmann::json& image = (*images)[imageIndex];
            if (const auto uri = image.find("uri");
                uri != image.end() && uri->is_string()) {
                const std::string text = uri->get<std::string>();
                if (externalUri(text)) {
                    return std::filesystem::path(text).filename();
                }
                if (text.starts_with("data:image/jpeg")) {
                    return "image-" + std::to_string(imageIndex) +
                        ".jpg";
                }
                if (text.starts_with("data:image/vnd-ms.dds")) {
                    return "image-" + std::to_string(imageIndex) +
                        ".dds";
                }
            }
            if (const auto mime = image.find("mimeType");
                mime != image.end() && mime->is_string()) {
                const std::string text = mime->get<std::string>();
                if (text == "image/jpeg") {
                    return "image-" + std::to_string(imageIndex) +
                        ".jpg";
                }
                if (text == "image/vnd-ms.dds") {
                    return "image-" + std::to_string(imageIndex) +
                        ".dds";
                }
            }
            return "image-" + std::to_string(imageIndex) +
                ".png";
        }

        std::string metadataFingerprint(
            std::string_view domain,
            const nlohmann::json& value) {
            std::string canonical(domain);
            canonical.push_back('\0');
            canonical +=
                nlohmann::ordered_json(value).dump();
            return sha256(std::as_bytes(
                std::span<const char>(
                    canonical.data(),
                    canonical.size())));
        }

        void validateExternalGltfDependencies(
            const nlohmann::json& root,
            const ImportSource& input) {
            const auto validateCollection =
                [&root, &input](
                    std::string_view collection,
                    std::string_view kind) {
                    const auto entries =
                        root.find(collection);
                    if (entries == root.end()) {
                        return;
                    }
                    if (!entries->is_array()) {
                        throw std::runtime_error(
                            "glTF " +
                            std::string(collection) +
                            " must be an array.");
                    }
                    for (size_t index = 0;
                        index < entries->size();
                        ++index) {
                        if (input.stopToken
                                .stop_requested()) {
                            throw std::runtime_error(
                                "Asset import cancelled.");
                        }
                        const auto& entry =
                            (*entries)[index];
                        if (!entry.is_object()) {
                            continue;
                        }
                        const auto uri =
                            entry.find("uri");
                        if (uri == entry.end() ||
                            !uri->is_string() ||
                            !externalUri(
                                uri->get_ref<
                                    const std::string&>())) {
                            continue;
                        }
                        const std::string& uriText =
                            uri->get_ref<
                                const std::string&>();
                        const std::filesystem::path
                            relative =
                                std::filesystem::path(
                                    uriText)
                                    .lexically_normal();
                        if (relative.empty() ||
                            relative.is_absolute() ||
                            relative == ".." ||
                            relative.generic_string()
                                .starts_with("../")) {
                            throw std::runtime_error(
                                "glTF " +
                                std::string(kind) +
                                " dependency escapes its source package: " +
                                uriText);
                        }
                        const auto dependency =
                            (input.resolvedPath
                                .parent_path() /
                                relative)
                                .lexically_normal();
                        if (!std::filesystem::
                                is_regular_file(
                                    dependency)) {
                            throw std::runtime_error(
                                "glTF " +
                                std::string(kind) +
                                " dependency is missing: " +
                                dependency
                                    .generic_string());
                        }
                        std::ifstream source(
                            dependency,
                            std::ios::binary);
                        std::array<std::byte, 256>
                            prefix{};
                        source.read(
                            reinterpret_cast<char*>(
                                prefix.data()),
                            static_cast<
                                std::streamsize>(
                                prefix.size()));
                        const size_t count =
                            static_cast<size_t>(
                                source.gcount());
                        if (isGitLfsPointer(
                                std::span(
                                    prefix.data(),
                                    count))) {
                            throw std::runtime_error(
                                "glTF " +
                                std::string(kind) + " " +
                                std::to_string(index) +
                                " dependency '" +
                                uriText +
                                "' is an unresolved Git LFS pointer, not source data. "
                                "Resolve the LFS object in the source package and import again.");
                        }
                    }
                };
            validateCollection(
                "buffers", "buffer");
            validateCollection(
                "images", "image");
        }

        void discoverGltfMetadata(
            const nlohmann::json& root,
            const ImportSource& input,
            ParsedSourceAsset& result) {
            const auto cancelled = [&input] {
                if (input.stopToken.stop_requested()) {
                    throw std::runtime_error(
                        "Asset import cancelled.");
                }
            };

            if (const auto images =
                    root.find("images");
                images != root.end()) {
                if (!images->is_array()) {
                    throw std::runtime_error(
                        "glTF images must be an array.");
                }
                for (size_t index = 0;
                    index < images->size();
                    ++index) {
                    cancelled();
                    const auto& image =
                        (*images)[index];
                    result.discoveredSubassets
                        .push_back({
                            .assetType =
                                "iridium.texture",
                            .sourceKey =
                                "images/" +
                                std::to_string(index),
                            .structuralFingerprint =
                                metadataFingerprint(
                                    "gltf-image-metadata-v1",
                                    image),
                        });
                }
            }

            if (const auto materials =
                    root.find("materials");
                materials != root.end()) {
                if (!materials->is_array()) {
                    throw std::runtime_error(
                        "glTF materials must be an array.");
                }
                for (size_t index = 0;
                    index < materials->size();
                    ++index) {
                    cancelled();
                    result.discoveredSubassets
                        .push_back({
                            .assetType =
                                "iridium.material",
                            .sourceKey =
                                "materials/" +
                                std::to_string(index),
                            .structuralFingerprint =
                                metadataFingerprint(
                                    "gltf-material-metadata-v1",
                                    (*materials)[index]),
                        });
                }
            }

            const auto scenes =
                root.find("scenes");
            const auto nodes =
                root.find("nodes");
            const auto meshes =
                root.find("meshes");
            if (scenes == root.end() ||
                !scenes->is_array() ||
                scenes->empty()) {
                throw std::runtime_error(
                    "glTF model contains no scene to import.");
            }
            if (nodes == root.end() ||
                !nodes->is_array() ||
                meshes == root.end() ||
                !meshes->is_array()) {
                throw std::runtime_error(
                    "glTF scene has no valid nodes or meshes.");
            }
            const size_t sceneIndex =
                root.value("scene", size_t{ 0 });
            if (sceneIndex >= scenes->size()) {
                throw std::runtime_error(
                    "glTF default scene index is out of bounds.");
            }

            bool defaultMaterialUsed = false;
            std::set<size_t> visited;
            std::set<size_t> active;
            std::function<void(size_t)> visitNode =
                [&](size_t nodeIndex) {
                    cancelled();
                    if (nodeIndex >= nodes->size()) {
                        throw std::runtime_error(
                            "glTF scene node index is out of bounds.");
                    }
                    if (active.contains(nodeIndex)) {
                        throw std::runtime_error(
                            "glTF node hierarchy contains a cycle.");
                    }
                    if (!visited.insert(
                            nodeIndex).second) {
                        return;
                    }
                    active.insert(nodeIndex);
                    const auto& node =
                        (*nodes)[nodeIndex];
                    if (!node.is_object()) {
                        throw std::runtime_error(
                            "glTF node must be an object.");
                    }
                    if (const auto mesh =
                            node.find("mesh");
                        mesh != node.end()) {
                        const size_t meshIndex =
                            mesh->get<size_t>();
                        if (meshIndex >=
                                meshes->size()) {
                            throw std::runtime_error(
                                "glTF node mesh index is out of bounds.");
                        }
                        const auto primitives =
                            (*meshes)[meshIndex]
                                .find("primitives");
                        if (primitives ==
                                (*meshes)[meshIndex]
                                    .end() ||
                            !primitives->is_array()) {
                            throw std::runtime_error(
                                "glTF mesh primitives must be an array.");
                        }
                        for (size_t primitiveIndex = 0;
                            primitiveIndex <
                                primitives->size();
                            ++primitiveIndex) {
                            cancelled();
                            const auto& primitive =
                                (*primitives)[
                                    primitiveIndex];
                            defaultMaterialUsed =
                                defaultMaterialUsed ||
                                !primitive.contains(
                                    "material");
                            nlohmann::ordered_json
                                identity{
                                    { "node", node },
                                    { "primitive",
                                        primitive },
                                };
                            result.discoveredSubassets
                                .push_back({
                                    .assetType =
                                        "iridium.model-primitive",
                                    .sourceKey =
                                        "nodes/" +
                                        std::to_string(
                                            nodeIndex) +
                                        "/meshes/" +
                                        std::to_string(
                                            meshIndex) +
                                        "/primitives/" +
                                        std::to_string(
                                            primitiveIndex),
                                    .structuralFingerprint =
                                        metadataFingerprint(
                                            "gltf-primitive-metadata-v1",
                                            identity),
                                });
                        }
                    }
                    if (const auto children =
                            node.find("children");
                        children != node.end()) {
                        if (!children->is_array()) {
                            throw std::runtime_error(
                                "glTF node children must be an array.");
                        }
                        for (const auto& child :
                            *children) {
                            visitNode(
                                child.get<size_t>());
                        }
                    }
                    active.erase(nodeIndex);
                };

            const auto sceneNodes =
                (*scenes)[sceneIndex]
                    .find("nodes");
            if (sceneNodes !=
                    (*scenes)[sceneIndex].end()) {
                if (!sceneNodes->is_array()) {
                    throw std::runtime_error(
                        "glTF scene nodes must be an array.");
                }
                for (const auto& node :
                    *sceneNodes) {
                    visitNode(
                        node.get<size_t>());
                }
            }
            if (result.discoveredSubassets.empty()) {
                throw std::runtime_error(
                    "glTF scene contains no importable subassets.");
            }
            if (defaultMaterialUsed) {
                result.discoveredSubassets
                    .push_back({
                        .assetType =
                            "iridium.material",
                        .sourceKey =
                            "materials/default",
                        .structuralFingerprint =
                            "gltf-default-material-v1",
                    });
            }
        }

        Json vec2Json(const glm::vec2& value) {
            return Json::array({ value.x, value.y });
        }

        Json vec3Json(const glm::vec3& value) {
            return Json::array({ value.x, value.y, value.z });
        }

        Json vec4Json(const glm::vec4& value) {
            return Json::array({ value.x, value.y, value.z, value.w });
        }

        template <typename Vector>
        Vector jsonVector(const Json& value);

        template <>
        glm::vec2 jsonVector<glm::vec2>(const Json& value) {
            if (!value.is_array() || value.size() != 2) {
                throw std::runtime_error("Expected a float2 array.");
            }
            return { value[0].get<float>(), value[1].get<float>() };
        }

        template <>
        glm::vec3 jsonVector<glm::vec3>(const Json& value) {
            if (!value.is_array() || value.size() != 3) {
                throw std::runtime_error("Expected a float3 array.");
            }
            return {
                value[0].get<float>(), value[1].get<float>(),
                value[2].get<float>(),
            };
        }

        template <>
        glm::vec4 jsonVector<glm::vec4>(const Json& value) {
            if (!value.is_array() || value.size() != 4) {
                throw std::runtime_error("Expected a float4 array.");
            }
            return {
                value[0].get<float>(), value[1].get<float>(),
                value[2].get<float>(), value[3].get<float>(),
            };
        }

        bool finite(const WorkingVertex& vertex) {
            const auto finiteValue = [](float value) {
                return std::isfinite(value);
            };
            return std::ranges::all_of(
                    std::span<const float>(&vertex.pos.x, 3), finiteValue) &&
                std::ranges::all_of(
                    std::span<const float>(&vertex.color.x, 4), finiteValue) &&
                std::ranges::all_of(
                    std::span<const float>(&vertex.normal.x, 3), finiteValue) &&
                std::ranges::all_of(
                    std::span<const float>(&vertex.uv0.x, 2), finiteValue) &&
                std::ranges::all_of(
                    std::span<const float>(&vertex.tangent.x, 4), finiteValue) &&
                std::ranges::all_of(
                    std::span<const float>(&vertex.uv1.x, 2), finiteValue);
        }

        std::vector<uint32_t> canonicalTriangleIndices(
            fastgltf::PrimitiveType topology,
            std::span<const uint32_t> source) {
            std::vector<uint32_t> result;
            switch (topology) {
            case fastgltf::PrimitiveType::Triangles:
                if (source.size() % 3 != 0) {
                    throw std::runtime_error(
                        "Triangle primitive index count is not divisible by three.");
                }
                result.assign(source.begin(), source.end());
                break;
            case fastgltf::PrimitiveType::TriangleStrip:
                if (source.size() < 3) {
                    throw std::runtime_error(
                        "Triangle strip contains fewer than three indices.");
                }
                result.reserve((source.size() - 2) * 3);
                for (size_t index = 2; index < source.size(); ++index) {
                    const uint32_t a = source[index - 2];
                    const uint32_t b = source[index - 1];
                    const uint32_t c = source[index];
                    if (a == b || b == c || a == c) continue;
                    if ((index & 1u) == 0) {
                        result.insert(result.end(), { a, b, c });
                    } else {
                        result.insert(result.end(), { b, a, c });
                    }
                }
                break;
            case fastgltf::PrimitiveType::TriangleFan:
                if (source.size() < 3) {
                    throw std::runtime_error(
                        "Triangle fan contains fewer than three indices.");
                }
                result.reserve((source.size() - 2) * 3);
                for (size_t index = 2; index < source.size(); ++index) {
                    const uint32_t a = source[0];
                    const uint32_t b = source[index - 1];
                    const uint32_t c = source[index];
                    if (a == b || b == c || a == c) continue;
                    result.insert(result.end(), { a, b, c });
                }
                break;
            default:
                throw std::runtime_error(
                    "The cooked production model path currently requires triangle topology.");
            }
            if (result.empty()) {
                throw std::runtime_error(
                    "Primitive canonicalization produced no triangles.");
            }
            return result;
        }

        std::optional<AssetGuid> subassetGuid(
            const AssetCookContext& context, std::string_view sourceKey) {
            const auto found = std::ranges::find_if(context.subassets,
                [sourceKey](const SubassetMetadata& subasset) {
                    return subasset.sourceKey == sourceKey;
                });
            return found == context.subassets.end()
                ? std::nullopt : std::optional(found->guid);
        }

        std::optional<TransparencyClass> transparencyClassSetting(
            std::string_view value) {
            if (value == "auto") return TransparencyClass::Auto;
            if (value == "alpha_clip") return TransparencyClass::AlphaClip;
            if (value == "sorted_surface") return TransparencyClass::SortedSurface;
            if (value == "thin_glass") return TransparencyClass::ThinGlass;
            if (value == "layered_glass") return TransparencyClass::LayeredGlass;
            if (value == "weighted_oit") return TransparencyClass::WeightedOit;
            return std::nullopt;
        }

        std::optional<TransparencyQuality> transparencyQualitySetting(
            std::string_view value) {
            if (value == "ordinary2") return TransparencyQuality::Ordinary2;
            if (value == "hero4") return TransparencyQuality::Hero4;
            if (value == "cinematic8") return TransparencyQuality::Cinematic8;
            return std::nullopt;
        }

        const char* transparencyClassSettingName(
            TransparencyClass value) {
            switch (value) {
            case TransparencyClass::Auto: return "auto";
            case TransparencyClass::AlphaClip: return "alpha_clip";
            case TransparencyClass::SortedSurface: return "sorted_surface";
            case TransparencyClass::ThinGlass: return "thin_glass";
            case TransparencyClass::LayeredGlass: return "layered_glass";
            case TransparencyClass::WeightedOit: return "weighted_oit";
            case TransparencyClass::None: break;
            }
            return "auto";
        }

        TransparencyPolicyV1 normalizedTransparencyPolicy(
            const Json& value) {
            TransparencyPolicyV1 result;
            result.requestedClass = transparencyClassSetting(
                value.at("class").get<std::string>()).value_or(
                    TransparencyClass::Auto);
            result.quality = transparencyQualitySetting(
                value.at("quality").get<std::string>()).value_or(
                    TransparencyQuality::Ordinary2);
            result.priority = value.at("priority").get<int32_t>();
            result.thinSheetThicknessMeters =
                value.at("thin_sheet_thickness_m").get<float>();
            return result;
        }

        std::optional<TransparencyPolicyV1> transparencyPolicyFor(
            const NormalizedImportSettings& settings,
            const AssetGuid& guid) {
            const Json& policies =
                settings.values.at("transparency_policies");
            const auto found = policies.find(guid.toString());
            if (found == policies.end()) return std::nullopt;
            return normalizedTransparencyPolicy(*found);
        }

        ModelCoverage coverageFromInteger(uint32_t value) {
            if (value > static_cast<uint32_t>(ModelCoverage::Transparent)) {
                throw std::runtime_error("Parsed material coverage is invalid.");
            }
            return static_cast<ModelCoverage>(value);
        }

        Json serializeWorkingVertex(const WorkingVertex& vertex) {
            return {
                { "color", vec4Json(vertex.color) },
                { "normal", vec3Json(vertex.normal) },
                { "position", vec3Json(vertex.pos) },
                { "tangent", vec4Json(vertex.tangent) },
                { "texcoord0", vec2Json(vertex.uv0) },
                { "texcoord1", vec2Json(vertex.uv1) },
            };
        }

        WorkingVertex readWorkingVertex(const Json& value) {
            WorkingVertex result;
            result.pos = jsonVector<glm::vec3>(value.at("position"));
            result.color = jsonVector<glm::vec4>(value.at("color"));
            result.normal = jsonVector<glm::vec3>(value.at("normal"));
            result.uv0 = jsonVector<glm::vec2>(value.at("texcoord0"));
            result.tangent = jsonVector<glm::vec4>(value.at("tangent"));
            result.uv1 = jsonVector<glm::vec2>(value.at("texcoord1"));
            if (!finite(result)) {
                throw std::runtime_error(
                    "Parsed vertex contains a non-finite value.");
            }
            return result;
        }

        ModelCoverage sourceCoverage(SourceAlphaMode value) {
            switch (value) {
            case SourceAlphaMode::Opaque: return ModelCoverage::Opaque;
            case SourceAlphaMode::Mask: return ModelCoverage::Masked;
            case SourceAlphaMode::Blend: return ModelCoverage::Transparent;
            }
            return ModelCoverage::Opaque;
        }

        TextureSemantic cookedTextureSemantic(
            SourceTextureSemantic semantic) {
            switch (semantic) {
            case SourceTextureSemantic::BaseColor:
            case SourceTextureSemantic::Emissive:
            case SourceTextureSemantic::Diffuse:
            case SourceTextureSemantic::SheenColor:
            case SourceTextureSemantic::SpecularColor:
            case SourceTextureSemantic::DiffuseTransmissionColor:
                return TextureSemantic::Color;
            case SourceTextureSemantic::Normal:
            case SourceTextureSemantic::ClearcoatNormal:
                return TextureSemantic::Normal;
            default:
                // Preserve packed channels. Scalar block formats cannot represent
                // metallic/roughness or other multi-channel source uses.
                return TextureSemantic::Data;
            }
        }

        const char* textureSemanticSetting(
            TextureSemantic semantic) {
            switch (semantic) {
            case TextureSemantic::Color: return "color";
            case TextureSemantic::Normal: return "normal";
            case TextureSemantic::Scalar: return "scalar";
            case TextureSemantic::HdrColor: return "hdr_color";
            case TextureSemantic::Data: return "data";
            }
            return "data";
        }

        const CookSection* findSection(
            const CookProduct& product, uint32_t id) {
            const auto found = std::ranges::find_if(
                product.sections,
                [id](const CookSection& section) {
                    return section.id == id;
                });
            return found == product.sections.end()
                ? nullptr : &*found;
        }

        const CookSection* findSection(
            const CookedArtifact& artifact, uint32_t id) {
            const auto found = std::ranges::find_if(
                artifact.sections,
                [id](const CookSection& section) {
                    return section.id == id;
                });
            return found == artifact.sections.end()
                ? nullptr : &*found;
        }

        std::vector<AssetDependency> embeddedTextureDependencies() {
            return {{
                .type = AssetDependencyType::Tool,
                .location = kDirectXTexCodecId,
                .contentHash = kDirectXTexCodecContentHash,
            }};
        }

        std::string embeddedTextureCookKey(
            const TextureImporter& importer,
            const AssetGuid& textureGuid,
            const ParsedSourceAsset::SubassetPayload& imagePayload,
            const NormalizedImportSettings& settings,
            const CookTarget& target) {
            const std::span<const std::byte> sourceBytes =
                !imagePayload.bytes.empty()
                ? std::span<const std::byte>(imagePayload.bytes)
                : std::span<const std::byte>(imagePayload.parsedBytes);
            return calculateCookKey({
                .assetGuid = textureGuid,
                .importerId = importer.descriptor().id,
                .importerImplementationVersion =
                    importer.descriptor().implementationVersion,
                .settingsSchemaVersion = settings.schemaVersion,
                .canonicalSettings = settings.canonicalBytes,
                .sourceContentHash = sha256(sourceBytes),
                .dependencies = embeddedTextureDependencies(),
                .target = target,
                .cookerFeatureVersion =
                    "gltf-embedded-texture-view-v1",
            });
        }

        std::optional<CookedModelTextureView> readEmbeddedTextureView(
            DerivedDataCache* cache,
            std::string_view cookKey,
            const AssetGuid& textureGuid,
            uint32_t sourceImageIndex) {
            if (!cache) return std::nullopt;
            DdcReadResult cached = cache->read(cookKey);
            if (cached.status != DdcLookupStatus::Hit ||
                !cached.blob) {
                return std::nullopt;
            }
            const CookedArtifactReadResult artifact =
                readCookedArtifact(cached.blob->bytes,
                    cached.blob->artifactHash);
            if (!artifact.valid() ||
                artifact.artifact->assetGuid != textureGuid ||
                artifact.artifact->artifactType != "iridium.texture" ||
                artifact.artifact->artifactSchemaVersion !=
                    kCookedTextureSchemaVersion ||
                artifact.artifact->cookKey != cookKey) {
                return std::nullopt;
            }
            const CookSection* manifestSection = findSection(
                *artifact.artifact, kCookedTextureManifestSection);
            const CookSection* payloadSection = findSection(
                *artifact.artifact, kCookedTexturePayloadSection);
            if (!manifestSection || !payloadSection) {
                return std::nullopt;
            }
            std::vector<CookDiagnostic> diagnostics;
            const auto manifest = readTextureManifest(
                manifestSection->bytes, diagnostics);
            if (!manifest || hasCookErrors(diagnostics) ||
                hasCookErrors(validateTextureProduct(
                    *manifest, payloadSection->bytes.size()))) {
                return std::nullopt;
            }
            CookedModelTextureView view{
                .textureGuid = textureGuid,
                .sourceImageIndex = sourceImageIndex,
                .manifest = *manifest,
                .payload = payloadSection->bytes,
            };
            view.viewKey = calculateModelTextureViewKey(view);
            return view;
        }

        void storeEmbeddedTextureView(
            DerivedDataCache* cache,
            std::string_view cookKey,
            const AssetGuid& textureGuid,
            const CookTarget& target,
            const CookProduct& product) {
            if (!cache) return;
            const CookedArtifactBlob blob = serializeCookedArtifact({
                .assetGuid = textureGuid,
                .artifactType = product.artifactType,
                .artifactSchemaVersion = product.artifactSchemaVersion,
                .target = target,
                .cookKey = std::string(cookKey),
                .dependencies = embeddedTextureDependencies(),
                .sections = product.sections,
            });
            // Reuse is optional. Failure to publish an optimization must not
            // invalidate the deterministic parent model product.
            (void)cache->storeAtomic(cookKey, blob);
        }

        void reportCookProgress(
            const AssetCookContext& context,
            std::string stage,
            uint64_t completed,
            uint64_t total,
            std::string detail) noexcept {
            if (!context.progress) return;
            try {
                context.progress({
                    .stage = std::move(stage),
                    .completed = completed,
                    .total = total,
                    .detail = std::move(detail),
                });
            }
            catch (...) {
                // Diagnostics must never alter deterministic cook behavior.
            }
        }

        bool progressCheckpoint(
            uint64_t completed,
            uint64_t total,
            uint64_t checkpointCount) noexcept {
            if (completed == 0 || completed >= total || total == 0) {
                return true;
            }
            const uint64_t interval = (std::max)(
                uint64_t{ 1 }, total / checkpointCount);
            return completed == 1 || completed % interval == 0;
        }

        struct EmbeddedTextureViewJob {
            uint32_t outputIndex = 0;
            AssetGuid textureGuid;
            uint32_t sourceImageIndex = 0;
            const ParsedSourceAsset::SubassetPayload* imagePayload = nullptr;
            NormalizedImportSettings settings;
            std::string derivedCookKey;
            std::string imageKey;
        };

        struct EmbeddedTextureViewResult {
            std::optional<CookedModelTextureView> view;
            std::string error;
            bool cacheHit = false;
        };

        EmbeddedTextureViewResult cookEmbeddedTextureView(
            const EmbeddedTextureViewJob& job,
            const CookTarget& target,
            DerivedDataCache* cache,
            std::stop_token stopToken) {
            try {
                if (stopToken.stop_requested()) {
                    return { .error = "Texture view cook was cancelled." };
                }
                if (std::optional<CookedModelTextureView> reused =
                        readEmbeddedTextureView(
                            cache, job.derivedCookKey,
                            job.textureGuid,
                            job.sourceImageIndex)) {
                    return {
                        .view = std::move(reused),
                        .cacheHit = true,
                    };
                }

                TextureImporter textureImporter;
                ParsedSourceAsset parsedTexture;
                if (!job.imagePayload->parsedBytes.empty()) {
                    parsedTexture.documentBytes =
                        job.imagePayload->parsedBytes;
                }
                else {
                    parsedTexture = textureImporter.parse({
                        .relativePath = job.imagePayload->suggestedPath,
                        .resolvedPath = {},
                        .bytes = job.imagePayload->bytes,
                        .stopToken = stopToken,
                    }, job.settings);
                    if (hasCookErrors(parsedTexture.diagnostics)) {
                        std::string detail;
                        for (const CookDiagnostic& diagnostic :
                                parsedTexture.diagnostics) {
                            if (diagnostic.severity ==
                                    CookDiagnosticSeverity::Error) {
                                detail = diagnostic.code + ": " +
                                    diagnostic.message;
                                break;
                            }
                        }
                        return {
                            .error = "Image subasset failed deterministic source decoding" +
                                (detail.empty() ? std::string(".")
                                    : ": " + detail),
                        };
                    }
                }

                const CookProduct textureProduct = textureImporter.cook(
                    parsedTexture, job.settings, target, {
                        .assetGuid = job.textureGuid,
                    }, stopToken);
                if (hasCookErrors(textureProduct.diagnostics)) {
                    return {
                        .error = "Image subasset failed deterministic texture cooking.",
                    };
                }
                const CookSection* manifestSection = findSection(
                    textureProduct, kCookedTextureManifestSection);
                const CookSection* payloadSection = findSection(
                    textureProduct, kCookedTexturePayloadSection);
                if (!manifestSection || !payloadSection) {
                    return {
                        .error = "Texture importer omitted a required product section.",
                    };
                }
                std::vector<CookDiagnostic> textureDiagnostics;
                const auto textureManifest = readTextureManifest(
                    manifestSection->bytes, textureDiagnostics);
                if (!textureManifest ||
                    hasCookErrors(textureDiagnostics)) {
                    return {
                        .error = "Texture importer emitted an invalid manifest.",
                    };
                }
                storeEmbeddedTextureView(
                    cache, job.derivedCookKey,
                    job.textureGuid, target,
                    textureProduct);
                CookedModelTextureView view{
                    .textureGuid = job.textureGuid,
                    .sourceImageIndex = job.sourceImageIndex,
                    .manifest = *textureManifest,
                    .payload = payloadSection->bytes,
                };
                view.viewKey = calculateModelTextureViewKey(view);
                return { .view = std::move(view) };
            }
            catch (const std::exception& exception) {
                return { .error = exception.what() };
            }
        }

        void cookEmbeddedTextureViews(
            CookedModelProductData& data,
            std::span<const EmbeddedTextureViewJob> jobs,
            const CookTarget& target,
            const AssetCookContext& context,
            std::stop_token stopToken,
            std::vector<CookDiagnostic>& diagnostics) {
            if (jobs.empty()) {
                reportCookProgress(context, "textures", 0, 0,
                    "No embedded texture views required");
                return;
            }
            const size_t hardware = (std::max)(
                1u, std::thread::hardware_concurrency());
            const size_t workerCount = (std::min)(
                jobs.size(), (std::min)(size_t{ 16 },
                    (std::max)(size_t{ 1 }, hardware / 2)));
            reportCookProgress(context, "textures", 0, jobs.size(),
                "Cooking unique embedded texture views on " +
                    std::to_string(workerCount) + " workers");

            std::vector<EmbeddedTextureViewResult> results(jobs.size());
            std::atomic_size_t nextJob{ 0 };
            std::atomic_uint64_t completed{ 0 };
            std::atomic_uint64_t cacheHits{ 0 };
            std::atomic_uint64_t built{ 0 };
            {
                std::vector<std::jthread> workers;
                workers.reserve(workerCount);
                for (size_t worker = 0; worker < workerCount; ++worker) {
                    workers.emplace_back([&](std::stop_token) {
                        while (!stopToken.stop_requested()) {
                            const size_t jobIndex =
                                nextJob.fetch_add(1,
                                    std::memory_order_relaxed);
                            if (jobIndex >= jobs.size()) break;
                            results[jobIndex] = cookEmbeddedTextureView(
                                jobs[jobIndex], target,
                                context.derivedDataCache,
                                stopToken);
                            const bool hit = results[jobIndex].cacheHit;
                            if (hit) {
                                cacheHits.fetch_add(1,
                                    std::memory_order_relaxed);
                            }
                            else if (results[jobIndex].view) {
                                built.fetch_add(1,
                                    std::memory_order_relaxed);
                            }
                            const uint64_t count = completed.fetch_add(
                                1, std::memory_order_relaxed) + 1;
                            const char* outcome = hit
                                ? "cache hit: "
                                : results[jobIndex].view
                                    ? "cooked: " : "failed: ";
                            reportCookProgress(context, "textures",
                                count, jobs.size(),
                                std::string(outcome) +
                                    jobs[jobIndex].imageKey);
                        }
                    });
                }
            }

            if (stopToken.stop_requested()) {
                throw std::runtime_error(
                    "glTF embedded texture cooking was cancelled.");
            }
            for (size_t index = 0; index < jobs.size(); ++index) {
                const EmbeddedTextureViewJob& job = jobs[index];
                EmbeddedTextureViewResult& result = results[index];
                if (!result.view) {
                    addError(diagnostics, "GLTF_TEXTURE_COOK",
                        job.imageKey,
                        result.error.empty()
                            ? "Image subasset texture view did not complete."
                            : std::move(result.error));
                    continue;
                }
                data.textureViews[job.outputIndex] =
                    std::move(*result.view);
            }
            reportCookProgress(context, "textures", jobs.size(), jobs.size(),
                "Embedded texture views complete (" +
                    std::to_string(built.load()) + " built, " +
                    std::to_string(cacheHits.load()) + " cache hits)");
        }

    } // namespace

    std::vector<TriangleConnectedComponent>
        findTriangleConnectedComponents(
            std::span<const uint32_t> triangleIndices) {
        if (triangleIndices.size() % 3 != 0) {
            throw std::invalid_argument(
                "Triangle component input must contain complete triangles.");
        }
        const uint32_t triangleCount = static_cast<uint32_t>(
            triangleIndices.size() / 3);
        std::vector<uint32_t> parents(triangleCount);
        std::iota(parents.begin(), parents.end(), 0u);
        const auto root = [&parents](uint32_t value) {
            while (parents[value] != value) {
                parents[value] = parents[parents[value]];
                value = parents[value];
            }
            return value;
        };
        const auto unite = [&parents, &root](uint32_t lhs, uint32_t rhs) {
            lhs = root(lhs);
            rhs = root(rhs);
            if (lhs == rhs) return;
            if (lhs > rhs) std::swap(lhs, rhs);
            parents[rhs] = lhs;
        };
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> edgeOwners;
        for (uint32_t triangle = 0; triangle < triangleCount; ++triangle) {
            const std::array vertices{
                triangleIndices[triangle * 3],
                triangleIndices[triangle * 3 + 1],
                triangleIndices[triangle * 3 + 2],
            };
            for (uint32_t edge = 0; edge < 3; ++edge) {
                uint32_t first = vertices[edge];
                uint32_t second = vertices[(edge + 1) % 3];
                if (first == second) continue;
                if (first > second) std::swap(first, second);
                const auto [found, inserted] = edgeOwners.emplace(
                    std::pair{ first, second }, triangle);
                if (!inserted) unite(triangle, found->second);
            }
        }

        std::map<uint32_t, TriangleConnectedComponent> grouped;
        for (uint32_t triangle = 0; triangle < triangleCount; ++triangle) {
            const uint32_t componentRoot = root(triangle);
            auto [found, inserted] = grouped.try_emplace(componentRoot);
            if (inserted) found->second.sourceTriangleSeed = triangle;
            found->second.sourceTriangleIndices.push_back(triangle);
        }
        std::vector<TriangleConnectedComponent> result;
        result.reserve(grouped.size());
        for (auto& [ignored, component] : grouped) {
            (void)ignored;
            result.push_back(std::move(component));
        }
        std::ranges::sort(result, {},
            &TriangleConnectedComponent::sourceTriangleSeed);
        return result;
    }

    ClosedTriangleTopologyAnalysis analyzeClosedTriangleTopology(
        std::span<const glm::vec3> positions,
        std::span<const uint32_t> triangleIndices,
        std::span<const uint32_t> sourceTriangleIndices) {
        if (triangleIndices.size() % 3 != 0) {
            throw std::invalid_argument(
                "Closed-topology input must contain complete triangles.");
        }
        ClosedTriangleTopologyAnalysis result;
        result.triangleCount = static_cast<uint32_t>(
            sourceTriangleIndices.size());
        if (sourceTriangleIndices.empty()) return result;

        glm::dvec3 boundsMin(std::numeric_limits<double>::max());
        glm::dvec3 boundsMax(std::numeric_limits<double>::lowest());
        for (uint32_t sourceTriangle : sourceTriangleIndices) {
            if (sourceTriangle >= triangleIndices.size() / 3) {
                throw std::invalid_argument(
                    "Closed-topology triangle index is outside the source span.");
            }
            for (uint32_t corner = 0; corner < 3; ++corner) {
                const uint32_t vertex = triangleIndices[
                    sourceTriangle * 3 + corner];
                if (vertex >= positions.size()) {
                    throw std::invalid_argument(
                        "Closed-topology vertex index is outside the position span.");
                }
                const glm::vec3 position = positions[vertex];
                if (!std::isfinite(position.x) ||
                    !std::isfinite(position.y) ||
                    !std::isfinite(position.z)) {
                    throw std::invalid_argument(
                        "Closed-topology positions must be finite.");
                }
                const glm::dvec3 point(position);
                boundsMin = glm::min(boundsMin, point);
                boundsMax = glm::max(boundsMax, point);
            }
        }
        const glm::dvec3 origin = (boundsMin + boundsMax) * 0.5;
        const double diagonal = glm::length(boundsMax - boundsMin);
        const double scale = (std::max)(diagonal, 1.0e-9);
        const double areaSquaredEpsilon = scale * scale * scale * scale *
            1.0e-20;
        const double volumeEpsilon = scale * scale * scale * 1.0e-12;

        struct EdgeUse {
            uint32_t count = 0;
            int32_t orientationBalance = 0;
        };
        std::map<std::pair<uint32_t, uint32_t>, EdgeUse> edges;
        double signedVolume = 0.0;
        for (uint32_t sourceTriangle : sourceTriangleIndices) {
            const std::array<uint32_t, 3> vertices{
                triangleIndices[sourceTriangle * 3],
                triangleIndices[sourceTriangle * 3 + 1],
                triangleIndices[sourceTriangle * 3 + 2],
            };
            if (vertices[0] == vertices[1] ||
                vertices[1] == vertices[2] ||
                vertices[2] == vertices[0]) {
                ++result.degenerateTriangleCount;
                continue;
            }
            const glm::dvec3 a = glm::dvec3(positions[vertices[0]]) - origin;
            const glm::dvec3 b = glm::dvec3(positions[vertices[1]]) - origin;
            const glm::dvec3 c = glm::dvec3(positions[vertices[2]]) - origin;
            const glm::dvec3 cross = glm::cross(b - a, c - a);
            if (glm::dot(cross, cross) <= areaSquaredEpsilon)
                ++result.degenerateTriangleCount;
            signedVolume += glm::dot(a, glm::cross(b, c)) / 6.0;

            for (uint32_t edge = 0; edge < 3; ++edge) {
                const uint32_t from = vertices[edge];
                const uint32_t to = vertices[(edge + 1) % 3];
                const auto key = (std::minmax)(from, to);
                EdgeUse& use = edges[{ key.first, key.second }];
                ++use.count;
                use.orientationBalance += from < to ? 1 : -1;
            }
        }
        for (const auto& [edge, use] : edges) {
            (void)edge;
            if (use.count == 1) {
                ++result.boundaryEdgeCount;
            }
            else if (use.count > 2) {
                ++result.nonManifoldEdgeCount;
            }
            else if (use.orientationBalance != 0) {
                ++result.inconsistentOrientationEdgeCount;
            }
        }
        result.signedVolume = std::abs(signedVolume) > volumeEpsilon
            ? signedVolume : 0.0;
        return result;
    }

    const ImporterDescriptor& GltfModelImporter::descriptor() const noexcept {
        static const ImporterDescriptor descriptor{
            .id = "iridium.gltf-model",
            .implementationVersion = kGltfModelImporterVersion,
            .currentSettingsSchemaVersion = 2,
            .assetTypes = { "iridium.model" },
            .extensions = { ".gltf", ".glb" },
        };
        return descriptor;
    }

    ImportProbeResult GltfModelImporter::probe(
        const std::filesystem::path& relativePath,
        std::span<const std::byte> sourceBytes) const {
        std::string extension = relativePath.extension().generic_string();
        std::ranges::transform(extension, extension.begin(),
            [](unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
        if (extension != ".gltf" && extension != ".glb") {
            return ImportProbeResult::Unsupported;
        }
        try {
            const auto root = sourceJson(sourceBytes);
            const auto asset = root.find("asset");
            return asset != root.end() && asset->is_object()
                ? ImportProbeResult::Supported
                : ImportProbeResult::Unsupported;
        } catch (...) {
            return ImportProbeResult::Unsupported;
        }
    }

    NormalizedImportSettings GltfModelImporter::normalizeSettings(
        uint32_t sourceSchemaVersion, const nlohmann::json& settings,
        bool strict) const {
        NormalizedImportSettings result;
        result.schemaVersion = descriptor().currentSettingsSchemaVersion;
        if ((sourceSchemaVersion != 1 && sourceSchemaVersion != 2) ||
            !settings.is_object()) {
            addError(result.diagnostics, "GLTF_SETTINGS_SCHEMA", "/",
                "glTF model settings must use object schema 1 or 2.");
            return result;
        }
        result.values = {
            { "bake_node_transforms", true },
            { "generate_missing_tangents", true },
            { "import_scale", 1.0 },
            { "preserve_rt_geometry", true },
            { "recalculate_normals", false },
            { "recalculate_tangents", false },
            { "reverse_winding", false },
            { "transparency_execution_mode", "legacy_two_bucket" },
            { "transparency_policies", Json::object() },
        };
        const std::set<std::string> known{
            "bake_node_transforms",
            "generate_missing_tangents",
            "import_scale",
            "preserve_rt_geometry",
            "recalculate_normals",
            "recalculate_tangents",
            "reverse_winding",
            "transparency_execution_mode",
            "transparency_policies",
        };
        for (const auto& [key, value] : settings.items()) {
            if (!known.contains(key)) {
                addDiagnostic(result.diagnostics,
                    strict ? CookDiagnosticSeverity::Error
                           : CookDiagnosticSeverity::Warning,
                    "GLTF_SETTINGS_UNKNOWN", "/" + key,
                    strict
                        ? "Strict cooking rejects unknown glTF model settings."
                        : "Unknown glTF model setting is ignored.");
                continue;
            }
            if (key == "import_scale") {
                if (!value.is_number()) {
                    addError(result.diagnostics,
                        "GLTF_SETTINGS_TYPE",
                        "/" + key,
                        "glTF import scale must be numeric.");
                    continue;
                }
                const double scale =
                    value.get<double>();
                if (!std::isfinite(scale) ||
                    scale < 1.0e-6 ||
                    scale > 1.0e6) {
                    addError(result.diagnostics,
                        "GLTF_IMPORT_SCALE_RANGE",
                        "/" + key,
                        "glTF import scale must be finite and between 0.000001 and 1000000.");
                    continue;
                }
                result.values[key] = scale;
                continue;
            }
            if (key == "transparency_execution_mode") {
                if (!value.is_string()) {
                    addDiagnostic(result.diagnostics,
                        CookDiagnosticSeverity::Warning,
                        "GLTF_TRANSPARENCY_EXECUTION_UNKNOWN",
                        "/" + key,
                        "Unknown transparency execution mode was normalized to legacy_two_bucket.");
                    continue;
                }
                const std::string mode = value.get<std::string>();
                if (mode != "legacy_two_bucket" && mode != "classified") {
                    addDiagnostic(result.diagnostics,
                        CookDiagnosticSeverity::Warning,
                        "GLTF_TRANSPARENCY_EXECUTION_UNKNOWN",
                        "/" + key,
                        "Unknown transparency execution mode was normalized to legacy_two_bucket.");
                    continue;
                }
                result.values[key] = mode;
                continue;
            }
            if (key == "transparency_policies") {
                if (!value.is_object()) {
                    addDiagnostic(result.diagnostics,
                        CookDiagnosticSeverity::Warning,
                        "GLTF_TRANSPARENCY_POLICIES_TYPE",
                        "/" + key,
                        "Transparency policies must be an object; an empty policy map was selected.");
                    continue;
                }
                Json normalized = Json::object();
                for (const auto& [guidText, rawPolicy] : value.items()) {
                    const std::string field = "/" + key + "/" + guidText;
                    const std::optional<AssetGuid> guid =
                        AssetGuid::parse(guidText);
                    if (!guid || guid->isNil()) {
                        addDiagnostic(result.diagnostics,
                            CookDiagnosticSeverity::Warning,
                            "GLTF_TRANSPARENCY_TARGET_GUID",
                            field,
                            "Transparency policy target is not a non-nil stable GUID and was ignored.");
                        continue;
                    }
                    TransparencyPolicyV1 policy;
                    const auto policySchema = rawPolicy.is_object()
                        ? rawPolicy.find("schema_version")
                        : rawPolicy.end();
                    const bool currentPolicySchema =
                        rawPolicy.is_object() &&
                        policySchema != rawPolicy.end() &&
                        policySchema->is_number_integer() &&
                        policySchema->get<int64_t>() ==
                            TransparencyPolicyV1::SchemaVersion;
                    if (!currentPolicySchema) {
                        addDiagnostic(result.diagnostics,
                            CookDiagnosticSeverity::Warning,
                            "GLTF_TRANSPARENCY_POLICY_SCHEMA",
                            field,
                            "Unknown transparency policy schema was normalized to Auto defaults.");
                    } else {
                        if (const auto found = rawPolicy.find("class");
                            found != rawPolicy.end()) {
                            if (found->is_string()) {
                                const auto parsed = transparencyClassSetting(
                                    found->get<std::string>());
                                if (parsed) policy.requestedClass = *parsed;
                                else addDiagnostic(result.diagnostics,
                                    CookDiagnosticSeverity::Warning,
                                    "GLTF_TRANSPARENCY_CLASS_UNKNOWN",
                                    field + "/class",
                                    "Unknown transparency class was normalized to Auto.");
                            } else addDiagnostic(result.diagnostics,
                                CookDiagnosticSeverity::Warning,
                                "GLTF_TRANSPARENCY_CLASS_UNKNOWN",
                                field + "/class",
                                "Unknown transparency class was normalized to Auto.");
                        }
                        if (const auto found = rawPolicy.find("quality");
                            found != rawPolicy.end()) {
                            if (found->is_string()) {
                                const auto parsed = transparencyQualitySetting(
                                    found->get<std::string>());
                                if (parsed) policy.quality = *parsed;
                                else addDiagnostic(result.diagnostics,
                                    CookDiagnosticSeverity::Warning,
                                    "GLTF_TRANSPARENCY_QUALITY_UNKNOWN",
                                    field + "/quality",
                                    "Unknown transparency quality was normalized to Ordinary2.");
                            } else addDiagnostic(result.diagnostics,
                                CookDiagnosticSeverity::Warning,
                                "GLTF_TRANSPARENCY_QUALITY_UNKNOWN",
                                field + "/quality",
                                "Unknown transparency quality was normalized to Ordinary2.");
                        }
                        if (const auto found = rawPolicy.find("priority");
                            found != rawPolicy.end()) {
                            if (found->is_number_integer()) {
                                const int64_t priority = found->get<int64_t>();
                                policy.priority = static_cast<int32_t>(
                                    std::clamp(priority,
                                        static_cast<int64_t>(
                                            std::numeric_limits<int32_t>::min()),
                                        static_cast<int64_t>(
                                            std::numeric_limits<int32_t>::max())));
                                if (priority != policy.priority) {
                                    addDiagnostic(result.diagnostics,
                                        CookDiagnosticSeverity::Warning,
                                        "GLTF_TRANSPARENCY_PRIORITY_RANGE",
                                        field + "/priority",
                                        "Transparency priority was clamped to the signed 32-bit range.");
                                }
                            } else {
                                addDiagnostic(result.diagnostics,
                                    CookDiagnosticSeverity::Warning,
                                    "GLTF_TRANSPARENCY_PRIORITY_RANGE",
                                    field + "/priority",
                                    "Invalid transparency priority was normalized to zero.");
                            }
                        }
                        if (const auto found = rawPolicy.find(
                                "thin_sheet_thickness_m");
                            found != rawPolicy.end()) {
                            if (found->is_number()) {
                                const double thickness = found->get<double>();
                                if (std::isfinite(thickness) && thickness >= 0.0 &&
                                    thickness <= 1.0e6) {
                                    policy.thinSheetThicknessMeters =
                                        static_cast<float>(thickness);
                                } else {
                                    addDiagnostic(result.diagnostics,
                                        CookDiagnosticSeverity::Warning,
                                        "GLTF_TRANSPARENCY_THICKNESS_RANGE",
                                        field + "/thin_sheet_thickness_m",
                                        "Invalid thin-sheet thickness was normalized to zero metres.");
                                }
                            } else {
                                addDiagnostic(result.diagnostics,
                                    CookDiagnosticSeverity::Warning,
                                    "GLTF_TRANSPARENCY_THICKNESS_RANGE",
                                    field + "/thin_sheet_thickness_m",
                                    "Invalid thin-sheet thickness was normalized to zero metres.");
                            }
                        }
                        const std::set<std::string> policyFields{
                            "class", "priority", "quality",
                            "schema_version", "thin_sheet_thickness_m",
                        };
                        for (const auto& [policyKey, ignored] :
                            rawPolicy.items()) {
                            (void)ignored;
                            if (!policyFields.contains(policyKey)) {
                                addDiagnostic(result.diagnostics,
                                    CookDiagnosticSeverity::Warning,
                                    "GLTF_TRANSPARENCY_POLICY_FIELD_UNKNOWN",
                                    field + "/" + policyKey,
                                    "Unknown transparency policy field was ignored.");
                            }
                        }
                    }
                    normalized[guid->toString()] = {
                        { "class", transparencyClassSettingName(
                            policy.requestedClass) },
                        { "priority", policy.priority },
                        { "quality", std::string(
                            transparencyQualityName(policy.quality)) },
                        { "schema_version",
                            TransparencyPolicyV1::SchemaVersion },
                        { "thin_sheet_thickness_m",
                            policy.thinSheetThicknessMeters },
                    };
                }
                result.values[key] = std::move(normalized);
                continue;
            }
            if (!value.is_boolean()) {
                addError(result.diagnostics, "GLTF_SETTINGS_TYPE",
                    "/" + key, "glTF model setting must be boolean.");
                continue;
            }
            result.values[key] = value;
        }
        if (result.values["bake_node_transforms"] != true ||
            result.values["preserve_rt_geometry"] != true) {
            addError(result.diagnostics, "GLTF_SETTINGS_REQUIRED_CONTRACT", "/",
                "M3.4 requires baked node transforms and RT-preserving geometry.");
        }
        if (hasCookErrors(result.diagnostics)) return result;
        const CanonicalSettingsResult canonical =
            canonicalizeSettings(result.values);
        result.canonicalBytes = canonical.bytes;
        result.diagnostics.insert(result.diagnostics.end(),
            canonical.diagnostics.begin(), canonical.diagnostics.end());
        return result;
    }

    ParsedSourceAsset GltfModelImporter::parse(
        const ImportSource& input,
        const NormalizedImportSettings& settings) const {
        ParsedSourceAsset result;
        const auto cancelled =
            [&input] {
                if (input.stopToken
                        .stop_requested()) {
                    throw std::runtime_error(
                        "Asset import cancelled.");
                }
            };
        if (input.stopToken.stop_requested()) {
            addError(
                result.diagnostics,
                "GLTF_IMPORT_CANCELLED", "/",
                "Asset import cancelled.");
            return result;
        }
        if (!settings.valid()) {
            addError(result.diagnostics, "GLTF_SETTINGS_NOT_NORMALIZED", "/",
                "glTF source cannot parse with invalid settings.");
            return result;
        }

        try {
            cancelled();
            const nlohmann::json root =
                sourceJson(
                    input.bytes,
                    input.stopToken);
            discoverExternalDependencies(root, input.relativePath, result);
            validateExternalGltfDependencies(
                root, input);
            result.dependencies.push_back({
                .type = AssetDependencyType::Tool,
                .location = "fastgltf-0.9.0-iridium-vendor",
                .contentHash =
                    "f6f40933a21eb3b2a3e51457a0caecbff74936b6e665ea5af14cf16ca320eb9e",
            });
            if (hasCookErrors(result.diagnostics)) return result;
            if (input.metadataOnly) {
                discoverGltfMetadata(
                    root, input, result);
                std::sort(
                    result.dependencies.begin(),
                    result.dependencies.end());
                std::sort(
                    result.discoveredSubassets.begin(),
                    result.discoveredSubassets.end(),
                    [](const DiscoveredSubasset& lhs,
                        const DiscoveredSubasset& rhs) {
                        return lhs.sourceKey <
                            rhs.sourceKey;
                    });
                return result;
            }

            const SourceMaterialDocument materialDocument =
                importGltfSourceMaterials(input.resolvedPath);
            cancelled();
            for (const SourceMaterialDiagnostic& diagnostic :
                materialDocument.diagnostics()) {
                addDiagnostic(result.diagnostics,
                    diagnostic.severity == SourceDiagnosticSeverity::Error
                        ? CookDiagnosticSeverity::Error
                        : diagnostic.severity == SourceDiagnosticSeverity::Warning
                            ? CookDiagnosticSeverity::Warning
                            : CookDiagnosticSeverity::Info,
                    "M2_" + diagnostic.code, diagnostic.path,
                    diagnostic.message);
            }
            const MaterialCompileDocumentResult compiled =
                compileSourceMaterialDocument(
                    materialDocument, MaterialCompilePolicy::Strict);
            cancelled();
            if (!compiled.succeeded()) {
                for (const MaterialCompileDiagnostic& diagnostic :
                    compiled.diagnostics) {
                    addDiagnostic(result.diagnostics,
                        diagnostic.severity == MaterialCompileSeverity::Error
                            ? CookDiagnosticSeverity::Error
                            : diagnostic.severity ==
                                MaterialCompileSeverity::Warning
                                ? CookDiagnosticSeverity::Warning
                                : CookDiagnosticSeverity::Info,
                        "M2_" + diagnostic.code, "/materials",
                        diagnostic.message);
                }
            }
            if (hasCookErrors(result.diagnostics)) return result;

            auto data = fastgltf::GltfDataBuffer::FromBytes(
                input.bytes.data(), input.bytes.size());
            if (data.error() != fastgltf::Error::None) {
                throw std::runtime_error(
                    "fastgltf rejected the source data buffer.");
            }
            constexpr auto extensions =
                fastgltf::Extensions::KHR_materials_emissive_strength |
                fastgltf::Extensions::KHR_materials_transmission;
            fastgltf::Parser parser(extensions);
            auto loaded = parser.loadGltf(data.get(),
                input.resolvedPath.parent_path(),
                fastgltf::Options::LoadExternalBuffers |
                    fastgltf::Options::LoadExternalImages |
                    fastgltf::Options::GenerateMeshIndices);
            cancelled();
            if (loaded.error() != fastgltf::Error::None) {
                throw std::runtime_error(
                    "fastgltf parse failed with error " +
                    std::to_string(static_cast<int>(loaded.error())) + ".");
            }
            fastgltf::Asset& asset = loaded.get();
            if (asset.scenes.empty()) {
                throw std::runtime_error(
                    "glTF model contains no scene to cook.");
            }

            if (!asset.images.empty() &&
                std::ranges::none_of(
                    result.dependencies,
                    [](const AssetDependency&
                        dependency) {
                        return dependency.type ==
                                AssetDependencyType::Tool &&
                            dependency.location ==
                                kDirectXTexCodecId;
                    })) {
                result.dependencies.push_back({
                    .type =
                        AssetDependencyType::Tool,
                    .location =
                        kDirectXTexCodecId,
                    .contentHash =
                        kDirectXTexCodecContentHash,
                });
            }
            for (size_t imageIndex = 0;
                imageIndex < asset.images.size(); ++imageIndex) {
                cancelled();
                const std::span<const std::byte> bytes =
                    encodedImageBytes(asset, asset.images[imageIndex]);
                if (bytes.empty()) {
                    throw std::runtime_error(
                        "glTF image " + std::to_string(imageIndex) +
                        " has no readable encoded source payload.");
                }
                result.discoveredSubassets.push_back({
                    .assetType = "iridium.texture",
                    .sourceKey =
                        "images/" + std::to_string(imageIndex),
                    .structuralFingerprint = sha256(bytes),
                });
                const std::filesystem::path imagePath =
                    suggestedImagePath(root, imageIndex);
                result.subassetPayloads.push_back({
                    .sourceKey =
                        "images/" + std::to_string(imageIndex),
                    .suggestedPath = imagePath,
                    .bytes = {
                        bytes.begin(), bytes.end(),
                    },
                });
            }

            Json parsed{
                { "materials", Json::array() },
                { "primitives", Json::array() },
                { "schema", kParsedDocumentSchema },
            };
            for (size_t index = 0; index < compiled.materials.size(); ++index) {
                cancelled();
                const MaterialCompileResult& material = compiled.materials[index];
                if (!material.succeeded() || !material.material) {
                    throw std::runtime_error(
                        "M2 material compilation failed for material " +
                        std::to_string(index) + ".");
                }
                parsed["materials"].push_back({
                    { "compiled_product", hexEncode(
                        serializeCompiledMaterial(
                            *material.material)) },
                    { "content_hash", material.material->contentHash },
                    { "coverage", static_cast<uint32_t>(
                        sourceCoverage(material.material->standard.alphaMode)) },
                    { "double_sided", material.material->standard.doubleSided },
                    { "source_index", index },
                });
                result.discoveredSubassets.push_back({
                    .assetType = "iridium.material",
                    .sourceKey = "materials/" + std::to_string(index),
                    .structuralFingerprint =
                        material.material->contentHash,
                });
            }
            SourceMaterial defaultSourceMaterial;
            defaultSourceMaterial.name = "__gltf_default_material__";
            const MaterialCompileResult defaultCompiled =
                compileSourceMaterial(defaultSourceMaterial,
                    MaterialCompilePolicy::Strict);
            if (!defaultCompiled.succeeded() ||
                !defaultCompiled.material) {
                throw std::runtime_error(
                    "M2 default material compilation failed.");
            }
            parsed["materials"].push_back({
                { "compiled_product", hexEncode(
                    serializeCompiledMaterial(
                        *defaultCompiled.material)) },
                { "content_hash",
                    defaultCompiled.material->contentHash },
                { "coverage", static_cast<uint32_t>(ModelCoverage::Opaque) },
                { "double_sided", false },
                { "source_index", -1 },
            });

            const size_t sceneIndex = asset.defaultScene.value_or(0);
            if (sceneIndex >= asset.scenes.size()) {
                throw std::runtime_error(
                    "glTF default scene index is out of bounds.");
            }
            bool defaultMaterialUsed = false;
            fastgltf::iterateSceneNodes(asset, sceneIndex,
                fastgltf::math::fmat4x4(1.0f),
                [&](fastgltf::Node& node,
                    const fastgltf::math::fmat4x4& nodeMatrix) {
                    cancelled();
                    if (!node.meshIndex) return;
                    const size_t nodeIndex =
                        static_cast<size_t>(&node - asset.nodes.data());
                    const size_t meshIndex = *node.meshIndex;
                    if (meshIndex >= asset.meshes.size()) {
                        throw std::runtime_error(
                            "glTF node mesh index is out of bounds.");
                    }
                    const glm::mat4 transform =
                        glm::make_mat4(nodeMatrix.data());
                    Json transformJson = Json::array();
                    for (uint32_t column = 0; column < 4; ++column) {
                        for (uint32_t row = 0; row < 4; ++row) {
                            transformJson.push_back(transform[column][row]);
                        }
                    }

                    const fastgltf::Mesh& mesh = asset.meshes[meshIndex];
                    for (size_t primitiveIndex = 0;
                        primitiveIndex < mesh.primitives.size();
                        ++primitiveIndex) {
                        cancelled();
                        const fastgltf::Primitive& primitive =
                            mesh.primitives[primitiveIndex];
                        defaultMaterialUsed =
                            defaultMaterialUsed ||
                            !primitive.materialIndex.has_value();
                        const auto position = primitive.findAttribute("POSITION");
                        if (position == primitive.attributes.end() ||
                            position->accessorIndex >= asset.accessors.size()) {
                            throw std::runtime_error(
                                "glTF primitive has no valid POSITION accessor.");
                        }
                        const fastgltf::Accessor& positionAccessor =
                            asset.accessors[position->accessorIndex];
                        if (positionAccessor.type != fastgltf::AccessorType::Vec3 ||
                            positionAccessor.count == 0) {
                            throw std::runtime_error(
                                "glTF POSITION accessor must be a non-empty vec3.");
                        }

                        std::vector<WorkingVertex> vertices(positionAccessor.count);
                        fastgltf::iterateAccessorWithIndex<glm::vec3>(
                            asset, positionAccessor,
                            [&](glm::vec3 value, size_t vertexIndex) {
                                if ((vertexIndex &
                                        0xfffu) == 0) {
                                    cancelled();
                                }
                                vertices[vertexIndex].pos = value;
                            });
                        uint32_t attributeMask = ModelAttributePosition;
                        const auto readVec2 = [&](std::string_view name,
                            auto member, uint32_t bit) {
                            const auto found = primitive.findAttribute(name);
                            if (found == primitive.attributes.end()) return;
                            const fastgltf::Accessor& accessor =
                                asset.accessors.at(found->accessorIndex);
                            if (accessor.type != fastgltf::AccessorType::Vec2 ||
                                accessor.count != vertices.size()) {
                                throw std::runtime_error(
                                    std::string(name) +
                                    " accessor type/count does not match POSITION.");
                            }
                            fastgltf::iterateAccessorWithIndex<glm::vec2>(
                                asset, accessor,
                                [&](glm::vec2 value, size_t vertexIndex) {
                                    if ((vertexIndex &
                                            0xfffu) == 0) {
                                        cancelled();
                                    }
                                    vertices[vertexIndex].*member = value;
                                });
                            attributeMask |= bit;
                        };
                        readVec2("TEXCOORD_0", &WorkingVertex::uv0,
                            ModelAttributeTexCoord0);
                        readVec2("TEXCOORD_1", &WorkingVertex::uv1,
                            ModelAttributeTexCoord1);

                        if (const auto found =
                            primitive.findAttribute("NORMAL");
                            found != primitive.attributes.end()) {
                            const fastgltf::Accessor& accessor =
                                asset.accessors.at(found->accessorIndex);
                            if (accessor.type != fastgltf::AccessorType::Vec3 ||
                                accessor.count != vertices.size()) {
                                throw std::runtime_error(
                                    "NORMAL accessor type/count does not match POSITION.");
                            }
                            fastgltf::iterateAccessorWithIndex<glm::vec3>(
                                asset, accessor,
                                [&](glm::vec3 value, size_t vertexIndex) {
                                    if ((vertexIndex &
                                            0xfffu) == 0) {
                                        cancelled();
                                    }
                                    vertices[vertexIndex].normal = value;
                                });
                            attributeMask |= ModelAttributeNormal;
                        }
                        if (const auto found =
                            primitive.findAttribute("TANGENT");
                            found != primitive.attributes.end()) {
                            const fastgltf::Accessor& accessor =
                                asset.accessors.at(found->accessorIndex);
                            if (accessor.type != fastgltf::AccessorType::Vec4 ||
                                accessor.count != vertices.size()) {
                                throw std::runtime_error(
                                    "TANGENT accessor type/count does not match POSITION.");
                            }
                            fastgltf::iterateAccessorWithIndex<glm::vec4>(
                                asset, accessor,
                                [&](glm::vec4 value, size_t vertexIndex) {
                                    if ((vertexIndex &
                                            0xfffu) == 0) {
                                        cancelled();
                                    }
                                    vertices[vertexIndex].tangent = value;
                                });
                            attributeMask |= ModelAttributeTangent;
                        }
                        if (const auto found =
                            primitive.findAttribute("COLOR_0");
                            found != primitive.attributes.end()) {
                            const fastgltf::Accessor& accessor =
                                asset.accessors.at(found->accessorIndex);
                            if (accessor.count != vertices.size()) {
                                throw std::runtime_error(
                                    "COLOR_0 accessor count does not match POSITION.");
                            }
                            if (accessor.type == fastgltf::AccessorType::Vec4) {
                                fastgltf::iterateAccessorWithIndex<glm::vec4>(
                                    asset, accessor,
                                    [&](glm::vec4 value, size_t vertexIndex) {
                                        if ((vertexIndex &
                                                0xfffu) == 0) {
                                            cancelled();
                                        }
                                        vertices[vertexIndex].color = value;
                                    });
                            } else if (accessor.type ==
                                fastgltf::AccessorType::Vec3) {
                                fastgltf::iterateAccessorWithIndex<glm::vec3>(
                                    asset, accessor,
                                    [&](glm::vec3 value, size_t vertexIndex) {
                                        if ((vertexIndex &
                                                0xfffu) == 0) {
                                            cancelled();
                                        }
                                        vertices[vertexIndex].color =
                                            glm::vec4(value, 1.0f);
                                    });
                            } else {
                                throw std::runtime_error(
                                    "COLOR_0 accessor must be vec3 or vec4.");
                            }
                            attributeMask |= ModelAttributeColor0;
                        }
                        if (!std::ranges::all_of(vertices, finite)) {
                            throw std::runtime_error(
                                "glTF primitive contains non-finite vertex data.");
                        }
                        if (!primitive.indicesAccessor ||
                            *primitive.indicesAccessor >= asset.accessors.size()) {
                            throw std::runtime_error(
                                "fastgltf did not provide generated primitive indices.");
                        }
                        std::vector<uint32_t> indices;
                        const fastgltf::Accessor& indexAccessor =
                            asset.accessors[*primitive.indicesAccessor];
                        indices.reserve(indexAccessor.count);
                        fastgltf::iterateAccessor<uint32_t>(
                            asset, indexAccessor,
                            [&](uint32_t value) {
                                if ((indices.size() &
                                        0xfffu) == 0) {
                                    cancelled();
                                }
                                if (value >= vertices.size()) {
                                    throw std::runtime_error(
                                        "glTF primitive index exceeds POSITION count.");
                                }
                                indices.push_back(value);
                            });

                        Json vertexJson = Json::array();
                        for (const WorkingVertex& vertex : vertices) {
                            if ((vertexJson.size() &
                                    0xfffu) == 0) {
                                cancelled();
                            }
                            vertexJson.push_back(serializeWorkingVertex(vertex));
                        }
                        Json primitiveRecord{
                            { "attribute_mask", attributeMask },
                            { "indices", indices },
                            { "material_index", primitive.materialIndex
                                ? Json(*primitive.materialIndex) : Json(-1) },
                            { "source_mesh", meshIndex },
                            { "source_node", nodeIndex },
                            { "source_primitive", primitiveIndex },
                            { "topology", static_cast<uint32_t>(primitive.type) },
                            { "transform", transformJson },
                            { "vertices", std::move(vertexJson) },
                        };
                        Json fingerprintRecord = primitiveRecord;
                        fingerprintRecord.erase("source_mesh");
                        fingerprintRecord.erase("source_node");
                        fingerprintRecord.erase("source_primitive");
                        const int64_t fingerprintMaterialIndex =
                            primitiveRecord.at("material_index").get<int64_t>();
                        fingerprintRecord["material_content_hash"] =
                            fingerprintMaterialIndex < 0
                            ? "default"
                            : compiled.materials.at(
                                static_cast<size_t>(
                                    fingerprintMaterialIndex))
                                .material->contentHash;
                        fingerprintRecord.erase("material_index");
                        const std::string fingerprintText =
                            fingerprintRecord.dump();
                        result.discoveredSubassets.push_back({
                            .assetType = "iridium.model-primitive",
                            .sourceKey =
                                "nodes/" + std::to_string(nodeIndex) +
                                "/meshes/" + std::to_string(meshIndex) +
                                "/primitives/" +
                                std::to_string(primitiveIndex),
                            .structuralFingerprint = sha256(std::as_bytes(
                                std::span<const char>(
                                    fingerprintText.data(),
                                    fingerprintText.size()))),
                        });
                        parsed["primitives"].push_back(
                            std::move(primitiveRecord));
                    }
                });

            if (parsed["primitives"].empty()) {
                throw std::runtime_error(
                    "glTF scene contains no model primitives.");
            }
            if (defaultMaterialUsed) {
                result.discoveredSubassets.push_back({
                    .assetType = "iridium.material",
                    .sourceKey = "materials/default",
                    .structuralFingerprint = "gltf-default-material-v1",
                });
            }
            const std::string text = parsed.dump();
            cancelled();
            result.documentBytes.assign(
                reinterpret_cast<const std::byte*>(text.data()),
                reinterpret_cast<const std::byte*>(text.data() + text.size()));
            std::sort(result.dependencies.begin(), result.dependencies.end());
            std::sort(result.discoveredSubassets.begin(),
                result.discoveredSubassets.end(),
                [](const DiscoveredSubasset& lhs,
                    const DiscoveredSubasset& rhs) {
                    return lhs.sourceKey < rhs.sourceKey;
                });
        } catch (const std::exception& exception) {
            addError(result.diagnostics, "GLTF_SOURCE_PARSE",
                input.relativePath.generic_string(), exception.what());
        }
        return result;
    }

    CookProduct GltfModelImporter::cook(
        const ParsedSourceAsset& source,
        const NormalizedImportSettings& settings,
        const CookTarget& target,
        const AssetCookContext& context,
        std::stop_token stopToken) const {
        (void)target;
        CookProduct failed{
            .artifactType = "iridium.model",
            .artifactSchemaVersion = kCookedModelSchemaVersion,
        };
        const auto cancelled =
            [&stopToken] {
                if (stopToken.stop_requested()) {
                    throw std::runtime_error(
                        "glTF model cook was cancelled.");
                }
            };
        if (stopToken.stop_requested()) {
            addError(
                failed.diagnostics,
                "GLTF_COOK_CANCELLED", "/",
                "glTF model cook was cancelled.");
            return failed;
        }
        if (hasCookErrors(source.diagnostics) || !settings.valid() ||
            context.assetGuid.isNil()) {
            addError(failed.diagnostics, "GLTF_COOK_INPUT", "/",
                "glTF cook input, settings, or root asset GUID is invalid.");
            return failed;
        }

        try {
            cancelled();
            const Json parsed = Json::parse(
                reinterpret_cast<const char*>(source.documentBytes.data()),
                reinterpret_cast<const char*>(source.documentBytes.data() +
                    source.documentBytes.size()));
            if (parsed.at("schema").get<uint32_t>() !=
                kParsedDocumentSchema) {
                throw std::runtime_error(
                    "Parsed glTF intermediate schema is unsupported.");
            }
            const Json& materials = parsed.at("materials");
            CookedModelProductData data;
            const std::string requestedExecutionMode =
                settings.values.at("transparency_execution_mode")
                    .get<std::string>();
            data.manifest.transparencyExecutionMode =
                requestedExecutionMode == "classified"
                    ? TransparencyExecutionMode::Classified
                    : TransparencyExecutionMode::LegacyTwoBucket;
            TextureImporter textureImporter;
            std::map<std::string, uint32_t>
                textureViewIndices;
            std::vector<EmbeddedTextureViewJob>
                textureViewJobs;

            std::set<int64_t> usedMaterialIndices;
            for (const Json& primitive :
                parsed.at("primitives")) {
                cancelled();
                usedMaterialIndices.insert(
                    primitive.at("material_index")
                        .get<int64_t>());
            }
            uint64_t completedMaterials = 0;
            reportCookProgress(context, "materials", 0, materials.size(),
                "Compiling model materials and texture recipes");
            for (const Json& raw : materials) {
                cancelled();
                ++completedMaterials;
                const int64_t sourceIndex =
                    raw.at("source_index").get<int64_t>();
                if (sourceIndex < 0 &&
                    !usedMaterialIndices.contains(sourceIndex)) {
                    if (progressCheckpoint(completedMaterials,
                            materials.size(), 4)) {
                        reportCookProgress(context, "materials",
                            completedMaterials, materials.size(),
                            "Skipped unused default material");
                    }
                    continue;
                }
                const std::string materialKey = sourceIndex < 0
                    ? "materials/default"
                    : "materials/" +
                        std::to_string(sourceIndex);
                const std::optional<AssetGuid> materialGuid =
                    subassetGuid(context, materialKey);
                if (!materialGuid) {
                    addError(failed.diagnostics,
                        "GLTF_MATERIAL_GUID_MISSING",
                        materialKey,
                        "Persist this material source key in the asset metadata sidecar.");
                    continue;
                }
                const std::vector<std::byte> compiledBytes =
                    hexDecode(raw.at("compiled_product")
                        .get<std::string>());
                const CompiledMaterialReadResult compiled =
                    readCompiledMaterial(compiledBytes);
                if (!compiled.valid()) {
                    addError(failed.diagnostics,
                        "GLTF_COMPILED_MATERIAL_INVALID",
                        materialKey,
                        "Parsed material payload failed canonical closure validation.");
                    continue;
                }

                CompiledMaterial compiledMaterial =
                    *compiled.material;
                if (const auto policy = transparencyPolicyFor(
                        settings, *materialGuid)) {
                    MaterialCompileResult applied =
                        applyCompiledTransparencyPolicy(
                            compiledMaterial, *policy,
                            TransparencyTopology::Unknown);
                    for (const MaterialCompileDiagnostic& diagnostic :
                        applied.diagnostics) {
                        addDiagnostic(failed.diagnostics,
                            diagnostic.severity ==
                                    MaterialCompileSeverity::Error
                                ? CookDiagnosticSeverity::Error
                                : diagnostic.severity ==
                                    MaterialCompileSeverity::Warning
                                    ? CookDiagnosticSeverity::Warning
                                    : CookDiagnosticSeverity::Info,
                            "M6_" + diagnostic.code,
                            "/transparency_policies/" +
                                materialGuid->toString(),
                            diagnostic.message);
                    }
                    if (!applied.succeeded()) {
                        addError(failed.diagnostics,
                            "GLTF_TRANSPARENCY_POLICY_COMPILE",
                            materialKey,
                            "Material transparency policy could not be compiled.");
                        continue;
                    }
                    compiledMaterial = *applied.material;
                }

                CookedModelMaterial material{
                    .materialGuid = *materialGuid,
                    .sourceKey = materialKey,
                    .compiled = std::move(compiledMaterial),
                };
                for (size_t operationIndex = 0;
                    operationIndex <
                        material.compiled.textureOperations.size();
                    ++operationIndex) {
                    cancelled();
                    const CompiledTextureOperation& operation =
                        material.compiled.textureOperations[
                            operationIndex];
                    if (!operation.sourceImageIndex) {
                        addError(failed.diagnostics,
                            "GLTF_TEXTURE_IMAGE_MISSING",
                            materialKey + "/texture_operations/" +
                                std::to_string(operationIndex),
                            "glTF material texture operation has no source image identity.");
                        continue;
                    }
                    const std::string imageKey =
                        "images/" + std::to_string(
                            *operation.sourceImageIndex);
                    const std::optional<AssetGuid> textureGuid =
                        subassetGuid(context, imageKey);
                    if (!textureGuid) {
                        addError(failed.diagnostics,
                            "GLTF_IMAGE_GUID_MISSING",
                            imageKey,
                            "Persist this image source key in the asset metadata sidecar.");
                        continue;
                    }
                    const auto imagePayload =
                        std::ranges::find_if(
                            source.subassetPayloads,
                            [&imageKey](
                                const ParsedSourceAsset::
                                    SubassetPayload& payload) {
                                return payload.sourceKey ==
                                    imageKey;
                            });
                    if (imagePayload ==
                        source.subassetPayloads.end()) {
                        addError(failed.diagnostics,
                            "GLTF_IMAGE_PAYLOAD_MISSING",
                            imageKey,
                            "Parsed image payload is unavailable for texture cooking.");
                        continue;
                    }

                    const TextureSemantic semantic =
                        cookedTextureSemantic(
                            operation.semantic);
                    const bool coverageSemantic =
                        operation.semantic ==
                            SourceTextureSemantic::BaseColor ||
                        operation.semantic ==
                            SourceTextureSemantic::Diffuse;
                    const char* alphaMode = "opaque";
                    if (coverageSemantic &&
                        material.compiled.standard.alphaMode ==
                            SourceAlphaMode::Mask) {
                        alphaMode = "coverage";
                    } else if (coverageSemantic &&
                        material.compiled.standard.alphaMode ==
                            SourceAlphaMode::Blend) {
                        alphaMode = "straight";
                    }
                    const Json textureSettings{
                        { "alpha_coverage_threshold",
                            material.compiled.standard.alphaCutoff },
                        { "alpha_mode", alphaMode },
                        { "flip_green", false },
                        { "mip_policy", "full_chain" },
                        { "quality",
                            target.profile ==
                                "editor"
                                ? "iteration"
                                : target.qualityPolicy ==
                                "production"
                                ? "production"
                                : "iteration" },
                        { "reconstruct_normal_z",
                            semantic ==
                                TextureSemantic::Normal },
                        { "semantic",
                            textureSemanticSetting(semantic) },
                        { "view_color_space",
                            operation.transfer ==
                                SourceTextureTransfer::Srgb
                                ? "srgb" : "linear" },
                    };
                    const NormalizedImportSettings normalized =
                        textureImporter.normalizeSettings(
                            1, textureSettings, true);
                    if (!normalized.valid()) {
                        addError(failed.diagnostics,
                            "GLTF_TEXTURE_SETTINGS",
                            imageKey,
                            "Derived texture-view settings are invalid.");
                        continue;
                    }
                    const std::string recipeKey =
                        textureGuid->toString() + ":" +
                        sha256(normalized.canonicalBytes);
                    uint32_t textureViewIndex = 0;
                    if (const auto existing =
                        textureViewIndices.find(recipeKey);
                        existing !=
                            textureViewIndices.end()) {
                        textureViewIndex =
                            existing->second;
                    } else {
                        const std::string derivedCookKey =
                            embeddedTextureCookKey(
                                textureImporter, *textureGuid,
                                *imagePayload, normalized,
                                target);
                        textureViewIndex = static_cast<uint32_t>(
                            data.textureViews.size());
                        data.textureViews.emplace_back();
                        textureViewJobs.push_back({
                            .outputIndex = textureViewIndex,
                            .textureGuid = *textureGuid,
                            .sourceImageIndex =
                                *operation.sourceImageIndex,
                            .imagePayload = &*imagePayload,
                            .settings = normalized,
                            .derivedCookKey = derivedCookKey,
                            .imageKey = imageKey,
                        });
                        textureViewIndices.emplace(
                            recipeKey, textureViewIndex);
                    }
                    material.textureBindings.push_back({
                        .operationIndex =
                            static_cast<uint32_t>(
                                operationIndex),
                        .sourceImageIndex =
                            *operation.sourceImageIndex,
                        .textureViewIndex =
                            textureViewIndex,
                        .textureGuid = *textureGuid,
                    });
                }
                data.materials.push_back(std::move(material));
                if (progressCheckpoint(completedMaterials,
                        materials.size(), 4)) {
                    reportCookProgress(context, "materials",
                        completedMaterials, materials.size(),
                        materialKey);
                }
            }
            cookEmbeddedTextureViews(data, textureViewJobs,
                target, context, stopToken,
                failed.diagnostics);
            if (hasCookErrors(failed.diagnostics)) return failed;

            const Json& primitives = parsed.at("primitives");
            uint64_t completedPrimitives = 0;
            reportCookProgress(context, "geometry", 0,
                primitives.size(),
                "Converting model geometry");
            for (const Json& raw : primitives) {
                cancelled();
                const uint32_t sourceNode = raw.at("source_node").get<uint32_t>();
                const uint32_t sourceMesh = raw.at("source_mesh").get<uint32_t>();
                const uint32_t sourcePrimitive =
                    raw.at("source_primitive").get<uint32_t>();
                const std::string primitiveKey =
                    "nodes/" + std::to_string(sourceNode) + "/meshes/" +
                    std::to_string(sourceMesh) + "/primitives/" +
                    std::to_string(sourcePrimitive);
                const std::optional<AssetGuid> primitiveGuid =
                    subassetGuid(context, primitiveKey);
                if (!primitiveGuid) {
                    addError(failed.diagnostics,
                        "GLTF_PRIMITIVE_GUID_MISSING", primitiveKey,
                        "Persist this primitive source key in the asset metadata sidecar.");
                    continue;
                }

                const int64_t materialIndex =
                    raw.at("material_index").get<int64_t>();
                const std::string materialKey = materialIndex < 0
                    ? "materials/default"
                    : "materials/" + std::to_string(materialIndex);
                const std::optional<AssetGuid> materialGuid =
                    subassetGuid(context, materialKey);
                if (!materialGuid) {
                    addError(failed.diagnostics,
                        "GLTF_MATERIAL_GUID_MISSING", materialKey,
                        "Persist this material source key in the asset metadata sidecar.");
                    continue;
                }
                const Json* material = nullptr;
                for (const Json& candidate : materials) {
                    cancelled();
                    if (candidate.at("source_index").get<int64_t>() ==
                        materialIndex) {
                        material = &candidate;
                        break;
                    }
                }
                if (!material) {
                    throw std::runtime_error(
                        "Parsed primitive material record is missing.");
                }
                const ModelCoverage coverage = coverageFromInteger(
                    material->at("coverage").get<uint32_t>());
                const bool doubleSided =
                    material->at("double_sided").get<bool>();
                const auto cookedMaterial = std::ranges::find_if(
                    data.materials,
                    [&materialGuid](
                        const CookedModelMaterial& candidate) {
                        return candidate.materialGuid ==
                            *materialGuid;
                    });
                if (cookedMaterial == data.materials.end()) {
                    throw std::runtime_error(
                        "Parsed primitive compiled material is missing.");
                }
                CompiledTransparencyPolicy primitiveTransparency =
                    cookedMaterial->compiled.transparency;
                TransparencyPolicyV1 effectiveTransparencyPolicy{
                    .requestedClass = primitiveTransparency.requestedClass,
                    .quality = primitiveTransparency.quality,
                    .priority = primitiveTransparency.priority,
                    .thinSheetThicknessMeters =
                        primitiveTransparency.thinSheetThicknessMeters,
                };
                if (const auto policy = transparencyPolicyFor(
                        settings, *primitiveGuid)) {
                    effectiveTransparencyPolicy = *policy;
                    const MaterialCompileResult applied =
                        applyCompiledTransparencyPolicy(
                            cookedMaterial->compiled, *policy,
                            TransparencyTopology::Unknown);
                    for (const MaterialCompileDiagnostic& diagnostic :
                        applied.diagnostics) {
                        addDiagnostic(failed.diagnostics,
                            diagnostic.severity ==
                                    MaterialCompileSeverity::Error
                                ? CookDiagnosticSeverity::Error
                                : diagnostic.severity ==
                                    MaterialCompileSeverity::Warning
                                    ? CookDiagnosticSeverity::Warning
                                    : CookDiagnosticSeverity::Info,
                            "M6_" + diagnostic.code,
                            "/transparency_policies/" +
                                primitiveGuid->toString(),
                            diagnostic.message);
                    }
                    if (!applied.succeeded()) {
                        addError(failed.diagnostics,
                            "GLTF_TRANSPARENCY_PRIMITIVE_POLICY_COMPILE",
                            primitiveKey,
                            "Primitive transparency policy could not be compiled.");
                        continue;
                    }
                    primitiveTransparency =
                        applied.material->transparency;
                }

                const Json& transformValues = raw.at("transform");
                if (!transformValues.is_array() ||
                    transformValues.size() != 16) {
                    throw std::runtime_error(
                        "Parsed primitive transform is invalid.");
                }
                glm::mat4 transform(1.0f);
                size_t transformIndex = 0;
                for (uint32_t column = 0; column < 4; ++column) {
                    for (uint32_t row = 0; row < 4; ++row) {
                        transform[column][row] =
                            transformValues[transformIndex++].get<float>();
                    }
                }
                const float importScale =
                    settings.values.at(
                        "import_scale").get<float>();
                transform = glm::scale(
                    glm::mat4(1.0f),
                    glm::vec3(importScale)) *
                    transform;
                const glm::mat3 linear(transform);
                const float determinant = glm::determinant(linear);
                if (!std::isfinite(determinant) ||
                    std::abs(determinant) <= 1.0e-12f) {
                    throw std::runtime_error(
                        "Primitive node transform is singular or non-finite.");
                }
                const bool mirrored = determinant < 0.0f;
                const glm::mat3 normalTransform =
                    glm::transpose(glm::inverse(linear));

                std::vector<WorkingVertex> vertices;
                for (const Json& vertex : raw.at("vertices")) {
                    if ((vertices.size() &
                            0xfffu) == 0) {
                        cancelled();
                    }
                    vertices.push_back(readWorkingVertex(vertex));
                }
                std::vector<uint32_t> sourceIndices =
                    raw.at("indices").get<std::vector<uint32_t>>();
                const auto sourceTopology = static_cast<fastgltf::PrimitiveType>(
                    raw.at("topology").get<uint32_t>());
                std::vector<uint32_t> localIndices =
                    canonicalTriangleIndices(sourceTopology, sourceIndices);
                const bool reverseWinding =
                    settings.values.at("reverse_winding").get<bool>();
                std::vector<uint32_t> repairIndices = localIndices;
                if (reverseWinding) {
                    for (size_t triangle = 0;
                        triangle + 2 < repairIndices.size();
                        triangle += 3) {
                        std::swap(repairIndices[triangle + 1],
                            repairIndices[triangle + 2]);
                    }
                    addDiagnostic(
                        failed.diagnostics,
                        CookDiagnosticSeverity::Warning,
                        "GLTF_WINDING_REVERSED",
                        primitiveKey,
                        "The source triangle winding was reversed by import settings.");
                }
                for (uint32_t index : localIndices) {
                    if ((data.indices.size() &
                            0xfffu) == 0) {
                        cancelled();
                    }
                    if (index >= vertices.size()) {
                        throw std::runtime_error(
                            "Canonical primitive index exceeds vertex count.");
                    }
                }

                uint32_t attributeMask =
                    raw.at("attribute_mask").get<uint32_t>();
                uint32_t primitiveFlags =
                    doubleSided ? ModelPrimitiveDoubleSided : 0;
                if (mirrored) {
                    primitiveFlags |= ModelPrimitiveMirroredTransform;
                }
                const bool missingNormals =
                    (attributeMask &
                        ModelAttributeNormal) == 0;
                const bool invalidNormals =
                    std::ranges::any_of(
                        vertices,
                        [](const WorkingVertex&
                            vertex) {
                            return !usableDirection(
                                vertex.normal);
                        });
                const bool recalculateNormals =
                    settings.values.at("recalculate_normals").get<bool>();
                if (missingNormals || invalidNormals ||
                    recalculateNormals) {
                    repairPrimitiveNormals(
                        std::span(vertices),
                        std::span<const uint32_t>(
                            repairIndices),
                        missingNormals || recalculateNormals);
                    attributeMask |=
                        ModelAttributeNormal;
                    addDiagnostic(
                        failed.diagnostics,
                        CookDiagnosticSeverity::Warning,
                        "GLTF_NORMAL_REGENERATED",
                        primitiveKey,
                        "Missing or degenerate vertex normals were regenerated deterministically.");
                }

                bool regenerateTangents =
                    settings.values.at(
                        "recalculate_tangents").get<bool>() ||
                    (attributeMask & ModelAttributeTangent) == 0;
                if (!regenerateTangents) {
                    regenerateTangents =
                        std::ranges::any_of(
                            vertices,
                            [&](const WorkingVertex&
                                vertex) {
                                const glm::vec3
                                    transformedNormal =
                                        normalTransform *
                                        vertex.normal;
                                if (!usableDirection(
                                        transformedNormal)) {
                                    return true;
                                }
                                const glm::vec3 normal =
                                    glm::normalize(
                                        transformedNormal);
                                glm::vec3 tangent =
                                    linear *
                                    glm::vec3(
                                        vertex.tangent);
                                tangent -= normal *
                                    glm::dot(
                                        normal,
                                        tangent);
                                return !usableDirection(
                                    tangent);
                            });
                }
                if (regenerateTangents) {
                    if (settings.values.at("generate_missing_tangents") !=
                        true) {
                        throw std::runtime_error(
                            "Primitive has no usable tangent and generation is disabled.");
                    }
                    generateMikkCompatibleTangents(
                        std::span(vertices), std::span<const uint32_t>(
                            repairIndices));
                    attributeMask |= ModelAttributeTangent;
                    primitiveFlags |= ModelPrimitiveGeneratedTangent;
                    addDiagnostic(
                        failed.diagnostics,
                        CookDiagnosticSeverity::Warning,
                        "GLTF_TANGENT_REGENERATED",
                        primitiveKey,
                        "Missing or degenerate vertex tangents were regenerated deterministically.");
                }

                for (WorkingVertex& vertex : vertices) {
                    if ((static_cast<size_t>(
                            &vertex -
                            vertices.data()) &
                            0xfffu) == 0) {
                        cancelled();
                    }
                    const glm::vec4 position =
                        transform * glm::vec4(vertex.pos, 1.0f);
                    vertex.pos = glm::vec3(position);
                    vertex.normal =
                        glm::normalize(normalTransform * vertex.normal);
                    glm::vec3 tangent = linear * glm::vec3(vertex.tangent);
                    tangent -= vertex.normal *
                        glm::dot(vertex.normal, tangent);
                    if (!std::isfinite(glm::dot(tangent, tangent)) ||
                        glm::dot(tangent, tangent) <= 1.0e-12f) {
                        throw std::runtime_error(
                            "Primitive tangent became degenerate after transform.");
                    }
                    tangent = glm::normalize(tangent);
                    vertex.tangent = glm::vec4(tangent,
                        vertex.tangent.w * (mirrored ? -1.0f : 1.0f));
                    if (!finite(vertex)) {
                        throw std::runtime_error(
                            "Cooked primitive contains non-finite vertex data.");
                    }
                }
                std::vector<glm::vec3> topologyPositions;
                topologyPositions.reserve(vertices.size());
                for (const WorkingVertex& vertex : vertices)
                    topologyPositions.push_back(vertex.pos);
                // Vulkan production pipelines use one clockwise front-face
                // convention. glTF defines counter-clockwise source faces; baked
                // mirrored transforms and the explicit repair override may change
                // that effective orientation. Reverse only the stored index order
                // needed to preserve the chosen semantic front face under the
                // engine's canonical pipeline convention.
                std::vector<uint32_t> runtimeIndices = localIndices;
                const bool reverseForCanonicalWinding =
                    (!mirrored) != reverseWinding;
                if (reverseForCanonicalWinding) {
                    for (size_t triangle = 0;
                        triangle + 2 < runtimeIndices.size();
                        triangle += 3) {
                        std::swap(runtimeIndices[triangle + 1],
                            runtimeIndices[triangle + 2]);
                    }
                }
                std::vector<TriangleConnectedComponent> components;
                const bool splitTransparent = transparentConnectedWork(
                    coverage, primitiveTransparency.resolvedClass);
                if (splitTransparent) {
                    components = findTriangleConnectedComponents(runtimeIndices);
                }
                else {
                    TriangleConnectedComponent component;
                    component.sourceTriangleIndices.resize(
                        runtimeIndices.size() / 3);
                    std::iota(component.sourceTriangleIndices.begin(),
                        component.sourceTriangleIndices.end(), 0u);
                    components.push_back(std::move(component));
                }
                if (splitTransparent && components.size() > 1) {
                    addDiagnostic(failed.diagnostics,
                        CookDiagnosticSeverity::Info,
                        "GLTF_TRANSPARENT_COMPONENT_SPLIT", primitiveKey,
                        "Disconnected transparent triangles were split into stable work items.");
                }

                for (const TriangleConnectedComponent& component : components) {
                    cancelled();
                    const std::string connectedKey = splitTransparent
                        ? primitiveKey + "/components/" +
                            std::to_string(component.sourceTriangleSeed)
                        : primitiveKey;
                    const bool volumeMaterial =
                        (cookedMaterial->compiled.featureFlags &
                            MaterialFeatureVolume) != 0;
                    const bool layeredCandidate = volumeMaterial &&
                        (effectiveTransparencyPolicy.requestedClass ==
                                TransparencyClass::Auto ||
                            effectiveTransparencyPolicy.requestedClass ==
                                TransparencyClass::LayeredGlass);
                    ClosedTriangleTopologyAnalysis topologyAnalysis;
                    TransparencyTopology topology =
                        TransparencyTopology::Unknown;
                    if (layeredCandidate) {
                        topologyAnalysis = analyzeClosedTriangleTopology(
                            topologyPositions, runtimeIndices,
                            component.sourceTriangleIndices);
                        topology = topologyAnalysis.validClosed()
                            ? TransparencyTopology::ValidClosed
                            : TransparencyTopology::Invalid;
                    }
                    else if (effectiveTransparencyPolicy.requestedClass ==
                            TransparencyClass::LayeredGlass) {
                        topology = TransparencyTopology::Invalid;
                    }
                    const MaterialCompileResult topologyApplied =
                        applyCompiledTransparencyPolicy(
                            cookedMaterial->compiled,
                            effectiveTransparencyPolicy, topology);
                    for (const MaterialCompileDiagnostic& diagnostic :
                            topologyApplied.diagnostics) {
                        addDiagnostic(failed.diagnostics,
                            diagnostic.severity ==
                                    MaterialCompileSeverity::Error
                                ? CookDiagnosticSeverity::Error
                                : diagnostic.severity ==
                                    MaterialCompileSeverity::Warning
                                    ? CookDiagnosticSeverity::Warning
                                    : CookDiagnosticSeverity::Info,
                            "M6_" + diagnostic.code, connectedKey,
                            diagnostic.message);
                    }
                    if (!topologyApplied.succeeded()) {
                        addError(failed.diagnostics,
                            "GLTF_TRANSPARENCY_TOPOLOGY_POLICY_COMPILE",
                            connectedKey,
                            "Connected primitive transparency policy could not be resolved after topology validation.");
                        continue;
                    }
                    const CompiledTransparencyPolicy componentTransparency =
                        topologyApplied.material->transparency;
                    if (layeredCandidate) {
                        if (topologyAnalysis.validClosed()) {
                            addDiagnostic(failed.diagnostics,
                                CookDiagnosticSeverity::Info,
                                "GLTF_TRANSPARENCY_CLOSED_TOPOLOGY",
                                connectedKey,
                                "Connected primitive is a consistently oriented closed manifold and resolved to LayeredGlass.");
                        }
                        else {
                            addDiagnostic(failed.diagnostics,
                                CookDiagnosticSeverity::Warning,
                                "GLTF_TRANSPARENCY_LAYERED_TOPOLOGY_INVALID",
                                connectedKey,
                                "LayeredGlass topology proof failed (boundary=" +
                                    std::to_string(topologyAnalysis.boundaryEdgeCount) +
                                    ", nonmanifold=" +
                                    std::to_string(topologyAnalysis.nonManifoldEdgeCount) +
                                    ", orientation=" +
                                    std::to_string(topologyAnalysis.inconsistentOrientationEdgeCount) +
                                    ", degenerate=" +
                                    std::to_string(topologyAnalysis.degenerateTriangleCount) +
                                    "); ThinGlass remains the safe execution class.");
                        }
                    }
                    const uint64_t firstVertex = data.vertices.size();
                    const uint64_t firstIndex = data.indices.size();
                    const uint64_t rtFirstPosition = data.rtPositions.size();
                    const uint64_t rtFirstIndex = data.rtIndices.size();
                    std::vector<uint32_t> remap(vertices.size(),
                        std::numeric_limits<uint32_t>::max());
                    glm::vec3 boundsMin(std::numeric_limits<float>::max());
                    glm::vec3 boundsMax(std::numeric_limits<float>::lowest());

                    std::vector<bool> referenced(vertices.size(), false);
                    for (uint32_t sourceTriangle :
                            component.sourceTriangleIndices) {
                        for (uint32_t corner = 0; corner < 3; ++corner)
                            referenced[runtimeIndices[
                                sourceTriangle * 3 + corner]] = true;
                    }
                    for (uint32_t sourceIndex = 0;
                        sourceIndex < referenced.size(); ++sourceIndex) {
                        if (!referenced[sourceIndex]) continue;
                        if (data.vertices.size() - firstVertex >=
                            std::numeric_limits<uint32_t>::max()) {
                            throw std::runtime_error(
                                "Connected primitive exceeds 32-bit vertex limits.");
                        }
                        remap[sourceIndex] = static_cast<uint32_t>(
                            data.vertices.size() - firstVertex);
                        const WorkingVertex& vertex = vertices[sourceIndex];
                        boundsMin = glm::min(boundsMin, vertex.pos);
                        boundsMax = glm::max(boundsMax, vertex.pos);
                        data.vertices.push_back({
                            .position = { vertex.pos.x, vertex.pos.y,
                                vertex.pos.z },
                            .color = { vertex.color.x, vertex.color.y,
                                vertex.color.z, vertex.color.w },
                            .normal = { vertex.normal.x, vertex.normal.y,
                                vertex.normal.z },
                            .texCoord0 = { vertex.uv0.x, vertex.uv0.y },
                            .tangent = { vertex.tangent.x,
                                vertex.tangent.y, vertex.tangent.z,
                                vertex.tangent.w },
                            .texCoord1 = { vertex.uv1.x, vertex.uv1.y },
                        });
                        data.rtPositions.push_back({ vertex.pos.x,
                            vertex.pos.y, vertex.pos.z });
                    }
                    for (uint32_t sourceTriangle :
                            component.sourceTriangleIndices) {
                        for (uint32_t corner = 0; corner < 3; ++corner) {
                            const uint32_t sourceIndex =
                                runtimeIndices[sourceTriangle * 3 + corner];
                            const uint32_t localIndex = remap[sourceIndex];
                            if (firstVertex + localIndex >
                                std::numeric_limits<uint32_t>::max()) {
                                throw std::runtime_error(
                                    "Cooked model exceeds 32-bit production index limits.");
                            }
                            data.indices.push_back(static_cast<uint32_t>(
                                firstVertex + localIndex));
                            data.rtIndices.push_back(localIndex);
                        }
                    }

                    const glm::vec3 center =
                        (boundsMin + boundsMax) * 0.5f;
                    const float radius = glm::length(boundsMax - center);
                    const AssetGuid connectedGuid = splitTransparent
                        ? connectedPrimitiveGuid(*primitiveGuid,
                            component.sourceTriangleSeed)
                        : *primitiveGuid;
                    data.manifest.primitives.push_back({
                        .sourcePrimitiveGuid = *primitiveGuid,
                        .primitiveGuid = connectedGuid,
                        .materialGuid = *materialGuid,
                        .sourceKey = connectedKey,
                        .sourceNode = sourceNode,
                        .sourceMesh = sourceMesh,
                        .sourcePrimitive = sourcePrimitive,
                        .attributeMask = attributeMask,
                        .firstVertex = firstVertex,
                        .vertexCount = data.vertices.size() - firstVertex,
                        .firstIndex = firstIndex,
                        .indexCount = data.indices.size() - firstIndex,
                        .rtFirstPosition = rtFirstPosition,
                        .rtPositionCount = data.rtPositions.size() -
                            rtFirstPosition,
                        .rtFirstIndex = rtFirstIndex,
                        .rtIndexCount = data.rtIndices.size() - rtFirstIndex,
                        .topology = ModelPrimitiveTopology::Triangles,
                        .winding = ModelWinding::Clockwise,
                        .coverage = coverage,
                        .indexFormat = ModelIndexFormat::UInt32,
                        .flags = primitiveFlags,
                        .rtFlags = ModelRtBuildInput |
                            (coverage == ModelCoverage::Opaque
                                ? ModelRtOpaque : ModelRtAllowAnyHit),
                        .transparency = componentTransparency,
                        .bounds = {
                            .aabbMin = { boundsMin.x, boundsMin.y,
                                boundsMin.z },
                            .aabbMax = { boundsMax.x, boundsMax.y,
                                boundsMax.z },
                            .sphereCenter = { center.x, center.y, center.z },
                            .sphereRadius = radius,
                        },
                    });
                }
                ++completedPrimitives;
                if (progressCheckpoint(completedPrimitives,
                        primitives.size(), 10)) {
                    reportCookProgress(context, "geometry",
                        completedPrimitives, primitives.size(),
                        primitiveKey);
                }
            }
            if (hasCookErrors(failed.diagnostics)) return failed;
            data.manifest.vertexCount = data.vertices.size();
            data.manifest.indexCount = data.indices.size();
            data.manifest.rtPositionCount = data.rtPositions.size();
            data.manifest.rtIndexCount = data.rtIndices.size();
            reportCookProgress(context, "model-product", 0, 1,
                "Serializing model sections");
            CookProduct product =
                makeCookedModelProduct(data);
            product.diagnostics.insert(
                product.diagnostics.end(),
                failed.diagnostics.begin(),
                failed.diagnostics.end());
            reportCookProgress(context, "model-product", 1, 1,
                "Model sections serialized");
            return product;
        } catch (const std::exception& exception) {
            addError(failed.diagnostics, "GLTF_CPU_COOK", "/",
                exception.what());
            return failed;
        }
    }

} // namespace Iridium
