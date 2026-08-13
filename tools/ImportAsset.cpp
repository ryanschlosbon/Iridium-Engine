#include "assets/AssetImport.h"

#include <filesystem>
#include <iostream>

#include <nlohmann/json.hpp>

int main(int argc, char** argv) {
    using namespace Iridium;
    if (argc != 2) {
        std::cerr << "Usage: IridiumImportAsset <source>\n";
        return 1;
    }
    try {
        const ImporterRegistry importers =
            createStandardAssetImporterRegistry();
        const AssetImportResult imported =
            importAssetSource(argv[1], importers);
        std::cout << nlohmann::ordered_json{
            { "status", "imported" },
            { "source", imported.sourcePath.generic_string() },
            { "sidecar", imported.metadataPath.generic_string() },
            { "assetGuid", imported.metadata.assetGuid.toString() },
            { "importer", imported.metadata.importerId },
            { "importerVersion", imported.metadata.importerVersion },
            { "subassets", imported.metadata.subassets.size() },
            { "preserved", imported.preservedSubassets },
            { "created", imported.createdSubassets },
            { "dependencies", imported.dependencyCount },
            { "diagnostics", imported.diagnostics.size() },
        }.dump(2) << '\n';
        return 0;
    }
    catch (const std::exception& exception) {
        std::cerr << "Asset import failed: "
            << exception.what() << '\n';
        return 3;
    }
}
