#include "assets/cooker/CookedArtifact.h"
#include "ecs/Registry.h"
#include "scene/runtime/CookedComponentIO.h"
#include "scene/runtime/CookedScene.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace {

    struct BoundaryComponent {
        int32_t value = 0;
    };

    bool resolve(Registry&, Entity, const Iridium::SceneIdentityMap&,
        Iridium::SceneReferenceState&) { return true; }

    bool validate(const Registry& registry, Entity entity) {
        const auto* pool = registry.findPool<BoundaryComponent>();
        return pool && pool->has(entity);
    }

    bool encode(const Registry&, Entity, Iridium::CookedComponentWriter&) {
        return false;
    }

    bool decode(Registry& registry, Entity entity,
        Iridium::CookedComponentReader& reader) {
        int32_t value = 0;
        if (!reader.readInt32(value) || !reader.finish()) return false;
        registry.addComponent<BoundaryComponent>(entity,
            BoundaryComponent{ value });
        return true;
    }

    template <typename Integer>
    void append(std::vector<std::byte>& bytes, Integer value) {
        using Unsigned = std::make_unsigned_t<Integer>;
        const Unsigned bits = static_cast<Unsigned>(value);
        for (size_t index = 0; index < sizeof(Integer); ++index) {
            bytes.push_back(static_cast<std::byte>(bits >> (index * 8)));
        }
    }

    void appendHash(std::vector<std::byte>& bytes, std::string_view hash) {
        const auto nibble = [](char value) -> uint8_t {
            return value <= '9' ? static_cast<uint8_t>(value - '0')
                : static_cast<uint8_t>(value - 'a' + 10);
        };
        for (size_t index = 0; index < 32; ++index) {
            bytes.push_back(static_cast<std::byte>(
                (nibble(hash[index * 2]) << 4) | nibble(hash[index * 2 + 1])));
        }
    }

    Iridium::SceneEntityUuid entityUuid() {
        return Iridium::SceneEntityUuid({
            0x01, 0x8f, 0, 0, 0, 1, 0x70, 0,
            0x80, 0, 0, 0, 0, 0, 0, 1,
        });
    }

    Iridium::AssetGuid sceneGuid() {
        return Iridium::AssetGuid({
            0x01, 0x8f, 0, 0, 0, 2, 0x70, 0,
            0x80, 0, 0, 0, 0, 0, 0, 2,
        });
    }

    Iridium::RuntimeComponentRegistry registry() {
        Iridium::RuntimeComponentRegistry result;
        (void)result.add({
            .id = *Iridium::ComponentTypeId::parse("studio.boundary.component"),
            .cookedSectionId = *Iridium::CookedSectionId::parse("BND1"),
            .currentCookedVersion = 1,
            .properties = {
                { .id = *Iridium::PropertyId::parse("value"),
                    .valueType = Iridium::PropertyValueType::Int32,
                    .serializationOrder = 0, .required = true },
            },
            .resolveReferences = resolve,
            .postLoadValidate = validate,
            .encodeCooked = encode,
            .decodeCooked = decode,
        });
        (void)result.freezeAndValidate();
        return result;
    }

    Iridium::CookedArtifactBlob fixture(
        const Iridium::RuntimeComponentRegistry& runtime) {
        const std::string componentId = "studio.boundary.component";
        std::vector<std::byte> strings;
        append<uint32_t>(strings, 1);
        append<uint32_t>(strings, static_cast<uint32_t>(componentId.size()));
        strings.insert(strings.end(),
            reinterpret_cast<const std::byte*>(componentId.data()),
            reinterpret_cast<const std::byte*>(componentId.data() +
                componentId.size()));

        std::vector<std::byte> entities;
        append<uint32_t>(entities, 1);
        append<uint32_t>(entities, 1);
        for (uint8_t value : entityUuid().bytes()) {
            entities.push_back(static_cast<std::byte>(value));
        }
        append<uint32_t>(entities, Iridium::kNullCookedSceneIndex);
        append<int32_t>(entities, 0);
        append<uint32_t>(entities, 0);
        append<uint32_t>(entities, 1);
        append<uint32_t>(entities, 0);
        append<uint32_t>(entities, 0);

        std::vector<std::byte> component;
        append<uint32_t>(component, 1);
        append<uint32_t>(component, 0);
        append<uint32_t>(component, 4);
        append<int32_t>(component, 73);

        constexpr uint32_t headerSize = 112;
        constexpr uint32_t directorySize = 16;
        const uint64_t decodedSize = headerSize + directorySize +
            strings.size() + entities.size() + component.size();
        std::vector<std::byte> header;
        append(header, Iridium::kCookedSceneHeaderSection);
        append(header, Iridium::kRuntimeSceneSchemaVersion);
        append<uint32_t>(header, 0x01020304u);
        append(header, headerSize);
        for (uint8_t value : sceneGuid().bytes()) {
            header.push_back(static_cast<std::byte>(value));
        }
        append(header, decodedSize);
        append<uint32_t>(header, 1);
        append<uint32_t>(header, 1);
        append<uint32_t>(header, 1);
        append<uint32_t>(header, 0);
        append<uint64_t>(header, headerSize);
        append<uint64_t>(header, directorySize);
        appendHash(header, Iridium::runtimeComponentManifestHash(runtime));
        append<uint64_t>(header, 0);
        append<uint32_t>(header, 0);
        append<uint32_t>(header,
            Iridium::cookedSceneSectionId('B', 'N', 'D', '1'));
        append<uint32_t>(header, 1);
        append<uint32_t>(header, 1);

        Iridium::CookedArtifact artifact{
            .assetGuid = sceneGuid(),
            .artifactType = std::string(Iridium::kRuntimeSceneArtifactType),
            .artifactSchemaVersion = Iridium::kRuntimeSceneSchemaVersion,
            .target = { .platform = "windows-x64", .profile = "release" },
            .cookKey = std::string(64, 'c'),
            .sections = {
                { Iridium::kCookedSceneHeaderSection, 1, 8, std::move(header) },
                { Iridium::kCookedSceneStringSection, 1, 8, std::move(strings) },
                { Iridium::kCookedSceneEntitySection, 1, 8, std::move(entities) },
                { Iridium::cookedSceneSectionId('B', 'N', 'D', '1'), 1, 8,
                    std::move(component) },
            },
        };
        return Iridium::serializeCookedArtifact(artifact);
    }

} // namespace

int main() {
    Iridium::RuntimeComponentRegistry runtime = registry();
    if (!runtime.isFrozen()) return 1;
    const Iridium::CookedArtifactBlob blob = fixture(runtime);
    auto loaded = Iridium::stageCookedScene(blob.bytes, runtime, {
        .expectedSceneAssetGuid = sceneGuid(),
    });
    if (!loaded) {
        for (const auto& diagnostic : loaded.diagnostics) {
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        }
        return 1;
    }
    const Entity entity = *loaded.staging->world->identities().resolve(entityUuid());
    if (loaded.staging->world->registry()
            .getComponent<BoundaryComponent>(entity).value != 73) return 1;
    std::cout << "runtime-only cooked scene load passed\n";
    return 0;
}
