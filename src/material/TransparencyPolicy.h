#pragma once

#include <bit>
#include <cstdint>
#include <string_view>

namespace Iridium {

    enum class TransparencyClass : uint8_t {
        Auto,
        None,
        AlphaClip,
        SortedSurface,
        ThinGlass,
        LayeredGlass,
        WeightedOit,
    };

    enum class TransparencyQuality : uint8_t {
        Ordinary2 = 2,
        Hero4 = 4,
        Cinematic8 = 8,
    };

    enum class TransparencyTopology : uint8_t {
        Unknown,
        ValidClosed,
        Invalid,
    };

    enum class TransparencyExecutionMode : uint8_t {
        LegacyTwoBucket,
        Classified,
    };

    enum CompiledTransparencyFlag : uint8_t {
        CompiledTransparencyNone = 0,
        CompiledTransparencyExplicitClass = 1u << 0u,
        CompiledTransparencyFallbackApplied = 1u << 1u,
        CompiledTransparencyTopologyRequired = 1u << 2u,
        CompiledTransparencyPolicySanitized = 1u << 3u,
    };

    struct TransparencyPolicyV1 {
        static constexpr uint32_t SchemaVersion = 1;

        TransparencyClass requestedClass = TransparencyClass::Auto;
        TransparencyQuality quality = TransparencyQuality::Ordinary2;
        int32_t priority = 0;
        float thinSheetThicknessMeters = 0.0f;

        bool operator==(const TransparencyPolicyV1&) const = default;
    };

    struct CompiledTransparencyPolicy {
        TransparencyClass requestedClass = TransparencyClass::Auto;
        TransparencyClass resolvedClass = TransparencyClass::None;
        TransparencyQuality quality = TransparencyQuality::Ordinary2;
        uint8_t flags = CompiledTransparencyNone;
        int32_t priority = 0;
        float thinSheetThicknessMeters = 0.0f;

        bool operator==(const CompiledTransparencyPolicy&) const = default;
    };

    static_assert(sizeof(CompiledTransparencyPolicy) == 12);

    [[nodiscard]] constexpr bool isAuthoredTransparencyClass(
        TransparencyClass value) noexcept {
        return value == TransparencyClass::Auto ||
            value == TransparencyClass::AlphaClip ||
            value == TransparencyClass::SortedSurface ||
            value == TransparencyClass::ThinGlass ||
            value == TransparencyClass::LayeredGlass ||
            value == TransparencyClass::WeightedOit;
    }

    [[nodiscard]] constexpr bool isTransparencyQuality(
        TransparencyQuality value) noexcept {
        return value == TransparencyQuality::Ordinary2 ||
            value == TransparencyQuality::Hero4 ||
            value == TransparencyQuality::Cinematic8;
    }

    [[nodiscard]] constexpr std::string_view transparencyClassName(
        TransparencyClass value) noexcept {
        switch (value) {
        case TransparencyClass::Auto: return "auto";
        case TransparencyClass::None: return "none";
        case TransparencyClass::AlphaClip: return "alpha-clip";
        case TransparencyClass::SortedSurface: return "sorted-surface";
        case TransparencyClass::ThinGlass: return "thin-glass";
        case TransparencyClass::LayeredGlass: return "layered-glass";
        case TransparencyClass::WeightedOit: return "weighted-oit";
        }
        return "invalid";
    }

    [[nodiscard]] constexpr std::string_view transparencyQualityName(
        TransparencyQuality value) noexcept {
        switch (value) {
        case TransparencyQuality::Ordinary2: return "ordinary2";
        case TransparencyQuality::Hero4: return "hero4";
        case TransparencyQuality::Cinematic8: return "cinematic8";
        }
        return "invalid";
    }

    [[nodiscard]] constexpr std::string_view transparencyExecutionModeName(
        TransparencyExecutionMode value) noexcept {
        switch (value) {
        case TransparencyExecutionMode::LegacyTwoBucket:
            return "legacy-two-bucket";
        case TransparencyExecutionMode::Classified:
            return "classified";
        }
        return "invalid";
    }

    [[nodiscard]] constexpr uint32_t packTransparencyPolicyWord(
        const CompiledTransparencyPolicy& policy) noexcept {
        return static_cast<uint32_t>(policy.requestedClass) |
            (static_cast<uint32_t>(policy.resolvedClass) << 8u) |
            (static_cast<uint32_t>(policy.quality) << 16u) |
            (static_cast<uint32_t>(policy.flags) << 24u);
    }

    [[nodiscard]] constexpr CompiledTransparencyPolicy
        unpackTransparencyPolicyWord(uint32_t word, int32_t priority,
            float thinSheetThicknessMeters) noexcept {
        return {
            .requestedClass = static_cast<TransparencyClass>(word & 0xffu),
            .resolvedClass = static_cast<TransparencyClass>((word >> 8u) & 0xffu),
            .quality = static_cast<TransparencyQuality>((word >> 16u) & 0xffu),
            .flags = static_cast<uint8_t>((word >> 24u) & 0xffu),
            .priority = priority,
            .thinSheetThicknessMeters = thinSheetThicknessMeters,
        };
    }

} // namespace Iridium
