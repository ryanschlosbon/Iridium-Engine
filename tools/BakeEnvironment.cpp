#include "assets/cooker/CookKey.h"
#include "assets/cooker/CookedArtifact.h"
#include "assets/environment/EnvironmentConvolution.h"
#include "utils/Sha256.h"

#include <stb_image.h>

#include <chrono>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace {

    using namespace Iridium;

    struct Options {
        std::filesystem::path source;
        std::filesystem::path output;
        AssetGuid sourceGuid;
        AssetGuid environmentGuid;
        EnvironmentConvolutionSettings settings;
    };

    uint32_t parseUnsigned(const std::string& value, const char* name) {
        size_t consumed = 0;
        const unsigned long parsed = std::stoul(value, &consumed);
        if (consumed != value.size() || parsed == 0 || parsed > 16384)
            throw std::invalid_argument(std::string(name) + " is invalid");
        return static_cast<uint32_t>(parsed);
    }

    std::optional<Options> parseOptions(int argc, char** argv) {
        Options result;
        for (int index = 1; index < argc; ++index) {
            if (index + 1 >= argc) return std::nullopt;
            const std::string argument = argv[index];
            const std::string value = argv[++index];
            if (argument == "--source") result.source = value;
            else if (argument == "--output") result.output = value;
            else if (argument == "--source-guid") {
                const auto guid = AssetGuid::parse(value);
                if (!guid) return std::nullopt;
                result.sourceGuid = *guid;
            }
            else if (argument == "--environment-guid") {
                const auto guid = AssetGuid::parse(value);
                if (!guid) return std::nullopt;
                result.environmentGuid = *guid;
            }
            else if (argument == "--radiance-size")
                result.settings.radianceSize = parseUnsigned(value, "radiance size");
            else if (argument == "--irradiance-size")
                result.settings.irradianceSize = parseUnsigned(value, "irradiance size");
            else if (argument == "--prefilter-size")
                result.settings.prefilteredSize = parseUnsigned(value, "prefilter size");
            else if (argument == "--brdf-size")
                result.settings.brdfLutSize = parseUnsigned(value, "BRDF size");
            else if (argument == "--samples") {
                const uint32_t samples = parseUnsigned(value, "sample count");
                result.settings.prefilteredSamples = samples;
                result.settings.brdfSamples = samples;
            }
            else if (argument == "--radiance-scale")
                result.settings.sourceRadianceScale = std::stof(value);
            else return std::nullopt;
        }
        if (result.source.empty() || result.output.empty() ||
            result.sourceGuid.isNil() || result.environmentGuid.isNil())
            return std::nullopt;
        return result;
    }

} // namespace

int main(int argc, char** argv) {
    const auto options = parseOptions(argc, argv);
    if (!options) {
        std::cerr << "Usage: IridiumBakeEnvironment --source image.hdr "
            "--output environment.irartifact --source-guid UUID "
            "--environment-guid UUID [--radiance-size N] "
            "[--irradiance-size N] [--prefilter-size N] [--brdf-size N] "
            "[--samples N] [--radiance-scale X]\n";
        return 1;
    }
    try {
        const auto start = std::chrono::steady_clock::now();
        stbi_set_flip_vertically_on_load(true);
        int width = 0, height = 0, channels = 0;
        std::unique_ptr<float, decltype(&stbi_image_free)> pixels(
            stbi_loadf(options->source.string().c_str(), &width, &height,
                &channels, 4), stbi_image_free);
        stbi_set_flip_vertically_on_load(false);
        if (!pixels || width <= 0 || height <= 0)
            throw std::runtime_error("Could not decode the source HDR image.");
        EnvironmentFloatImage source{
            .width = static_cast<uint32_t>(width),
            .height = static_cast<uint32_t>(height),
            .pixels = std::vector<glm::vec4>(
                static_cast<size_t>(width) * height),
        };
        std::memcpy(source.pixels.data(), pixels.get(),
            source.pixels.size() * sizeof(glm::vec4));
        const ConvolvedEnvironment convolved = convolveEnvironmentReference(
            source, options->settings);
        CookProduct product = makeConvolvedEnvironmentProduct(
            options->sourceGuid, convolved, options->settings,
            "iridium-environment-baker-v3");
        if (hasCookErrors(product.diagnostics))
            throw std::runtime_error("Environment product validation failed.");

        const std::string sourceHash = sha256File(options->source);
        const nlohmann::ordered_json settingsJson{
            { "brdf_size", options->settings.brdfLutSize },
            { "irradiance_convolution", "exact-source-texel-sh9-v1" },
            { "irradiance_size", options->settings.irradianceSize },
            { "prefilter_samples", options->settings.prefilteredSamples },
            { "prefilter_size", options->settings.prefilteredSize },
            { "radiance_scale", options->settings.sourceRadianceScale },
            { "radiance_size", options->settings.radianceSize },
            { "source_primaries", options->settings.sourcePrimaries },
        };
        const std::string settingsText = settingsJson.dump();
        const auto settingsBytes = std::as_bytes(std::span(
            settingsText.data(), settingsText.size()));
        const AssetDependency sourceDependency{
            .type = AssetDependencyType::Asset,
            .assetGuid = options->sourceGuid,
            .location = options->source.generic_string(),
            .contentHash = sourceHash,
            .artifactHash = sourceHash,
        };
        const CookTarget target{
            .platform = "windows-x64",
            .profile = "release",
            .qualityPolicy = "reference",
        };
        const std::array dependencies{ sourceDependency };
        const std::string cookKey = calculateCookKey({
            .assetGuid = options->environmentGuid,
            .importerId = "iridium.environment.hdri",
            .importerImplementationVersion = 3,
            .settingsSchemaVersion = 1,
            .canonicalSettings = settingsBytes,
            .sourceContentHash = sourceHash,
            .dependencies = dependencies,
            .target = target,
            .cookerFeatureVersion = "reflection-resolution-v3",
        });
        const CookedArtifactBlob artifact = serializeCookedArtifact({
            .assetGuid = options->environmentGuid,
            .artifactType = product.artifactType,
            .artifactSchemaVersion = product.artifactSchemaVersion,
            .target = target,
            .cookKey = cookKey,
            .dependencies = { sourceDependency },
            .sections = std::move(product.sections),
        });
        if (!options->output.parent_path().empty())
            std::filesystem::create_directories(options->output.parent_path());
        std::ofstream output(options->output, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(artifact.bytes.data()),
            static_cast<std::streamsize>(artifact.bytes.size()));
        output.flush();
        if (!output) throw std::runtime_error("Could not publish output artifact.");
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        std::cout << nlohmann::ordered_json{
            { "artifactHash", artifact.artifactHash },
            { "bytes", artifact.bytes.size() },
            { "cookKey", cookKey },
            { "seconds", seconds },
        }.dump(2) << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Environment bake failed: " << exception.what() << '\n';
        return 2;
    }
}
