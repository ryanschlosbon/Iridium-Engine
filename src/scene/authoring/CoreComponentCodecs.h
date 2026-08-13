#pragma once

#include "scene/authoring/SourceComponentRegistry.h"

namespace Iridium {

    struct SourceComponentCallbacks {
        SerializeSourceComponentFn serializeSource = nullptr;
        DeserializeSourceComponentFn deserializeLocal = nullptr;
        ValidateSourceComponentFn validateLocal = nullptr;
        uint32_t currentSourceVersion = 1;
        std::vector<ComponentMigration> migrations;
    };

    struct CoreSourceComponentCallbacks {
        SourceComponentCallbacks name;
        SourceComponentCallbacks transform;
        SourceComponentCallbacks relationship;
        SourceComponentCallbacks mesh;
        SourceComponentCallbacks light;
        SourceComponentCallbacks sky;
        SourceComponentCallbacks reflectionProbe;
        SourceComponentCallbacks bakedLightingSet;
    };

    struct CoreSourceRegistryResult {
        ComponentSerializerRegistry registry;
        SourceRegistryResult status;

        [[nodiscard]] explicit operator bool() const noexcept {
            return static_cast<bool>(status) && registry.isFrozen();
        }
    };

    [[nodiscard]] CoreSourceRegistryResult createSourceComponentSerializerRegistry(
        const RuntimeComponentRegistry& runtimeRegistry,
        const CoreSourceComponentCallbacks& callbacks);

} // namespace Iridium
