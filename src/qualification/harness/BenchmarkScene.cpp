// Deterministic benchmark content: manifest and startup content (ContentLoad),
// the fixture scene (SceneConstruction/SceneComplete) and per-frame benchmark
// animation (PreSceneUpdate). Entity creation order is part of the frozen-capture
// contract; keep it.

#include "qualification/harness/QualificationHarness.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>

#include "assets/AssetManager.h"
#include "assets/cooker/CookTypes.h"
#include "assets/environment/EnvironmentConvolution.h"
#include "renderer/rhi/Mesh.h"
#include "scene/SceneWorld.h"
#include "scene/components/LightComponent.h"
#include "scene/components/MeshComponent.h"
#include "scene/components/NameComponent.h"
#include "scene/components/ReflectionProbeComponent.h"
#include "scene/components/RelationshipComponent.h"
#include "scene/components/RenderInstanceBatchComponent.h"
#include "scene/components/TransformComponent.h"
#include "utils/Sha256.h"

namespace Iridium {

    namespace {
        uint64_t elapsedNanoseconds(
            std::chrono::steady_clock::time_point start) {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start).count());
        }
    }

    void QualificationHarness::loadBenchmarkContent(
        AppStartupContext& context) {
        ApplicationConfig& config = context.config;
        const auto manifestStart = std::chrono::steady_clock::now();
        const std::filesystem::path manifestPath = options_.benchmarkManifest.empty()
            ? std::filesystem::path(PROJECT_ROOT_DIR) /
                "assets" / "benchmarks" / "m0" / "manifest.v1.json"
            : options_.benchmarkManifest;
        const BenchmarkManifest manifest = loadBenchmarkManifest(manifestPath);
        const std::filesystem::path projectRoot =
            std::filesystem::weakly_canonical(PROJECT_ROOT_DIR);
        benchmarkManifestPath_ = std::filesystem::relative(
            manifest.sourcePath, projectRoot).generic_string();
        benchmarkManifestSha256_ = sha256File(manifest.sourcePath);
        benchmark_ = findBenchmarkFixture(manifest, options_.benchmarkId);
        context.timings.manifestVerificationNanoseconds =
            elapsedNanoseconds(manifestStart);
        const auto importStart = std::chrono::steady_clock::now();
        if (config.cookedModelArtifact.empty()) {
            throw std::invalid_argument(
                "Benchmark runtime requires --cooked-model-artifact after the M3 production cutover.");
        }
        (void)context.control.loadCookedStartupModel();
        context.timings.modelLoadNanoseconds = elapsedNanoseconds(importStart);
        const auto environmentStart = std::chrono::steady_clock::now();
        if (!config.cookedEnvironmentArtifact.empty()) {
            context.control.loadCookedStartupEnvironment();
        }
        else {
            // Honor the frozen fixture's declared illumination. Leaving the
            // backend's neutral black fallback here made no-light fixtures
            // produce identical all-black captures despite drawing geometry.
            const AssetGuid fixtureEnvironmentGuid = *AssetGuid::parse(
                "019fc681-2110-7000-8000-000000000001");
            const glm::vec3 color = benchmark_->constantEnvironmentLinear;
            const CookProduct product = makeConstantEnvironmentProduct(
                fixtureEnvironmentGuid, color);
            if (hasCookErrors(product.diagnostics))
                throw std::runtime_error("Constant benchmark environment is invalid.");
            const std::string recipe = "iridium.benchmark.constant_environment.v1/" +
                std::to_string(std::bit_cast<uint32_t>(color.x)) + "/" +
                std::to_string(std::bit_cast<uint32_t>(color.y)) + "/" +
                std::to_string(std::bit_cast<uint32_t>(color.z));
            LoadedEnvironmentAsset environment = context.assets->
                loadEnvironmentFromCookedArtifact({
                    .assetGuid = fixtureEnvironmentGuid,
                    .artifactType = product.artifactType,
                    .artifactSchemaVersion = product.artifactSchemaVersion,
                    .cookKey = sha256(std::as_bytes(std::span(recipe))),
                    .sections = product.sections,
                });
            context.control.publishStartupEnvironment(std::move(environment));
        }
        context.timings.environmentCreationNanoseconds =
            elapsedNanoseconds(environmentStart);
        AppCamera& camera = context.camera;
        camera.position = benchmark_->camera.position;
        camera.front = glm::normalize(benchmark_->camera.target - camera.position);
        camera.up = glm::normalize(benchmark_->camera.up);
        camera.verticalFovDegrees = benchmark_->camera.verticalFovDegrees;
        camera.nearPlane = benchmark_->camera.nearPlane;
        camera.farPlane = benchmark_->camera.farPlane;
        if (!config.warmupFrameCountSpecified) {
            config.warmupFrameCount = benchmark_->warmupFrames;
        }
        if (!config.frameLimitSpecified) {
            config.frameLimit = benchmark_->measuredFrames;
        }
    }

    namespace {
        // A composition entity draws one top-level glTF node of the startup
        // model: a non-owning view sharing the cooked model's geometry,
        // materials and textures (the AssetManager material-preview pattern).
        // Each view is a distinct ModelAsset address, which is the identity
        // extraction and the GPU-scene observation key per-model state on.
        std::shared_ptr<ModelAsset> makeModelNodeView(const ModelAsset& model,
            uint32_t sourceNode) {
            auto view = std::make_shared<ModelAsset>(model);
            view->ownsGeometry = view->ownsMaterials = view->ownsTextures =
                false;
            view->ownedTextures.clear();
            view->subMeshes.clear();
            view->lodChains.clear();
            view->totalIndices = 0;
            for (const SubMesh& subMesh : model.subMeshes) {
                if (subMesh.sourceNode != sourceNode) continue;
                view->subMeshes.push_back(subMesh);
                view->totalIndices += subMesh.indexCount;
                for (const ModelLodChain& chain : model.lodChains) {
                    if (chain.basePrimitiveGuid == subMesh.primitiveGuid)
                        view->lodChains.push_back(chain);
                }
            }
            if (view->subMeshes.empty()) {
                throw std::invalid_argument(
                    "Composition entity names glTF node " +
                    std::to_string(sourceNode) +
                    ", which has no cooked primitives in the startup model");
            }
            return view;
        }

        // Stable identity: a fixed UUIDv7 timestamp per ordinal plus a hash
        // of the fixture and entity IDs, so it survives ECS index reuse and
        // differs between fixtures.
        SceneEntityUuid compositionEntityUuid(const std::string& fixtureId,
            const std::string& entityId, uint32_t ordinal) {
            uint64_t hash = 1469598103934665603ull;
            for (const std::string* text : { &fixtureId, &entityId }) {
                for (const unsigned char character : *text) {
                    hash = (hash ^ character) * 1099511628211ull;
                }
                hash = (hash ^ 0xffu) * 1099511628211ull;
            }
            std::array<uint8_t, 10> random{ 0x49, 0x52 };
            for (size_t byte = 0; byte < sizeof(hash); ++byte) {
                random[2 + byte] = static_cast<uint8_t>(hash >> (byte * 8u));
            }
            return SceneEntityUuid::fromUuidV7Fields(
                1'775'000'600'000ull + ordinal, random);
        }

        void applyCompositionPose(TransformComponent& transform,
            const BenchmarkEntityPose& pose) {
            if (transform.position == pose.translation &&
                transform.rotation == pose.rotationDegrees &&
                transform.scale == pose.scale) return;
            transform.position = pose.translation;
            transform.rotation = pose.rotationDegrees;
            transform.scale = pose.scale;
            transform.isDirty = true;
        }
    }

    void QualificationHarness::constructBenchmarkComposition(
        AppStartupContext& context) {
        if (!context.mainModel) {
            throw std::logic_error(
                "Composition benchmark requires a loaded startup model");
        }
        SceneWorld& sceneWorld = context.scene;
        Registry& registry = sceneWorld.registry();
        const std::vector<BenchmarkCompositionEntity>& entities =
            benchmark_->sceneFactory.compositionEntities;
        std::map<uint32_t, std::shared_ptr<ModelAsset>> nodeViews;
        benchmarkInstances_.reserve(benchmarkInstances_.size() +
            entities.size());
        Entity firstEntity = NULL_ENTITY;
        for (uint32_t ordinal = 0; ordinal < entities.size(); ++ordinal) {
            const BenchmarkCompositionEntity& spec = entities[ordinal];
            std::shared_ptr<ModelAsset>& view = nodeViews[spec.sourceNode];
            if (!view) view = makeModelNodeView(*context.mainModel,
                spec.sourceNode);
            const Entity entity = sceneWorld.createEntity(
                compositionEntityUuid(benchmark_->id, spec.id, ordinal));
            registry.addComponent<NameComponent>(entity).name =
                "Benchmark " + spec.id;
            auto& transform = registry.addComponent<TransformComponent>(entity);
            const BenchmarkEntityPose pose =
                evaluateBenchmarkCompositionEntity(spec, 0);
            applyCompositionPose(transform, pose);
            transform.worldMatrix = glm::mat4(1.0f);
            transform.isDirty = true;
            registry.addComponent<RelationshipComponent>(entity).siblingOrder =
                static_cast<int32_t>(ordinal);
            auto& mesh = registry.addComponent<MeshComponent>(entity);
            // No requestedAssetGuid: resolving it would replace the node view
            // with the whole startup model.
            mesh.model = view;
            mesh.assetGuid = context.startupModelGuid;
            mesh.enabled = true;
            if (firstEntity == NULL_ENTITY) firstEntity = entity;
            benchmarkInstances_.push_back({ entity, pose.translation });
        }
        context.firstEntity = firstEntity;
        context.initialSelection = options_.selectBenchmarkEntity
            ? firstEntity : NULL_ENTITY;
    }

    void QualificationHarness::constructBenchmarkScene(
        AppStartupContext& context) {
        if (benchmark_->sceneFactory.kind ==
            BenchmarkSceneFactoryKind::Composition) {
            constructBenchmarkComposition(context);
            return;
        }
        Registry& registry = context.scene.registry();
        const std::shared_ptr<ModelAsset>& mainModel = context.mainModel;
        const AssetGuid startupModelGuid = context.startupModelGuid;
        const BenchmarkSceneFactory& sceneFactory = benchmark_->sceneFactory;
        const glm::uvec3 grid = sceneFactory.instanceGrid;
        const glm::vec3 spacing = sceneFactory.instanceSpacing;
        const glm::vec3 gridCenter = (glm::vec3(grid) - glm::vec3(1.0f)) * 0.5f;
        const uint64_t gridInstanceCount = benchmarkInstanceCount(grid);
        const bool renderInstanceBatch = sceneFactory.renderInstanceBatch;
        const glm::uvec3 constructionGrid = renderInstanceBatch
            ? glm::uvec3(1u) : grid;
        benchmarkInstances_.reserve(benchmarkInstances_.size() +
            static_cast<size_t>(renderInstanceBatch
                ? 1u : gridInstanceCount));
        Entity firstEntity = NULL_ENTITY;
        uint32_t benchmarkInstanceOrdinal = 0u;
        for (uint32_t z = 0; z < constructionGrid.z; ++z) {
            for (uint32_t y = 0; y < constructionGrid.y; ++y) {
                for (uint32_t x = 0; x < constructionGrid.x; ++x) {
                    const glm::vec3 position = !renderInstanceBatch
                        ? (glm::vec3(x, y, z) - gridCenter) * spacing
                        : glm::vec3(0.0f);
                    // The ordinary editor helper deliberately scans existing
                    // names and sibling order. A deterministic benchmark grid
                    // already owns both values, so repeating those scans would
                    // turn fixture construction into O(n^2) editor-only work.
                    const Entity entity = registry.createEntity();
                    auto& name = registry.addComponent<NameComponent>(entity);
                    name.name = benchmarkInstanceOrdinal == 0u
                        ? "Benchmark Model"
                        : "Benchmark Model (" +
                            std::to_string(benchmarkInstanceOrdinal + 1u) + ")";
                    auto& transform =
                        registry.addComponent<TransformComponent>(entity);
                    transform.position = position;
                    transform.isDirty = true;
                    auto& relationship =
                        registry.addComponent<RelationshipComponent>(entity);
                    relationship.siblingOrder = static_cast<int32_t>(
                        benchmarkInstanceOrdinal);
                    auto& mesh = registry.addComponent<MeshComponent>(entity);
                    mesh.assetGuid = startupModelGuid;
                    mesh.requestedAssetGuid = startupModelGuid;
                    if (firstEntity == NULL_ENTITY) firstEntity = entity;
                    transform.rotation = glm::vec3(0.0f);
                    transform.scale = glm::vec3(1.0f);
                    if (!renderInstanceBatch) {
                        transform.scale =
                            sceneFactory.instanceScaleOverrideEnabled &&
                                sceneFactory.instanceScaleOverrideIndex ==
                                    benchmarkInstanceOrdinal
                            ? sceneFactory.instanceScaleOverride
                            : sceneFactory.instanceScale;
                    }
                    transform.worldMatrix = glm::mat4(1.0f);
                    transform.isDirty = true;

                    mesh.model = mainModel;
                    mesh.assetGuid = startupModelGuid;
                    if (!mainModel) {
                        mesh.requestedAssetGuid = startupModelGuid;
                    }
                    mesh.enabled = true;
                    benchmarkInstances_.push_back({ entity, transform.position });
                    ++benchmarkInstanceOrdinal;
                }
            }
        }
        if (renderInstanceBatch) {
            if (firstEntity == NULL_ENTITY || !mainModel) {
                throw std::logic_error(
                    "Render-batch benchmark requires a loaded model entity");
            }
            auto& batch = registry.addComponent<
                RenderInstanceBatchComponent>(firstEntity);
            batch.localTransforms.reserve(static_cast<size_t>(
                gridInstanceCount));
            for (uint32_t z = 0; z < grid.z; ++z) {
                for (uint32_t y = 0; y < grid.y; ++y) {
                    for (uint32_t x = 0; x < grid.x; ++x) {
                        const glm::vec3 position =
                            (glm::vec3(x, y, z) - gridCenter) * spacing;
                        glm::mat4 local = glm::translate(
                            glm::mat4(1.0f), position);
                        local = glm::scale(local, sceneFactory.instanceScale);
                        batch.localTransforms.push_back(local);
                    }
                }
            }
            batch.subMeshBounds.resize(mainModel->subMeshes.size());
            for (size_t subMeshIndex = 0u;
                subMeshIndex < mainModel->subMeshes.size(); ++subMeshIndex) {
                const SubMesh& subMesh = mainModel->subMeshes[subMeshIndex];
                glm::vec3 minimum((std::numeric_limits<float>::max)());
                glm::vec3 maximum((std::numeric_limits<float>::lowest)());
                for (const glm::mat4& local : batch.localTransforms) {
                    for (uint32_t corner = 0u; corner < 8u; ++corner) {
                        const glm::vec3 point{
                            (corner & 1u) != 0u ? subMesh.boundsMax.x :
                                subMesh.boundsMin.x,
                            (corner & 2u) != 0u ? subMesh.boundsMax.y :
                                subMesh.boundsMin.y,
                            (corner & 4u) != 0u ? subMesh.boundsMax.z :
                                subMesh.boundsMin.z,
                        };
                        const glm::vec4 transformed = local *
                            glm::vec4(point, 1.0f);
                        minimum = glm::min(minimum,
                            glm::vec3(transformed));
                        maximum = glm::max(maximum,
                            glm::vec3(transformed));
                    }
                }
                batch.subMeshBounds[subMeshIndex] = {
                    .minimum = minimum,
                    .maximum = maximum,
                    .valid = !batch.localTransforms.empty(),
                };
            }
        }
        context.firstEntity = firstEntity;
        context.initialSelection = options_.selectBenchmarkEntity
            ? firstEntity : NULL_ENTITY;
    }

    void QualificationHarness::constructGeneratedLights(
        AppStartupContext& context) {
        SceneWorld& sceneWorld = context.scene;
        Registry& registry = sceneWorld.registry();
        const std::optional<BenchmarkFixture>& activeBenchmark = benchmark_;
        const bool sampleCarLightingFixture = activeBenchmark &&
            activeBenchmark->id == "sample_car_lighting_local_v1";
        const bool spotShadowContactFixture = activeBenchmark &&
            (activeBenchmark->id == "spot_shadow_contact_v1" ||
                activeBenchmark->id == "spot_shadow_contact_forward_v1");
        const bool pointShadowContactFixture = activeBenchmark &&
            (activeBenchmark->id == "point_shadow_contact_v1" ||
                activeBenchmark->id == "point_shadow_contact_forward_v1");
        const bool directionalShadowFixture = activeBenchmark &&
            (activeBenchmark->id == "directional_shadow_contact_v1" ||
                activeBenchmark->id == "directional_shadow_motion_v1");
        const bool explicitFixtureLights = activeBenchmark &&
            !activeBenchmark->lights.empty() &&
            options_.clusterStressLightCount == 0 &&
            options_.validateLightTableScale == 0;
        const uint32_t fixtureLightCount = explicitFixtureLights
            ? static_cast<uint32_t>(activeBenchmark->lights.size())
            : directionalShadowFixture
            ? 1u : (spotShadowContactFixture || pointShadowContactFixture)
            ? 2u : sampleCarLightingFixture ? 3u : 0u;
        const uint32_t generatedLightCount = options_.clusterStressLightCount != 0
            ? options_.clusterStressLightCount
            : options_.validateLightTableScale != 0
                ? options_.validateLightTableScale
                : fixtureLightCount;
        generatedLightCount_ = generatedLightCount;
        if (generatedLightCount == 0) return;
        for (uint32_t index = 0;
            index < generatedLightCount; ++index) {
            std::array<uint8_t, 10> random{};
            const uint64_t value = static_cast<uint64_t>(index) + 1;
            for (size_t byte = 0; byte < sizeof(value); ++byte) {
                random[byte] = static_cast<uint8_t>(value >> (byte * 8u));
            }
            const Entity lightEntity = sceneWorld.createEntity(
                SceneEntityUuid::fromUuidV7Fields(
                    1'775'000'300'000ull + index, random));
            auto& transform = registry.addComponent<TransformComponent>(
                lightEntity);
            const BenchmarkLight* fixtureLight = explicitFixtureLights
                ? &activeBenchmark->lights[index] : nullptr;
            if (fixtureLight) {
                transform.position = fixtureLight->position;
                transform.rotation = fixtureLight->rotationDegrees;
            }
            else if (sampleCarLightingFixture) {
                constexpr std::array<glm::vec3, 3> kRigPositions{
                    glm::vec3(-3.0f, 4.0f, 3.0f),
                    glm::vec3(3.0f, 2.25f, 1.5f),
                    glm::vec3(0.0f, 5.0f, -2.0f),
                };
                transform.position = kRigPositions[index];
                if (index != 1u) {
                    const glm::vec3 emissionDirection = glm::normalize(
                        glm::vec3(0.0f, 1.0f, 0.0f) - transform.position);
                    transform.rotation.x = -glm::degrees(
                        std::asin(emissionDirection.y));
                    transform.rotation.y = glm::degrees(std::atan2(
                        emissionDirection.x, emissionDirection.z));
                }
            }
            else if (spotShadowContactFixture || pointShadowContactFixture) {
                const float lightHeight = spotShadowContactFixture
                    ? 4.0f : 3.0f;
                transform.position = index % 2u == 0u
                    ? glm::vec3(-3.0f, lightHeight, 3.0f)
                    : glm::vec3(3.0f, lightHeight, 3.0f);
                if (spotShadowContactFixture) {
                    const glm::vec3 emissionDirection = glm::normalize(
                        -transform.position);
                    transform.rotation.x = -glm::degrees(
                        std::asin(emissionDirection.y));
                    transform.rotation.y = glm::degrees(std::atan2(
                        emissionDirection.x, emissionDirection.z));
                }
            }
            else if (options_.clusterStressLightCount != 0) {
                transform.position = {
                    (static_cast<float>(index % 32u) - 15.5f) * 2.0f,
                    (static_cast<float>((index / 32u) % 16u) - 7.5f) * 2.0f,
                    -10.0f - static_cast<float>(index % 16u) * 4.0f,
                };
            }
            else {
                transform.position = {
                    static_cast<float>(index % 64u) - 31.5f,
                    static_cast<float>((index / 64u) % 64u) - 31.5f,
                    static_cast<float>(index / 4'096u),
                };
            }
            if (options_.clusterStressLightCount != 0 && index < 4u) {
                // Spread global stress lights across opposing azimuths so
                // multi-owner shadow composition is exercised, not merely
                // duplicate projections from coincident directions.
                transform.rotation.y = 135.0f +
                    static_cast<float>(index) * 90.0f;
            }
            else if (!fixtureLight && activeBenchmark &&
                (activeBenchmark->id == "directional_shadow_contact_v1" ||
                    activeBenchmark->id == "directional_shadow_motion_v1") &&
                index == 0u) {
                // Preserve the fixture's incoming-light direction after +Z
                // became the authored emission axis.
                transform.rotation.y = 210.0f;
            }
            registry.addComponent<RelationshipComponent>(lightEntity)
                .siblingOrder = static_cast<int32_t>(index);
            auto& light = registry.addComponent<LightComponent>(lightEntity);
            if (fixtureLight) {
                switch (fixtureLight->type) {
                case BenchmarkLightType::Directional:
                    light.type = LightType::Directional;
                    break;
                case BenchmarkLightType::Point:
                    light.type = LightType::Point;
                    break;
                case BenchmarkLightType::Spot:
                    light.type = LightType::Spot;
                    break;
                }
                light.colorLinearRec709 = fixtureLight->colorLinearRec709;
                light.illuminanceLux = fixtureLight->illuminanceLux;
                light.luminousIntensityCandela =
                    fixtureLight->luminousIntensityCandela;
                light.rangeMeters = fixtureLight->rangeMeters;
                light.sourceRadiusMeters = fixtureLight->sourceRadiusMeters;
                light.innerConeDegrees = fixtureLight->innerConeDegrees;
                light.outerConeDegrees = fixtureLight->outerConeDegrees;
                light.castsShadows = fixtureLight->castsShadows &&
                    !options_.disableBenchmarkLocalShadows;
                light.shadowQuality = static_cast<LightShadowQuality>(
                    fixtureLight->shadowQuality);
                light.priority = fixtureLight->priority;
                continue;
            }
            light.type = sampleCarLightingFixture
                ? (index == 0u ? LightType::Spot :
                    index == 1u ? LightType::Point : LightType::Directional)
                : directionalShadowFixture
                ? LightType::Directional
                : spotShadowContactFixture
                ? LightType::Spot
                : pointShadowContactFixture ? LightType::Point
                : options_.clusterStressLightCount == 0
                ? static_cast<LightType>(index % 3u)
                : (index < 4u ? LightType::Directional :
                    (index % 2u == 0u ? LightType::Point : LightType::Spot));
            if (directionalShadowFixture || spotShadowContactFixture ||
                pointShadowContactFixture) {
                light.castsShadows =
                    !options_.disableBenchmarkLocalShadows;
            }
            else if (sampleCarLightingFixture) {
                light.castsShadows = true;
            }
            if (!spotShadowContactFixture && !pointShadowContactFixture &&
                options_.clusterStressLightCount != 0 &&
                light.type == LightType::Spot) {
                // The stress volume is centered in front of these negative-Z
                // lights, so the authored +Z emission axis already aims back
                // through the volume.
                transform.rotation.y = 0.0f;
            }
            light.colorLinearRec709 = { 1.0f, 0.5f, 0.25f };
            if (sampleCarLightingFixture || directionalShadowFixture ||
                spotShadowContactFixture || pointShadowContactFixture) {
                light.shadowQuality = LightShadowQuality::Ultra;
            }
            light.illuminanceLux = 100'000.0f;
            light.luminousIntensityCandela = 1'250.0f;
            light.rangeMeters = options_.clusterStressLightCount == 0
                ? 25.0f : 4.0f;
            if (sampleCarLightingFixture) {
                constexpr std::array<glm::vec3, 3> kRigColors{
                    glm::vec3(1.0f, 0.82f, 0.64f),
                    glm::vec3(0.32f, 0.5f, 1.0f),
                    glm::vec3(1.0f, 0.96f, 0.9f),
                };
                light.colorLinearRec709 = kRigColors[index];
                light.luminousIntensityCandela = index == 0u
                    ? 45'000.0f : 18'000.0f;
                light.illuminanceLux = 35'000.0f;
                light.rangeMeters = 12.0f;
                light.innerConeDegrees = 22.0f;
                light.outerConeDegrees = 38.0f;
                light.priority = static_cast<int32_t>(3u - index);
            }
            else if (spotShadowContactFixture) {
                light.colorLinearRec709 = index % 2u == 0u
                    ? glm::vec3(1.0f, 0.35f, 0.12f)
                    : glm::vec3(0.12f, 0.35f, 1.0f);
                light.luminousIntensityCandela = 1'000'000.0f;
                light.rangeMeters = 15.0f;
                light.innerConeDegrees = 20.0f;
                light.outerConeDegrees = 35.0f;
                light.priority = static_cast<int32_t>(
                    generatedLightCount - index);
            }
            else if (pointShadowContactFixture) {
                light.colorLinearRec709 = index % 2u == 0u
                    ? glm::vec3(1.0f, 0.3f, 0.08f)
                    : glm::vec3(0.08f, 0.3f, 1.0f);
                light.luminousIntensityCandela = 500'000.0f;
                light.rangeMeters = 15.0f;
                light.priority = static_cast<int32_t>(
                    generatedLightCount - index);
            }
        }
    }

    void QualificationHarness::constructProbeValidationEntities(
        AppStartupContext& context) {
        if (!options_.validateReflectionProbes) return;
        SceneWorld& sceneWorld = context.scene;
        Registry& registry = sceneWorld.registry();
        const AssetGuid activeEnvironmentAssetGuid =
            context.environmentAssetGuid;
        const uint32_t generatedLightCount = generatedLightCount_;
        if (activeEnvironmentAssetGuid.isNil())
            throw std::invalid_argument(
                "--validate-reflection-probes requires --cooked-environment-artifact");
        const Entity probeEntity = sceneWorld.createEntity(
            SceneEntityUuid::fromUuidV7Fields(
                1'775'000'410'000ull,
                std::array<uint8_t, 10>{ 0x49, 0x52, 0x49, 0x44, 0x49,
                    0x55, 0x4d, 0x50, 0x52, 0x42 }));
        registry.addComponent<NameComponent>(probeEntity,
            "Reflection Probe Validation");
        registry.addComponent<TransformComponent>(probeEntity);
        registry.addComponent<RelationshipComponent>(probeEntity)
            .siblingOrder = static_cast<int32_t>(
                generatedLightCount + 2u);
        auto& probe = registry.addComponent<ReflectionProbeComponent>(
            probeEntity);
        probe.shape = ReflectionProbeShape::Sphere;
        probe.sphereRadiusMeters = 1'000.0f;
        probe.blendDistanceMeters = 0.0f;
        probe.parallaxMode = ReflectionProbeParallaxMode::None;
        probe.environmentAssetGuid = activeEnvironmentAssetGuid;
        probe.resolvedEnvironmentAssetGuid = activeEnvironmentAssetGuid;
        environmentProbeEntity_ = probeEntity;

        const BenchmarkReflectionProbeCapture* fixtureCapture =
            benchmark_ && benchmark_->reflectionProbeCapture
            ? &*benchmark_->reflectionProbeCapture : nullptr;

        // The default route keeps the self-capture exclusion proof. An
        // explicit benchmark capture instead owns no renderable geometry,
        // so every fixture primitive contributes to all six faces and the
        // published cubemap can be judged in visible surface reflections.
        Entity captureProbeEntity = fixtureCapture
            ? NULL_ENTITY : context.firstEntity;
        if (captureProbeEntity == NULL_ENTITY) {
            captureProbeEntity = sceneWorld.createEntity(
                SceneEntityUuid::fromUuidV7Fields(
                    1'775'000'410'001ull,
                    std::array<uint8_t, 10>{ 0x49, 0x52, 0x49, 0x44,
                        0x49, 0x55, 0x4d, 0x43, 0x41, 0x50 }));
            registry.addComponent<NameComponent>(captureProbeEntity,
                "Runtime Reflection Capture Validation");
            auto& captureTransform =
                registry.addComponent<TransformComponent>(
                    captureProbeEntity);
            if (fixtureCapture)
                captureTransform.position = fixtureCapture->position;
            registry.addComponent<RelationshipComponent>(captureProbeEntity)
                .siblingOrder = static_cast<int32_t>(
                    generatedLightCount + 3u);
        }
        auto& captureProbe =
            registry.addComponent<ReflectionProbeComponent>(
                captureProbeEntity);
        captureProbeEntity_ = captureProbeEntity;
        captureProbe.shape = ReflectionProbeShape::Sphere;
        captureProbe.sphereRadiusMeters = 1'000.0f;
        captureProbe.blendDistanceMeters = 0.0f;
        captureProbe.parallaxMode = ReflectionProbeParallaxMode::None;
        captureProbe.priority = fixtureCapture
            ? fixtureCapture->priority : 1;
        if (fixtureCapture) {
            captureProbe.sphereRadiusMeters =
                fixtureCapture->influenceRadiusMeters;
            captureProbe.updateMode = fixtureCapture->updateMode ==
                    BenchmarkReflectionProbeUpdateMode::Realtime
                ? ReflectionProbeUpdateMode::Realtime
                : ReflectionProbeUpdateMode::OnDemand;
            captureProbe.captureResolution = static_cast<int32_t>(
                fixtureCapture->resolution);
            captureProbe.captureNearMeters = fixtureCapture->nearPlane;
            captureProbe.captureFarMeters = fixtureCapture->farPlane;
            captureProbe.captureSky = fixtureCapture->captureSky;
        }
    }

    void QualificationHarness::updateBenchmarkState(AppFrameContext& context) {
        if (!benchmark_) return;
        // Benchmark frames are application frames (warmup included). With
        // --benchmark-hold-frame F every state below is F's from frame F on.
        const uint64_t frameIndex = benchmarkStateFrameIndex(options_,
            context.applicationFrameIndex);
        const bool held = frameIndex != context.applicationFrameIndex;
        Registry& registry = context.scene.registry();
        const BenchmarkSceneFactory& factory = benchmark_->sceneFactory;
        if (factory.kind == BenchmarkSceneFactoryKind::Composition) {
            auto* transforms = registry.getPool<TransformComponent>();
            const std::vector<BenchmarkCompositionEntity>& entities =
                factory.compositionEntities;
            for (size_t index = 0; transforms != nullptr &&
                    index < entities.size() &&
                    index < benchmarkInstances_.size(); ++index) {
                BenchmarkInstanceState& instance = benchmarkInstances_[index];
                if (entities[index].motion.kind ==
                        BenchmarkEntityMotionKind::None ||
                    !transforms->has(instance.entity)) continue;
                const BenchmarkEntityPose pose =
                    evaluateBenchmarkCompositionEntity(entities[index],
                        frameIndex);
                // A held pose does not move, so it is never a teleport again.
                instance.teleportedThisFrame = pose.teleported && !held;
                applyCompositionPose(transforms->get(instance.entity), pose);
            }
        }
        if (factory.animateInstances || factory.objectStepEnabled) {
            auto* transforms = registry.getPool<TransformComponent>();
            if (transforms != nullptr) {
                for (size_t index = 0; index < benchmarkInstances_.size(); ++index) {
                    BenchmarkInstanceState& instance = benchmarkInstances_[index];
                    if (!transforms->has(instance.entity)) continue;
                    TransformComponent& transform = transforms->get(instance.entity);
                    const glm::vec3 position = instance.basePosition +
                        evaluateBenchmarkInstanceOffset(factory, frameIndex,
                            index);
                    if (transform.position.x != position.x ||
                        transform.position.y != position.y ||
                        transform.position.z != position.z) {
                        transform.position = position;
                        transform.isDirty = true;
                    }
                }
            }
        }
        if (factory.objectVisibilityStepEnabled) {
            auto* meshes = registry.getPool<MeshComponent>();
            const size_t index = factory.objectVisibilityStepInstanceIndex;
            if (meshes != nullptr && index < benchmarkInstances_.size()) {
                const Entity entity = benchmarkInstances_[index].entity;
                if (meshes->has(entity)) {
                    MeshComponent& mesh = meshes->get(entity);
                    const bool enabled = frameIndex >=
                            factory.objectVisibilityStepFrame
                        ? factory.objectVisibilityAfterStep
                        : !factory.objectVisibilityAfterStep;
                    mesh.enabled = enabled;
                }
            }
        }

        const BenchmarkCameraPose camera = evaluateBenchmarkCamera(
            *benchmark_, frameIndex);
        context.camera.position = camera.position;
        context.camera.front = glm::normalize(camera.target - camera.position);
        context.requests.viewHistoryResetRevision =
            evaluateBenchmarkViewHistoryResetRevision(*benchmark_, frameIndex);
    }

} // namespace Iridium
