#pragma once

#include "renderer/transparency/LayeredGlass.h"

#include <array>
#include <cstdint>

namespace Iridium {

    struct Ordinary2CaptureValidationResult {
        uint64_t validationId = 0;
        uint32_t atlasWidth = 0;
        uint32_t atlasHeight = 0;
        uint32_t expectedDrawCount = 0;
        uint32_t workItemCount = 0;
        uint64_t inspectedPixelCount = 0;
        uint64_t entryPixelCount = 0;
        uint64_t exitPixelCount = 0;
        uint64_t pairedPixelCount = 0;
        uint64_t entryOnlyPixelCount = 0;
        uint64_t invalidWorkIndexPixelCount = 0;
        uint64_t invalidOrientationPixelCount = 0;
        uint64_t unpairedExitPixelCount = 0;
        uint64_t workMismatchPixelCount = 0;
        uint64_t invalidDepthPixelCount = 0;
        uint64_t nonIncreasingDepthPixelCount = 0;
        uint64_t localColorPixelCount = 0;
        uint64_t localColorInvalidPixelCount = 0;
        float minimumPairedDepthDelta = 0.0f;
        float maximumPairedDepthDelta = 0.0f;
        float minimumLocalAlpha = 0.0f;
        float maximumLocalAlpha = 0.0f;

        [[nodiscard]] constexpr bool passed() const noexcept {
            return expectedDrawCount != 0u && workItemCount != 0u &&
                entryPixelCount != 0u && exitPixelCount != 0u &&
                pairedPixelCount == exitPixelCount &&
                invalidWorkIndexPixelCount == 0u &&
                invalidOrientationPixelCount == 0u &&
                unpairedExitPixelCount == 0u &&
                workMismatchPixelCount == 0u &&
                invalidDepthPixelCount == 0u &&
                nonIncreasingDepthPixelCount == 0u &&
                localColorPixelCount == pairedPixelCount &&
                localColorInvalidPixelCount == 0u &&
                minimumPairedDepthDelta > 0.0f &&
                maximumPairedDepthDelta >= minimumPairedDepthDelta &&
                minimumLocalAlpha > 0.0f &&
                maximumLocalAlpha >= minimumLocalAlpha &&
                maximumLocalAlpha <= 1.0f;
        }
    };

    struct DeepLayeredCaptureValidationResult {
        uint64_t validationId = 0;
        TransparencyQuality quality = TransparencyQuality::Hero4;
        uint32_t atlasWidth = 0;
        uint32_t atlasHeight = 0;
        uint32_t interfaceCount = 0;
        uint32_t expectedDrawCount = 0;
        uint32_t sceneResolveDrawCount = 0;
        uint32_t compatibilityForwardDrawCount = 0;
        uint32_t workItemCount = 0;
        uint32_t maximumObservedInterfaceCount = 0;
        uint64_t inspectedPixelCount = 0;
        std::array<uint64_t, kMaximumLayeredInterfaceCount>
            interfacePixelCounts{};
        std::array<uint64_t, kMaximumLayeredInterfaceCount>
            terminatedOccupiedTileCounts{};
        uint64_t pairedPixelCount = 0;
        uint64_t nestedFourInterfacePixelCount = 0;
        uint64_t crossingPairPixelCount = 0;
        uint64_t earlyTerminatedPixelCount = 0;
        uint64_t terminatedOccupiedTileCount = 0;
        uint64_t invalidWorkIndexPixelCount = 0;
        uint64_t invalidOrientationPixelCount = 0;
        uint64_t invalidDepthPixelCount = 0;
        uint64_t nonIncreasingDepthPixelCount = 0;
        uint64_t interfaceGapPixelCount = 0;
        uint64_t duplicateEntryPixelCount = 0;
        uint64_t unmatchedExitPixelCount = 0;
        uint64_t unclosedEntryPixelCount = 0;
        uint64_t saturatedResidualPixelCount = 0;
        uint64_t localColorPixelCount = 0;
        uint64_t localColorInvalidPixelCount = 0;
        float minimumDepthDelta = 0.0f;
        float maximumDepthDelta = 0.0f;
        float minimumLocalAlpha = 0.0f;
        float maximumLocalAlpha = 0.0f;

        [[nodiscard]] constexpr bool passed() const noexcept {
            return (quality == TransparencyQuality::Hero4 ||
                    quality == TransparencyQuality::Cinematic8) &&
                interfaceCount == layeredQualityTierContract(
                    quality).maximumInterfaceCount &&
                expectedDrawCount >= 2u && workItemCount >= 2u &&
                sceneResolveDrawCount == expectedDrawCount &&
                compatibilityForwardDrawCount == 0u &&
                (maximumObservedInterfaceCount == interfaceCount ||
                    terminatedOccupiedTileCount != 0u) &&
                (interfacePixelCounts[interfaceCount - 1u] != 0u ||
                    earlyTerminatedPixelCount != 0u) &&
                pairedPixelCount != 0u &&
                (nestedFourInterfacePixelCount != 0u ||
                    earlyTerminatedPixelCount != 0u) &&
                invalidWorkIndexPixelCount == 0u &&
                invalidOrientationPixelCount == 0u &&
                invalidDepthPixelCount == 0u &&
                nonIncreasingDepthPixelCount == 0u &&
                interfaceGapPixelCount == 0u &&
                duplicateEntryPixelCount == 0u &&
                unmatchedExitPixelCount == 0u &&
                unclosedEntryPixelCount == 0u &&
                localColorPixelCount == pairedPixelCount &&
                localColorInvalidPixelCount == 0u &&
                minimumDepthDelta > 0.0f &&
                maximumDepthDelta >= minimumDepthDelta &&
                minimumLocalAlpha > 0.0f &&
                maximumLocalAlpha >= minimumLocalAlpha &&
                maximumLocalAlpha <= 1.0f;
        }
    };

} // namespace Iridium
