#include "assets/model/GltfModelImporter.h"

#include "assets/AssetSourceValidation.h"
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
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

    } // namespace

    const ImporterDescriptor& GltfModelImporter::descriptor() const noexcept {
        static const ImporterDescriptor descriptor{
            .id = "iridium.gltf-model",
            .implementationVersion = kGltfModelImporterVersion,
            .currentSettingsSchemaVersion = 1,
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
        if (sourceSchemaVersion != 1 || !settings.is_object()) {
            addError(result.diagnostics, "GLTF_SETTINGS_SCHEMA", "/",
                "glTF model settings must use object schema 1.");
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
        };
        const std::set<std::string> known{
            "bake_node_transforms",
            "generate_missing_tangents",
            "import_scale",
            "preserve_rt_geometry",
            "recalculate_normals",
            "recalculate_tangents",
            "reverse_winding",
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
            TextureImporter textureImporter;
            std::map<std::string, uint32_t>
                textureViewIndices;

            std::set<int64_t> usedMaterialIndices;
            for (const Json& primitive :
                parsed.at("primitives")) {
                cancelled();
                usedMaterialIndices.insert(
                    primitive.at("material_index")
                        .get<int64_t>());
            }
            for (const Json& raw : materials) {
                cancelled();
                const int64_t sourceIndex =
                    raw.at("source_index").get<int64_t>();
                if (sourceIndex < 0 &&
                    !usedMaterialIndices.contains(sourceIndex)) {
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

                CookedModelMaterial material{
                    .materialGuid = *materialGuid,
                    .sourceKey = materialKey,
                    .compiled = *compiled.material,
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
                        ParsedSourceAsset parsedTexture;
                        if (!imagePayload->
                                parsedBytes.empty()) {
                            parsedTexture.documentBytes =
                                imagePayload->
                                    parsedBytes;
                        }
                        else {
                            parsedTexture =
                                textureImporter.parse({
                                    .relativePath =
                                        imagePayload->
                                            suggestedPath,
                                    .resolvedPath = {},
                                    .bytes =
                                        imagePayload->
                                            bytes,
                                    .stopToken =
                                        stopToken,
                                }, normalized);
                            if (hasCookErrors(
                                    parsedTexture
                                        .diagnostics)) {
                                std::string detail;
                                for (const CookDiagnostic&
                                    diagnostic :
                                    parsedTexture
                                        .diagnostics) {
                                    if (diagnostic
                                            .severity ==
                                        CookDiagnosticSeverity::
                                            Error) {
                                        detail =
                                            diagnostic.code +
                                            ": " +
                                            diagnostic
                                                .message;
                                        break;
                                    }
                                }
                                addError(
                                    failed.diagnostics,
                                    "GLTF_TEXTURE_PARSE",
                                    imageKey,
                                    "Image subasset failed deterministic source decoding" +
                                        (detail.empty()
                                            ? std::string(
                                                ".")
                                            : ": " +
                                                detail));
                                continue;
                            }
                        }
                        const CookProduct textureProduct =
                            textureImporter.cook(
                                parsedTexture, normalized,
                                target, {
                                    .assetGuid =
                                        *textureGuid,
                                }, stopToken);
                        if (hasCookErrors(
                            textureProduct.diagnostics)) {
                            addError(failed.diagnostics,
                                "GLTF_TEXTURE_COOK",
                                imageKey,
                                "Image subasset failed deterministic texture cooking.");
                            continue;
                        }
                        const CookSection* manifestSection =
                            findSection(textureProduct,
                                kCookedTextureManifestSection);
                        const CookSection* payloadSection =
                            findSection(textureProduct,
                                kCookedTexturePayloadSection);
                        if (!manifestSection ||
                            !payloadSection) {
                            throw std::runtime_error(
                                "Texture importer omitted a required product section.");
                        }
                        std::vector<CookDiagnostic>
                            textureDiagnostics;
                        const auto textureManifest =
                            readTextureManifest(
                                manifestSection->bytes,
                                textureDiagnostics);
                        if (!textureManifest ||
                            hasCookErrors(
                                textureDiagnostics)) {
                            throw std::runtime_error(
                                "Texture importer emitted an invalid manifest.");
                        }
                        CookedModelTextureView view{
                            .textureGuid = *textureGuid,
                            .sourceImageIndex =
                                *operation.sourceImageIndex,
                            .manifest = *textureManifest,
                            .payload =
                                payloadSection->bytes,
                        };
                        view.viewKey =
                            calculateModelTextureViewKey(
                                view);
                        textureViewIndex =
                            static_cast<uint32_t>(
                                data.textureViews.size());
                        data.textureViews.push_back(
                            std::move(view));
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
            }
            if (hasCookErrors(failed.diagnostics)) return failed;

            for (const Json& raw : parsed.at("primitives")) {
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

                const uint64_t firstVertex = data.vertices.size();
                const uint64_t firstIndex = data.indices.size();
                const uint64_t rtFirstPosition = data.rtPositions.size();
                const uint64_t rtFirstIndex = data.rtIndices.size();
                glm::vec3 boundsMin(std::numeric_limits<float>::max());
                glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
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
                    boundsMin = glm::min(boundsMin, vertex.pos);
                    boundsMax = glm::max(boundsMax, vertex.pos);
                    data.vertices.push_back({
                        .position = { vertex.pos.x, vertex.pos.y, vertex.pos.z },
                        .color = {
                            vertex.color.x, vertex.color.y,
                            vertex.color.z, vertex.color.w,
                        },
                        .normal = {
                            vertex.normal.x, vertex.normal.y, vertex.normal.z,
                        },
                        .texCoord0 = { vertex.uv0.x, vertex.uv0.y },
                        .tangent = {
                            vertex.tangent.x, vertex.tangent.y,
                            vertex.tangent.z, vertex.tangent.w,
                        },
                        .texCoord1 = { vertex.uv1.x, vertex.uv1.y },
                    });
                    data.rtPositions.push_back({
                        vertex.pos.x, vertex.pos.y, vertex.pos.z,
                    });
                }
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
                for (uint32_t index : runtimeIndices) {
                    if ((data.indices.size() &
                            0xfffu) == 0) {
                        cancelled();
                    }
                    data.indices.push_back(
                        static_cast<uint32_t>(firstVertex + index));
                    data.rtIndices.push_back(index);
                }

                const glm::vec3 center = (boundsMin + boundsMax) * 0.5f;
                const float radius = glm::length(boundsMax - center);
                data.manifest.primitives.push_back({
                    .primitiveGuid = *primitiveGuid,
                    .materialGuid = *materialGuid,
                    .sourceKey = primitiveKey,
                    .sourceNode = sourceNode,
                    .sourceMesh = sourceMesh,
                    .sourcePrimitive = sourcePrimitive,
                    .attributeMask = attributeMask,
                    .firstVertex = firstVertex,
                    .vertexCount = vertices.size(),
                    .firstIndex = firstIndex,
                    .indexCount = runtimeIndices.size(),
                    .rtFirstPosition = rtFirstPosition,
                    .rtPositionCount = vertices.size(),
                    .rtFirstIndex = rtFirstIndex,
                    .rtIndexCount = runtimeIndices.size(),
                    .topology = ModelPrimitiveTopology::Triangles,
                    .winding = ModelWinding::Clockwise,
                    .coverage = coverage,
                    .indexFormat = ModelIndexFormat::UInt32,
                    .flags = primitiveFlags,
                    .rtFlags = ModelRtBuildInput |
                        (coverage == ModelCoverage::Opaque
                            ? ModelRtOpaque : ModelRtAllowAnyHit),
                    .bounds = {
                        .aabbMin = {
                            boundsMin.x, boundsMin.y, boundsMin.z,
                        },
                        .aabbMax = {
                            boundsMax.x, boundsMax.y, boundsMax.z,
                        },
                        .sphereCenter = { center.x, center.y, center.z },
                        .sphereRadius = radius,
                    },
                });
            }
            if (hasCookErrors(failed.diagnostics)) return failed;
            data.manifest.vertexCount = data.vertices.size();
            data.manifest.indexCount = data.indices.size();
            data.manifest.rtPositionCount = data.rtPositions.size();
            data.manifest.rtIndexCount = data.rtIndices.size();
            CookProduct product =
                makeCookedModelProduct(data);
            product.diagnostics.insert(
                product.diagnostics.end(),
                failed.diagnostics.begin(),
                failed.diagnostics.end());
            return product;
        } catch (const std::exception& exception) {
            addError(failed.diagnostics, "GLTF_CPU_COOK", "/",
                exception.what());
            return failed;
        }
    }

} // namespace Iridium
