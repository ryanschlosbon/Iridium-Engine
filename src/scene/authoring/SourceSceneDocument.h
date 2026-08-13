#pragma once

#include "scene/SceneEntityUuid.h"
#include "scene/authoring/SourceComponentRegistry.h"
#include "scene/authoring/SourceJsonParser.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Iridium {

    inline constexpr uint32_t kCurrentSourceSceneSchemaVersion = 1;
    inline constexpr std::string_view kSourceSceneFormat = "iridium.scene";

    struct SourceSceneComponent {
        ComponentTypeId id;
        uint32_t version = 0;
        SourceJson data = SourceJson::object();
        SourceJson unknownEnvelopeFields = SourceJson::object();
        bool known = false;
    };

    struct SourceSceneEntity {
        SceneEntityUuid uuid;
        std::vector<SourceSceneComponent> components;
        SourceJson extensions = SourceJson::object();
        SourceJson unknownFields = SourceJson::object();
    };

    struct SourceSceneDocument {
        std::string name;
        std::vector<SourceSceneEntity> entities;
        SourceJson extensions = SourceJson::object();
        SourceJson unknownFields = SourceJson::object();
    };

    struct SourceSceneReadResult {
        std::optional<SourceSceneDocument> document;
        std::vector<SceneDiagnostic> diagnostics;

        [[nodiscard]] explicit operator bool() const noexcept {
            return document.has_value() && !hasSceneErrors(diagnostics);
        }
    };

    struct SourceSceneWriteResult {
        std::optional<std::string> bytes;
        std::vector<SceneDiagnostic> diagnostics;

        [[nodiscard]] explicit operator bool() const noexcept {
            return bytes.has_value() && !hasSceneErrors(diagnostics);
        }
    };

    [[nodiscard]] SourceSceneReadResult readSourceSceneSchema1(
        std::string_view bytes,
        const RuntimeComponentRegistry& runtimeRegistry,
        const ComponentSerializerRegistry& sourceRegistry,
        SourceJsonParseOptions options = {});

    [[nodiscard]] SourceSceneWriteResult writeSourceSceneCanonical(
        const SourceSceneDocument& document,
        const RuntimeComponentRegistry& runtimeRegistry,
        const ComponentSerializerRegistry& sourceRegistry);

} // namespace Iridium
