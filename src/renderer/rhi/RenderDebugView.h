#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace Iridium {

    enum class RenderDebugView : uint32_t {
        Final = 0,
        BaseColor = 1,
        Normal = 2,
        Roughness = 3,
        Metallic = 4,
        Emissive = 5,
        Depth = 6,
        AmbientOcclusion = 7,
        F0 = 8,
        F90 = 9,
        MaterialId = 10,
        MaterialFlags = 11,
        ClosureClass = 12,
        ClusterOccupancy = 13,
        ClusterOverflow = 14,
        DirectLighting = 15,
        ShadowCascade = 16,
        ShadowVisibility = 17,
    };

    [[nodiscard]] constexpr std::string_view renderDebugViewName(RenderDebugView view) noexcept {
        switch (view) {
        case RenderDebugView::Final: return "final";
        case RenderDebugView::BaseColor: return "base-color";
        case RenderDebugView::Normal: return "normal";
        case RenderDebugView::Roughness: return "roughness";
        case RenderDebugView::Metallic: return "metallic";
        case RenderDebugView::Emissive: return "emissive";
        case RenderDebugView::Depth: return "depth";
        case RenderDebugView::AmbientOcclusion: return "ao";
        case RenderDebugView::F0: return "f0";
        case RenderDebugView::F90: return "f90";
        case RenderDebugView::MaterialId: return "material-id";
        case RenderDebugView::MaterialFlags: return "material-flags";
        case RenderDebugView::ClosureClass: return "closure-class";
        case RenderDebugView::ClusterOccupancy: return "cluster-occupancy";
        case RenderDebugView::ClusterOverflow: return "cluster-overflow";
        case RenderDebugView::DirectLighting: return "direct-lighting";
        case RenderDebugView::ShadowCascade: return "shadow-cascade";
        case RenderDebugView::ShadowVisibility: return "shadow-visibility";
        }
        return "unknown";
    }

    [[nodiscard]] constexpr std::optional<RenderDebugView> parseRenderDebugView(
        std::string_view name) noexcept {
        if (name == "final") return RenderDebugView::Final;
        if (name == "base-color" || name == "basecolor" || name == "albedo") {
            return RenderDebugView::BaseColor;
        }
        if (name == "normal" || name == "normals") return RenderDebugView::Normal;
        if (name == "roughness") return RenderDebugView::Roughness;
        if (name == "metallic") return RenderDebugView::Metallic;
        if (name == "emissive") return RenderDebugView::Emissive;
        if (name == "depth") return RenderDebugView::Depth;
        if (name == "ao" || name == "ambient-occlusion") return RenderDebugView::AmbientOcclusion;
        if (name == "f0") return RenderDebugView::F0;
        if (name == "f90") return RenderDebugView::F90;
        if (name == "material-id") return RenderDebugView::MaterialId;
        if (name == "material-flags") return RenderDebugView::MaterialFlags;
        if (name == "closure" || name == "closure-class") return RenderDebugView::ClosureClass;
        if (name == "cluster" || name == "cluster-occupancy")
            return RenderDebugView::ClusterOccupancy;
        if (name == "cluster-overflow") return RenderDebugView::ClusterOverflow;
        if (name == "direct" || name == "direct-lighting")
            return RenderDebugView::DirectLighting;
        if (name == "shadow-cascade" || name == "cascades")
            return RenderDebugView::ShadowCascade;
        if (name == "shadow-visibility" || name == "shadows")
            return RenderDebugView::ShadowVisibility;
        return std::nullopt;
    }

    [[nodiscard]] constexpr std::string_view renderDebugViewDescription(
        RenderDebugView view) noexcept {
        switch (view) {
        case RenderDebugView::Final:
            return "scene-linear composition mapped once by the final output pass";
        case RenderDebugView::BaseColor:
            return "canonical diffuse albedo for deferred materials; source base color for complex forward materials";
        case RenderDebugView::Normal:
            return "decoded opaque world normal remapped from [-1,1] to [0,1]";
        case RenderDebugView::Roughness:
            return "opaque G-buffer perceptual roughness as linear grayscale";
        case RenderDebugView::Metallic:
            return "source metallic for complex forward materials; deferred canonical closures store RGB F0 instead";
        case RenderDebugView::Emissive:
            return "opaque scene-linear ACEScg/AP1 emissive routed through final output";
        case RenderDebugView::Depth:
            return "raw non-linear Vulkan device depth in [0,1]";
        case RenderDebugView::AmbientOcclusion:
            return "canonical opaque ambient-occlusion multiplier";
        case RenderDebugView::F0:
            return "canonical opaque RGB normal-incidence reflectance";
        case RenderDebugView::F90:
            return "canonical opaque scalar grazing reflectance";
        case RenderDebugView::MaterialId:
            return "stable frame-local packed material index, hash-colored";
        case RenderDebugView::MaterialFlags:
            return "canonical material feature flags, hash-colored";
        case RenderDebugView::ClosureClass:
            return "compiled closure class, hash-colored";
        case RenderDebugView::ClusterOccupancy:
            return "shared local-light cluster occupancy, blue through red at the 256-light limit";
        case RenderDebugView::ClusterOverflow:
            return "magenta when the shared cluster product uses deterministic top-64 fallback";
        case RenderDebugView::DirectLighting:
            return "scene-linear direct-light contribution from authored directional, point, and spot lights only";
        case RenderDebugView::ShadowCascade:
            return "directional shadow cascade selection; red, green, blue, then yellow, with black for an unpublished cascade";
        case RenderDebugView::ShadowVisibility:
            return "minimum filtered visibility across contributing directional, spot, and point shadows; black blocked through white visible";
        }
        return "unknown debug-view semantics";
    }

} // namespace Iridium
