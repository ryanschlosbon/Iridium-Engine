#pragma once

#include "renderer/rhi/ShadowSettings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Iridium {

    // Backend-neutral physical result used by conventional maps now and by
    // virtual/RT shadow representations later. Radii are deliberately split:
    // the blocker search is an input-footprint bound, while the filter radius
    // is the measured penumbra at the receiver.
    struct ShadowContactFilterPlan {
        float blockerSearchRadiusTexels = 0.0f;
        float penumbraRadiusTexels = 0.0f;
        uint32_t blockerSearchSamples = 0;
        uint32_t filterSamples = 1;
        bool contactHardening = false;
    };

    [[nodiscard]] inline ShadowContactFilterPlan directionalShadowFilterPlan(
        float receiverDepth, float averageBlockerDepth,
        float shadowDepthSpanMeters, float worldUnitsPerTexel,
        float sourceAngularDiameterDegrees, ShadowFilterProfile profile,
        float projectMaximumPenumbraTexels) noexcept {
        ShadowContactFilterPlan result{
            .filterSamples = profile.filterSamples,
        };
        if (!profile.contactHardening || profile.blockerSearchSamples == 0 ||
            !(receiverDepth > averageBlockerDepth) ||
            !(shadowDepthSpanMeters > 0.0f) || !(worldUnitsPerTexel > 0.0f) ||
            !(sourceAngularDiameterDegrees > 0.0f))
            return result;

        constexpr float Pi = 3.14159265358979323846f;
        const float angularRadius = sourceAngularDiameterDegrees *
            (Pi / 360.0f);
        const float tangent = std::tan(angularRadius);
        if (!std::isfinite(tangent) || !(tangent > 0.0f)) return result;
        const float receiverSeparationMeters =
            (receiverDepth - averageBlockerDepth) * shadowDepthSpanMeters;
        const float maximumRadius = (std::max)(0.0f,
            (std::min)(profile.maximumPenumbraTexels,
                projectMaximumPenumbraTexels));
        result.penumbraRadiusTexels = std::clamp(
            receiverSeparationMeters * tangent / worldUnitsPerTexel,
            0.0f, maximumRadius);
        result.blockerSearchRadiusTexels = result.penumbraRadiusTexels;
        result.blockerSearchSamples = profile.blockerSearchSamples;
        result.contactHardening = result.penumbraRadiusTexels > 0.0f;
        return result;
    }

    [[nodiscard]] inline ShadowContactFilterPlan localShadowFilterPlan(
        float receiverDistanceMeters, float averageBlockerDistanceMeters,
        float sourceRadiusMeters, float texelsPerRadian,
        ShadowFilterProfile profile,
        float projectMaximumPenumbraTexels) noexcept {
        ShadowContactFilterPlan result{
            .filterSamples = profile.filterSamples,
        };
        if (!profile.contactHardening || profile.blockerSearchSamples == 0 ||
            !(receiverDistanceMeters > averageBlockerDistanceMeters) ||
            !(averageBlockerDistanceMeters > 0.0f) ||
            !(sourceRadiusMeters > 0.0f) || !(texelsPerRadian > 0.0f))
            return result;

        // Similar triangles: an emitter radius R behind a blocker produces a
        // receiver-space penumbra R * (zr-zb) / zb. Dividing by receiver
        // distance converts that footprint to angle for spot/cube maps.
        const float penumbraAngle = sourceRadiusMeters *
            (receiverDistanceMeters - averageBlockerDistanceMeters) /
            (averageBlockerDistanceMeters * receiverDistanceMeters);
        const float maximumRadius = (std::max)(0.0f,
            (std::min)(profile.maximumPenumbraTexels,
                projectMaximumPenumbraTexels));
        result.penumbraRadiusTexels = std::clamp(
            penumbraAngle * texelsPerRadian, 0.0f, maximumRadius);
        result.blockerSearchRadiusTexels = std::clamp(
            sourceRadiusMeters / receiverDistanceMeters * texelsPerRadian,
            0.0f, maximumRadius);
        result.blockerSearchSamples = profile.blockerSearchSamples;
        result.contactHardening = result.penumbraRadiusTexels > 0.0f;
        return result;
    }

} // namespace Iridium
