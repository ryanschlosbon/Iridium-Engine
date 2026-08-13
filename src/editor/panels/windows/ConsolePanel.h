#pragma once

#include "core/EngineLog.h"
#include "editor/panels/EditorPanel.h"

#include <array>
#include <cstdint>
#include <vector>

class ConsolePanel final : public EditorPanel {
public:
    ConsolePanel(
        bool* isOpen,
        Iridium::EngineLog* log);

    void OnImGuiRender(
        Registry& registry,
        Iridium::AssetManager*
            assetManager) override;

private:
    void rebuildVisible();

    bool* isOpen_ = nullptr;
    Iridium::EngineLog* log_ = nullptr;
    uint64_t cachedRevision_ = UINT64_MAX;
    std::vector<Iridium::EngineLogEntry>
        entries_;
    std::vector<size_t> visible_;
    std::array<char, 256> filter_{};
    bool showInfo_ = true;
    bool showWarnings_ = true;
    bool showErrors_ = true;
    bool autoScroll_ = true;
    bool filterDirty_ = true;
};
