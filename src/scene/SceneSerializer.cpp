#include "SceneSerializer.h"
#include "ComponentRegistry.h"
#include "components/TransformComponent.h"
#include <fstream>

using json = nlohmann::json;

SceneSerializer::SceneSerializer(Registry* registry) : registry(registry) {}

bool SceneSerializer::serialize(const std::string& filepath) {
    json sceneJson;
    sceneJson["Scene"] = "DefaultScene";
    json entitiesJson = json::array();

    // Iterate through all entities
    for (uint32_t entity = 0; entity < registry->getMaxEntities(); ++entity) {
        // Quick check to see if the entity is actually alive/used (e.g., has a transform)
        // You might want a dedicated "alive" list in your Registry later!
        if (!registry->getPool<TransformComponent>()->has(entity)) continue;

        json entityJson;
        entityJson["EntityID"] = entity;

        // THE MAGIC: Loop through every registered component in the engine!
        for (auto& [name, funcs] : ComponentRegistry::RegistryMap) {
            funcs.serialize(*registry, entity, entityJson);
        }

        entitiesJson.push_back(entityJson);
    }

    sceneJson["Entities"] = entitiesJson;

    std::ofstream file(filepath);
    if (file.is_open()) {
        file << sceneJson.dump(4);
        return true;
    }
    return false;
}

bool SceneSerializer::deserialize(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) return false;

    json sceneJson;
    file >> sceneJson;

    for (auto& entityJson : sceneJson["Entities"]) {
        uint32_t entity = registry->createEntity();

        // THE MAGIC: Loop through every registered component and let them try to load!
        for (auto& [name, funcs] : ComponentRegistry::RegistryMap) {
            funcs.deserialize(*registry, entity, entityJson);
        }
    }

    return true;
}