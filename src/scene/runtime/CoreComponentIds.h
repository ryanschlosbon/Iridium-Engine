#pragma once

#include <string_view>

namespace Iridium {

    inline constexpr std::string_view CoreNameComponentId =
        "iridium.component.name";
    inline constexpr std::string_view CoreTransformComponentId =
        "iridium.component.transform";
    inline constexpr std::string_view CoreRelationshipComponentId =
        "iridium.component.relationship";
    inline constexpr std::string_view CoreMeshComponentId =
        "iridium.component.mesh";
    inline constexpr std::string_view CoreLightComponentId =
        "iridium.component.light";
    inline constexpr std::string_view CoreSkyComponentId =
        "iridium.component.sky";
    inline constexpr std::string_view CoreReflectionProbeComponentId =
        "iridium.component.reflection_probe";
    inline constexpr std::string_view CoreBakedLightingSetComponentId =
        "iridium.component.baked_lighting_set";

} // namespace Iridium
