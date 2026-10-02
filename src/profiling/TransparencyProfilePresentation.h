#pragma once

#include <string_view>

namespace Iridium {

    enum class TransparencyProfileGroup {
        None,
        Routing,
        Refraction,
        Ordinary2,
        DeepLayered,
        WeightedOit,
        LegacyComparison,
    };

    [[nodiscard]] constexpr TransparencyProfileGroup
        transparencyProfileGroup(std::string_view name) noexcept {
        if (name.starts_with("draw.recorded.transparent.oit") ||
            name.starts_with("transparent.oit.")) {
            return TransparencyProfileGroup::WeightedOit;
        }
        if (name.starts_with("transparent.ordinary2.")) {
            return TransparencyProfileGroup::Ordinary2;
        }
        if (name.starts_with("transparent.layered.")) {
            return TransparencyProfileGroup::DeepLayered;
        }
        if (name.starts_with("transparent.pyramid.")) {
            return TransparencyProfileGroup::Refraction;
        }
        if (name.starts_with("transparent.bucket.") ||
            name.starts_with("transparent.background.") ||
            name.starts_with("transparent.foreground.")) {
            return TransparencyProfileGroup::LegacyComparison;
        }
        if (name.starts_with("transparent.") ||
            name.starts_with("draw.recorded.transparent")) {
            return TransparencyProfileGroup::Routing;
        }
        return TransparencyProfileGroup::None;
    }

    [[nodiscard]] constexpr std::string_view
        transparencyProfileGroupName(
            TransparencyProfileGroup group) noexcept {
        switch (group) {
        case TransparencyProfileGroup::None: return "other";
        case TransparencyProfileGroup::Routing: return "routing";
        case TransparencyProfileGroup::Refraction: return "refraction";
        case TransparencyProfileGroup::Ordinary2: return "Ordinary2";
        case TransparencyProfileGroup::DeepLayered: return "Hero/Cinematic";
        case TransparencyProfileGroup::WeightedOit: return "Weighted OIT";
        case TransparencyProfileGroup::LegacyComparison: return "legacy A/B";
        }
        return "other";
    }

    [[nodiscard]] constexpr bool isTransparencyRiskCounter(
        std::string_view name) noexcept {
        if (transparencyProfileGroup(name) ==
            TransparencyProfileGroup::None) {
            return false;
        }
        constexpr std::string_view riskTerms[]{
            "fallback", "rejected", "failure", "overflow",
            "invalid", "ambiguous",
        };
        for (const std::string_view term : riskTerms) {
            if (name.find(term) != std::string_view::npos) return true;
        }
        return false;
    }

    [[nodiscard]] constexpr bool isTransparencyGpuRange(
        std::string_view name) noexcept {
        return name.starts_with("gpu.transparency.");
    }

} // namespace Iridium
