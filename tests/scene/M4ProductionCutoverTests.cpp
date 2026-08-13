#include "scene/authoring/CoreSceneComponentAdapters.h"
#include "scene/authoring/CookedSceneCompiler.h"
#include "scene/authoring/SourceSceneDocument.h"
#include "scene/authoring/SourceSceneLoadTransaction.h"
#include "scene/runtime/CookedScene.h"
#include "utils/Sha256.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "check failed: " #condition \
                << " (line " << __LINE__ << ")\n"; \
            return false; \
        } \
    } while (false)

    std::filesystem::path root() {
        return std::filesystem::path(PROJECT_ROOT_DIR);
    }

    std::vector<std::byte> readBytes(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) throw std::runtime_error("could not read " + path.string());
        const auto length = input.tellg();
        input.seekg(0);
        std::vector<std::byte> bytes(static_cast<size_t>(length));
        input.read(reinterpret_cast<char*>(bytes.data()), length);
        return bytes;
    }

    std::string readText(const std::filesystem::path& path) {
        const auto bytes = readBytes(path);
        return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
    }

    bool frozenSourceAndCookedContractsMatch() {
        const auto fixture = root() / "tests" / "scene" / "fixtures" /
            "m4_acceptance_schema1.iridium.scene.json";
        const auto contractPath = root() / "tests" / "scene" / "fixtures" /
            "m4_acceptance_contract.json";
        const nlohmann::json contract = nlohmann::json::parse(
            readText(contractPath));
        const nlohmann::json& current = contract.at("m5_10Supersession");
        bool contractMatches = true;
        const auto compareContract = [&contractMatches, &current](
            std::string_view field, const auto& actual) {
            const auto expected = current.at(std::string(field))
                .template get<std::decay_t<decltype(actual)>>();
            if (actual == expected) return;
            std::cerr << "contract mismatch " << field << ": expected "
                << expected << ", actual " << actual << '\n';
            contractMatches = false;
        };
        const auto sourceBytes = readBytes(fixture);
        CHECK(Iridium::sha256(sourceBytes) ==
            contract.at("sourceSha256").get<std::string>());

        auto registries = Iridium::createCoreSceneRegistryBundle();
        CHECK(registries);
        const std::string manifestHash =
            Iridium::runtimeComponentManifestHash(registries.runtime);
        compareContract("runtimeComponentManifestSha256", manifestHash);
        const std::string sourceText(
            reinterpret_cast<const char*>(sourceBytes.data()),
            sourceBytes.size());
        const auto read = Iridium::readSourceSceneSchema1(
            sourceText, registries.runtime, registries.source);
        CHECK(read);
        const auto canonical = Iridium::writeSourceSceneCanonical(
            *read.document, registries.runtime, registries.source);
        CHECK(canonical);
        const std::string canonicalHash = Iridium::sha256(std::as_bytes(std::span(
            canonical.bytes->data(), canonical.bytes->size())));
        compareContract("canonicalSha256", canonicalHash);
        CHECK(canonical.bytes->find("EntityID") == std::string::npos);
        CHECK(canonical.bytes->find("meshPath") == std::string::npos);
        CHECK(canonical.bytes->find("\"children\"") == std::string::npos);
        CHECK(canonical.bytes->find("\"depth\"") == std::string::npos);

        auto staged = Iridium::stageSourceScene(
            *read.document, registries.runtime, registries.source);
        CHECK(staged);
        const auto compiled = Iridium::compileCookedScene(
            *staged.staging, registries.runtime, registries.source, {
                .sceneAssetGuid = *Iridium::AssetGuid::parse(
                    contract.at("sceneAssetGuid").get<std::string>()),
                .sourceContentHash =
                    contract.at("sourceSha256").get<std::string>(),
                .canonicalContentHash =
                    current.at("canonicalSha256").get<std::string>(),
                .target = {
                    .platform = contract["target"]["platform"].get<std::string>(),
                    .profile = contract["target"]["profile"].get<std::string>(),
                    .qualityPolicy =
                        contract["target"]["qualityPolicy"].get<std::string>(),
                },
            });
        CHECK(compiled);
        compareContract("cookKey", compiled.artifact->cookKey);
        const auto blob = Iridium::serializeCookedArtifact(*compiled.artifact);
        compareContract("artifactSha256", blob.artifactHash);
        compareContract("artifactBytes", blob.bytes.size());
        const auto loaded = Iridium::stageCookedScene(
            blob.bytes, registries.runtime, {
                .expectedSceneAssetGuid = compiled.artifact->assetGuid,
                .expectedTarget = compiled.artifact->target,
                .expectedCookKey = compiled.artifact->cookKey,
                .expectedArtifactHash = blob.artifactHash,
            });
        CHECK(loaded);
        CHECK(loaded.staging->world->registry().aliveCount() ==
            contract.at("entityCount").get<size_t>());
        CHECK(contractMatches);
        return true;
    }

    bool retiredProductionPathsStayRemoved() {
        CHECK(!std::filesystem::exists(
            root() / "src" / "scene" / "SceneSerializer.h"));
        CHECK(!std::filesystem::exists(
            root() / "src" / "scene" / "SceneSerializer.cpp"));
        CHECK(!std::filesystem::exists(
            root() / "tests" / "scene" /
                "SceneSerializerCharacterizationTests.cpp"));
        const std::string cmake = readText(root() / "CMakeLists.txt");
        CHECK(cmake.find("SceneSerializer") == std::string::npos);
        const std::string inspector = readText(root() / "src" / "editor" /
            "panels" / "core" / "InspectorPanel.cpp");
        CHECK(inspector.find("descriptor.add(registry, entity)") ==
            std::string::npos);
        CHECK(inspector.find("descriptor.remove(registry, entity)") ==
            std::string::npos);
        const std::string hierarchy = readText(root() / "src" / "editor" /
            "panels" / "core" / "SceneHierarchyPanel.cpp");
        CHECK(hierarchy.find("namePool->get(entity).name =") ==
            std::string::npos);
        return true;
    }

} // namespace

int main() {
    if (!frozenSourceAndCookedContractsMatch()) return 1;
    std::cout << "[PASS] frozen source and cooked contracts\n";
    if (!retiredProductionPathsStayRemoved()) return 1;
    std::cout << "[PASS] retired production paths remain absent\n";
    return 0;
}
