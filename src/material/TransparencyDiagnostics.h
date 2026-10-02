#pragma once

#include "material/TransparencyPolicy.h"

#include <string_view>

namespace Iridium {

    enum class TransparencyTopologyDiagnostic : uint8_t {
        NotRequired,
        RequiredUnverified,
        ValidatedClosed,
        InvalidOrUnavailable,
    };

    enum class TransparencyFallbackReason : uint8_t {
        None,
        LayeredTopology,
        AlphaClipIncompatible,
        SortedTransportIncompatible,
        WeightedOitTransportIncompatible,
        SafeAutoResolution,
    };

    struct TransparencyDiagnosticSummary {
        TransparencyTopologyDiagnostic topology =
            TransparencyTopologyDiagnostic::NotRequired;
        TransparencyFallbackReason fallback =
            TransparencyFallbackReason::None;
        bool explicitClass = false;
        bool fallbackApplied = false;
        bool policySanitized = false;
    };

    [[nodiscard]] constexpr TransparencyDiagnosticSummary
        describeTransparencyPolicy(
            const CompiledTransparencyPolicy& policy) noexcept {
        TransparencyDiagnosticSummary result{
            .explicitClass = (policy.flags &
                CompiledTransparencyExplicitClass) != 0u,
            .fallbackApplied = (policy.flags &
                CompiledTransparencyFallbackApplied) != 0u,
            .policySanitized = (policy.flags &
                CompiledTransparencyPolicySanitized) != 0u,
        };

        if (policy.resolvedClass == TransparencyClass::LayeredGlass) {
            result.topology =
                TransparencyTopologyDiagnostic::ValidatedClosed;
        }
        else if ((policy.flags &
                CompiledTransparencyTopologyRequired) != 0u) {
            result.topology = result.fallbackApplied
                ? TransparencyTopologyDiagnostic::InvalidOrUnavailable
                : TransparencyTopologyDiagnostic::RequiredUnverified;
        }

        if (!result.fallbackApplied) return result;
        if (policy.requestedClass == TransparencyClass::LayeredGlass &&
            policy.resolvedClass == TransparencyClass::ThinGlass) {
            result.fallback = TransparencyFallbackReason::LayeredTopology;
        }
        else if (policy.requestedClass == TransparencyClass::AlphaClip) {
            result.fallback =
                TransparencyFallbackReason::AlphaClipIncompatible;
        }
        else if (policy.requestedClass ==
                TransparencyClass::SortedSurface &&
            policy.resolvedClass == TransparencyClass::ThinGlass) {
            result.fallback =
                TransparencyFallbackReason::SortedTransportIncompatible;
        }
        else if (policy.requestedClass ==
                TransparencyClass::WeightedOit &&
            policy.resolvedClass == TransparencyClass::SortedSurface) {
            result.fallback =
                TransparencyFallbackReason::WeightedOitTransportIncompatible;
        }
        else {
            result.fallback =
                TransparencyFallbackReason::SafeAutoResolution;
        }
        return result;
    }

    [[nodiscard]] constexpr std::string_view
        transparencyTopologyDiagnosticName(
            TransparencyTopologyDiagnostic value) noexcept {
        switch (value) {
        case TransparencyTopologyDiagnostic::NotRequired:
            return "not-required";
        case TransparencyTopologyDiagnostic::RequiredUnverified:
            return "required-unverified";
        case TransparencyTopologyDiagnostic::ValidatedClosed:
            return "validated-closed";
        case TransparencyTopologyDiagnostic::InvalidOrUnavailable:
            return "invalid-or-unavailable";
        }
        return "invalid";
    }

    [[nodiscard]] constexpr std::string_view
        transparencyTopologyDiagnosticDescription(
            TransparencyTopologyDiagnostic value) noexcept {
        switch (value) {
        case TransparencyTopologyDiagnostic::NotRequired:
            return "The resolved class does not require closed-volume topology.";
        case TransparencyTopologyDiagnostic::RequiredUnverified:
            return "Closed-volume topology is required but has not been validated for this material-only result.";
        case TransparencyTopologyDiagnostic::ValidatedClosed:
            return "The active Layered Glass route was accepted only after closed, consistently oriented topology validation.";
        case TransparencyTopologyDiagnostic::InvalidOrUnavailable:
            return "Closed-volume topology was invalid or unavailable, so the volume-safe Thin Glass fallback is active.";
        }
        return "Unknown topology diagnostic.";
    }

    [[nodiscard]] constexpr std::string_view
        transparencyFallbackReasonName(
            TransparencyFallbackReason value) noexcept {
        switch (value) {
        case TransparencyFallbackReason::None:
            return "none";
        case TransparencyFallbackReason::LayeredTopology:
            return "layered-topology";
        case TransparencyFallbackReason::AlphaClipIncompatible:
            return "alpha-clip-incompatible";
        case TransparencyFallbackReason::SortedTransportIncompatible:
            return "sorted-transport-incompatible";
        case TransparencyFallbackReason::WeightedOitTransportIncompatible:
            return "weighted-oit-transport-incompatible";
        case TransparencyFallbackReason::SafeAutoResolution:
            return "safe-auto-resolution";
        }
        return "invalid";
    }

    [[nodiscard]] constexpr std::string_view
        transparencyFallbackReasonDescription(
            TransparencyFallbackReason value) noexcept {
        switch (value) {
        case TransparencyFallbackReason::None:
            return "The requested policy is active without a compatibility fallback.";
        case TransparencyFallbackReason::LayeredTopology:
            return "Layered Glass requires a closed, consistently oriented volume; Thin Glass preserves transmission when that proof fails.";
        case TransparencyFallbackReason::AlphaClipIncompatible:
            return "Alpha Clip requires non-transmissive mask coverage; the safe Auto class is active.";
        case TransparencyFallbackReason::SortedTransportIncompatible:
            return "Sorted Surface cannot carry refractive, volume, or dispersive transport; Thin Glass is active.";
        case TransparencyFallbackReason::WeightedOitTransportIncompatible:
            return "Weighted OIT is non-refractive; Sorted Surface is active for incompatible optical transport.";
        case TransparencyFallbackReason::SafeAutoResolution:
            return "The requested class was incompatible and the compiler selected its safe Auto resolution.";
        }
        return "Unknown fallback reason.";
    }

    [[nodiscard]] constexpr std::string_view
        transparencyExecutionRouteName(
            TransparencyClass value) noexcept {
        switch (value) {
        case TransparencyClass::Auto:
            return "unresolved-auto";
        case TransparencyClass::None:
            return "opaque";
        case TransparencyClass::AlphaClip:
            return "opaque-alpha-clip";
        case TransparencyClass::SortedSurface:
            return "sorted-premultiplied";
        case TransparencyClass::ThinGlass:
            return "thin-glass-refraction";
        case TransparencyClass::LayeredGlass:
            return "bounded-layered-glass";
        case TransparencyClass::WeightedOit:
            return "weighted-oit-accumulation";
        }
        return "invalid";
    }

} // namespace Iridium
