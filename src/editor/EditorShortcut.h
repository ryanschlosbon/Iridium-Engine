#pragma once

#include <imgui.h>

#include <string_view>

namespace Iridium {

    // Shortcut prefixes communicate ownership before the action key:
    // Ctrl = document/transaction commands, Alt = viewport presentation/tools,
    // Shift = additive or alternate behavior. Context remains authoritative, so
    // a viewport binding never steals input from text fields or another panel.
    enum class EditorShortcutGroup {
        Document,
        Viewport,
        Alternate,
    };

    struct EditorShortcutBinding {
        EditorShortcutGroup group = EditorShortcutGroup::Document;
        ImGuiKeyChord chord = ImGuiKey_None;
        std::string_view label;
    };

    inline constexpr EditorShortcutBinding ToggleViewportGrid{
        .group = EditorShortcutGroup::Viewport,
        .chord = ImGuiMod_Alt | ImGuiKey_G,
        .label = "Alt+G",
    };

} // namespace Iridium

