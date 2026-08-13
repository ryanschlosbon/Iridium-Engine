#pragma once

#include "scene/SceneEntityUuid.h"
#include "renderer/rhi/ShadowSettings.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace Iridium {

    struct DirectionalShadowConfig {
        uint32_t resolution = 4096;
        float splitLambda = 0.85f;
        float guardBandFraction = 0.05f;
        float depthPaddingMeters = 100.0f;
    };

    struct DirectionalShadowCamera {
        glm::vec3 position{};
        glm::vec3 forward{ 0.0f, 0.0f, -1.0f };
        glm::vec3 up{ 0.0f, 1.0f, 0.0f };
        float verticalFovRadians = glm::radians(60.0f);
        float aspectRatio = 16.0f / 9.0f;
        float nearPlane = 0.1f;
        float farPlane = 1000.0f;
    };

    struct DirectionalShadowSelection {
        SceneEntityUuid owner;
        uint32_t lightSlot = 0;
        uint32_t quality = 0;
        int32_t priority = 0;
        glm::vec3 lightForward{ 0.0f, -1.0f, 0.0f };
        uint32_t omittedShadowDirectionalLights = 0;
    };

    struct DirectionalShadowCascade {
        float splitNear = 0.0f;
        float splitFar = 0.0f;
        float radiusMeters = 0.0f;
        float worldUnitsPerTexel = 0.0f;
        float depthSpanMeters = 0.0f;
        glm::vec2 snappedLightCenter{};
        glm::mat4 worldToShadowClip{ 1.0f };
    };

    struct DirectionalShadowCascadePlan {
        std::array<float, kDirectionalShadowCascadeCount> splitFar{};
        std::array<DirectionalShadowCascade,
            kDirectionalShadowCascadeCount> cascades{};
    };

    // Complete backend-neutral submission contract. A backend must only publish
    // cascades in sampleableMask and must render every cascade in updateMask
    // before later lighting consumers sample the map in the same frame.
    struct DirectionalShadowFramePacket {
        DirectionalShadowSelection selection;
        DirectionalShadowCascadePlan plan;
        uint32_t shadowIndex = 0;
        uint32_t updateMask = 0;
        uint32_t sampleableMask = 0;
        uint32_t resolution = 4096;
        float sourceAngularDiameterDegrees = 0.535f;
        ShadowFilterProfile filterProfile{};
    };

    // Backend-neutral persistent-atlas submission. The application owns
    // allocation/cache policy; the backend owns only physical storage and
    // rasterization. Atlas coordinates include the guard band.
    struct SpotShadowFramePacket {
        SceneEntityUuid owner;
        glm::mat4 worldToShadowClip{ 1.0f };
        uint32_t lightSlot = 0;
        uint32_t shadowDataSlot = 0;
        uint32_t atlasX = 0;
        uint32_t atlasY = 0;
        uint32_t tileSize = 0;
        uint32_t guardTexels = 0;
        bool update = false;
        bool sampleable = false;
        bool stale = false;
        uint32_t staleAgeFrames = 0;
        float nearPlane = 0.0f;
        float farPlane = 0.0f;
        float sourceRadiusMeters = 0.0f;
        ShadowFilterProfile filterProfile{};
    };

    // Backend-neutral point-cube submission. One update refreshes all six faces;
    // a sampleable packet always references a complete compatible cube.
    struct PointShadowFramePacket {
        SceneEntityUuid owner;
        std::array<glm::mat4, 6> worldToShadowClip{};
        glm::vec3 lightPosition{};
        float nearPlane = 0.0f;
        float farPlane = 0.0f;
        uint32_t lightSlot = 0;
        uint32_t shadowDataSlot = 0;
        uint32_t resolution = 0;
        uint32_t cubeIndex = 0;
        bool update = false;
        bool sampleable = false;
        bool stale = false;
        uint32_t staleAgeFrames = 0;
        float sourceRadiusMeters = 0.0f;
        ShadowFilterProfile filterProfile{};
    };

} // namespace Iridium
