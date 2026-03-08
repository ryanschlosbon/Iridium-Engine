#include "SceneSerializer.h"
#include "scene/Components.h"
#include "scene/components/TransformComponent.h"
#include "scene/components/MeshComponent.h"
#include "scene/components/LightComponent.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

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

    // We need to iterate over all active entities. 
    // Assuming you have a way to get the max entity ID or active entity list:
    for (uint32_t entity = 0; entity < registry->getMaxEntities(); ++entity) {
        // If the entity doesn't have a transform, it's either dead or not a physical object
        if (!transformPool->sparseMap.contains(entity)) continue;

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
                {"type", static_cast<int>(lc.type)} // 0 = Directional, 1 = Point, etc.
            };
        }

        entitiesJson.push_back(entityJson);
    }

    sceneJson["Entities"] = entitiesJson;

    // Write to file
    std::ofstream file(filepath);
    if (file.is_open()) {
        file << sceneJson.dump(4); // The '4' adds pretty-printing indentation!
        return true;
    }
    return false;
}

bool SceneSerializer::deserialize(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) return false;

    json sceneJson;
    file >> sceneJson;

    // Optional: registry->clearAllEntities(); // Reset scene before loading

    for (auto& entityJson : sceneJson["Entities"]) {
        uint32_t entity = registry->createEntity();

        // --- TRANSFORM COMPONENT ---
        if (entityJson.contains("TransformComponent")) {
            auto tcJson = entityJson["TransformComponent"];
            glm::vec3 pos = { tcJson["position"][0], tcJson["position"][1], tcJson["position"][2] };
            glm::vec3 rot = { tcJson["rotation"][0], tcJson["rotation"][1], tcJson["rotation"][2] };
            glm::vec3 scale = { tcJson["scale"][0], tcJson["scale"][1], tcJson["scale"][2] };

            registry->addComponent<TransformComponent>(entity, pos, rot, scale);
        }

        // --- MESH COMPONENT ---
        if (entityJson.contains("MeshComponent")) {
            auto mcJson = entityJson["MeshComponent"];
            std::string path = mcJson["meshPath"];

            // We set the requestedMeshPath so your ProcessMeshSwaps logic loads it on the main thread!
            auto& mc = registry->addComponent<MeshComponent>(entity, nullptr);
            mc.requestedMeshPath = path;
            mc.enabled = mcJson["enabled"];
        }

        // --- LIGHT COMPONENT ---
        if (entityJson.contains("LightComponent")) {
            auto lcJson = entityJson["LightComponent"];
            auto& lc = registry->addComponent<LightComponent>(entity);
            lc.color = { lcJson["color"][0], lcJson["color"][1], lcJson["color"][2] };
            lc.intensity = lcJson["intensity"];
            lc.type = static_cast<LightType>(lcJson["type"]);
        }
    }

    return true;
}