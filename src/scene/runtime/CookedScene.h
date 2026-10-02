#pragma once

#include "assets/AssetGuid.h"
#include "assets/cooker/CookTypes.h"
#include "scene/SceneWorld.h"
#include "scene/runtime/RuntimeComponentRegistry.h"
#include "scene/runtime/SceneDiagnostic.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Iridium {

    inline constexpr std::string_view kRuntimeSceneArtifactType =
        "iridium.scene.runtime";
    inline constexpr uint32_t kRuntimeSceneSchemaVersion = 1;

    [[nodiscard]] constexpr uint32_t cookedSceneSectionId(
        char a, char b, char c, char d) noexcept {
        return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
            (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
            (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
            (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
    }

    inline constexpr uint32_t kCookedSceneHeaderSection =
        cookedSceneSectionId('S', 'C', 'N', '1');
    inline constexpr uint32_t kCookedSceneStringSection =
        cookedSceneSectionId('S', 'T', 'R', '1');
    inline constexpr uint32_t kCookedSceneEntitySection =
        cookedSceneSectionId('E', 'N', 'T', '1');

    enum class CookedAssetAvailability {
        Available,
        Pending,
        Missing,
    };

    struct CookedSceneLoadOptions {
        std::optional<AssetGuid> expectedSceneAssetGuid;
        std::optional<CookTarget> expectedTarget;
        std::optional<std::string_view> expectedCookKey;
        std::optional<std::string_view> expectedArtifactHash;
        std::function<CookedAssetAvailability(const AssetDependency&)>
            assetAvailability;
    };

    struct StagedCookedScene {
        std::unique_ptr<SceneWorld> world;
        AssetGuid sceneAssetGuid;
        std::string artifactHash;
    };

    struct CookedSceneStageResult {
        std::unique_ptr<StagedCookedScene> staging;
        std::vector<SceneDiagnostic> diagnostics;

        [[nodiscard]] explicit operator bool() const noexcept {
            return staging && !hasSceneErrors(diagnostics);
        }
    };

    [[nodiscard]] CookedSceneStageResult stageCookedScene(
        std::span<const std::byte> bytes,
        const RuntimeComponentRegistry& registry,
        CookedSceneLoadOptions options = {});

    void commitStagedCookedScene(SceneWorld& active, StagedCookedScene& staging);

} // namespace Iridium
