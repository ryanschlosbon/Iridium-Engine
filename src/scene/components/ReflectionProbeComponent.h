#pragma once

#include "core/types/AssetGuid.h"
#include "core/types/ReflectionProbeModes.h"

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

namespace Iridium {

    // Renderer-neutral authoring contract. TransformComponent owns probe position
    // and orientation; these values define influence, capture, and publication.
    struct ReflectionProbeComponent {
        bool enabled = true;
        ReflectionProbeShape shape = ReflectionProbeShape::Box;
        float sphereRadiusMeters = 5.0f;
        glm::vec3 boxExtentsMeters{ 5.0f };
        float blendDistanceMeters = 1.0f;
        float intensity = 1.0f;
        int32_t priority = 0;

        ReflectionProbeUpdateMode updateMode =
            ReflectionProbeUpdateMode::OnDemand;
        ReflectionProbeParallaxMode parallaxMode =
            ReflectionProbeParallaxMode::BoxProjection;
        int32_t captureResolution = 512;
        float captureNearMeters = 0.1f;
        float captureFarMeters = 100.0f;
        bool captureSky = true;
        AssetGuid environmentAssetGuid;

        // Transient publication state; never serialized or cooked.
        AssetGuid resolvedEnvironmentAssetGuid;
        AssetGuid requestedEnvironmentAssetGuid;
        std::string publicationDiagnostic;
        uint64_t explicitCaptureRevision = 0;
    };

} // namespace Iridium
