#pragma once

#include "scene/SceneEntityUuid.h"
#include "scene/runtime/ComponentIdentity.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Iridium {

    enum class SceneDiagnosticSeverity {
        Info,
        Warning,
        Error,
    };

    enum class ScenePhase {
        Read,
        Parse,
        EnvelopeMigration,
        Identity,
        ComponentMigration,
        Deserialize,
        ReferenceResolution,
        AssetResolution,
        Hierarchy,
        PostLoad,
        Save,
        Cook,
        RuntimeLoad,
        Transaction,
    };

    struct SceneDiagnostic {
        SceneDiagnosticSeverity severity = SceneDiagnosticSeverity::Error;
        std::string code;
        ScenePhase phase = ScenePhase::Parse;
        std::optional<SceneEntityUuid> entity;
        std::optional<ComponentTypeId> component;
        std::optional<uint32_t> componentVersion;
        std::string propertyPath;
        std::optional<uint32_t> migrationFrom;
        std::optional<uint32_t> migrationTo;
        std::string message;
    };

    [[nodiscard]] bool sceneDiagnosticLess(
        const SceneDiagnostic& lhs,
        const SceneDiagnostic& rhs) noexcept;
    void sortSceneDiagnostics(std::vector<SceneDiagnostic>& diagnostics);
    [[nodiscard]] bool hasSceneErrors(
        const std::vector<SceneDiagnostic>& diagnostics) noexcept;

} // namespace Iridium

