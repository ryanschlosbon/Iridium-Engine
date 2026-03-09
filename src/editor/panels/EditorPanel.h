#pragma once

#include "ecs/Registry.h"
#include "assets/AssetManager.h"

class AssetManager;

class EditorPanel {
public:
    // A virtual destructor is mandatory for base classes to ensure 
    // derived classes are properly destroyed and don't leak memory.
    virtual ~EditorPanel() = default;

    // The pure virtual rendering function. 
    // The '= 0' forces every derived panel to implement its own version of this.
    virtual void OnImGuiRender(Registry& registry, AssetManager* assetManager) = 0;
};