#include "assets/AssetMetadata.h"
#include "assets/cooker/LocalDerivedDataCache.h"
#include "scene/authoring/CookedSceneCompiler.h"
#include "scene/authoring/CoreSceneComponentAdapters.h"
#include "scene/authoring/SourceSceneDocument.h"
#include "scene/authoring/SourceSceneLoadTransaction.h"
#include "scene/runtime/CookedScene.h"
#include "utils/Sha256.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace {

    using Json = nlohmann::ordered_json;

    struct Options {
        std::filesystem::path source;
        std::filesystem::path metadata;
        std::filesystem::path ddc;
        Iridium::CookTarget target{
            .platform = "windows-x64",
            .profile = "release",
            .qualityPolicy = "high",
        };
        std::map<Iridium::AssetGuid, std::string> dependencyHashes;
    };

    bool canonicalHash(std::string_view value) {
        return value.size() == 64 && std::ranges::all_of(value, [](char field) {
            return (field >= '0' && field <= '9') ||
                (field >= 'a' && field <= 'f');
        });
    }

    std::optional<Options> parseOptions(int argc, char** argv) {
        Options result;
        for (int index = 1; index < argc; ++index) {
            if (index + 1 >= argc) return std::nullopt;
            const std::string argument(argv[index]);
            const std::string value(argv[++index]);
            if (argument == "--source") result.source = value;
            else if (argument == "--metadata") result.metadata = value;
            else if (argument == "--ddc") result.ddc = value;
            else if (argument == "--platform") result.target.platform = value;
            else if (argument == "--profile") result.target.profile = value;
            else if (argument == "--quality") result.target.qualityPolicy = value;
            else if (argument == "--dependency") {
                const size_t separator = value.find('=');
                if (separator == std::string::npos) return std::nullopt;
                const auto guid = Iridium::AssetGuid::parse(
                    std::string_view(value).substr(0, separator));
                const std::string hash = value.substr(separator + 1);
                if (!guid || guid->isNil() || !canonicalHash(hash) ||
                    !result.dependencyHashes.emplace(*guid, hash).second) {
                    return std::nullopt;
                }
            }
            else return std::nullopt;
        }
        if (result.source.empty() || result.ddc.empty()) return std::nullopt;
        result.source = std::filesystem::absolute(result.source);
        result.ddc = std::filesystem::absolute(result.ddc);
        result.metadata = result.metadata.empty()
            ? Iridium::assetMetadataSidecarPath(result.source)
            : std::filesystem::absolute(result.metadata);
        return result;
    }

    std::vector<std::byte> readFile(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) throw std::runtime_error("Could not open scene source");
        const std::streamsize size = input.tellg();
        if (size < 0) throw std::runtime_error("Could not size scene source");
        input.seekg(0, std::ios::beg);
        std::vector<std::byte> bytes(static_cast<size_t>(size));
        if (size != 0 && !input.read(reinterpret_cast<char*>(bytes.data()), size)) {
            throw std::runtime_error("Could not read scene source");
        }
        return bytes;
    }

    Json diagnostics(const std::vector<Iridium::SceneDiagnostic>& values) {
        Json result = Json::array();
        for (const Iridium::SceneDiagnostic& value : values) {
            result.push_back({
                { "severity", value.severity ==
                    Iridium::SceneDiagnosticSeverity::Error ? "error" :
                    value.severity == Iridium::SceneDiagnosticSeverity::Warning
                        ? "warning" : "info" },
                { "code", value.code },
                { "entity", value.entity
                    ? Json(value.entity->toString()) : Json(nullptr) },
                { "component", value.component
                    ? Json(value.component->value()) : Json(nullptr) },
                { "message", value.message },
            });
        }
        return result;
    }

    Json dependenciesJson(std::span<const Iridium::AssetDependency> values) {
        Json result = Json::array();
        for (const Iridium::AssetDependency& value : values) {
            result.push_back({
                { "type", static_cast<uint32_t>(value.type) },
                { "assetGuid", value.assetGuid
                    ? Json(value.assetGuid->toString()) : Json(nullptr) },
                { "artifactHash", value.artifactHash },
            });
        }
        return result;
    }

    Json dependencyRequestJson(
        const std::map<Iridium::AssetGuid, std::string>& values) {
        Json result = Json::array();
        for (const auto& [guid, hash] : values) {
            result.push_back({ { "assetGuid", guid.toString() },
                { "artifactHash", hash } });
        }
        return result;
    }

    std::string identityHash(const Iridium::AssetMetadata& metadata,
        const Options& options, std::string_view manifestHash) {
        const Json identity{
            { "assetGuid", metadata.assetGuid.toString() },
            { "compilerVersion", Iridium::kSceneCompilerImplementationVersion },
            { "containerVersion", options.target.artifactContainerVersion },
            { "dependencies", dependencyRequestJson(options.dependencyHashes) },
            { "featureVersion", Iridium::kSceneCookerFeatureVersion },
            { "manifestHash", manifestHash },
            { "materialSchemaVersion", options.target.materialSchemaVersion },
            { "platform", options.target.platform },
            { "profile", options.target.profile },
            { "quality", options.target.qualityPolicy },
        };
        const std::string bytes = identity.dump();
        return Iridium::sha256(std::as_bytes(std::span(bytes.data(), bytes.size())));
    }

    std::filesystem::path receiptPath(const Options& options,
        const Iridium::AssetMetadata& metadata, std::string_view identity) {
        return options.ddc / "scene-receipts" / metadata.assetGuid.toString() /
            (std::string(identity) + ".json");
    }

    std::optional<Json> readReceipt(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) return std::nullopt;
        try { return Json::parse(input); }
        catch (...) { return std::nullopt; }
    }

    bool atomicReplace(const std::filesystem::path& source,
        const std::filesystem::path& destination) {
#if defined(_WIN32)
        return MoveFileExW(source.c_str(), destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        return std::rename(source.c_str(), destination.c_str()) == 0;
#endif
    }

    void storeReceipt(const std::filesystem::path& path, const Json& receipt) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) throw std::runtime_error("Could not create scene receipt directory");
        const std::filesystem::path temporary = path.string() + "." +
            Iridium::createAssetGuidV7().toString() + ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output << receipt.dump(2) << '\n';
            output.flush();
            if (!output) throw std::runtime_error("Could not write scene receipt");
        }
        if (!atomicReplace(temporary, path)) {
            std::filesystem::remove(temporary, error);
            throw std::runtime_error("Could not publish scene receipt atomically");
        }
    }

    const char* statusName(Iridium::DdcRequestStatus status) {
        switch (status) {
        case Iridium::DdcRequestStatus::CacheHit: return "cache-hit";
        case Iridium::DdcRequestStatus::Built: return "built";
        case Iridium::DdcRequestStatus::Cancelled: return "cancelled";
        case Iridium::DdcRequestStatus::Failed: return "failed";
        }
        return "unknown";
    }

} // namespace

int main(int argc, char** argv) {
    const std::optional<Options> options = parseOptions(argc, argv);
    if (!options) {
        std::cerr << "Usage: IridiumCookScene --source scene.iridium.scene.json "
            "--ddc directory [--metadata path] [--platform value] "
            "[--profile value] [--quality value] "
            "[--dependency asset-guid=artifact-hash ...]\n";
        return 1;
    }
    try {
        const Iridium::AssetMetadataReadResult metadataRead =
            Iridium::readAssetMetadata(options->metadata);
        if (!metadataRead.metadata || metadataRead.hasErrors()) {
            throw std::runtime_error("Scene metadata sidecar is invalid");
        }
        const Iridium::AssetMetadata& metadata = *metadataRead.metadata;
        if (metadata.assetType != "iridium.scene" ||
            metadata.importerId != "iridium.scene" ||
            metadata.importerVersion != 1 ||
            metadata.settingsSchemaVersion != 1 || !metadata.settings.empty()) {
            throw std::runtime_error(
                "Scene metadata must select iridium.scene@1 with empty schema-1 settings");
        }
        auto registries = Iridium::createCoreSceneRegistryBundle();
        if (!registries) throw std::runtime_error(registries.diagnostic);
        const std::string manifestHash =
            Iridium::runtimeComponentManifestHash(registries.runtime);
        const std::string requestIdentity = identityHash(metadata, *options,
            manifestHash);
        const std::filesystem::path receipt = receiptPath(*options, metadata,
            requestIdentity);
        const std::vector<std::byte> sourceBytes = readFile(options->source);
        const std::string sourceHash = Iridium::sha256(sourceBytes);
        Iridium::LocalDerivedDataCache cache(options->ddc);

        if (const std::optional<Json> value = readReceipt(receipt);
            value && value->value("schema", 0u) == 1u &&
            value->value("identityHash", std::string{}) == requestIdentity &&
            value->value("sourceHash", std::string{}) == sourceHash) {
            const std::string cookKey = value->value("cookKey", std::string{});
            const std::string artifactHash = value->value(
                "artifactHash", std::string{});
            Iridium::DdcReadResult cached = cache.read(cookKey);
            if (cached.status == Iridium::DdcLookupStatus::Hit && cached.blob &&
                cached.blob->artifactHash == artifactHash) {
                auto validated = Iridium::stageCookedScene(cached.blob->bytes,
                    registries.runtime, {
                        .expectedSceneAssetGuid = metadata.assetGuid,
                        .expectedTarget = options->target,
                        .expectedCookKey = cookKey,
                        .expectedArtifactHash = artifactHash,
                    });
                if (validated) {
                    std::cout << Json{
                        { "status", "cache-hit" },
                        { "preparation", "receipt-hit" },
                        { "assetGuid", metadata.assetGuid.toString() },
                        { "sourceHash", sourceHash },
                        { "cookKey", cookKey },
                        { "artifactHash", artifactHash },
                        { "artifactPath", cache.entryPath(cookKey).string() },
                        { "sourceParse", false },
                        { "sceneCompile", false },
                    }.dump(2) << '\n';
                    return 0;
                }
            }
        }

        const std::string sourceText(reinterpret_cast<const char*>(
            sourceBytes.data()), sourceBytes.size());
        const auto document = Iridium::readSourceSceneSchema1(sourceText,
            registries.runtime, registries.source);
        if (!document) {
            std::cout << Json{ { "status", "failed" },
                { "diagnostics", diagnostics(document.diagnostics) } }.dump(2)
                << '\n';
            return 2;
        }
        auto staged = Iridium::stageSourceScene(*document.document,
            registries.runtime, registries.source);
        if (!staged) {
            std::cout << Json{ { "status", "failed" },
                { "diagnostics", diagnostics(staged.diagnostics) } }.dump(2)
                << '\n';
            return 2;
        }

        std::map<Iridium::AssetGuid, bool> referenced;
        for (const Iridium::SceneReferenceRecord& reference :
            staged.staging->world->references().records()) {
            if (reference.kind == Iridium::StableReferenceKind::Entity) continue;
            const Iridium::AssetGuid guid(reference.target);
            referenced[guid] = referenced[guid] || reference.required;
        }
        if (referenced.size() != options->dependencyHashes.size()) {
            throw std::runtime_error(
                "Dependency arguments must exactly cover scene asset references");
        }
        std::vector<Iridium::AssetDependency> dependencies;
        dependencies.reserve(referenced.size());
        for (const auto& [guid, required] : referenced) {
            const auto found = options->dependencyHashes.find(guid);
            if (found == options->dependencyHashes.end()) {
                throw std::runtime_error("Scene asset dependency hash is missing");
            }
            dependencies.push_back({
                .type = required ? Iridium::AssetDependencyType::Asset
                    : Iridium::AssetDependencyType::OptionalAsset,
                .assetGuid = guid,
                .artifactHash = found->second,
            });
        }
        const auto canonical = Iridium::writeSourceSceneCanonical(
            *document.document, registries.runtime, registries.source);
        if (!canonical) throw std::runtime_error(
            "Could not produce canonical current-schema scene source");
        const std::string canonicalHash = Iridium::sha256(std::as_bytes(
            std::span(canonical.bytes->data(), canonical.bytes->size())));
        auto compiled = Iridium::compileCookedScene(*staged.staging,
            registries.runtime, registries.source, {
                .sceneAssetGuid = metadata.assetGuid,
                .sourceContentHash = sourceHash,
                .canonicalContentHash = canonicalHash,
                .target = options->target,
                .dependencies = dependencies,
            });
        if (!compiled) {
            std::cout << Json{ { "status", "failed" },
                { "diagnostics", diagnostics(compiled.diagnostics) } }.dump(2)
                << '\n';
            return 2;
        }
        const auto blob = std::make_shared<Iridium::CookedArtifactBlob>(
            Iridium::serializeCookedArtifact(*compiled.artifact));
        const std::string cookKey = compiled.artifact->cookKey;
        Iridium::DdcRequestResult cooked = cache.request(cookKey, {},
            [blob](std::stop_token) { return *blob; }).get();
        if ((cooked.status != Iridium::DdcRequestStatus::Built &&
                cooked.status != Iridium::DdcRequestStatus::CacheHit) ||
            !cooked.blob) {
            throw std::runtime_error("Scene DDC publication failed");
        }
        storeReceipt(receipt, {
            { "schema", 1 },
            { "identityHash", requestIdentity },
            { "sourceHash", sourceHash },
            { "canonicalHash", canonicalHash },
            { "cookKey", cookKey },
            { "artifactHash", cooked.blob->artifactHash },
            { "dependencies", dependenciesJson(dependencies) },
        });
        std::cout << Json{
            { "status", statusName(cooked.status) },
            { "preparation", "source-parse" },
            { "assetGuid", metadata.assetGuid.toString() },
            { "sourceHash", sourceHash },
            { "canonicalHash", canonicalHash },
            { "cookKey", cookKey },
            { "artifactHash", cooked.blob->artifactHash },
            { "artifactPath", cache.entryPath(cookKey).string() },
            { "dependencies", dependenciesJson(dependencies) },
            { "sourceParse", true },
            { "sceneCompile", true },
        }.dump(2) << '\n';
        return 0;
    }
    catch (const std::exception& exception) {
        std::cerr << "Scene cook failed: " << exception.what() << '\n';
        return 3;
    }
}
