#include "scene/runtime/SceneDiagnostic.h"

#include <algorithm>
#include <tuple>

namespace Iridium {
    namespace {

        [[nodiscard]] std::string_view componentText(
            const std::optional<ComponentTypeId>& component) noexcept {
            return component ? std::string_view(component->value()) :
                std::string_view();
        }

        [[nodiscard]] SceneEntityUuid::Bytes entityBytes(
            const std::optional<SceneEntityUuid>& entity) noexcept {
            return entity ? entity->bytes() : SceneEntityUuid::Bytes{};
        }

    } // namespace

    bool sceneDiagnosticLess(
        const SceneDiagnostic& lhs,
        const SceneDiagnostic& rhs) noexcept {
        return std::tuple{
            lhs.phase,
            entityBytes(lhs.entity),
            componentText(lhs.component),
            lhs.propertyPath,
            lhs.code,
            lhs.severity,
            lhs.message,
        } < std::tuple{
            rhs.phase,
            entityBytes(rhs.entity),
            componentText(rhs.component),
            rhs.propertyPath,
            rhs.code,
            rhs.severity,
            rhs.message,
        };
    }

    void sortSceneDiagnostics(std::vector<SceneDiagnostic>& diagnostics) {
        std::ranges::stable_sort(diagnostics, sceneDiagnosticLess);
    }

    bool hasSceneErrors(
        const std::vector<SceneDiagnostic>& diagnostics) noexcept {
        return std::ranges::any_of(diagnostics,
            [](const SceneDiagnostic& diagnostic) {
                return diagnostic.severity == SceneDiagnosticSeverity::Error;
            });
    }

} // namespace Iridium
