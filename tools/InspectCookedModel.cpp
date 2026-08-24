#include "assets/cooker/CookedArtifact.h"
#include "assets/model/ModelProduct.h"
#include "material/MaterialCompiler.h"
#include "material/SourceMaterial.h"
#include "renderer/rhi/Mesh.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

    using namespace Iridium;
    using Json = nlohmann::ordered_json;

    std::vector<std::byte> readFile(
        const std::filesystem::path& path) {
        std::ifstream input(path,
            std::ios::binary | std::ios::ate);
        if (!input) {
            throw std::runtime_error(
                "Could not open cooked artifact.");
        }
        const std::streamsize size = input.tellg();
        if (size < 0) {
            throw std::runtime_error(
                "Could not size cooked artifact.");
        }
        input.seekg(0, std::ios::beg);
        std::vector<std::byte> bytes(
            static_cast<size_t>(size));
        if (size != 0 &&
            !input.read(reinterpret_cast<char*>(
                bytes.data()), size)) {
            throw std::runtime_error(
                "Could not read cooked artifact.");
        }
        return bytes;
    }

    const char* alphaName(SourceAlphaMode mode) {
        switch (mode) {
        case SourceAlphaMode::Opaque: return "opaque";
        case SourceAlphaMode::Mask: return "mask";
        case SourceAlphaMode::Blend: return "blend";
        }
        return "invalid";
    }

} // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 4) {
        std::cerr << "Usage: IridiumInspectCookedModel "
            "<artifact> [--material-index N | --verify-source gltf]\n";
        return 1;
    }
    try {
        std::optional<uint32_t> selectedIndex;
        std::optional<std::filesystem::path> verificationSource;
        if (argc == 4) {
            const std::string_view option(argv[2]);
            if (option == "--material-index") {
                selectedIndex = static_cast<uint32_t>(
                    std::stoul(argv[3]));
            }
            else if (option == "--verify-source") {
                verificationSource = argv[3];
            }
            else {
                throw std::runtime_error(
                    "Unknown inspection option.");
            }
        }
        const std::vector<std::byte> bytes =
            readFile(argv[1]);
        const CookedArtifactReadResult artifact =
            readCookedArtifact(bytes);
        if (!artifact.valid()) {
            throw std::runtime_error(
                "Cooked artifact container validation failed.");
        }
        const CookedModelReadResult model =
            readCookedModelProduct(*artifact.artifact);
        if (!model.valid()) {
            throw std::runtime_error(
                "Typed cooked model validation failed.");
        }
        uint64_t texturePayloadBytes = 0;
        for (const CookedModelTextureView&
                view : model.data->textureViews) {
            texturePayloadBytes +=
                view.payload.size();
        }
        const uint64_t gpuUploadBytes =
            model.data->vertices.size() *
                sizeof(Vertex) +
            model.data->indices.size() *
                sizeof(uint32_t) +
            model.data->materials.size() *
                sizeof(PackedGpuMaterial) +
            texturePayloadBytes;
        std::array<float, 3> boundsMin{
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
        };
        std::array<float, 3> boundsMax{
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
        };
        for (const CookedModelPrimitive&
                primitive :
            model.data->manifest.primitives) {
            for (size_t axis = 0;
                axis < 3; ++axis) {
                boundsMin[axis] = std::min(
                    boundsMin[axis],
                    primitive.bounds
                        .aabbMin[axis]);
                boundsMax[axis] = std::max(
                    boundsMax[axis],
                    primitive.bounds
                        .aabbMax[axis]);
            }
        }

        Json output{
            { "status", "ok" },
            { "assetGuid",
                artifact.artifact->assetGuid.toString() },
            { "artifactHash", artifact.artifactHash },
            { "schema",
                artifact.artifact->artifactSchemaVersion },
            { "transparencyExecutionMode",
                transparencyExecutionModeName(
                    model.data->manifest.transparencyExecutionMode) },
            { "materials", model.data->materials.size() },
            { "textureViews",
                model.data->textureViews.size() },
            { "primitives",
                model.data->manifest.primitives.size() },
            { "vertices", model.data->vertices.size() },
            { "indices", model.data->indices.size() },
            { "texturePayloadBytes",
                texturePayloadBytes },
            { "gpuUploadBytes",
                gpuUploadBytes },
            { "boundsMin", boundsMin },
            { "boundsMax", boundsMax },
        };
        if (selectedIndex) {
            const auto found = std::ranges::find_if(
                model.data->materials,
                [selectedIndex](
                    const CookedModelMaterial& material) {
                    return material.compiled
                        .sourceMaterialIndex ==
                        *selectedIndex;
                });
            if (found == model.data->materials.end()) {
                throw std::runtime_error(
                    "Requested source material index is absent.");
            }
            size_t primitiveCount = 0;
            Json primitivePolicies = Json::array();
            Json textures = Json::array();
            for (const CookedModelPrimitive& primitive :
                model.data->manifest.primitives) {
                if (primitive.materialGuid ==
                    found->materialGuid) {
                    ++primitiveCount;
                    primitivePolicies.push_back({
                        { "primitiveGuid",
                            primitive.primitiveGuid.toString() },
                        { "requestedClass", transparencyClassName(
                            primitive.transparency.requestedClass) },
                        { "resolvedClass", transparencyClassName(
                            primitive.transparency.resolvedClass) },
                        { "quality", transparencyQualityName(
                            primitive.transparency.quality) },
                        { "priority",
                            primitive.transparency.priority },
                        { "thinSheetThicknessMeters",
                            primitive.transparency
                                .thinSheetThicknessMeters },
                        { "flags", primitive.transparency.flags },
                    });
                }
            }
            for (size_t operationIndex = 0;
                operationIndex <
                    found->compiled.textureOperations.size();
                ++operationIndex) {
                const CompiledTextureOperation& operation =
                    found->compiled.textureOperations[
                        operationIndex];
                const auto binding = std::ranges::find_if(
                    found->textureBindings,
                    [operationIndex](
                        const CookedModelTextureBinding& value) {
                        return value.operationIndex ==
                            operationIndex;
                    });
                const CookedTextureManifest* textureManifest = nullptr;
                if (binding != found->textureBindings.end() &&
                    binding->textureViewIndex < model.data->textureViews.size()) {
                    textureManifest = &model.data->textureViews[
                        binding->textureViewIndex].manifest;
                }
                textures.push_back({
                    { "operation", operationIndex },
                    { "semantic",
                        sourceTextureSemanticName(
                            operation.semantic) },
                    { "sourceImageIndex",
                        operation.sourceImageIndex
                            ? Json(*operation.sourceImageIndex)
                            : Json(nullptr) },
                    { "textureGuid",
                        binding == found->textureBindings.end()
                            ? ""
                            : binding->textureGuid.toString() },
                    { "textureViewIndex",
                        binding == found->textureBindings.end()
                            ? Json(nullptr)
                            : Json(binding->textureViewIndex) },
                    { "residentWidth", textureManifest
                        ? Json(textureManifest->width) : Json(nullptr) },
                    { "residentHeight", textureManifest
                        ? Json(textureManifest->height) : Json(nullptr) },
                    { "residentMipCount", textureManifest
                        ? Json(textureManifest->mips.size()) : Json(nullptr) },
                    { "storageFormat", textureManifest
                        ? Json(static_cast<uint32_t>(
                            textureManifest->storageFormat)) : Json(nullptr) },
                    { "compressionQuality", textureManifest
                        ? Json(static_cast<uint32_t>(
                            textureManifest->quality)) : Json(nullptr) },
                    { "transfer",
                        operation.transfer ==
                            SourceTextureTransfer::Srgb
                            ? "srgb" : "linear" },
                    { "texCoord",
                        operation.transform
                            .texCoordOverride.value_or(
                                operation.texCoord) },
                });
            }
            output["selectedMaterial"] = {
                { "sourceIndex",
                    found->compiled.sourceMaterialIndex },
                { "sourceName",
                    found->compiled.sourceName },
                { "sourceKey", found->sourceKey },
                { "materialGuid",
                    found->materialGuid.toString() },
                { "contentHash",
                    found->compiled.contentHash },
                { "closure",
                    materialClosureClassName(
                        found->compiled.closureClass) },
                { "alphaMode",
                    alphaName(
                        found->compiled.standard.alphaMode) },
                { "transparency", {
                    { "requestedClass", transparencyClassName(
                        found->compiled.transparency.requestedClass) },
                    { "resolvedClass", transparencyClassName(
                        found->compiled.transparency.resolvedClass) },
                    { "quality", transparencyQualityName(
                        found->compiled.transparency.quality) },
                    { "priority",
                        found->compiled.transparency.priority },
                    { "thinSheetThicknessMeters",
                        found->compiled.transparency
                            .thinSheetThicknessMeters },
                    { "flags", found->compiled.transparency.flags },
                } },
                { "doubleSided",
                    found->compiled.standard.doubleSided },
                { "metallic",
                    found->compiled.standard.metallicFactor },
                { "roughness",
                    found->compiled.standard.roughnessFactor },
                { "primitiveCount", primitiveCount },
                { "primitivePolicies", std::move(primitivePolicies) },
                { "textures", std::move(textures) },
            };
        }
        bool sourceParityValid = true;
        if (verificationSource) {
            const SourceMaterialDocument source =
                importGltfSourceMaterials(*verificationSource);
            if (source.hasErrors()) {
                throw std::runtime_error(
                    "Source material verification could not parse the glTF.");
            }
            const MaterialCompileDocumentResult compiled =
                compileSourceMaterialDocument(
                    source, MaterialCompilePolicy::Strict);
            if (!compiled.succeeded()) {
                throw std::runtime_error(
                    "Source material verification could not compile the glTF.");
            }

            Json mismatches = Json::array();
            size_t matched = 0;
            for (const MaterialCompileResult& sourceMaterial :
                compiled.materials) {
                if (!sourceMaterial.material) {
                    continue;
                }
                const auto cooked = std::ranges::find_if(
                    model.data->materials,
                    [&sourceMaterial](
                        const CookedModelMaterial& material) {
                        return material.compiled.sourceMaterialIndex ==
                            sourceMaterial.material->sourceMaterialIndex;
                    });
                if (cooked == model.data->materials.end() ||
                    cooked->compiled.contentHash !=
                        sourceMaterial.material->contentHash) {
                    mismatches.push_back({
                        { "sourceIndex",
                            sourceMaterial.material->sourceMaterialIndex },
                        { "sourceName",
                            sourceMaterial.material->sourceName },
                        { "expectedHash",
                            sourceMaterial.material->contentHash },
                        { "cookedHash",
                            cooked == model.data->materials.end()
                                ? "" : cooked->compiled.contentHash },
                    });
                }
                else {
                    ++matched;
                }
            }
            sourceParityValid = mismatches.empty() &&
                matched == compiled.materials.size();
            output["sourceMaterialParity"] = {
                { "status",
                    sourceParityValid ? "match" : "mismatch" },
                { "sourceMaterials", compiled.materials.size() },
                { "cookedMaterials", model.data->materials.size() },
                { "matched", matched },
                { "mismatches", std::move(mismatches) },
            };
        }
        std::cout << output.dump(2) << '\n';
        return sourceParityValid ? 0 : 3;
    } catch (const std::exception& exception) {
        std::cerr << "Cooked model inspection failed: "
            << exception.what() << '\n';
        return 2;
    }
}
