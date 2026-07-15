#pragma once
#include "ecs/Registry.h"

// Forward declare the namespace and class
namespace Iridium { class AssetManager; }

class EditorPanel {
public:
    virtual ~EditorPanel() = default;

    // Update the pointer to use the Iridium namespace
    virtual void OnImGuiRender(Registry& registry, Iridium::AssetManager* assetManager) = 0;
};