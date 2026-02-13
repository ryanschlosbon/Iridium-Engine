#pragma once
#include <memory>
#include "assets/AssetManager.h" // Fix path relative to this folder

struct MeshComponent {
    std::shared_ptr<ModelAsset> model;
    bool enabled = true;
};