#include "scene/authoring/SourceComponentRegistry.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <utility>

namespace Iridium {
    namespace {

        [[nodiscard]] SourceRegistryResult validateCodec(
            const SourceComponentCodec& codec) {
            if (codec.componentId.empty()) {
                return { SourceRegistryError::InvalidComponentId,
                    "Source component codec ID is empty or invalid" };
            }
            if (codec.currentSourceVersion == 0) {
                return { SourceRegistryError::InvalidSourceVersion,
                    "Source component version zero is reserved" };
            }
            if (!codec.serializeSource || !codec.deserializeLocal ||
                !codec.validateLocal) {
                return { SourceRegistryError::MissingCallback,
                    "Source component codec is missing a required callback" };
            }

            std::unordered_set<std::string> propertyIds;
            std::unordered_set<std::string> sourceNames;
            for (const SourcePropertyBinding& property : codec.properties) {
                if (property.propertyId.empty() || property.sourceName.empty()) {
                    return { SourceRegistryError::InvalidSourceProperty,
                        "Source property binding has an empty identity or name" };
                }
                if (!propertyIds.insert(property.propertyId.value()).second ||
                    !sourceNames.insert(property.sourceName).second) {
                    return { SourceRegistryError::DuplicateSourceProperty,
                        "Source property identity and name must be unique" };
                }
                for (unsigned char value : property.sourceName) {
                    if (value < 0x20u || value == 0x7fu) {
                        return { SourceRegistryError::InvalidSourceProperty,
                            "Source property name contains a control character" };
                    }
                }
            }

            uint32_t expectedFrom = 0;
            bool first = true;
            std::unordered_set<uint32_t> fromVersions;
            for (const ComponentMigration& migration : codec.migrations) {
                if (!migration.migrate ||
                    migration.toVersion != migration.fromVersion + 1) {
                    return { SourceRegistryError::InvalidMigrationStep,
                        "Component migration must be a non-null one-version step" };
                }
                if (!fromVersions.insert(migration.fromVersion).second) {
                    return { SourceRegistryError::DuplicateMigrationFromVersion,
                        "Component migration branches from one source version" };
                }
                if (!first && migration.fromVersion != expectedFrom) {
                    return { SourceRegistryError::MigrationGap,
                        "Component migration chain contains a version gap" };
                }
                first = false;
                expectedFrom = migration.toVersion;
            }
            if (!codec.migrations.empty() &&
                codec.migrations.back().toVersion != codec.currentSourceVersion) {
                return { SourceRegistryError::MigrationDoesNotReachCurrent,
                    "Component migration chain does not reach current version" };
            }
            return {};
        }

    } // namespace

    SourceRegistryResult ComponentSerializerRegistry::add(
        SourceComponentCodec codec) {
        if (frozen_) {
            return { SourceRegistryError::Frozen,
                "Source component serializer registry is already frozen" };
        }
        std::ranges::sort(codec.migrations, {}, &ComponentMigration::fromVersion);
        if (const SourceRegistryResult result = validateCodec(codec); !result) {
            return result;
        }
        for (const SourceComponentCodec& existing : codecs_) {
            if (existing.componentId == codec.componentId) {
                return { SourceRegistryError::DuplicateComponentId,
                    "Source component codec ID is already registered" };
            }
            if (existing.sourceOrder == codec.sourceOrder) {
                return { SourceRegistryError::DuplicateSourceOrder,
                    "Source component codec order is already registered" };
            }
        }
        codecs_.push_back(std::move(codec));
        return {};
    }

    SourceRegistryResult ComponentSerializerRegistry::freezeAndValidate(
        const RuntimeComponentRegistry& runtimeRegistry) {
        if (frozen_) return {};
        if (!runtimeRegistry.isFrozen()) {
            return { SourceRegistryError::RuntimeRegistryNotFrozen,
                "Runtime component registry must be frozen first" };
        }
        for (const SourceComponentCodec& codec : codecs_) {
            const RuntimeComponentDescriptor* runtime =
                runtimeRegistry.find(codec.componentId);
            if (!runtime) {
                return { SourceRegistryError::UnknownRuntimeComponent,
                    "Source codec has no matching runtime component" };
            }
            for (const SourcePropertyBinding& property : codec.properties) {
                const auto found = std::ranges::find(
                    runtime->properties, property.propertyId,
                    &PropertyDescriptor::id);
                if (found == runtime->properties.end()) {
                    return { SourceRegistryError::UnknownRuntimeProperty,
                        "Source property has no matching runtime property" };
                }
            }
        }
        for (const RuntimeComponentDescriptor& descriptor :
            runtimeRegistry.descriptors()) {
            const auto found = std::ranges::find(
                codecs_, descriptor.id, &SourceComponentCodec::componentId);
            if (found == codecs_.end()) {
                return { SourceRegistryError::MissingSourceCodec,
                    "Runtime component has no source codec" };
            }
        }
        std::ranges::sort(codecs_,
            [](const SourceComponentCodec& lhs, const SourceComponentCodec& rhs) {
                if (lhs.sourceOrder != rhs.sourceOrder) {
                    return lhs.sourceOrder < rhs.sourceOrder;
                }
                return lhs.componentId < rhs.componentId;
            });
        frozen_ = true;
        return {};
    }

    const SourceComponentCodec* ComponentSerializerRegistry::find(
        const ComponentTypeId& id) const noexcept {
        if (!frozen_) return nullptr;
        const auto found = std::ranges::find(
            codecs_, id, &SourceComponentCodec::componentId);
        return found == codecs_.end() ? nullptr : &*found;
    }

    std::span<const SourceComponentCodec>
    ComponentSerializerRegistry::codecs() const noexcept {
        return frozen_
            ? std::span<const SourceComponentCodec>(codecs_)
            : std::span<const SourceComponentCodec>();
    }

    SourceMigrationResult ComponentSerializerRegistry::migrateToCurrent(
        const ComponentTypeId& id,
        uint32_t sourceVersion,
        const SourceJson& data) const {
        const SourceComponentCodec* codec = find(id);
        if (!codec || sourceVersion > codec->currentSourceVersion) {
            return { { SourceRegistryError::UnsupportedSourceVersion,
                "Component version is not supported by the frozen registry" },
                {}, {}, sourceVersion, codec ? codec->currentSourceVersion : 0 };
        }
        if (sourceVersion == codec->currentSourceVersion) {
            return { {}, data, {}, sourceVersion, codec->currentSourceVersion };
        }

        SourceJson current = data;
        std::vector<SourceMigrationNotice> notices;
        uint32_t version = sourceVersion;
        while (version < codec->currentSourceVersion) {
            const auto found = std::ranges::find(
                codec->migrations, version, &ComponentMigration::fromVersion);
            if (found == codec->migrations.end()) {
                return { { SourceRegistryError::UnsupportedSourceVersion,
                    "No migration exists from the component source version" },
                    {}, {}, sourceVersion, codec->currentSourceVersion };
            }
            SourceJson next;
            std::string error;
            if (!found->migrate(current, next, notices, error)) {
                return { { SourceRegistryError::MigrationFailed,
                    error.empty() ? "Component migration failed" : error },
                    {}, {}, sourceVersion, codec->currentSourceVersion };
            }
            current = std::move(next);
            version = found->toVersion;
        }
        return { {}, std::move(current), std::move(notices), sourceVersion,
            codec->currentSourceVersion };
    }

    std::string_view ComponentSerializerRegistry::sourceName(
        const ComponentTypeId& component,
        const PropertyId& property) const noexcept {
        const SourceComponentCodec* codec = find(component);
        if (!codec) return {};
        const auto found = std::ranges::find(
            codec->properties, property, &SourcePropertyBinding::propertyId);
        return found == codec->properties.end()
            ? std::string_view(property.value())
            : std::string_view(found->sourceName);
    }

} // namespace Iridium
