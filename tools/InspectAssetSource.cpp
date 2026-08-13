#include "assets/cooker/ImporterRegistry.h"
#include "assets/cooker/TextFixtureImporter.h"
#include "assets/environment/EnvironmentImporter.h"
#include "assets/model/GltfModelImporter.h"
#include "assets/texture/TextureImporter.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

    using namespace Iridium;

    std::vector<std::byte> readFile(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) throw std::runtime_error(
            "Could not open source: " + path.generic_string());
        const std::streamsize size = input.tellg();
        if (size < 0) throw std::runtime_error(
            "Could not size source: " + path.generic_string());
        input.seekg(0, std::ios::beg);
        std::vector<std::byte> result(static_cast<size_t>(size));
        if (size != 0 &&
            !input.read(reinterpret_cast<char*>(result.data()), size)) {
            throw std::runtime_error(
                "Could not read source: " + path.generic_string());
        }
        return result;
    }

    nlohmann::ordered_json diagnosticJson(
        const CookDiagnostic& diagnostic) {
        return {
            { "severity", diagnostic.severity ==
                CookDiagnosticSeverity::Error ? "error" :
                diagnostic.severity == CookDiagnosticSeverity::Warning
                    ? "warning" : "info" },
            { "code", diagnostic.code },
            { "field", diagnostic.field },
            { "message", diagnostic.message },
        };
    }

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: IridiumInspectAssetSource <source>\n";
        return 1;
    }
    try {
        const std::filesystem::path source =
            std::filesystem::absolute(argv[1]);
        const std::vector<std::byte> bytes = readFile(source);
        ImporterRegistry registry;
        registry.registerImporter(std::make_shared<TextFixtureImporter>());
        registry.registerImporter(std::make_shared<TextureImporter>());
        registry.registerImporter(std::make_shared<EnvironmentImporter>());
        registry.registerImporter(std::make_shared<GltfModelImporter>());
        const ImporterSelection selection =
            registry.selectAutomatic(source.filename(), bytes);
        if (!selection.valid()) {
            nlohmann::ordered_json output{
                { "status", "failed" },
                { "diagnostics", nlohmann::ordered_json::array() },
            };
            for (const CookDiagnostic& diagnostic : selection.diagnostics) {
                output["diagnostics"].push_back(
                    diagnosticJson(diagnostic));
            }
            std::cout << output.dump(2) << '\n';
            return 2;
        }
        const NormalizedImportSettings settings =
            selection.importer->normalizeSettings(
                selection.importer->descriptor()
                    .currentSettingsSchemaVersion,
                nlohmann::json::object(), true);
        const ParsedSourceAsset parsed = selection.importer->parse({
            .relativePath = source.filename(),
            .resolvedPath = source,
            .bytes = bytes,
        }, settings);
        nlohmann::ordered_json output{
            { "status", parsed.diagnostics.empty() ? "ok" :
                hasCookErrors(parsed.diagnostics) ? "failed" :
                    "warning" },
            { "importer", selection.importer->descriptor().id },
            { "importerVersion",
                selection.importer->descriptor().implementationVersion },
            { "parsedBytes", parsed.documentBytes.size() },
            { "subassetPayloadBytes",
                std::accumulate(
                    parsed.subassetPayloads.begin(),
                    parsed.subassetPayloads.end(),
                    uint64_t{ 0 },
                    [](uint64_t total,
                        const ParsedSourceAsset::SubassetPayload&
                            payload) {
                        return total + payload.bytes.size();
                    }) },
            { "subassetParsedBytes",
                std::accumulate(
                    parsed.subassetPayloads.begin(),
                    parsed.subassetPayloads.end(),
                    uint64_t{ 0 },
                    [](uint64_t total,
                        const ParsedSourceAsset::SubassetPayload&
                            payload) {
                        return total +
                            payload.parsedBytes.size();
                    }) },
            { "dependencies", nlohmann::ordered_json::array() },
            { "subassets", nlohmann::ordered_json::array() },
            { "diagnostics", nlohmann::ordered_json::array() },
        };
        for (const AssetDependency& dependency : parsed.dependencies) {
            output["dependencies"].push_back({
                { "type", static_cast<uint32_t>(dependency.type) },
                { "location", dependency.location },
                { "contentHash", dependency.contentHash },
            });
        }
        for (const DiscoveredSubasset& subasset :
            parsed.discoveredSubassets) {
            output["subassets"].push_back({
                { "assetType", subasset.assetType },
                { "sourceKey", subasset.sourceKey },
                { "structuralFingerprint",
                    subasset.structuralFingerprint },
            });
        }
        for (const CookDiagnostic& diagnostic : parsed.diagnostics) {
            output["diagnostics"].push_back(diagnosticJson(diagnostic));
        }
        std::cout << output.dump(2) << '\n';
        return hasCookErrors(parsed.diagnostics) ? 2 : 0;
    } catch (const std::exception& exception) {
        std::cerr << "Asset inspection failed: " << exception.what() << '\n';
        return 3;
    }
}
