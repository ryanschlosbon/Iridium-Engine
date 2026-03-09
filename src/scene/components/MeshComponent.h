#pragma once
#include <memory>
#include <string>
#include "assets/AssetManager.h" 
#include "editor/Reflection.h"

struct MeshComponent {
    std::shared_ptr<ModelAsset> model;
    bool enabled = true;

    std::string currentMeshPath = "";
    std::string requestedMeshPath = "";

    REFLECT_BEGIN()
        PROPERTY(enabled)
        PROPERTY_READONLY(currentMeshPath)

        // THE FIX: This will now automatically generate the text box + browse button!
        PROPERTY_PATH(requestedMeshPath)

        BUTTON("Swap Mesh") {
        if (!requestedMeshPath.empty()) {
            currentMeshPath = requestedMeshPath;
            requestedMeshPath = "";
        }
    }
    REFLECT_END()
};

AUTO_REGISTER_COMPONENT(MeshComponent)