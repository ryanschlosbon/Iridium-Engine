#pragma once

#include "ecs/Registry.h"
#include <string>

class AssetManager;

class SceneSerializer {
public:
    SceneSerializer(Registry& registry, AssetManager* assetManager);
    void Serialize(const std::string& filepath);
    bool Deserialize(const std::string& filepath);

private:
    Registry& m_Registry;
    AssetManager* m_AssetManager;
};