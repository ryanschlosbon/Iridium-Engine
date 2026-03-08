#pragma once
#include "ecs/Registry.h"
#include <string>
#include <memory>

class SceneSerializer {
public:
    SceneSerializer(Registry* registry);

    // Saves the current state of the Registry to a JSON file
    bool serialize(const std::string& filepath);

    // Clears the Registry and loads entities/components from a JSON file
    bool deserialize(const std::string& filepath);

private:
    Registry* registry;
};