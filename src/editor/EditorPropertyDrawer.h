#pragma once

#include "editor/EditorComponentRegistry.h"

#include <cstdint>
#include <span>

namespace Iridium {

    class EditorTransactionService;

    struct EditorPropertyEditSessionState {
        uint32_t activeItemId = 0;
        uint64_t activeSession = 0;
        uint64_t nextSession = 1;

        [[nodiscard]] uint64_t observe(
            uint32_t itemId, bool activated) noexcept;
    };

    struct GenericEditorPropertyDrawContext {
        Registry* registry = nullptr;
        Entity entity = NULL_ENTITY;
        EditorTransactionService* transactions = nullptr;
        EditorPropertyEditSessionState* sessions = nullptr;
        std::span<const Entity> entities;
    };

    // Generic editor fallback for scalar/vector properties. Stable references and
    // collections are presented safely but require a registered custom drawer to
    // offer asset pickers, hierarchy changes, or collection editing.
    [[nodiscard]] bool drawGenericEditorProperties(
        const EditorComponentDescriptor& descriptor,
        void* component,
        GenericEditorPropertyDrawContext context = {});

} // namespace Iridium
