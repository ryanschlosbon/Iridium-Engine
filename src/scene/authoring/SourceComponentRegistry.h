#pragma once

#include "ecs/Entity.h"
#include "scene/runtime/RuntimeComponentRegistry.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

class Registry;

namespace Iridium {

    using SourceJson = nlohmann::ordered_json;

    using SerializeSourceComponentFn = bool (*)(
        const Registry&, Entity, const SceneIdentityMap&,
        SourceJson&, std::string&);
    using DeserializeSourceComponentFn = bool (*)(
        Registry&, Entity, const SourceJson&, std::string&);
    using ValidateSourceComponentFn = bool (*)(
        const SourceJson&, std::string&);
    struct SourceMigrationNotice {
        std::string code;
        std::string propertyPath;
        std::string message;
    };

    using MigrateSourceComponentFn = bool (*)(
        const SourceJson&, SourceJson&,
        std::vector<SourceMigrationNotice>&, std::string&);

    struct ComponentMigration {
        uint32_t fromVersion = 0;
        uint32_t toVersion = 0;
        MigrateSourceComponentFn migrate = nullptr;
    };

    struct SourcePropertyBinding {
        PropertyId propertyId;
        std::string sourceName;
    };

    struct SourceComponentCodec {
        ComponentTypeId componentId;
        uint32_t currentSourceVersion = 0;
        uint32_t sourceOrder = 0;
        std::vector<SourcePropertyBinding> properties;
        SerializeSourceComponentFn serializeSource = nullptr;
        DeserializeSourceComponentFn deserializeLocal = nullptr;
        ValidateSourceComponentFn validateLocal = nullptr;
        std::vector<ComponentMigration> migrations;
    };

    enum class SourceRegistryError {
        None,
        Frozen,
        RuntimeRegistryNotFrozen,
        InvalidComponentId,
        UnknownRuntimeComponent,
        MissingSourceCodec,
        DuplicateComponentId,
        DuplicateSourceOrder,
        InvalidSourceVersion,
        InvalidSourceProperty,
        DuplicateSourceProperty,
        UnknownRuntimeProperty,
        MissingCallback,
        InvalidMigrationStep,
        DuplicateMigrationFromVersion,
        MigrationGap,
        MigrationDoesNotReachCurrent,
        UnsupportedSourceVersion,
        MigrationFailed,
    };

    struct SourceRegistryResult {
        SourceRegistryError error = SourceRegistryError::None;
        std::string message;

        [[nodiscard]] explicit operator bool() const noexcept {
            return error == SourceRegistryError::None;
        }
    };

    struct SourceMigrationResult {
        SourceRegistryResult status;
        SourceJson data;
        std::vector<SourceMigrationNotice> notices;
        uint32_t sourceVersion = 0;
        uint32_t targetVersion = 0;

        [[nodiscard]] explicit operator bool() const noexcept {
            return static_cast<bool>(status);
        }
    };

    class ComponentSerializerRegistry {
    public:
        [[nodiscard]] SourceRegistryResult add(SourceComponentCodec codec);
        [[nodiscard]] SourceRegistryResult freezeAndValidate(
            const RuntimeComponentRegistry& runtimeRegistry);

        [[nodiscard]] const SourceComponentCodec* find(
            const ComponentTypeId& id) const noexcept;
        [[nodiscard]] std::span<const SourceComponentCodec> codecs() const noexcept;
        [[nodiscard]] SourceMigrationResult migrateToCurrent(
            const ComponentTypeId& id,
            uint32_t sourceVersion,
            const SourceJson& data) const;
        [[nodiscard]] std::string_view sourceName(
            const ComponentTypeId& component,
            const PropertyId& property) const noexcept;
        [[nodiscard]] bool isFrozen() const noexcept { return frozen_; }

    private:
        std::vector<SourceComponentCodec> codecs_;
        bool frozen_ = false;
    };

} // namespace Iridium
