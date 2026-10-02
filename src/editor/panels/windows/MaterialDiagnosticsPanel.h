#pragma once

#include "editor/panels/EditorPanel.h"
#include "ecs/Entity.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <string>

class MaterialDiagnosticsPanel final : public EditorPanel {
public:
    MaterialDiagnosticsPanel(bool* isOpen, Entity* selectedEntity);

    void OnImGuiRender(Registry& registry,
        Iridium::AssetManager* assetManager) override;

private:
    bool* isOpen_ = nullptr;
    Entity* selectedEntity_ = nullptr;
    size_t selectedMaterial_ = 0;
    std::array<char, 128> search_{};
    float selectedUv_[2]{ 0.0f, 0.0f };
    std::string cachedSnapshotHash_;
    nlohmann::json cachedSnapshot_;
};
