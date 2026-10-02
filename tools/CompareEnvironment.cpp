#include "assets/cooker/CookedArtifact.h"
#include "assets/environment/EnvironmentProduct.h"
#include "renderer/color/SceneColor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

    using namespace Iridium;

    CookedArtifact load(const std::filesystem::path& path) {
        const CookedArtifactBlob blob = readCookedArtifactBlobFile(path);
        const CookedArtifactReadResult decoded =
            readCookedArtifact(blob.bytes, blob.artifactHash);
        if (!decoded.valid()) throw std::runtime_error("Invalid environment artifact.");
        return *decoded.artifact;
    }

    nlohmann::ordered_json compare(std::span<const std::byte> candidate,
        std::span<const std::byte> reference, uint32_t storedChannels,
        uint32_t comparedChannels) {
        if (candidate.size() != reference.size() || candidate.size() %
            (storedChannels * sizeof(uint16_t)) != 0)
            throw std::runtime_error("Environment payload layouts differ.");
        const size_t texels = candidate.size() /
            (storedChannels * sizeof(uint16_t));
        double sum = 0.0, squared = 0.0, referenceSquared = 0.0;
        double maximum = 0.0, relativeMaximum = 0.0;
        std::vector<float> absolute;
        std::vector<float> relative;
        absolute.reserve(texels * comparedChannels);
        relative.reserve(texels * comparedChannels);
        for (size_t texel = 0; texel < texels; ++texel) {
            for (uint32_t channel = 0; channel < comparedChannels; ++channel) {
                const size_t offset = (texel * storedChannels + channel) *
                    sizeof(uint16_t);
                uint16_t ah = 0, bh = 0;
                std::memcpy(&ah, candidate.data() + offset, sizeof(ah));
                std::memcpy(&bh, reference.data() + offset, sizeof(bh));
                const float av = Color::halfToFloat(ah);
                const float bv = Color::halfToFloat(bh);
                if (!std::isfinite(av) || !std::isfinite(bv))
                    throw std::runtime_error("Environment payload contains nonfinite data.");
                const double difference = std::abs(static_cast<double>(av) - bv);
                sum += difference;
                squared += difference * difference;
                referenceSquared += static_cast<double>(bv) * bv;
                maximum = (std::max)(maximum, difference);
                absolute.push_back(static_cast<float>(difference));
                if (std::abs(bv) >= 1.0e-4f) {
                    const float error = static_cast<float>(difference / std::abs(bv));
                    relative.push_back(error);
                    relativeMaximum = (std::max)(relativeMaximum,
                        static_cast<double>(error));
                }
            }
        }
        std::sort(absolute.begin(), absolute.end());
        std::sort(relative.begin(), relative.end());
        const size_t count = texels * comparedChannels;
        const double rmse = std::sqrt(squared / count);
        const double referenceRms = std::sqrt(referenceSquared / count);
        const double absoluteP99 = absolute.empty() ? 0.0 : absolute[
            (std::min)(absolute.size() - 1,
                static_cast<size_t>(absolute.size() * 0.99))];
        const double p99 = relative.empty() ? 0.0 : relative[
            (std::min)(relative.size() - 1,
                static_cast<size_t>(relative.size() * 0.99))];
        return {
            { "finite", true },
            { "absolute_p99", absoluteP99 },
            { "max_absolute", maximum },
            { "mean_absolute", sum / count },
            { "normalized_rmse", referenceRms > 0.0 ? rmse / referenceRms : 0.0 },
            { "reference_rms", referenceRms },
            { "relative_max_above_1e_4", relativeMaximum },
            { "relative_p99", p99 },
            { "rmse", rmse },
            { "samples", count },
        };
    }

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: IridiumCompareEnvironment candidate reference\n";
        return 1;
    }
    try {
        const CookedEnvironmentReadResult candidate =
            readCookedEnvironmentProduct(load(argv[1]));
        const CookedEnvironmentReadResult reference =
            readCookedEnvironmentProduct(load(argv[2]));
        if (!candidate.valid() || !reference.valid() ||
            candidate.data->manifest.radiance != reference.data->manifest.radiance ||
            candidate.data->manifest.irradiance != reference.data->manifest.irradiance ||
            candidate.data->manifest.prefilteredSpecular !=
                reference.data->manifest.prefilteredSpecular ||
            candidate.data->manifest.brdfLut != reference.data->manifest.brdfLut)
            throw std::runtime_error("Environment product layouts are incompatible.");
        std::cout << nlohmann::ordered_json{
            { "radiance", compare(candidate.data->radiance,
                reference.data->radiance, 4, 3) },
            { "irradiance", compare(candidate.data->irradiance,
                reference.data->irradiance, 4, 3) },
            { "prefiltered_specular", compare(
                candidate.data->prefilteredSpecular,
                reference.data->prefilteredSpecular, 4, 3) },
            { "brdf_lut", compare(candidate.data->brdfLut,
                reference.data->brdfLut, 2, 2) },
        }.dump(2) << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Environment comparison failed: " << exception.what() << '\n';
        return 2;
    }
}
