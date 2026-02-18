#include "SceneSerializer.h"
#include "Components.h"
#include "Entity.h"
#include "assets/AssetManager.h"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

// Convenience alias
using json = nlohmann::json;

// Helper to convert GLM vec3 to JSON array [x, y, z]
namespace glm {
    void to_json(json& j, const vec3& v) {
        j = json{ v.x, v.y, v.z };
    }

    void from_json(const json& j, vec3& v) {
        v.x = j[0];
        v.y = j[1];
        v.z = j[2];
    }
}

SceneSerializer::SceneSerializer(Registry& registry, AssetManager* assetManager)
    : m_Registry(registry), m_AssetManager(assetManager) {}

void SceneSerializer::Serialize(const std::string& filepath) {
    json sceneJson;
    sceneJson["Scene"] = "Untitled Scene";
    sceneJson["Entities"] = json::array();

    // Debug: Check if we are actually finding the pool
    auto* transformPool = m_Registry.getPool<TransformComponent>();
    if (!transformPool) {
        std::cerr << "ERROR: TransformPool is null!" << std::endl;
        return;
    }

    std::cout << "Serialize: Found " << transformPool->entities.size() << " entities with Transforms." << std::endl;

    for (size_t i = 0; i < transformPool->entities.size(); i++) {
        Entity entity = transformPool->entities[i];

        // Skip invalid entities
        if (entity == NULL_ENTITY) continue;

        json entityJson;
        entityJson["EntityID"] = (uint32_t)entity;

        // --- Serialize Transform ---
        if (transformPool->has(entity)) {
            auto& tc = transformPool->get(entity);
            entityJson["Transform"] = {
                { "Position", tc.position },
                { "Rotation", tc.rotation },
                { "Scale", tc.scale }
            };
        }

        // --- Serialize Mesh ---
        auto* meshPool = m_Registry.getPool<MeshComponent>();
        if (meshPool && meshPool->has(entity)) {
            auto& mc = meshPool->get(entity);

            // CHECK IF MODEL EXISTS
            if (mc.model) {
                entityJson["Mesh"] = {
                    // USE THE REAL PATH
                    { "FilePath", mc.model->filePath }
                };
            }
        }

        sceneJson["Entities"].push_back(entityJson);
    }

    // --- WRITE TO FILE ---
    std::ofstream fout(filepath);

    // ERROR CHECKING
    if (!fout.is_open()) {
        std::cerr << "CRITICAL ERROR: Could not create file: " << filepath << std::endl;
        std::cerr << "Check that the folder 'assets/scenes' exists!" << std::endl;
        return;
    }

    fout << sceneJson.dump(4);
    fout.close();

    std::cout << "SUCCESS: Scene saved to " << filepath << std::endl;
}

bool SceneSerializer::Deserialize(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open file: " << filepath << std::endl;
        return false;
    }

    json data;
    try {
        file >> data;
    }
    catch (json::parse_error& e) {
        std::cerr << "JSON Parse Error: " << e.what() << std::endl;
        return false;
    }

    auto entities = data["Entities"];
    if (entities.is_null()) return false;

    // Loop through every entity in the file
    for (auto& entityJson : entities) {

        // 1. Create a new empty entity
        Entity deserializedEntity = m_Registry.createEntity();

        // 2. Load Transform (If it exists)
        if (entityJson.contains("Transform")) {
            auto& tcJson = entityJson["Transform"];

            // Extract values using our glm helper
            glm::vec3 pos = tcJson["Position"].get<glm::vec3>();
            glm::vec3 rot = tcJson["Rotation"].get<glm::vec3>();
            glm::vec3 scl = tcJson["Scale"].get<glm::vec3>();

            // Add the component
            m_Registry.addComponent<TransformComponent>(deserializedEntity, pos, rot, scl);
        }

        // 3. Load Mesh (If it exists)
        if (entityJson.contains("Mesh")) {
            std::string meshPath = entityJson["Mesh"]["FilePath"];

            if (m_AssetManager) {
                // CHANGE 'Model*' to 'auto' or 'std::shared_ptr<ModelAsset>'
                // AND ensure you are using the correct type name 'ModelAsset'
                std::shared_ptr<ModelAsset> model = m_AssetManager->getModel(meshPath);

                if (model) {
                    m_Registry.addComponent<MeshComponent>(deserializedEntity, model);
                    std::cout << "Loaded Mesh: " << meshPath << std::endl;
                }
                else {
                    std::cerr << "Failed to load mesh: " << meshPath << std::endl;
                }
            }
        }
    }

    std::cout << "Scene Loaded: " << filepath << std::endl;
    return true;
}