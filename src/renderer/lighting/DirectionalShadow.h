#pragma once

#include "renderer/rhi/LightingTypes.h"
#include "renderer/rhi/ShadowTypes.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace Iridium {

    struct DirectionalShadowCacheInput {
        DirectionalShadowSelection selection;
        DirectionalShadowCascadePlan plan;
        uint64_t lightRevision = 0;
        uint64_t casterRevision = 0;
        uint64_t pipelineRevision = 1;
    };

    struct DirectionalShadowSchedule {
        uint32_t dirtyMask = 0;
        uint32_t updateMask = 0;
        uint32_t sampleableMask = 0;
        uint32_t cacheHitCount = 0;
        uint32_t invalidatedCount = 0;
        uint32_t deferredCount = 0;
    };

    struct DirectionalShadowCascadeBlend {
        uint32_t primaryCascade = 0;
        uint32_t secondaryCascade = 0;
        float secondaryWeight = 0.0f;
    };

    class DirectionalShadowCache final {
    public:
        [[nodiscard]] DirectionalShadowSchedule schedule(
            const DirectionalShadowCacheInput& input,
            uint32_t maximumCascadeUpdates);
        void markRendered(uint32_t renderedMask);
        void reset() noexcept;

    private:
        struct CascadeState {
            SceneEntityUuid owner;
            glm::mat4 worldToShadowClip{ 1.0f };
            uint64_t lightRevision = 0;
            uint64_t casterRevision = 0;
            uint64_t pipelineRevision = 0;
            bool valid = false;
        };
        std::array<CascadeState, kDirectionalShadowCascadeCount> states_{};
        std::optional<DirectionalShadowCacheInput> pending_;
        uint32_t pendingUpdateMask_ = 0;
    };

    [[nodiscard]] std::optional<DirectionalShadowSelection>
        selectDirectionalShadowLight(const LightingFramePacket& lighting);
    [[nodiscard]] std::vector<DirectionalShadowSelection>
        selectDirectionalShadowLights(const LightingFramePacket& lighting,
            uint32_t maximumLights = kDirectionalShadowLightCapacity);
    [[nodiscard]] DirectionalShadowCascadePlan buildDirectionalShadowCascades(
        const DirectionalShadowCamera& camera, glm::vec3 lightForward,
        DirectionalShadowConfig config = {});
    [[nodiscard]] DirectionalShadowCascadeBlend directionalShadowCascadeBlend(
        const DirectionalShadowCascadePlan& plan, float viewDepth,
        uint32_t sampleableMask) noexcept;
    [[nodiscard]] float directionalShadowTentWeight(
        int32_t x, int32_t y) noexcept;
    [[nodiscard]] glm::vec3 directionalShadowNormalOffset(
        glm::vec3 worldPosition, glm::vec3 surfaceNormal,
        float worldUnitsPerTexel, float scale) noexcept;

} // namespace Iridium
