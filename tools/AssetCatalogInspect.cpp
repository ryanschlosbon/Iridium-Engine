#include "assets/AssetDiscovery.h"
#include "assets/SqliteAssetCatalog.h"

#include <charconv>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

    using namespace Iridium;

    struct Options {
        std::filesystem::path database = "iridium-asset-catalog.sqlite";
        std::vector<AssetRoot> roots;
        AssetCatalogQuery query;
    };

    std::optional<AssetCatalogStatus> parseStatus(std::string_view value) {
        if (value == "ready") return AssetCatalogStatus::Ready;
        if (value == "missing-source") return AssetCatalogStatus::MissingSource;
        if (value == "duplicate-guid") return AssetCatalogStatus::DuplicateGuid;
        return std::nullopt;
    }

    void printUsage() {
        std::cerr
            << "Usage: IridiumAssetCatalogInspect --asset-root [id=]path "
               "[--asset-root ...] [--database path] [--search text] "
               "[--type asset-type] [--status ready|missing-source|duplicate-guid] "
               "[--limit count]\n";
    }

    std::optional<Options> parseOptions(int argc, char** argv) {
        Options options;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (index + 1 >= argc) return std::nullopt;
            const std::string value = argv[++index];
            if (argument == "--asset-root") {
                const size_t separator = value.find('=');
                if (separator == std::string::npos) {
                    const std::filesystem::path path = value;
                    options.roots.push_back({
                        .id = path.filename().generic_string(),
                        .path = path,
                    });
                } else {
                    options.roots.push_back({
                        .id = value.substr(0, separator),
                        .path = value.substr(separator + 1),
                    });
                }
            } else if (argument == "--database") {
                options.database = value;
            } else if (argument == "--search") {
                options.query.text = value;
            } else if (argument == "--type") {
                options.query.assetType = value;
            } else if (argument == "--status") {
                options.query.status = parseStatus(value);
                if (!options.query.status) return std::nullopt;
            } else if (argument == "--limit") {
                uint32_t limit = 0;
                const auto [end, error] = std::from_chars(
                    value.data(), value.data() + value.size(), limit);
                if (error != std::errc() || end != value.data() + value.size()) {
                    return std::nullopt;
                }
                options.query.limit = limit;
            } else {
                return std::nullopt;
            }
        }
        if (options.roots.empty()) return std::nullopt;
        return options;
    }

} // namespace

int main(int argc, char** argv) {
    const auto options = parseOptions(argc, argv);
    if (!options) {
        printUsage();
        return 1;
    }

    try {
        const AssetDiscoveryResult discovery = discoverAssetRoots(options->roots);
        const auto catalog = createSqliteAssetCatalog(options->database);
        catalog->rebuild(
            discovery.records,
            discovery.sourceDirectories);
        const AssetCatalogQueryPage page = catalog->query(options->query);

        nlohmann::ordered_json output;
        output["database"] = options->database.generic_string();
        output["recordCount"] = catalog->recordCount();
        output["totalMatches"] = page.totalMatches
            ? nlohmann::json(*page.totalMatches) : nlohmann::json(nullptr);
        output["diagnostics"] = nlohmann::ordered_json::array();
        for (const AssetDiscoveryDiagnostic& diagnostic : discovery.diagnostics) {
            output["diagnostics"].push_back({
                { "severity", diagnostic.severity == AssetMetadataSeverity::Error
                    ? "error" : "warning" },
                { "code", diagnostic.code },
                { "path", diagnostic.path },
                { "message", diagnostic.message },
            });
        }
        output["records"] = nlohmann::ordered_json::array();
        for (const AssetCatalogRecord& record : page.records) {
            output["records"].push_back({
                { "guid", record.guid.toString() },
                { "parentGuid", record.parentGuid
                    ? nlohmann::json(record.parentGuid->toString()) : nlohmann::json(nullptr) },
                { "assetType", record.assetType },
                { "root", record.assetRoot },
                { "sourcePath", record.sourcePath },
                { "sourceKey", record.sourceKey },
                { "status", assetCatalogStatusName(record.status) },
                { "tags", record.tags },
            });
        }
        std::cout << output.dump(2) << '\n';
        return discovery.hasErrors() ? 2 : 0;
    } catch (const std::exception& exception) {
        std::cerr << "Asset catalog inspection failed: " << exception.what() << '\n';
        return 3;
    }
}
