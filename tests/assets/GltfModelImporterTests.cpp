#include "assets/AssetMetadata.h"
#include "assets/cooker/AssetCooker.h"
#include "assets/cooker/CookReceipt.h"
#include "assets/cooker/CookedArtifact.h"
#include "assets/cooker/LocalDerivedDataCache.h"
#include "assets/model/GltfModelImporter.h"
#include "assets/model/ModelProduct.h"
#include "assets/model/ModelRuntimeProduct.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <stop_token>
#include <vector>

namespace {

    using namespace Iridium;

    #define CHECK(condition)                                                        \
        do {                                                                        \
            if (!(condition)) {                                                      \
                std::cerr << __FILE__ << ':' << __LINE__                             \
                          << ": CHECK failed: " #condition << '\n';                  \
                return false;                                                        \
            }                                                                        \
        } while (false)

    std::filesystem::path fixtureRoot() {
        return std::filesystem::path(PROJECT_ROOT_DIR) / "tests" / "assets";
    }

    struct TemporaryDirectory {
        std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("iridium-gltf-metadata-" +
                std::to_string(
                    std::chrono::steady_clock::now()
                        .time_since_epoch().count()));

        TemporaryDirectory() {
            std::filesystem::create_directories(
                path);
        }

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(
                path, error);
        }
    };

    std::vector<std::byte> readFile(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) return {};
        const std::streamsize size = input.tellg();
        input.seekg(0, std::ios::beg);
        std::vector<std::byte> result(static_cast<size_t>(size));
        if (size != 0) {
            input.read(reinterpret_cast<char*>(result.data()), size);
        }
        return result;
    }

    const CookSection* findSection(const CookedArtifact& artifact, uint32_t id) {
        const auto found = std::ranges::find_if(artifact.sections,
            [id](const CookSection& section) { return section.id == id; });
        return found == artifact.sections.end() ? nullptr : &*found;
    }

    std::optional<CookedModelProductData> decodeModel(
        const CookedArtifact& artifact,
        std::vector<CookDiagnostic>& diagnostics) {
        const CookSection* manifest =
            findSection(artifact, kCookedModelManifestSection);
        const CookSection* materials =
            findSection(artifact, kCookedModelMaterialSection);
        const CookSection* textureViews =
            findSection(artifact, kCookedModelTextureViewSection);
        const CookSection* vertices =
            findSection(artifact, kCookedModelVertexSection);
        const CookSection* indices =
            findSection(artifact, kCookedModelIndexSection);
        const CookSection* rtPositions =
            findSection(artifact, kCookedModelRtPositionSection);
        const CookSection* rtIndices =
            findSection(artifact, kCookedModelRtIndexSection);
        if (!manifest || !materials || !textureViews ||
            !vertices || !indices ||
            !rtPositions || !rtIndices) {
            return std::nullopt;
        }
        auto decodedManifest =
            readModelManifest(manifest->bytes, diagnostics);
        auto decodedMaterials =
            readModelMaterials(materials->bytes, diagnostics);
        auto decodedTextureViews =
            readModelTextureViews(
                textureViews->bytes, diagnostics);
        auto decodedVertices =
            readModelVertices(vertices->bytes, diagnostics);
        auto decodedIndices =
            readModelIndices(indices->bytes, diagnostics);
        auto decodedRtPositions =
            readModelRtPositions(rtPositions->bytes, diagnostics);
        auto decodedRtIndices =
            readModelIndices(rtIndices->bytes, diagnostics, "/rt_indices");
        if (!decodedManifest || !decodedMaterials ||
            !decodedTextureViews ||
            !decodedVertices || !decodedIndices ||
            !decodedRtPositions || !decodedRtIndices) {
            return std::nullopt;
        }
        return CookedModelProductData{
            .manifest = std::move(*decodedManifest),
            .materials = std::move(*decodedMaterials),
            .textureViews = std::move(*decodedTextureViews),
            .vertices = std::move(*decodedVertices),
            .indices = std::move(*decodedIndices),
            .rtPositions = std::move(*decodedRtPositions),
            .rtIndices = std::move(*decodedRtIndices),
        };
    }

    std::optional<PreparedAssetCook> preparedFixture() {
        const auto metadata = readAssetMetadata(
            fixtureRoot() / "gltf_model_cooker_fixture.gltf.iridium.meta");
        if (!metadata.metadata) return std::nullopt;
        ImporterRegistry registry;
        registry.registerImporter(std::make_shared<GltfModelImporter>());
        return prepareAssetCook(registry, fixtureRoot(),
            "gltf_model_cooker_fixture.gltf", *metadata.metadata, {
                .platform = "windows-x64",
                .profile = "release",
                .qualityPolicy = "reference",
            }, "m3.4-model-v1");
    }

    bool testDeterministicCookAndArtifactRoundTrip() {
        const auto prepared = preparedFixture();
        CHECK(prepared.has_value());
        if (!prepared->valid()) {
            for (const CookDiagnostic& diagnostic : prepared->diagnostics) {
                std::cerr << diagnostic.code << ": "
                          << diagnostic.message << '\n';
            }
        }
        CHECK(prepared->valid());
        CHECK(prepared->resolvedDependencies.size() == 2);
        CHECK(std::ranges::all_of(
            prepared->resolvedDependencies,
            [](const AssetDependency& dependency) {
                return dependency.type ==
                    AssetDependencyType::Tool;
            }));
        CHECK(prepared->source.discoveredSubassets.size() == 7);
        CHECK(std::ranges::all_of(
            prepared->source.subassetPayloads,
            [](const ParsedSourceAsset::
                SubassetPayload& payload) {
                return !payload.bytes.empty() &&
                    payload.parsedBytes.empty();
            }));
        const auto subassetMatches = matchSubassets(
            prepared->context.subassets,
            prepared->source.discoveredSubassets);
        CHECK(subassetMatches.size() == 7);
        CHECK(std::ranges::all_of(subassetMatches,
            [](const SubassetMatch& match) {
                return match.method ==
                    SubassetMatchMethod::ExactSourceKey &&
                    match.existingGuid.has_value();
            }));
        const auto imageIdentity = std::ranges::find_if(
            prepared->source.discoveredSubassets,
            [](const DiscoveredSubasset& subasset) {
                return subasset.sourceKey == "images/0";
            });
        CHECK(imageIdentity !=
            prepared->source.discoveredSubassets.end());
        CHECK(imageIdentity->assetType == "iridium.texture");
        CHECK(imageIdentity->structuralFingerprint ==
            "431ced6916a2a21a156e38701afe55bbd7f88969fbbfc56d7fe099d47f265460");

        const CookedArtifactBlob first = buildPreparedArtifact(*prepared);
        const CookedArtifactBlob second = buildPreparedArtifact(*prepared);
        CHECK(first.bytes == second.bytes);
        CHECK(first.artifactHash == second.artifactHash);

        const CookedArtifactReadResult artifact =
            readCookedArtifact(first.bytes, first.artifactHash);
        CHECK(artifact.valid());
        CHECK(artifact.artifact->artifactType == "iridium.model");
        CHECK(artifact.artifact->assetGuid == prepared->assetGuid);

        const CookedModelReadResult typed =
            readCookedModelProduct(*artifact.artifact);
        CHECK(typed.valid());
        const RuntimeModelCpuResult runtime =
            makeRuntimeModelCpuData(*typed.data);
        CHECK(runtime.valid());
        CHECK(runtime.data->vertices.size() == 12);
        CHECK(runtime.data->indices.size() == 12);
        CHECK(runtime.data->primitives.size() == 4);
        CHECK(typed.data->materials.size() == 2);
        CHECK(typed.data->materials[0].sourceKey ==
            "materials/0");
        CHECK(typed.data->materials[0]
            .compiled.textureOperations.size() == 1);
        CHECK(typed.data->materials[0]
            .textureBindings.size() == 1);
        CHECK(typed.data->materials[0]
            .textureBindings[0].operationIndex == 0);
        CHECK(typed.data->materials[0]
            .textureBindings[0].sourceImageIndex == 0);
        CHECK(typed.data->materials[0]
            .textureBindings[0].textureViewIndex == 0);
        CHECK(typed.data->materials[0]
            .textureBindings[0].textureGuid ==
            AssetGuid::parse(
                "019f9bce-85b8-7120-890a-0b0c0d0e0f10")
                .value());
        CHECK(typed.data->materials[1]
            .textureBindings.empty());
        CHECK(typed.data->textureViews.size() == 1);
        CHECK(typed.data->textureViews[0].textureGuid ==
            typed.data->materials[0]
                .textureBindings[0].textureGuid);
        CHECK(typed.data->textureViews[0]
            .manifest.semantic == TextureSemantic::Color);
        CHECK(typed.data->textureViews[0]
            .manifest.viewColorSpace ==
            TextureViewColorSpace::sRGB);
        const RuntimeMaterialFallbacks fallbacks{
            .white = {
                TextureHandle::fromParts(1, 1),
                SamplerHandle::fromParts(1, 1),
            },
            .normal = {
                TextureHandle::fromParts(2, 1),
                SamplerHandle::fromParts(2, 1),
            },
            .linearData = {
                TextureHandle::fromParts(3, 1),
                SamplerHandle::fromParts(3, 1),
            },
        };
        const std::vector<RuntimeTextureViewBinding>
            textureViews{
                {
                    .materialGuid =
                        typed.data->materials[0].materialGuid,
                    .operationIndex = 0,
                    .textureGuid = typed.data->materials[0]
                        .textureBindings[0].textureGuid,
                    .binding = {
                        TextureHandle::fromParts(10, 2),
                        SamplerHandle::fromParts(11, 3),
                    },
                },
            };
        const RuntimeCanonicalMaterialResult
            runtimeMaterials =
                makeRuntimeCanonicalMaterials(
                    *typed.data, textureViews, fallbacks);
        CHECK(runtimeMaterials.valid());
        CHECK(runtimeMaterials.materials.size() == 2);
        CHECK(runtimeMaterials.materials[0]
            .asset.packed.textureIndices[
                static_cast<uint32_t>(
                    SourceTextureSemantic::BaseColor)] == 10);
        CHECK(runtimeMaterials.materials[0]
            .asset.pipelineState.renderPass ==
            RenderPassClass::GBuffer);
        CHECK(runtimeMaterials.materials[0]
            .asset.pipelineState.depthWrite);
        CHECK(runtimeMaterials.materials[1]
            .asset.pipelineState.renderPass ==
            RenderPassClass::Forward);
        CHECK(runtimeMaterials.materials[1]
            .asset.pipelineState.blendMode ==
            BlendMode::AlphaBlend);
        CHECK(!runtimeMaterials.materials[1]
            .asset.pipelineState.depthWrite);
        auto wrongViews = textureViews;
        wrongViews[0].textureGuid =
            AssetGuid::parse(
                "019f9bce-85b8-7121-890a-0b0c0d0e0f11")
                .value();
        CHECK(!makeRuntimeCanonicalMaterials(
            *typed.data, wrongViews, fallbacks).valid());
        CHECK(runtime.data->primitives[0].primitiveGuid ==
            typed.data->manifest.primitives[0].primitiveGuid);
        CHECK(runtime.data->primitives[0].materialGuid ==
            typed.data->manifest.primitives[0].materialGuid);
        CHECK(runtime.data->primitives[0].materialIndex == -1);
        std::vector<RuntimeMaterialBinding> bindings;
        for (const CookedModelPrimitive& primitive :
            typed.data->manifest.primitives) {
            if (std::ranges::none_of(bindings,
                [&primitive](const RuntimeMaterialBinding& binding) {
                    return binding.materialGuid == primitive.materialGuid;
                })) {
                bindings.push_back({
                    .materialGuid = primitive.materialGuid,
                });
            }
        }
        CHECK(bindings.size() == 2);
        const ResolvedRuntimeModelCpuResult resolved =
            resolveRuntimeModelMaterials(*runtime.data, bindings);
        CHECK(resolved.valid());
        CHECK(resolved.data->materials.size() == 2);
        CHECK(resolved.data->geometry.primitives[0].materialIndex == 0);
        CHECK(resolved.data->geometry.primitives[1].materialIndex == 1);
        CHECK(resolved.data->geometry.primitives[2].materialIndex == 0);
        CHECK(resolved.data->geometry.primitives[3].materialIndex == 1);
        bindings.pop_back();
        const ResolvedRuntimeModelCpuResult unresolved =
            resolveRuntimeModelMaterials(*runtime.data, bindings);
        CHECK(!unresolved.valid());
        CHECK(hasCookErrors(unresolved.diagnostics));
        std::vector<CookDiagnostic> diagnostics;
        const auto model = decodeModel(*artifact.artifact, diagnostics);
        CHECK(model.has_value());
        CHECK(diagnostics.empty());
        CHECK(validateModelProduct(*model).empty());
        CHECK(model->manifest.primitives.size() == 4);
        CHECK(model->vertices.size() == 12);
        CHECK(model->indices.size() == 12);
        CHECK(model->rtPositions.size() == 12);
        CHECK(model->rtIndices.size() == 12);
        CHECK(*typed.data == *model);

        CookedArtifact missingSection = *artifact.artifact;
        missingSection.sections.erase(
            std::ranges::find_if(missingSection.sections,
                [](const CookSection& section) {
                    return section.id == kCookedModelRtIndexSection;
                }));
        const CookedModelReadResult rejected =
            readCookedModelProduct(missingSection);
        CHECK(!rejected.valid());
        CHECK(hasCookErrors(rejected.diagnostics));
        return true;
    }

    bool testPrimitiveMaterialCoverageBoundsAndRtParity() {
        const auto prepared = preparedFixture();
        CHECK(prepared && prepared->valid());
        const CookedArtifactBlob blob = buildPreparedArtifact(*prepared);
        const CookedArtifactReadResult artifact =
            readCookedArtifact(blob.bytes, blob.artifactHash);
        CHECK(artifact.valid());
        std::vector<CookDiagnostic> diagnostics;
        const auto model = decodeModel(*artifact.artifact, diagnostics);
        CHECK(model && diagnostics.empty());

        size_t opaqueCount = 0;
        size_t transparentCount = 0;
        size_t mirroredCount = 0;
        std::vector<AssetGuid> opaqueMaterials;
        std::vector<AssetGuid> transparentMaterials;
        for (const CookedModelPrimitive& primitive :
            model->manifest.primitives) {
            CHECK(primitive.winding == ModelWinding::Clockwise);
            CHECK((primitive.attributeMask & ModelAttributeTangent) != 0);
            CHECK((primitive.flags & ModelPrimitiveGeneratedTangent) != 0);
            if (primitive.coverage == ModelCoverage::Opaque) {
                ++opaqueCount;
                opaqueMaterials.push_back(primitive.materialGuid);
                CHECK((primitive.rtFlags & ModelRtOpaque) != 0);
            } else if (primitive.coverage ==
                ModelCoverage::Transparent) {
                ++transparentCount;
                transparentMaterials.push_back(primitive.materialGuid);
                CHECK((primitive.flags & ModelPrimitiveDoubleSided) != 0);
                CHECK((primitive.rtFlags & ModelRtAllowAnyHit) != 0);
            }
            if ((primitive.flags & ModelPrimitiveMirroredTransform) != 0) {
                ++mirroredCount;
                CHECK(primitive.winding == ModelWinding::Clockwise);
                for (uint64_t vertex = primitive.firstVertex;
                    vertex < primitive.firstVertex + primitive.vertexCount;
                    ++vertex) {
                    CHECK(model->vertices[static_cast<size_t>(vertex)]
                        .tangent[3] == -1.0f);
                }
            }

            for (uint64_t item = 0; item < primitive.indexCount; ++item) {
                const uint32_t rasterIndex = model->indices[
                    static_cast<size_t>(primitive.firstIndex + item)];
                const uint32_t rtIndex = model->rtIndices[
                    static_cast<size_t>(primitive.rtFirstIndex + item)];
                CHECK(rasterIndex - primitive.firstVertex == rtIndex);
                CHECK(model->vertices[rasterIndex].position ==
                    model->rtPositions[
                        static_cast<size_t>(
                            primitive.rtFirstPosition + rtIndex)]);
            }
        }
        CHECK(opaqueCount == 2);
        CHECK(transparentCount == 2);
        CHECK(mirroredCount == 2);
        CHECK(opaqueMaterials[0] == opaqueMaterials[1]);
        CHECK(transparentMaterials[0] == transparentMaterials[1]);
        CHECK(opaqueMaterials[0] != transparentMaterials[0]);

        const auto nodeZero = std::ranges::find_if(
            model->manifest.primitives,
            [](const CookedModelPrimitive& primitive) {
                return primitive.sourceNode == 0 &&
                    primitive.sourcePrimitive == 0;
            });
        const auto nodeOne = std::ranges::find_if(
            model->manifest.primitives,
            [](const CookedModelPrimitive& primitive) {
                return primitive.sourceNode == 1 &&
                    primitive.sourcePrimitive == 0;
            });
        CHECK(nodeZero != model->manifest.primitives.end());
        CHECK(nodeOne != model->manifest.primitives.end());
        CHECK(nodeZero->primitiveGuid != nodeOne->primitiveGuid);
        CHECK(nodeZero->bounds.aabbMax[0] == 1.0f);
        CHECK(nodeOne->bounds.aabbMin[0] == 9.0f);
        CHECK(nodeOne->bounds.aabbMax[0] == 10.0f);
        return true;
    }

    bool testOrientationRepairSettingsAndCanonicalWinding() {
        auto prepared = preparedFixture();
        CHECK(prepared && prepared->valid());
        CHECK(prepared->settings.values.at("recalculate_normals") == false);
        CHECK(prepared->settings.values.at("recalculate_tangents") == false);
        CHECK(prepared->settings.values.at("reverse_winding") == false);

        const CookProduct baseline = prepared->importer->cook(
            prepared->source, prepared->settings, prepared->target,
            prepared->context);
        CHECK(!hasCookErrors(baseline.diagnostics));
        const NormalizedImportSettings repaired =
            prepared->importer->normalizeSettings(1, {
                { "bake_node_transforms", true },
                { "generate_missing_tangents", true },
                { "preserve_rt_geometry", true },
                { "recalculate_normals", true },
                { "recalculate_tangents", true },
                { "reverse_winding", true },
            }, true);
        CHECK(repaired.valid());
        CHECK(repaired.canonicalBytes != prepared->settings.canonicalBytes);
        const CookProduct reversed = prepared->importer->cook(
            prepared->source, repaired, prepared->target,
            prepared->context);
        CHECK(!hasCookErrors(reversed.diagnostics));
        CHECK(std::ranges::any_of(reversed.diagnostics,
            [](const CookDiagnostic& diagnostic) {
                return diagnostic.code == "GLTF_WINDING_REVERSED";
            }));

        const auto readGeometry = [](const CookProduct& product) {
            std::vector<CookDiagnostic> diagnostics;
            const auto manifestSection = std::ranges::find_if(
                product.sections, [](const CookSection& section) {
                    return section.id == kCookedModelManifestSection;
                });
            const auto indexSection = std::ranges::find_if(
                product.sections, [](const CookSection& section) {
                    return section.id == kCookedModelIndexSection;
                });
            if (manifestSection == product.sections.end() ||
                indexSection == product.sections.end()) {
                return std::pair<std::optional<CookedModelManifest>,
                    std::optional<std::vector<uint32_t>>>{};
            }
            return std::pair{
                readModelManifest(manifestSection->bytes, diagnostics),
                readModelIndices(indexSection->bytes, diagnostics),
            };
        };
        const auto [baselineManifest, baselineIndices] =
            readGeometry(baseline);
        const auto [reversedManifest, reversedIndices] =
            readGeometry(reversed);
        CHECK(baselineManifest && baselineIndices);
        CHECK(reversedManifest && reversedIndices);
        CHECK(baselineManifest->primitives.size() ==
            reversedManifest->primitives.size());
        for (size_t primitiveIndex = 0;
            primitiveIndex < baselineManifest->primitives.size();
            ++primitiveIndex) {
            const CookedModelPrimitive& before =
                baselineManifest->primitives[primitiveIndex];
            const CookedModelPrimitive& after =
                reversedManifest->primitives[primitiveIndex];
            CHECK(before.winding == ModelWinding::Clockwise);
            CHECK(after.winding == ModelWinding::Clockwise);
            CHECK(before.firstIndex == after.firstIndex);
            CHECK(before.indexCount == after.indexCount);
            for (uint64_t triangle = 0;
                triangle + 2 < before.indexCount;
                triangle += 3) {
                const size_t offset = static_cast<size_t>(
                    before.firstIndex + triangle);
                CHECK(baselineIndices->at(offset) ==
                    reversedIndices->at(offset));
                CHECK(baselineIndices->at(offset + 1) ==
                    reversedIndices->at(offset + 2));
                CHECK(baselineIndices->at(offset + 2) ==
                    reversedIndices->at(offset + 1));
            }
        }
        return true;
    }

    bool testSubassetOrderDoesNotAffectCookAndMissingGuidFails() {
        auto prepared = preparedFixture();
        CHECK(prepared && prepared->valid());
        const CookProduct baseline = prepared->importer->cook(
            prepared->source, prepared->settings, prepared->target,
            prepared->context);
        CHECK(!hasCookErrors(baseline.diagnostics));

        std::reverse(prepared->context.subassets.begin(),
            prepared->context.subassets.end());
        const CookProduct reordered = prepared->importer->cook(
            prepared->source, prepared->settings, prepared->target,
            prepared->context);
        CHECK(!hasCookErrors(reordered.diagnostics));
        CHECK(baseline.sections == reordered.sections);

        prepared->context.subassets.erase(
            std::ranges::find_if(prepared->context.subassets,
                [](const SubassetMetadata& subasset) {
                    return subasset.sourceKey ==
                        "nodes/1/meshes/0/primitives/1";
                }));
        const CookProduct missing = prepared->importer->cook(
            prepared->source, prepared->settings, prepared->target,
            prepared->context);
        CHECK(hasCookErrors(missing.diagnostics));
        CHECK(missing.sections.empty());
        CHECK(std::ranges::any_of(missing.diagnostics,
            [](const CookDiagnostic& diagnostic) {
                return diagnostic.code ==
                    "GLTF_PRIMITIVE_GUID_MISSING";
            }));

        prepared = preparedFixture();
        CHECK(prepared && prepared->valid());
        prepared->context.subassets.erase(
            std::ranges::find_if(prepared->context.subassets,
                [](const SubassetMetadata& subasset) {
                    return subasset.sourceKey == "images/0";
                }));
        const CookProduct missingImage =
            prepared->importer->cook(
                prepared->source, prepared->settings,
                prepared->target, prepared->context);
        CHECK(hasCookErrors(missingImage.diagnostics));
        CHECK(missingImage.sections.empty());
        CHECK(std::ranges::any_of(
            missingImage.diagnostics,
            [](const CookDiagnostic& diagnostic) {
                return diagnostic.code ==
                    "GLTF_IMAGE_GUID_MISSING";
            }));
        return true;
    }

    bool testMalformedAccessorFailsDeterministically() {
        GltfModelImporter importer;
        const auto settings = importer.normalizeSettings(1, {
            { "bake_node_transforms", true },
            { "generate_missing_tangents", true },
            { "preserve_rt_geometry", true },
        }, true);
        CHECK(settings.valid());
        std::vector<std::byte> bytes =
            readFile(fixtureRoot() / "gltf_model_cooker_fixture.gltf");
        CHECK(!bytes.empty());
        std::string text(
            reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const size_t count = text.find("\"count\": 3");
        CHECK(count != std::string::npos);
        text.replace(count, std::string("\"count\": 3").size(),
            "\"count\": 4");
        bytes.assign(
            reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size()));

        const ParsedSourceAsset first = importer.parse({
            .relativePath = "gltf_model_cooker_fixture.gltf",
            .resolvedPath =
                fixtureRoot() / "gltf_model_cooker_fixture.gltf",
            .bytes = bytes,
        }, settings);
        const ParsedSourceAsset second = importer.parse({
            .relativePath = "gltf_model_cooker_fixture.gltf",
            .resolvedPath =
                fixtureRoot() / "gltf_model_cooker_fixture.gltf",
            .bytes = bytes,
        }, settings);
        CHECK(hasCookErrors(first.diagnostics));
        CHECK(first.documentBytes.empty());
        CHECK(first.diagnostics.size() == second.diagnostics.size());
        CHECK(first.diagnostics.back().code == second.diagnostics.back().code);
        CHECK(first.diagnostics.back().message ==
            second.diagnostics.back().message);
        return true;
    }

    bool testMetadataDiscoveryAvoidsCookDocumentAndCancels() {
        GltfModelImporter importer;
        const auto settings =
            importer.normalizeSettings(
                1, {
                    { "bake_node_transforms", true },
                    { "generate_missing_tangents", true },
                    { "preserve_rt_geometry", true },
                }, true);
        CHECK(settings.valid());
        const auto path =
            fixtureRoot() /
            "gltf_model_cooker_fixture.gltf";
        const auto bytes =
            readFile(path);
        const ParsedSourceAsset metadata =
            importer.parse({
                .relativePath =
                    path.filename(),
                .resolvedPath = path,
                .bytes = bytes,
                .metadataOnly = true,
            }, settings);
        CHECK(!hasCookErrors(
            metadata.diagnostics));
        CHECK(metadata.documentBytes.empty());
        CHECK(metadata.subassetPayloads.empty());
        CHECK(metadata.discoveredSubassets.size() ==
            7);

        std::stop_source stop;
        stop.request_stop();
        const ParsedSourceAsset cancelled =
            importer.parse({
                .relativePath =
                    path.filename(),
                .resolvedPath = path,
                .bytes = bytes,
                .stopToken =
                    stop.get_token(),
                .metadataOnly = true,
            }, settings);
        CHECK(hasCookErrors(
            cancelled.diagnostics));
        CHECK(cancelled.documentBytes.empty());
        CHECK(cancelled.diagnostics.back()
            .message.find("cancelled") !=
            std::string::npos);
        return true;
    }

    bool testMetadataDiscoveryRejectsUnresolvedGitLfsDependencies() {
        TemporaryDirectory temporary;
        const auto gltfPath =
            temporary.path / "lfs.gltf";
        {
            std::ofstream pointer(
                temporary.path / "white.png",
                std::ios::binary);
            pointer <<
                "version https://git-lfs.github.com/spec/v1\n"
                "oid sha256:e2ab2939dd0b2c771b68c418987fb9fdfb60e7f2a5e3f9a3a646e2571ec20b15\n"
                "size 70\n";
            std::ofstream gltf(
                gltfPath,
                std::ios::binary);
            gltf << R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [{ "nodes": [0] }],
  "nodes": [{ "mesh": 0 }],
  "meshes": [{ "primitives": [{}] }],
  "images": [{ "uri": "white.png", "mimeType": "image/png" }]
})";
        }
        GltfModelImporter importer;
        const auto settings =
            importer.normalizeSettings(1, {
                { "bake_node_transforms", true },
                { "generate_missing_tangents", true },
                { "preserve_rt_geometry", true },
            }, true);
        CHECK(settings.valid());
        const ParsedSourceAsset parsed =
            importer.parse({
                .relativePath =
                    gltfPath.filename(),
                .resolvedPath = gltfPath,
                .bytes = readFile(gltfPath),
                .metadataOnly = true,
            }, settings);
        CHECK(hasCookErrors(
            parsed.diagnostics));
        CHECK(parsed.diagnostics.back()
            .message.find(
                "image 0 dependency 'white.png'") !=
            std::string::npos);
        CHECK(parsed.diagnostics.back()
            .message.find("Git LFS") !=
            std::string::npos);
        return true;
    }

    bool testUniformImportScaleAffectsAllGeometry() {
        auto prepared = preparedFixture();
        CHECK(prepared && prepared->valid());
        const CookProduct baseline =
            prepared->importer->cook(
                prepared->source,
                prepared->settings,
                prepared->target,
                prepared->context);
        CHECK(!hasCookErrors(
            baseline.diagnostics));
        const NormalizedImportSettings scaled =
            prepared->importer->normalizeSettings(
                1, {
                    { "bake_node_transforms", true },
                    { "generate_missing_tangents", true },
                    { "import_scale", 0.01 },
                    { "preserve_rt_geometry", true },
                }, true);
        CHECK(scaled.valid());
        const CookProduct reduced =
            prepared->importer->cook(
                prepared->source, scaled,
                prepared->target,
                prepared->context);
        CHECK(!hasCookErrors(
            reduced.diagnostics));
        const auto baselineSection =
            std::ranges::find_if(
                baseline.sections,
                [](const CookSection& section) {
                    return section.id ==
                        kCookedModelVertexSection;
                });
        const auto reducedSection =
            std::ranges::find_if(
                reduced.sections,
                [](const CookSection& section) {
                    return section.id ==
                        kCookedModelVertexSection;
                });
        CHECK(baselineSection !=
            baseline.sections.end());
        CHECK(reducedSection !=
            reduced.sections.end());
        std::vector<CookDiagnostic> diagnostics;
        const auto baselineVertices =
            readModelVertices(
                baselineSection->bytes,
                diagnostics);
        const auto reducedVertices =
            readModelVertices(
                reducedSection->bytes,
                diagnostics);
        CHECK(baselineVertices &&
            reducedVertices &&
            diagnostics.empty());
        CHECK(baselineVertices->size() ==
            reducedVertices->size());
        for (size_t index = 0;
            index < baselineVertices->size();
            ++index) {
            for (size_t axis = 0;
                axis < 3; ++axis) {
                CHECK(std::abs(
                    reducedVertices->at(index)
                        .position[axis] -
                    baselineVertices->at(index)
                        .position[axis] *
                        0.01f) < 1.0e-5f);
            }
            for (size_t axis = 0;
                axis < 3; ++axis) {
                CHECK(std::abs(
                    reducedVertices->at(index)
                        .normal[axis] -
                    baselineVertices->at(index)
                        .normal[axis]) <
                    1.0e-5f);
            }
        }
        return true;
    }

    bool testDegenerateAuthoredTangentsAreRegenerated() {
        auto prepared = preparedFixture();
        CHECK(prepared && prepared->valid());
        nlohmann::ordered_json parsed =
            nlohmann::ordered_json::parse(
                reinterpret_cast<const char*>(
                    prepared->source.documentBytes.data()),
                reinterpret_cast<const char*>(
                    prepared->source.documentBytes.data() +
                    prepared->source.documentBytes.size()));
        auto& primitive =
            parsed.at("primitives").at(0);
        auto& vertex =
            primitive.at("vertices").at(0);
        vertex["tangent"] =
            vertex.at("normal");
        vertex["tangent"].push_back(1.0f);
        const std::string encoded = parsed.dump();
        prepared->source.documentBytes.assign(
            reinterpret_cast<const std::byte*>(
                encoded.data()),
            reinterpret_cast<const std::byte*>(
                encoded.data() +
                encoded.size()));

        const CookProduct product =
            prepared->importer->cook(
                prepared->source,
                prepared->settings,
                prepared->target,
                prepared->context);
        CHECK(!hasCookErrors(
            product.diagnostics));
        const auto warning =
            std::ranges::find_if(
                product.diagnostics,
                [](const CookDiagnostic&
                    diagnostic) {
                    return diagnostic.code ==
                        "GLTF_TANGENT_REGENERATED";
                });
        CHECK(warning !=
            product.diagnostics.end());
        CHECK(warning->field ==
            "nodes/0/meshes/0/primitives/0");
        const auto manifestSection =
            std::ranges::find_if(
                product.sections,
                [](const CookSection&
                    section) {
                    return section.id ==
                        kCookedModelManifestSection;
                });
        CHECK(manifestSection !=
            product.sections.end());
        std::vector<CookDiagnostic>
            manifestDiagnostics;
        const auto manifest =
            readModelManifest(
                manifestSection->bytes,
                manifestDiagnostics);
        CHECK(manifest.has_value());
        CHECK(manifestDiagnostics.empty());
        CHECK((manifest->primitives[0].flags &
            ModelPrimitiveGeneratedTangent) != 0);
        return true;
    }

    bool testWarmReceiptAvoidsSourceParse() {
        const auto prepared = preparedFixture();
        CHECK(prepared && prepared->valid());
        const CookedArtifactBlob blob =
            buildPreparedArtifact(*prepared);
        const std::filesystem::path cacheRoot =
            std::filesystem::temp_directory_path() /
            ("iridium-m3-4-receipt-" +
                createAssetGuidV7().toString());
        {
            LocalDerivedDataCache cache(cacheRoot);
            CHECK(storePreparedCookReceipt(cache,
                "gltf_model_cooker_fixture.gltf", *prepared).empty());
            CHECK(cache.storeAtomic(prepared->cookKey, blob).empty());

            const auto metadata = readAssetMetadata(
                fixtureRoot() /
                    "gltf_model_cooker_fixture.gltf.iridium.meta");
            CHECK(metadata.metadata.has_value());
            ImporterRegistry registry;
            registry.registerImporter(
                std::make_shared<GltfModelImporter>());
            std::vector<CookDiagnostic> diagnostics;
            const auto warm = tryPrepareAssetCookFromReceipt(
                registry, cache, fixtureRoot(),
                "gltf_model_cooker_fixture.gltf",
                *metadata.metadata, prepared->target,
                "m3.4-model-v1", diagnostics);
            CHECK(warm.has_value());
            CHECK(warm->valid());
            CHECK(warm->cookKey == prepared->cookKey);
            CHECK(warm->source.documentBytes.empty());
            CHECK(warm->resolvedDependencies ==
                prepared->resolvedDependencies);
            CHECK(diagnostics.empty());
        }
        std::error_code error;
        std::filesystem::remove_all(cacheRoot, error);
        CHECK(!error);
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*function)();
    };
    const std::vector<TestCase> tests{
        { "deterministic artifact", testDeterministicCookAndArtifactRoundTrip },
        { "primitive material bounds RT parity",
            testPrimitiveMaterialCoverageBoundsAndRtParity },
        { "orientation repair settings and canonical winding",
            testOrientationRepairSettingsAndCanonicalWinding },
        { "subasset identity", testSubassetOrderDoesNotAffectCookAndMissingGuidFails },
        { "malformed accessor", testMalformedAccessorFailsDeterministically },
        { "metadata-only discovery and cancellation",
            testMetadataDiscoveryAvoidsCookDocumentAndCancels },
        { "metadata-only discovery rejects Git LFS pointers",
            testMetadataDiscoveryRejectsUnresolvedGitLfsDependencies },
        { "uniform import scale",
            testUniformImportScaleAffectsAllGeometry },
        { "degenerate authored tangent regeneration",
            testDegenerateAuthoredTangentsAreRegenerated },
        { "warm receipt", testWarmReceiptAvoidsSourceParse },
    };
    for (const TestCase& test : tests) {
        if (!test.function()) {
            std::cerr << "FAILED: " << test.name << '\n';
            return 1;
        }
        std::cout << "PASSED: " << test.name << '\n';
    }
    return 0;
}
