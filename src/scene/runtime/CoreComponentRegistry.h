#pragma once

#include "scene/runtime/RuntimeComponentRegistry.h"

namespace Iridium {

    struct RuntimeComponentCallbacks {
        ResolveComponentReferencesFn resolveReferences = nullptr;
        ValidateRuntimeComponentFn postLoadValidate = nullptr;
        EncodeCookedComponentFn encodeCooked = nullptr;
        DecodeCookedComponentFn decodeCooked = nullptr;
    };

    struct CoreRuntimeComponentCallbacks {
        RuntimeComponentCallbacks name;
        RuntimeComponentCallbacks transform;
        RuntimeComponentCallbacks relationship;
        RuntimeComponentCallbacks mesh;
        RuntimeComponentCallbacks light;
        RuntimeComponentCallbacks sky;
        RuntimeComponentCallbacks reflectionProbe;
        RuntimeComponentCallbacks bakedLightingSet;
    };

    struct CoreRuntimeRegistryResult {
        RuntimeComponentRegistry registry;
        ComponentRegistryResult status;

        [[nodiscard]] explicit operator bool() const noexcept {
            return static_cast<bool>(status) && registry.isFrozen();
        }
    };

    [[nodiscard]] CoreRuntimeRegistryResult createRuntimeSceneComponentRegistry(
        const CoreRuntimeComponentCallbacks& callbacks);

} // namespace Iridium
