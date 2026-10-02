#include "assets/cooker/CookedArtifact.h"
#include "assets/model/ModelLodGenerator.h"
#include "assets/model/ModelProduct.h"
#include "assets/model/ModelRuntimeProduct.h"
#include "material/MaterialCompiler.h"
#include "material/SourceMaterial.h"
#include "renderer/rhi/Mesh.h"
#include "utils/Sha256.h"

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

    const char* lodTopologyFailureName(size_t index) {
        switch (static_cast<ModelLodTopologyFailure>(index)) {
        case ModelLodTopologyFailure::MixedComponentTriangle:
            return "mixed_component_triangle";
        case ModelLodTopologyFailure::NonManifoldEdge:
            return "nonmanifold_edge";
        case ModelLodTopologyFailure::InconsistentEdgeOrientation:
            return "inconsistent_edge_orientation";
        case ModelLodTopologyFailure::BoundaryDegree:
            return "boundary_degree";
        case ModelLodTopologyFailure::VertexLinkDegree:
            return "vertex_link_degree";
        case ModelLodTopologyFailure::VertexLinkDisconnected:
            return "vertex_link_disconnected";
        case ModelLodTopologyFailure::DisconnectedComponent:
            return "disconnected_component";
        case ModelLodTopologyFailure::Count: break;
        }
        return "invalid";
    }

    const char* lodLevelOutcomeName(size_t index) {
        switch (static_cast<ModelLodLevelOutcome>(index)) {
        case ModelLodLevelOutcome::NotAttempted: return "not_attempted";
        case ModelLodLevelOutcome::Accepted: return "accepted";
        case ModelLodLevelOutcome::OrientationRejected:
            return "orientation_rejected";
        case ModelLodLevelOutcome::NoReduction: return "no_reduction";
        case ModelLodLevelOutcome::BelowMinimumReduction:
            return "below_minimum_reduction";
        case ModelLodLevelOutcome::CorrespondenceRejected:
            return "correspondence_rejected";
        case ModelLodLevelOutcome::ReducedTopologyInvalid:
            return "reduced_topology_invalid";
        case ModelLodLevelOutcome::TopologySignatureMismatch:
            return "topology_signature_mismatch";
        case ModelLodLevelOutcome::Count: break;
        }
        return "invalid";
    }

} // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 4) {
        std::cerr << "Usage: IridiumInspectCookedModel "
            "<artifact> [--material-index N | --verify-source gltf | "
            "--lod-generation-analysis locked|boundary|boundary-fans|"
            "transactional|topology-transactional]\n";
        return 1;
    }
    try {
        std::optional<uint32_t> selectedIndex;
        std::optional<std::filesystem::path> verificationSource;
        std::optional<std::string> lodGenerationAnalysisMode;
        if (argc == 4) {
            const std::string_view option(argv[2]);
            if (option == "--material-index") {
                selectedIndex = static_cast<uint32_t>(
                    std::stoul(argv[3]));
            }
            else if (option == "--verify-source") {
                verificationSource = argv[3];
            }
            else if (option == "--lod-generation-analysis") {
                const std::string_view mode(argv[3]);
                if (mode != "locked" && mode != "boundary" &&
                    mode != "boundary-fans" && mode != "transactional" &&
                    mode != "topology-transactional")
                    throw std::runtime_error(
                        "LOD generation analysis mode must be locked, boundary, "
                        "boundary-fans, transactional, or topology-transactional.");
                lodGenerationAnalysisMode = mode;
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
        const RuntimeModelCpuResult runtime =
            makeRuntimeModelCpuData(*model.data, false);
        if (!runtime.valid()) {
            throw std::runtime_error(
                "Runtime geometry-arena conversion failed.");
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
            runtime.data->geometryArena.stats.arenaIndexBytes +
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
            { "canonicalPrimitives", runtime.data->primitives.size() },
            { "lodChainCount", model.data->lodChains.size() },
            { "vertices", model.data->vertices.size() },
            { "indices", model.data->indices.size() },
            { "sourceIndexBytes",
                runtime.data->geometryArena.stats.sourceIndexBytes },
            { "arenaIndexBytes",
                runtime.data->geometryArena.stats.arenaIndexBytes },
            { "arenaSavedIndexBytes",
                runtime.data->geometryArena.stats.savedIndexBytes },
            { "arenaUInt16Indices",
                runtime.data->geometryArena.stats.uint16IndexCount },
            { "arenaUInt32Indices",
                runtime.data->geometryArena.stats.uint32IndexCount },
            { "texturePayloadBytes",
                texturePayloadBytes },
            { "gpuUploadBytes",
                gpuUploadBytes },
            { "boundsMin", boundsMin },
            { "boundsMax", boundsMax },
        };
        const std::vector<bool> lodChildMask =
            makeCookedModelLodChildMask(*model.data);
        uint64_t canonicalTriangles = 0;
        uint64_t opaqueTrianglePrimitives = 0;
        uint64_t opaqueTriangles = 0;
        uint64_t nonOpaquePrimitives = 0;
        uint64_t nonOpaqueTriangles = 0;
        uint64_t nonTrianglePrimitives = 0;
        uint64_t generatedBasePrimitives = 0;
        uint64_t generatedBaseTriangles = 0;
        uint64_t unchainedOpaquePrimitives = 0;
        uint64_t unchainedOpaqueTriangles = 0;
        uint64_t unchainedBelowMinimumPrimitives = 0;
        uint64_t unchainedBelowMinimumTriangles = 0;
        uint64_t unchainedAtLeastMinimumPrimitives = 0;
        uint64_t unchainedAtLeastMinimumTriangles = 0;
        std::array<uint64_t, 4> opaquePrimitiveBins{};
        std::array<uint64_t, 4> opaqueTriangleBins{};
        for (size_t primitiveIndex = 0;
                primitiveIndex < model.data->manifest.primitives.size();
                ++primitiveIndex) {
            if (lodChildMask[primitiveIndex]) continue;
            const CookedModelPrimitive& primitive =
                model.data->manifest.primitives[primitiveIndex];
            const uint64_t triangles = primitive.indexCount / 3u;
            canonicalTriangles += triangles;
            if (primitive.topology != ModelPrimitiveTopology::Triangles) {
                ++nonTrianglePrimitives;
                continue;
            }
            if (primitive.coverage != ModelCoverage::Opaque) {
                ++nonOpaquePrimitives;
                nonOpaqueTriangles += triangles;
                continue;
            }
            ++opaqueTrianglePrimitives;
            opaqueTriangles += triangles;
            const size_t bin = triangles < 64u ? 0u :
                triangles < 256u ? 1u : triangles < 1024u ? 2u : 3u;
            ++opaquePrimitiveBins[bin];
            opaqueTriangleBins[bin] += triangles;
            if (primitive.lodSection != kNoModelSection) {
                ++generatedBasePrimitives;
                generatedBaseTriangles += triangles;
                continue;
            }
            ++unchainedOpaquePrimitives;
            unchainedOpaqueTriangles += triangles;
            if (triangles < 64u) {
                ++unchainedBelowMinimumPrimitives;
                unchainedBelowMinimumTriangles += triangles;
            }
            else {
                ++unchainedAtLeastMinimumPrimitives;
                unchainedAtLeastMinimumTriangles += triangles;
            }
        }
        output["lodCoverage"] = {
            { "canonicalTriangles", canonicalTriangles },
            { "opaqueTrianglePrimitives", opaqueTrianglePrimitives },
            { "opaqueTriangles", opaqueTriangles },
            { "nonOpaquePrimitives", nonOpaquePrimitives },
            { "nonOpaqueTriangles", nonOpaqueTriangles },
            { "nonTrianglePrimitives", nonTrianglePrimitives },
            { "generatedBasePrimitives", generatedBasePrimitives },
            { "generatedBaseTriangles", generatedBaseTriangles },
            { "unchainedOpaquePrimitives", unchainedOpaquePrimitives },
            { "unchainedOpaqueTriangles", unchainedOpaqueTriangles },
            { "unchainedBelowMinimumPrimitives",
                unchainedBelowMinimumPrimitives },
            { "unchainedBelowMinimumTriangles", unchainedBelowMinimumTriangles },
            { "unchainedAtLeastMinimumPrimitives",
                unchainedAtLeastMinimumPrimitives },
            { "unchainedAtLeastMinimumTriangles", unchainedAtLeastMinimumTriangles },
            { "opaqueTriangleBins", {
                { "lt64", {
                    { "primitives", opaquePrimitiveBins[0] },
                    { "triangles", opaqueTriangleBins[0] } } },
                { "64to255", {
                    { "primitives", opaquePrimitiveBins[1] },
                    { "triangles", opaqueTriangleBins[1] } } },
                { "256to1023", {
                    { "primitives", opaquePrimitiveBins[2] },
                    { "triangles", opaqueTriangleBins[2] } } },
                { "ge1024", {
                    { "primitives", opaquePrimitiveBins[3] },
                    { "triangles", opaqueTriangleBins[3] } } },
            } },
        };
        if (lodGenerationAnalysisMode) {
            CookedModelProductData analysisProduct = *model.data;
            analysisProduct.manifest.primitives.resize(
                runtime.data->primitives.size());
            uint64_t canonicalVertexEnd = 0;
            uint64_t canonicalIndexEnd = 0;
            for (CookedModelPrimitive& primitive :
                    analysisProduct.manifest.primitives) {
                canonicalVertexEnd = std::max(canonicalVertexEnd,
                    primitive.firstVertex + primitive.vertexCount);
                canonicalIndexEnd = std::max(canonicalIndexEnd,
                    primitive.firstIndex + primitive.indexCount);
                primitive.lodSection = kNoModelSection;
            }
            analysisProduct.vertices.resize(
                static_cast<size_t>(canonicalVertexEnd));
            analysisProduct.indices.resize(
                static_cast<size_t>(canonicalIndexEnd));
            analysisProduct.lodChains.clear();
            ModelLodGenerationSettings analysisSettings;
            analysisSettings.allowBoundaryEdgeCollapse =
                *lodGenerationAnalysisMode != "locked";
            analysisSettings.splitNonManifoldBoundaryFans =
                *lodGenerationAnalysisMode == "boundary-fans" ||
                *lodGenerationAnalysisMode == "transactional" ||
                *lodGenerationAnalysisMode == "topology-transactional";
            analysisSettings.preventOrientationChangingMerges =
                *lodGenerationAnalysisMode == "transactional" ||
                *lodGenerationAnalysisMode == "topology-transactional";
            analysisSettings.preventTopologyChangingMerges =
                *lodGenerationAnalysisMode == "topology-transactional";
            ModelLodGenerationStatistics statistics;
            const size_t generatedChains = appendGeneratedModelLods(
                analysisProduct, analysisSettings, {}, &statistics);
            Json topologyFailures = Json::object();
            Json reducedTopologyFailures = Json::object();
            for (size_t index = 0;
                    index < ModelLodTopologyFailureCount; ++index) {
                topologyFailures[lodTopologyFailureName(index)] = {
                    { "primitives",
                        statistics.sourceTopologyFailurePrimitiveCounts[index] },
                    { "triangles",
                        statistics.sourceTopologyFailureTriangleCounts[index] },
                };
                reducedTopologyFailures[lodTopologyFailureName(index)] =
                    statistics.reducedTopologyFailureLevelCounts[index];
            }
            Json levelOutcomes = Json::array();
            for (size_t budget = 0; budget < ModelLodBudgetCount; ++budget) {
                Json outcomes = Json::object();
                for (size_t outcome = 0;
                        outcome < ModelLodLevelOutcomeCount; ++outcome) {
                    outcomes[lodLevelOutcomeName(outcome)] = {
                        { "primitives",
                            statistics.levelOutcomeCounts[budget][outcome] },
                        { "triangles",
                            statistics.levelOutcomeTriangleCounts[budget][outcome] },
                    };
                }
                levelOutcomes.push_back({
                    { "budgetIndex", budget },
                    { "relativeError",
                        analysisSettings.maximumRelativeErrors[budget] },
                    { "outcomes", std::move(outcomes) },
                });
            }
            Json noAcceptedTerminalOutcomes = Json::object();
            for (size_t outcome = 0;
                    outcome < ModelLodLevelOutcomeCount; ++outcome) {
                noAcceptedTerminalOutcomes[lodLevelOutcomeName(outcome)] = {
                    { "primitives", statistics
                        .noAcceptedTerminalOutcomePrimitiveCounts[outcome] },
                    { "triangles", statistics
                        .noAcceptedTerminalOutcomeTriangleCounts[outcome] },
                };
            }
            output["lodGenerationAnalysis"] = {
                { "mode", *lodGenerationAnalysisMode },
                { "generatedChains", generatedChains },
                { "canonicalPrimitives", statistics.canonicalPrimitiveCount },
                { "canonicalTriangles", statistics.canonicalTriangleCount },
                { "skippedNonOpaquePrimitives",
                    statistics.skippedNonOpaquePrimitiveCount },
                { "skippedNonTrianglePrimitives",
                    statistics.skippedNonTrianglePrimitiveCount },
                { "attemptedPrimitives", statistics.attemptedPrimitiveCount },
                { "attemptedTriangles", statistics.attemptedTriangleCount },
                { "belowMinimumPrimitives",
                    statistics.belowMinimumPrimitiveCount },
                { "belowMinimumTriangles",
                    statistics.belowMinimumTriangleCount },
                { "invalidExtentPrimitives",
                    statistics.invalidExtentPrimitiveCount },
                { "invalidExtentTriangles",
                    statistics.invalidExtentTriangleCount },
                { "degenerateSourcePrimitives",
                    statistics.degenerateSourcePrimitiveCount },
                { "degenerateSourceTriangles",
                    statistics.degenerateSourceTriangleCount },
                { "invalidSourceTopologyPrimitives",
                    statistics.invalidSourceTopologyPrimitiveCount },
                { "invalidSourceTopologyTriangles",
                    statistics.invalidSourceTopologyTriangleCount },
                { "sourceTopologyFailures", std::move(topologyFailures) },
                { "splitBoundaryFanVertices",
                    statistics.splitBoundaryFanVertexCount },
                { "duplicatedBoundaryFanVertices",
                    statistics.duplicatedBoundaryFanVertexCount },
                { "noAcceptedLevelPrimitives",
                    statistics.noAcceptedLevelPrimitiveCount },
                { "noAcceptedLevelTriangles",
                    statistics.noAcceptedLevelTriangleCount },
                { "noAcceptedTerminalOutcomes",
                    std::move(noAcceptedTerminalOutcomes) },
                { "generatedBaseTriangles",
                    statistics.generatedBaseTriangleCount },
                { "generatedLevels", statistics.generatedLevelCount },
                { "levelOutcomes", std::move(levelOutcomes) },
                { "edgeConsiderations", statistics.edgeConsiderationCount },
                { "acceptedMerges", statistics.acceptedMergeCount },
                { "orientationRejectedMerges",
                    statistics.orientationRejectedEdgeCount },
                { "correspondenceRejectedMerges",
                    statistics.correspondenceRejectedEdgeCount },
                { "topologyRejectedMerges",
                    statistics.topologyRejectedEdgeCount },
                { "boundaryRejectedEdges",
                    statistics.boundaryRejectedEdgeCount },
                { "attributeRejectedEdges",
                    statistics.attributeRejectedEdgeCount },
                { "budgetRejectedEdges", statistics.budgetRejectedEdgeCount },
                { "orientationRejectedLevels",
                    statistics.orientationRejectedLevelCount },
                { "noReductionLevels", statistics.noReductionLevelCount },
                { "belowMinimumReductionLevels",
                    statistics.belowMinimumReductionLevelCount },
                { "correspondenceRejectedLevels",
                    statistics.correspondenceRejectedLevelCount },
                { "topologyRejectedLevels",
                    statistics.topologyRejectedLevelCount },
                { "reducedTopologyInvalidLevels",
                    statistics.reducedTopologyInvalidLevelCount },
                { "topologySignatureMismatchLevels",
                    statistics.topologySignatureMismatchLevelCount },
                { "topologyComponentMismatchLevels",
                    statistics.topologyComponentMismatchLevelCount },
                { "topologyEulerMismatchLevels",
                    statistics.topologyEulerMismatchLevelCount },
                { "topologyBoundaryMismatchLevels",
                    statistics.topologyBoundaryMismatchLevelCount },
                { "reducedTopologyFailures",
                    std::move(reducedTopologyFailures) },
            };
        }
        output["sections"] = Json::array();
        for (const CookSection& section : artifact.artifact->sections) {
            output["sections"].push_back({
                { "id", section.id }, { "schema", section.schemaVersion },
                { "bytes", section.bytes.size() },
                { "sha256", sha256(section.bytes) },
            });
        }
        output["textureViewProducts"] = Json::array();
        for (const CookedModelTextureView& view : model.data->textureViews) {
            output["textureViewProducts"].push_back({
                { "sourceImageIndex", view.sourceImageIndex },
                { "textureGuid", view.textureGuid.toString() },
                { "viewKey", view.viewKey },
                { "manifestSha256", sha256(serializeTextureManifest(view.manifest)) },
                { "payloadSha256", sha256(view.payload) },
            });
        }
        output["lodChains"] = Json::array();
        for (const CookedModelLodChain& chain : model.data->lodChains) {
            const CookedModelPrimitive& base =
                model.data->manifest.primitives[chain.basePrimitiveIndex];
            Json levels = Json::array();
            for (const CookedModelLodLevel& level : chain.levels) {
                const CookedModelPrimitive& primitive =
                    model.data->manifest.primitives[level.primitiveIndex];
                levels.push_back({
                    { "primitiveGuid", primitive.primitiveGuid.toString() },
                    { "geometricError", level.geometricError },
                    { "triangles", primitive.indexCount / 3u },
                    { "vertices", primitive.vertexCount },
                });
            }
            output["lodChains"].push_back({
                { "sourcePrimitiveGuid", base.sourcePrimitiveGuid.toString() },
                { "basePrimitiveGuid", base.primitiveGuid.toString() },
                { "materialGuid", base.materialGuid.toString() },
                { "levels", std::move(levels) },
            });
        }
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
