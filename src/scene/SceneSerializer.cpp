#include "SceneSerializer.h"
#include "scene/Components.h"
#include "scene/components/TransformComponent.h"
#include "scene/components/MeshComponent.h"
#include "scene/components/LightComponent.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <unordered_map>

using json = nlohmann::json;

SceneSerializer::SceneSerializer(Registry* registry) : registry(registry) {}

bool SceneSerializer::serialize(const std::string& filepath) {
    json sceneJson;
    sceneJson["Scene"] = "DefaultScene";
    json entitiesJson = json::array();

    // Grab the pools we care about
    auto* transformPool = registry->getPool<TransformComponent>();
    auto* meshPool = registry->getPool<MeshComponent>();
    auto* lightPool = registry->getPool<LightComponent>();
    auto* relationshipPool = registry->getPool<RelationshipComponent>();

    // Safety check: If there are no transforms, there's nothing to save!
    if (transformPool) {
        // FAST ECS ITERATION: Loop directly over the packed array of entities 
        // that are guaranteed to have a transform component!
        for (uint32_t entity : transformPool->entities) {

            json entityJson;
            entityJson["EntityID"] = entity;

            // --- TRANSFORM COMPONENT ---
            auto& tc = transformPool->get(entity);
            entityJson["TransformComponent"] = {
                {"position", {tc.position.x, tc.position.y, tc.position.z}},
                {"rotation", {tc.rotation.x, tc.rotation.y, tc.rotation.z}},
                {"scale",    {tc.scale.x, tc.scale.y, tc.scale.z}}
            };

            // --- MESH COMPONENT ---
            if (meshPool && meshPool->sparseMap.contains(entity)) {
                auto& mc = meshPool->get(entity);
                // We save the file path of the asset, NOT the vertex data!
                std::string path = mc.model ? mc.model->filePath : "";
                entityJson["MeshComponent"] = {
                    {"meshPath", path},
                    {"enabled", mc.enabled}
                };
            }

            // --- LIGHT COMPONENT ---
            if (lightPool && lightPool->sparseMap.contains(entity)) {
                auto& lc = lightPool->get(entity);
                entityJson["LightComponent"] = {
                    {"color", {lc.color.r, lc.color.g, lc.color.b}},
                    {"intensity", lc.intensity},
                    {"type", static_cast<int>(lc.type)},
                    {"range", lc.range},
                    {"radius", lc.radius},
                    {"innerCone", lc.innerCone},
                    {"outerCone", lc.outerCone},
                    {"castsShadows", lc.castsShadows}
                };
            }

            if (relationshipPool && relationshipPool->sparseMap.contains(entity)) {
                const auto& relationship = relationshipPool->get(entity);
                json children = json::array();
                for (Entity child : relationship.children) children.push_back(child);

                entityJson["RelationshipComponent"] = {
                    {"parent", relationship.parent == NULL_ENTITY
                        ? json(nullptr)
                        : json(relationship.parent)},
                    {"children", std::move(children)},
                    {"depth", relationship.depth}
                };
            }

            entitiesJson.push_back(entityJson);
        }
    }

    sceneJson["Entities"] = entitiesJson;

    // Write to file
    const std::filesystem::path outputPath(filepath);
    if (outputPath.has_parent_path()) {
        std::error_code error;
        std::filesystem::create_directories(outputPath.parent_path(), error);
        if (error) return false;
    }

    std::ofstream file(outputPath);
    if (file.is_open()) {
        file << sceneJson.dump(4); // The '4' adds pretty-printing indentation!
        return file.good();
    }
    return false;
}

bool SceneSerializer::deserialize(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) return false;

    json sceneJson;
    try {
        file >> sceneJson;
    }
    catch (const json::exception&) {
        return false;
    }

    if (!sceneJson.contains("Entities") || !sceneJson["Entities"].is_array()) {
        return false;
    }

    // Do not destroy the current scene until the selected file has parsed and
    // passed the minimum schema check.
    registry->clear();

    try {
        const json& entities = sceneJson["Entities"];
        std::vector<Entity> loadedEntities;
        loadedEntities.reserve(entities.size());
        std::unordered_map<Entity, Entity> savedToLoaded;

        // Establish every entity mapping first so relationship references are
        // valid regardless of save order or gaps in the original entity IDs.
        for (size_t index = 0; index < entities.size(); ++index) {
            const Entity loaded = registry->createEntity();
            const Entity saved = entities[index].value("EntityID", static_cast<Entity>(index));
            loadedEntities.push_back(loaded);
            savedToLoaded[saved] = loaded;
        }

        for (size_t index = 0; index < entities.size(); ++index) {
            const json& entityJson = entities[index];
            const Entity entity = loadedEntities[index];

            if (entityJson.contains("TransformComponent")) {
                const json& tcJson = entityJson["TransformComponent"];
                const glm::vec3 pos = { tcJson["position"][0], tcJson["position"][1], tcJson["position"][2] };
                const glm::vec3 rot = { tcJson["rotation"][0], tcJson["rotation"][1], tcJson["rotation"][2] };
                const glm::vec3 scale = { tcJson["scale"][0], tcJson["scale"][1], tcJson["scale"][2] };
                registry->addComponent<TransformComponent>(entity, pos, rot, scale);
            }

            if (entityJson.contains("MeshComponent")) {
                const json& mcJson = entityJson["MeshComponent"];
                auto& mesh = registry->addComponent<MeshComponent>(entity, nullptr);
                mesh.requestedMeshPath = mcJson.value("meshPath", std::string{});
                mesh.enabled = mcJson.value("enabled", true);
            }

            if (entityJson.contains("LightComponent")) {
                const json& lcJson = entityJson["LightComponent"];
                auto& light = registry->addComponent<LightComponent>(entity);
                light.color = { lcJson["color"][0], lcJson["color"][1], lcJson["color"][2] };
                light.intensity = lcJson.value("intensity", 1.0f);
                light.type = static_cast<LightType>(lcJson.value("type", 0));
                light.range = lcJson.value("range", light.range);
                light.radius = lcJson.value("radius", light.radius);
                light.innerCone = lcJson.value("innerCone", light.innerCone);
                light.outerCone = lcJson.value("outerCone", light.outerCone);
                light.castsShadows = lcJson.value("castsShadows", light.castsShadows);
            }

            if (entityJson.contains("RelationshipComponent")) {
                const json& rcJson = entityJson["RelationshipComponent"];
                auto& relationship = registry->addComponent<RelationshipComponent>(entity);
                relationship.depth = rcJson.value("depth", 0);

                if (rcJson.contains("parent") && !rcJson["parent"].is_null()) {
                    const Entity savedParent = rcJson["parent"];
                    if (const auto parent = savedToLoaded.find(savedParent); parent != savedToLoaded.end()) {
                        relationship.parent = parent->second;
                    }
                }

                if (rcJson.contains("children") && rcJson["children"].is_array()) {
                    for (const json& childJson : rcJson["children"]) {
                        const Entity savedChild = childJson;
                        if (const auto child = savedToLoaded.find(savedChild); child != savedToLoaded.end()) {
                            relationship.children.push_back(child->second);
                        }
                    }
                }
            }
        }
    }
    catch (const json::exception&) {
        registry->clear();
        return false;
    }

    return true;
}
