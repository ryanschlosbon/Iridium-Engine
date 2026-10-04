// M7R R5c.5: the change-driven GPU-scene observation must produce exactly the
// full walk's observations, revisions, metadata and direct-fallback count after
// every frame, under random scene edits: transforms, enable toggles, model and
// LOD changes, entity creation and destruction, instance-batch components,
// selection, in-place model changes and entities whose metadata is invalid
// (transparent-only and malformed models), which make their slot volatile.

#include "extraction/GpuSceneObservation.h"

#include "renderer/rhi/Mesh.h"
#include "scene/SceneWorld.h"
#include "scene/components/MeshComponent.h"
#include "scene/components/RenderInstanceBatchComponent.h"
#include "scene/components/TransformComponent.h"
#include "scene/systems/TransformSystem.h"

#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {
    using namespace Iridium;

    #define CHECK(condition) do { if (!(condition)) { \
        std::cerr << "check failed: " #condition " at line " << __LINE__ << '\n'; \
        return false; } } while (false)

    AssetGuid guid(std::mt19937_64& random) {
        AssetGuid::Bytes bytes{};
        for (auto& byte : bytes) byte = static_cast<uint8_t>(random() & 0xffu);
        bytes[0] = static_cast<uint8_t>(bytes[0] | 1u);
        return AssetGuid(bytes);
    }

    enum class ModelShape { Opaque, TransparentOnly, Malformed, Lod };

    std::shared_ptr<ModelAsset> makeModel(std::mt19937_64& random,
        ModelShape shape, uint32_t index) {
        auto model = std::make_shared<ModelAsset>();
        model->assetGuid = guid(random);
        model->artifactCookKey = "model-cook-key-long-enough-to-allocate-" +
            std::to_string(index);
        model->geometry = GeometryHandle{ 100u + index };
        const uint32_t subMeshCount = 1u + static_cast<uint32_t>(random() % 3u);
        for (uint32_t subMeshIndex = 0; subMeshIndex < subMeshCount;
                ++subMeshIndex) {
            model->materials.push_back({
                .material = MaterialHandle{ 10u + subMeshIndex },
                .pipeline = PipelineHandle{ 20u + subMeshIndex },
                .renderQueue = shape == ModelShape::TransparentOnly
                    ? RenderQueue::Transparent
                    : (subMeshIndex == 1u ? RenderQueue::ForwardOpaque
                        : RenderQueue::Opaque),
                .opaqueSortKey = subMeshIndex,
            });
            SubMesh subMesh{};
            subMesh.materialIndex = static_cast<int>(subMeshIndex);
            subMesh.indexStart = 6u * subMeshIndex;
            subMesh.indexCount = shape == ModelShape::Malformed &&
                subMeshIndex + 1u == subMeshCount ? 0u : 36u;
            subMesh.sourcePrimitiveGuid = guid(random);
            subMesh.primitiveGuid = guid(random);
            subMesh.materialGuid = guid(random);
            subMesh.boundsMin = glm::vec3(-1.0f - static_cast<float>(subMeshIndex));
            subMesh.boundsMax = glm::vec3(1.0f + static_cast<float>(subMeshIndex));
            subMesh.boundsSphereRadius = 2.0f;
            model->subMeshes.push_back(subMesh);
            if (shape == ModelShape::Lod) {
                ModelLodChain chain;
                chain.basePrimitiveGuid = subMesh.primitiveGuid;
                chain.levels.push_back({ 0.0f, subMesh });
                SubMesh coarse = subMesh;
                coarse.primitiveGuid = guid(random);
                coarse.indexCount = 12u;
                chain.levels.push_back({ 0.05f, coarse });
                model->lodChains.push_back(chain);
            }
        }
        return model;
    }

    struct Scene {
        SceneWorld world;
        TransformSystem transforms;
        std::vector<Entity> changed;
        std::vector<std::shared_ptr<ModelAsset>> models;
        std::vector<Entity> entities;
        Entity selected = NULL_ENTITY;
    };

    Entity addEntity(Scene& scene, std::mt19937_64& random) {
        Registry& registry = scene.world.registry();
        const Entity entity = scene.world.createEntity();
        auto& transform = registry.addComponent<TransformComponent>(entity);
        transform.position = glm::vec3(static_cast<float>(random() % 50), 0.0f, 0.0f);
        transform.isDirty = true;
        auto& mesh = registry.addComponent<MeshComponent>(entity);
        mesh.model = scene.models[random() % scene.models.size()];
        mesh.enabled = random() % 8u != 0u;
        mesh.maximumLodLevel = static_cast<int32_t>(random() % 16u);
        if (random() % 16u == 0u)
            registry.addComponent<RenderInstanceBatchComponent>(entity);
        scene.entities.push_back(entity);
        return entity;
    }

    bool randomizedEditsMatchFullWalk(uint64_t seed, uint64_t& changeDriven,
        uint64_t& visitedRuns, uint64_t& fullWalks) {
        std::mt19937_64 random(seed);
        Scene scene;
        uint32_t modelIndex = 0;
        for (const ModelShape shape : { ModelShape::Opaque, ModelShape::Opaque,
                ModelShape::TransparentOnly, ModelShape::Malformed, ModelShape::Lod })
            scene.models.push_back(makeModel(random, shape, modelIndex++));
        for (uint32_t index = 0; index < 48u; ++index) addEntity(scene, random);
        Registry& registry = scene.world.registry();
        GpuSceneObservation changeDrivenObservation(scene.world,
            GpuSceneObservation::Mode::ChangeDriven);
        GpuSceneObservation fullWalkObservation(scene.world,
            GpuSceneObservation::Mode::FullWalk);
        for (uint32_t frame = 0; frame < 400u; ++frame) {
            const uint32_t edits = static_cast<uint32_t>(random() % 4u);
            for (uint32_t edit = 0; edit < edits && !scene.entities.empty(); ++edit) {
                const size_t pick = random() % scene.entities.size();
                const Entity entity = scene.entities[pick];
                switch (random() % 16u) {
                case 0: case 1: case 2: case 3: {
                    auto& transform = registry.getComponent<TransformComponent>(entity);
                    transform.setPosition(transform.position + glm::vec3(0.5f, 0.0f, 0.0f));
                    break;
                }
                case 4: {
                    auto& mesh = registry.getComponent<MeshComponent>(entity);
                    mesh.enabled = !mesh.enabled;
                    break;
                }
                case 5:
                    registry.getComponent<MeshComponent>(entity).model =
                        scene.models[random() % scene.models.size()];
                    break;
                case 6:
                    registry.getComponent<MeshComponent>(entity).maximumLodLevel =
                        static_cast<int32_t>(random() % 16u);
                    break;
                case 7:
                    // A mutable access that changes nothing.
                    (void)registry.getComponent<MeshComponent>(entity);
                    break;
                case 8:
                    scene.selected = random() % 3u == 0u ? NULL_ENTITY : entity;
                    break;
                case 9:
                    if (scene.entities.size() > 8u) {
                        (void)scene.world.destroyEntity(entity);
                        scene.entities.erase(scene.entities.begin() +
                            static_cast<std::ptrdiff_t>(pick));
                    }
                    break;
                case 10:
                    addEntity(scene, random);
                    break;
                case 11: {
                    auto* batches = registry.getPool<RenderInstanceBatchComponent>();
                    if (batches->has(entity)) batches->remove(entity);
                    else registry.addComponent<RenderInstanceBatchComponent>(entity);
                    break;
                }
                case 12: {
                    // An in-place model change the full walk notices.
                    auto& model = *scene.models[random() % scene.models.size()];
                    model.geometry = GeometryHandle{ model.geometry.id ^ 0x1000u };
                    break;
                }
                case 13:
                    if (random() % 4u == 0u)
                        scene.models[random() % scene.models.size()]->artifactCookKey +=
                            "x";
                    break;
                default:
                    break;  // a quiet frame for this edit
                }
            }
            (void)scene.transforms.update(registry, &scene.changed);
            const GpuSceneObservationInputs inputs{
                .assets = nullptr,
                .selectedEntity = scene.selected,
                .changedTransforms = scene.changed,
            };
            changeDrivenObservation.observe(inputs);
            fullWalkObservation.observe(inputs);
            const std::string difference = GpuSceneObservation::compare(
                fullWalkObservation, changeDrivenObservation);
            if (!difference.empty()) {
                std::cerr << "seed " << seed << " frame " << frame << ": "
                    << difference << '\n';
                return false;
            }
        }
        changeDriven += changeDrivenObservation.stats().changeDrivenFrames;
        visitedRuns += changeDrivenObservation.stats().visitedRuns;
        fullWalks += changeDrivenObservation.stats().fullWalks;
        return true;
    }

    bool randomizedSequences() {
        uint64_t changeDriven = 0, visitedRuns = 0, fullWalks = 0;
        for (uint64_t seed = 1; seed <= 48u; ++seed)
            if (!randomizedEditsMatchFullWalk(seed, changeDriven, visitedRuns,
                    fullWalks)) return false;
        std::cout << "  48 sequences x 400 frames: " << changeDriven
            << " change-driven frames, " << fullWalks << " full walks, "
            << visitedRuns << " runs re-run\n";
        // Both paths must be exercised.
        CHECK(changeDriven > 4000u && fullWalks > 1000u);
        return true;
    }

    // The verifier's comparison catches a writer that bypasses the journal.
    bool bypassingWriterIsDetected() {
        std::mt19937_64 random(7);
        Scene scene;
        scene.models.push_back(makeModel(random, ModelShape::Opaque, 0));
        for (uint32_t index = 0; index < 8u; ++index) addEntity(scene, random);
        Registry& registry = scene.world.registry();
        for (const Entity entity : scene.entities)
            registry.getComponent<MeshComponent>(entity).enabled = true;
        GpuSceneObservation changeDrivenObservation(scene.world,
            GpuSceneObservation::Mode::ChangeDriven);
        GpuSceneObservation fullWalkObservation(scene.world,
            GpuSceneObservation::Mode::FullWalk);
        const auto frame = [&] {
            (void)scene.transforms.update(registry, &scene.changed);
            const GpuSceneObservationInputs inputs{ .changedTransforms = scene.changed };
            changeDrivenObservation.observe(inputs);
            fullWalkObservation.observe(inputs);
            return GpuSceneObservation::compare(fullWalkObservation,
                changeDrivenObservation);
        };
        CHECK(frame().empty());
        CHECK(frame().empty());
        auto* meshes = registry.getPool<MeshComponent>();
        meshes->components[3].maximumLodLevel ^= 1;  // not journaled
        CHECK(!frame().empty());
        return true;
    }
}

int main() {
    struct Test { const char* name; bool (*run)(); };
    const Test tests[] = {
        { "randomized edits match the full walk", randomizedSequences },
        { "a writer bypassing the journal is detected", bypassingWriterIsDetected },
    };
    size_t passed = 0;
    for (const Test& test : tests) {
        if (!test.run()) { std::cerr << "[FAIL] " << test.name << '\n'; return 1; }
        std::cout << "[PASS] " << test.name << '\n'; ++passed;
    }
    std::cout << passed << '/' << std::size(tests) << " tests passed\n";
    return 0;
}
