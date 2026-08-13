#pragma once

#include "editor/EditorComponentRegistry.h"

namespace Iridium {

    struct CoreEditorComponentRegistryResult {
        EditorComponentRegistry registry;
        EditorComponentRegistryResult status;

        [[nodiscard]] explicit operator bool() const noexcept {
            return static_cast<bool>(status) && registry.isFrozen();
        }
    };

    [[nodiscard]] CoreEditorComponentRegistryResult
        createCoreEditorComponentRegistry();

} // namespace Iridium
