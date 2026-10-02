#pragma once

#include "editor/EditorComponentRegistry.h"
#include "editor/EditorTransactionService.h"

#include <glm/glm.hpp>

#include <optional>
#include <variant>

namespace Iridium {

    using EditorPropertyValue = std::variant<
        bool, int32_t, float, std::string, glm::vec3>;

    [[nodiscard]] std::optional<EditorPropertyValue>
        captureEditorPropertyValue(
            const EditorPropertyDescriptor& property,
            const void* component);
    [[nodiscard]] EditorMutationResult writeEditorPropertyValue(
        const EditorPropertyDescriptor& property,
        void* component,
        const EditorPropertyValue& value);
    [[nodiscard]] bool sameEditorPropertyValue(
        const EditorPropertyValue& lhs,
        const EditorPropertyValue& rhs) noexcept;
    [[nodiscard]] EditorTransactionOperation makeEditorPropertyOperation(
        Registry& registry, Entity entity,
        const EditorComponentDescriptor& component,
        const EditorPropertyDescriptor& property,
        EditorPropertyValue before,
        EditorPropertyValue after);

} // namespace Iridium
