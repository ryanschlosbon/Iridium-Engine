#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Iridium {

    // M6.7 uses an intentionally conservative absolute scale for FP16 weighted
    // accumulation. The scale cancels during resolve, while keeping the
    // qualified 4,096-fragment / 128-scene-linear-radiance envelope below the
    // largest finite half value. Work outside that envelope remains bounded in
    // memory but must be reported as numerical saturation risk.
    inline constexpr float WeightedOitMinimumWeight = 1.0f / 4096.0f;
    inline constexpr float WeightedOitMaximumWeight = 1.0f / 16.0f;
    inline constexpr float WeightedOitMaximumPremultipliedRadiance = 128.0f;
    inline constexpr uint32_t WeightedOitQualifiedMaximumFragments = 4096u;
    inline constexpr double WeightedOitMaximumFiniteHalf = 65504.0;

    struct WeightedOitContribution {
        glm::vec3 premultipliedRadiance{ 0.0f };
        float coverage = 0.0f;
        // Zero is the near plane and one is the far plane. The Vulkan shader
        // derives this from linear view depth rather than device depth.
        float normalizedLinearDepth = 1.0f;
    };

    struct WeightedOitAccumulator {
        glm::dvec3 weightedPremultipliedRadiance{ 0.0 };
        double weightedCoverage = 0.0;
        double revealage = 1.0;
        uint32_t fragmentCount = 0u;
        bool sanitized = false;
        bool radianceClamped = false;
    };

    struct WeightedOitResolveResult {
        glm::vec3 premultipliedRadiance{ 0.0f };
        float coverage = 0.0f;
        bool finite = true;
    };

    [[nodiscard]] inline float weightedOitWeight(float coverage,
        float normalizedLinearDepth, bool* sanitized = nullptr) noexcept {
        bool changed = false;
        if (!std::isfinite(coverage)) {
            coverage = 0.0f;
            changed = true;
        }
        if (!std::isfinite(normalizedLinearDepth)) {
            normalizedLinearDepth = 1.0f;
            changed = true;
        }
        const float clampedCoverage = std::clamp(coverage, 0.0f, 1.0f);
        const float clampedDepth = std::clamp(
            normalizedLinearDepth, 0.0f, 1.0f);
        changed |= clampedCoverage != coverage ||
            clampedDepth != normalizedLinearDepth;
        if (sanitized != nullptr) *sanitized = changed;
        if (clampedCoverage == 0.0f) return 0.0f;
        const float depthWeight = 1.0f /
            (1.0f + 8.0f * clampedDepth * clampedDepth);
        return std::clamp(clampedCoverage * depthWeight *
            WeightedOitMaximumWeight, WeightedOitMinimumWeight,
            WeightedOitMaximumWeight);
    }

    inline void accumulateWeightedOit(WeightedOitAccumulator& accumulator,
        WeightedOitContribution contribution) noexcept {
        bool sanitized = false;
        const float weight = weightedOitWeight(contribution.coverage,
            contribution.normalizedLinearDepth, &sanitized);
        float coverage = contribution.coverage;
        if (!std::isfinite(coverage)) coverage = 0.0f;
        coverage = std::clamp(coverage, 0.0f, 1.0f);
        glm::vec3 radiance = contribution.premultipliedRadiance;
        for (uint32_t channel = 0u; channel < 3u; ++channel) {
            if (!std::isfinite(radiance[channel])) {
                radiance[channel] = 0.0f;
                sanitized = true;
            }
            const float clamped = std::clamp(radiance[channel], 0.0f,
                WeightedOitMaximumPremultipliedRadiance);
            accumulator.radianceClamped |= clamped != radiance[channel];
            radiance[channel] = clamped;
        }
        accumulator.sanitized |= sanitized;
        if (coverage == 0.0f) return;
        accumulator.weightedPremultipliedRadiance +=
            glm::dvec3(radiance) * static_cast<double>(weight);
        accumulator.weightedCoverage +=
            static_cast<double>(coverage) * weight;
        accumulator.revealage *= static_cast<double>(1.0f - coverage);
        ++accumulator.fragmentCount;
    }

    [[nodiscard]] inline bool weightedOitWithinQualifiedFp16Envelope(
        const WeightedOitAccumulator& accumulator) noexcept {
        if (accumulator.fragmentCount >
                WeightedOitQualifiedMaximumFragments ||
            accumulator.radianceClamped || accumulator.sanitized ||
            !std::isfinite(accumulator.weightedCoverage) ||
            !std::isfinite(accumulator.revealage)) {
            return false;
        }
        if (accumulator.weightedCoverage > WeightedOitMaximumFiniteHalf)
            return false;
        for (uint32_t channel = 0u; channel < 3u; ++channel) {
            if (!std::isfinite(
                    accumulator.weightedPremultipliedRadiance[channel]) ||
                accumulator.weightedPremultipliedRadiance[channel] >
                    WeightedOitMaximumFiniteHalf) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] inline WeightedOitResolveResult resolveWeightedOit(
        const WeightedOitAccumulator& accumulator) noexcept {
        WeightedOitResolveResult result{};
        if (!std::isfinite(accumulator.weightedCoverage) ||
            !std::isfinite(accumulator.revealage) ||
            accumulator.weightedCoverage <=
                std::numeric_limits<double>::epsilon()) {
            result.finite = std::isfinite(accumulator.weightedCoverage) &&
                std::isfinite(accumulator.revealage);
            return result;
        }
        const double coverage = std::clamp(
            1.0 - accumulator.revealage, 0.0, 1.0);
        const glm::dvec3 average =
            accumulator.weightedPremultipliedRadiance /
            accumulator.weightedCoverage;
        const glm::dvec3 premultiplied = average * coverage;
        result.coverage = static_cast<float>(coverage);
        result.premultipliedRadiance = glm::vec3(premultiplied);
        result.finite = std::isfinite(premultiplied.x) &&
            std::isfinite(premultiplied.y) &&
            std::isfinite(premultiplied.z);
        if (!result.finite) {
            result.premultipliedRadiance = glm::vec3(0.0f);
            result.coverage = 0.0f;
        }
        return result;
    }

    [[nodiscard]] constexpr uint64_t weightedOitLogicalStorageBytes(
        uint32_t width, uint32_t height) noexcept {
        // One RGBA16F additive accumulator and one R16F multiplicative
        // revealage product per frame context. Opaque depth is read-only and
        // shared; WeightedOIT owns no depth image.
        return static_cast<uint64_t>(width) * height * 10u;
    }

} // namespace Iridium
