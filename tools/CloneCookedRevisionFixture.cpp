#include "assets/cooker/CookedArtifact.h"
#include "utils/Sha256.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    using namespace Iridium;
    try {
        if (argc != 4) {
            std::cerr <<
                "Usage: IridiumCloneCookedRevisionFixture INPUT OUTPUT REVISION_LABEL\n";
            return 2;
        }
        const std::filesystem::path inputPath =
            argv[1];
        const std::filesystem::path outputPath =
            argv[2];
        const std::string label = argv[3];
        CookedArtifactBlob source =
            readCookedArtifactBlobFile(
                inputPath);
        CookedArtifactReadResult decoded =
            readCookedArtifact(
                source.bytes,
                source.artifactHash);
        if (!decoded.valid()) {
            throw std::runtime_error(
                "Input artifact is invalid.");
        }
        const std::string oldCookKey =
            decoded.artifact->cookKey;
        decoded.artifact->cookKey =
            sha256(std::as_bytes(
                std::span(label.data(),
                    label.size())));
        CookedArtifactBlob cloned =
            serializeCookedArtifact(
                *decoded.artifact);
        std::filesystem::create_directories(
            outputPath.parent_path());
        std::ofstream output(
            outputPath,
            std::ios::binary |
                std::ios::trunc);
        output.write(
            reinterpret_cast<const char*>(
                cloned.bytes.data()),
            static_cast<std::streamsize>(
                cloned.bytes.size()));
        output.flush();
        if (!output) {
            throw std::runtime_error(
                "Could not write cloned artifact.");
        }
        std::cout <<
            "{\"asset_guid\":\"" <<
            decoded.artifact->assetGuid.toString() <<
            "\",\"old_cook_key\":\"" <<
            oldCookKey <<
            "\",\"new_cook_key\":\"" <<
            decoded.artifact->cookKey <<
            "\",\"artifact_hash\":\"" <<
            cloned.artifactHash <<
            "\",\"bytes\":" <<
            cloned.bytes.size() << "}\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
