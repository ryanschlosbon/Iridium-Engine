#pragma once

#include "assets/cooker/CookedArtifact.h"
#include "scene/authoring/SourceSceneLoadTransaction.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Iridium {

    inline constexpr uint32_t kSceneCompilerImplementationVersion = 2;
    inline constexpr std::string_view kSceneCookerFeatureVersion =
        "M5.1-light-component-v2";

    struct CookedSceneCompileInput {
        AssetGuid sceneAssetGuid;
        std::string sourceContentHash;
        std::string canonicalContentHash;
        uint32_t sourceSceneSchemaVersion = kCurrentSourceSceneSchemaVersion;
        uint32_t compilerImplementationVersion =
            kSceneCompilerImplementationVersion;
        std::string cookerFeatureVersion = std::string(
            kSceneCookerFeatureVersion);
        std::string cookPolicy = "strict";
        CookTarget target;
        std::vector<AssetDependency> dependencies;
    };

    struct CookedSceneCompileResult {
        std::optional<CookedArtifact> artifact;
        std::vector<SceneDiagnostic> diagnostics;

        [[nodiscard]] explicit operator bool() const noexcept {
            return artifact.has_value() && !hasSceneErrors(diagnostics);
        }
    };

    [[nodiscard]] CookedSceneCompileResult compileCookedScene(
        const StagedSourceScene& source,
        const RuntimeComponentRegistry& runtimeRegistry,
        const ComponentSerializerRegistry& sourceRegistry,
        CookedSceneCompileInput input);

} // namespace Iridium
