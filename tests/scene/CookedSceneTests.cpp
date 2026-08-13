#include "assets/cooker/CookedArtifact.h"
#include "assets/cooker/LocalDerivedDataCache.h"
#include "ecs/Registry.h"
#include "scene/authoring/CookedSceneCompiler.h"
#include "scene/runtime/CookedComponentIO.h"
#include "scene/runtime/CookedScene.h"

#include <array>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "  check failed: " #condition \
                << " (line " << __LINE__ << ")\n"; \
            return false; \
        } \
    } while (false)

    struct TestComponent {
        std::string label;
        float value = 0.0f;
    };

    bool resolve(Registry&, Entity, const Iridium::SceneIdentityMap&,
        Iridium::SceneReferenceState&) { return true; }

    bool validate(const Registry& registry, Entity entity) {
        const auto* pool = registry.findPool<TestComponent>();
        return pool && pool->has(entity) &&
            std::isfinite(pool->get(entity).value);
    }

    bool encode(const Registry& registry, Entity entity,
        Iridium::CookedComponentWriter& writer) {
        const auto* pool = registry.findPool<TestComponent>();
        if (!pool || !pool->has(entity)) return false;
        const TestComponent& value = pool->get(entity);
        return writer.writeString(value.label) && writer.writeFloat32(value.value);
    }

    bool decode(Registry& registry, Entity entity,
        Iridium::CookedComponentReader& reader) {
        std::string label;
        float value = 0.0f;
        if (!reader.readString(label) || !reader.readFloat32(value) ||
            !reader.finish()) return false;
        registry.addComponent<TestComponent>(entity,
            TestComponent{ std::move(label), value });
        return true;
    }

    bool serializeSource(const Registry& registry, Entity entity,
        const Iridium::SceneIdentityMap&, Iridium::SourceJson& data,
        std::string&) {
        const auto* pool = registry.findPool<TestComponent>();
        if (!pool || !pool->has(entity)) { data = nullptr; return true; }
        data = { { "label", pool->get(entity).label },
            { "value", pool->get(entity).value } };
        return true;
    }

    bool deserializeSource(Registry& registry, Entity entity,
        const Iridium::SourceJson& data, std::string&) {
        registry.addComponent<TestComponent>(entity, TestComponent{
            data.at("label").get<std::string>(),
            data.at("value").get<float>(),
        });
        return true;
    }

    bool validateSource(const Iridium::SourceJson& data, std::string& error) {
        if (!data.is_object() || !data.contains("label") ||
            !data.contains("value")) {
            error = "test component fields missing";
            return false;
        }
        return true;
    }

    struct Registries {
        Iridium::RuntimeComponentRegistry runtime;
        Iridium::ComponentSerializerRegistry source;
    };

    Registries createRegistries() {
        Registries registries;
        auto componentId = *Iridium::ComponentTypeId::parse(
            "studio.test.component");
        Iridium::RuntimeComponentDescriptor runtime{
            .id = componentId,
            .cookedSectionId = *Iridium::CookedSectionId::parse("TST1"),
            .currentCookedVersion = 1,
            .properties = {
                { .id = *Iridium::PropertyId::parse("label"),
                    .valueType = Iridium::PropertyValueType::String,
                    .serializationOrder = 0, .required = true },
                { .id = *Iridium::PropertyId::parse("value"),
                    .valueType = Iridium::PropertyValueType::Float32,
                    .serializationOrder = 1, .required = true },
            },
            .resolveReferences = resolve,
            .postLoadValidate = validate,
            .encodeCooked = encode,
            .decodeCooked = decode,
        };
        if (!registries.runtime.add(std::move(runtime)) ||
            !registries.runtime.freezeAndValidate()) return registries;
        Iridium::SourceComponentCodec source{
            .componentId = componentId,
            .currentSourceVersion = 1,
            .sourceOrder = 0,
            .properties = {
                { *Iridium::PropertyId::parse("label"), "label" },
                { *Iridium::PropertyId::parse("value"), "value" },
            },
            .serializeSource = serializeSource,
            .deserializeLocal = deserializeSource,
            .validateLocal = validateSource,
        };
        (void)registries.source.add(std::move(source));
        (void)registries.source.freezeAndValidate(registries.runtime);
        return registries;
    }

    Iridium::SceneEntityUuid uuid(uint8_t tail) {
        Iridium::SceneEntityUuid::Bytes bytes{
            0x01, 0x8f, 0x00, 0x00, 0x00, tail, 0x70, 0x00,
            0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, tail,
        };
        return Iridium::SceneEntityUuid(bytes);
    }

    Iridium::AssetGuid assetGuid(uint8_t tail) {
        Iridium::AssetGuid::Bytes bytes{
            0x01, 0x8f, 0x00, 0x00, 0x00, tail, 0x70, 0x00,
            0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, tail,
        };
        return Iridium::AssetGuid(bytes);
    }

    Iridium::SourceSceneDocument document(bool reverse = false) {
        const auto componentId = *Iridium::ComponentTypeId::parse(
            "studio.test.component");
        Iridium::SourceSceneDocument result;
        result.name = "Cooked test";
        for (uint8_t index : { uint8_t{ 1 }, uint8_t{ 2 } }) {
            Iridium::SourceSceneEntity entity;
            entity.uuid = uuid(index);
            entity.components.push_back({
                .id = componentId,
                .version = 1,
                .data = { { "label", index == 1 ? "alpha" : "beta" },
                    { "value", index == 1 ? 1.25f : -3.5f } },
                .known = true,
            });
            result.entities.push_back(std::move(entity));
        }
        if (reverse) std::ranges::reverse(result.entities);
        return result;
    }

    Iridium::CookedSceneCompileInput compileInput() {
        return {
            .sceneAssetGuid = assetGuid(9),
            .sourceContentHash = std::string(64, 'a'),
            .canonicalContentHash = std::string(64, 'b'),
            .target = { .platform = "windows-x64", .profile = "release",
                .qualityPolicy = "high" },
        };
    }

    struct Fixture {
        Registries registries;
        Iridium::SourceSceneStageResult staged;
        Iridium::CookedArtifact artifact;
        Iridium::CookedArtifactBlob blob;
    };

    Fixture fixture() {
        Fixture result;
        result.registries = createRegistries();
        result.staged = Iridium::stageSourceScene(document(),
            result.registries.runtime, result.registries.source);
        if (!result.staged) return result;
        auto compiled = Iridium::compileCookedScene(*result.staged.staging,
            result.registries.runtime, result.registries.source, compileInput());
        if (!compiled) return result;
        result.artifact = *compiled.artifact;
        result.blob = Iridium::serializeCookedArtifact(result.artifact);
        return result;
    }

    bool deterministicCompileAndSemanticLoad() {
        Fixture value = fixture();
        CHECK(value.staged);
        CHECK(!value.blob.bytes.empty());
        auto reversed = Iridium::stageSourceScene(document(true),
            value.registries.runtime, value.registries.source);
        CHECK(reversed);
        auto second = Iridium::compileCookedScene(*reversed.staging,
            value.registries.runtime, value.registries.source, compileInput());
        CHECK(second);
        const auto secondBlob = Iridium::serializeCookedArtifact(*second.artifact);
        CHECK(value.blob.bytes == secondBlob.bytes);

        auto loaded = Iridium::stageCookedScene(value.blob.bytes,
            value.registries.runtime, {
                .expectedSceneAssetGuid = assetGuid(9),
                .expectedTarget = compileInput().target,
            });
        for (const auto& diagnostic : loaded.diagnostics) {
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        }
        CHECK(loaded);
        CHECK(loaded.staging->world->registry().aliveCount() == 2);
        const Entity first = *loaded.staging->world->identities().resolve(uuid(1));
        const TestComponent& component =
            loaded.staging->world->registry().getComponent<TestComponent>(first);
        CHECK(component.label == "alpha");
        CHECK(component.value == 1.25f);
        Iridium::SceneWorld active;
        Iridium::commitStagedCookedScene(active, *loaded.staging);
        CHECK(active.registry().aliveCount() == 2);
        return true;
    }

    bool strictCookRejectsUnknownSource() {
        Registries registries = createRegistries();
        Iridium::SourceSceneDocument source = document();
        source.entities.front().components.front().data["future"] = 42;
        auto staged = Iridium::stageSourceScene(std::move(source),
            registries.runtime, registries.source);
        CHECK(staged);
        auto compiled = Iridium::compileCookedScene(*staged.staging,
            registries.runtime, registries.source, compileInput());
        CHECK(!compiled);
        CHECK(std::ranges::any_of(compiled.diagnostics, [](const auto& diagnostic) {
            return diagnostic.code == "scene.cook.unknown_property";
        }));
        return true;
    }

    bool validatedCorruptionFailsWithoutChangingActiveWorld() {
        Fixture value = fixture();
        CHECK(!value.blob.bytes.empty());
        Iridium::SceneWorld active;
        (void)active.createEntity(uuid(8));
        const size_t priorCount = active.registry().aliveCount();

        auto rawCorruption = value.blob.bytes;
        rawCorruption.back() ^= std::byte{ 1 };
        CHECK(!Iridium::stageCookedScene(rawCorruption,
            value.registries.runtime));

        Iridium::CookedArtifact corrupt = value.artifact;
        auto header = std::ranges::find_if(corrupt.sections, [](const auto& section) {
            return section.id == Iridium::kCookedSceneHeaderSection;
        });
        CHECK(header != corrupt.sections.end());
        header->bytes[72] ^= std::byte{ 1 };
        const auto corruptBlob = Iridium::serializeCookedArtifact(corrupt);
        auto failed = Iridium::stageCookedScene(corruptBlob.bytes,
            value.registries.runtime);
        CHECK(!failed);
        CHECK(active.registry().aliveCount() == priorCount);

        corrupt = value.artifact;
        auto entities = std::ranges::find_if(corrupt.sections, [](const auto& section) {
            return section.id == Iridium::kCookedSceneEntitySection;
        });
        CHECK(entities != corrupt.sections.end());
        // ENT1 header is 8 bytes and each fixed entity record is 32 bytes.
        std::copy_n(entities->bytes.begin() + 8, 16,
            entities->bytes.begin() + 40);
        const auto duplicateBlob = Iridium::serializeCookedArtifact(corrupt);
        CHECK(!Iridium::stageCookedScene(duplicateBlob.bytes,
            value.registries.runtime));

        corrupt = value.artifact;
        auto component = std::ranges::find_if(corrupt.sections,
            [](const auto& section) {
                return section.id == Iridium::cookedSceneSectionId(
                    'T', 'S', 'T', '1');
            });
        CHECK(component != corrupt.sections.end());
        component->schemaVersion = 2;
        CHECK(!Iridium::stageCookedScene(
            Iridium::serializeCookedArtifact(corrupt).bytes,
            value.registries.runtime));

        corrupt = value.artifact;
        component = std::ranges::find_if(corrupt.sections,
            [](const auto& section) {
                return section.id == Iridium::cookedSceneSectionId(
                    'T', 'S', 'T', '1');
            });
        CHECK(component != corrupt.sections.end());
        // First framed record payload size begins at byte 8.
        for (size_t index = 0; index < 4; ++index) {
            component->bytes[8 + index] = static_cast<std::byte>(
                uint32_t{ 0xfffffff0u } >> (index * 8));
        }
        CHECK(!Iridium::stageCookedScene(
            Iridium::serializeCookedArtifact(corrupt).bytes,
            value.registries.runtime));

        corrupt = value.artifact;
        entities = std::ranges::find_if(corrupt.sections,
            [](const auto& section) {
                return section.id == Iridium::kCookedSceneEntitySection;
            });
        CHECK(entities != corrupt.sections.end());
        // First entity parent field follows ENT1's 8-byte header and 16-byte UUID.
        for (size_t index = 0; index < 4; ++index) {
            entities->bytes[24 + index] = std::byte{ 0 };
        }
        CHECK(!Iridium::stageCookedScene(
            Iridium::serializeCookedArtifact(corrupt).bytes,
            value.registries.runtime));
        CHECK(active.registry().aliveCount() == priorCount);
        return true;
    }

    bool warmDdcHitBypassesSceneBuilder() {
        Fixture value = fixture();
        CHECK(!value.blob.bytes.empty());
        const auto root = std::filesystem::temp_directory_path() /
            ("iridium-m4-scene-ddc-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        size_t buildCount = 0;
        Iridium::CookedArtifactBlob warmBlob;
        {
            Iridium::LocalDerivedDataCache cache(root);
            auto first = cache.request(value.artifact.cookKey, {},
                [&](std::stop_token) {
                    ++buildCount;
                    return value.blob;
                }).get();
            CHECK(first.status == Iridium::DdcRequestStatus::Built);
            CHECK(first.blob);
            auto warm = cache.request(value.artifact.cookKey, {},
                [&](std::stop_token) {
                    ++buildCount;
                    return value.blob;
                }).get();
            CHECK(warm.status == Iridium::DdcRequestStatus::CacheHit);
            CHECK(warm.blob);
            warmBlob = std::move(*warm.blob);
        }
        CHECK(buildCount == 1);
        auto loaded = Iridium::stageCookedScene(warmBlob.bytes,
            value.registries.runtime, {
                .expectedCookKey = value.artifact.cookKey,
            });
        CHECK(loaded);
        std::error_code cleanupError;
        std::filesystem::remove_all(root, cleanupError);
        CHECK(!cleanupError);
        return true;
    }

    struct TestCase { const char* name; bool (*run)(); };

    int emitFixture(const std::filesystem::path& path, bool reverse) {
        Registries registries = createRegistries();
        auto staged = Iridium::stageSourceScene(document(reverse),
            registries.runtime, registries.source);
        if (!staged) return 2;
        auto compiled = Iridium::compileCookedScene(*staged.staging,
            registries.runtime, registries.source, compileInput());
        if (!compiled) return 3;
        const auto blob = Iridium::serializeCookedArtifact(*compiled.artifact);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) return 4;
        output.write(reinterpret_cast<const char*>(blob.bytes.data()),
            static_cast<std::streamsize>(blob.bytes.size()));
        return output ? 0 : 5;
    }

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--emit") {
        return emitFixture(argv[2], false);
    }
    if (argc == 3 && std::string_view(argv[1]) == "--emit-reversed") {
        return emitFixture(argv[2], true);
    }
    const std::array tests{
        TestCase{ "deterministic semantic load", deterministicCompileAndSemanticLoad },
        TestCase{ "unknown source rejection", strictCookRejectsUnknownSource },
        TestCase{ "corruption retention", validatedCorruptionFailsWithoutChangingActiveWorld },
        TestCase{ "warm DDC source-free hit", warmDdcHitBypassesSceneBuilder },
    };
    size_t passed = 0;
    for (const TestCase& test : tests) {
        if (!test.run()) {
            std::cerr << "[FAIL] " << test.name << '\n';
            return 1;
        }
        ++passed;
        std::cout << "[PASS] " << test.name << '\n';
    }
    std::cout << passed << '/' << tests.size() << " tests passed\n";
    return 0;
}
