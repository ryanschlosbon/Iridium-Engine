#pragma once
#include <memory>
#include "assets/AssetManager.h" // To access the ModelAsset struct

struct MeshComponent { 
    std::shared_ptr<ModelAsset> model; 
    bool enabled = true;
};