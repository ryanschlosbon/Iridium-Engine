#include "capture/ImageComparison.h"
#include "capture/TgaImage.h"
#include "utils/Sha256.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace {

    using namespace Iridium;

    struct CommandLineOptions {
        std::filesystem::path reference;
        std::filesystem::path candidate;
        std::filesystem::path report;
        std::optional<std::filesystem::path> heatmap;
        std::optional<std::filesystem::path> referenceMetadata;
        std::optional<std::filesystem::path> candidateMetadata;
        ImageComparisonOptions comparison{};
        ImageComparisonThresholds thresholds{};
        double heatmapScale = 4.0;
        bool allowMissingMetadata = false;
        bool showHelp = false;
    };

    struct MetadataCompatibility {
        bool checked = false;
        std::filesystem::path referencePath;
        std::filesystem::path candidatePath;
        nlohmann::ordered_json referenceContext;
        nlohmann::ordered_json candidateContext;
        std::vector<std::string> checkedFields;
    };

    void printUsage(std::ostream& output) {
        output <<
            "Usage: IridiumImageCompare --reference FILE --candidate FILE "
            "--report FILE [options]\n"
            "\n"
            "Required:\n"
            "  --reference FILE                    Canonical BGRA8 TGA baseline\n"
            "  --candidate FILE                    Canonical BGRA8 TGA candidate\n"
            "  --report FILE                       JSON comparison report\n"
            "\n"
            "Thresholds (strict equality defaults):\n"
            "  --max-abs-code N                    Maximum RGBA code difference [0,255]\n"
            "  --max-changed-pixel-fraction F      Allowed changed RGB pixel fraction [0,1]\n"
            "  --min-luma-ssim F                   Required mean luma SSIM [-1,1]\n"
            "  --changed-pixel-code-threshold N    RGB code delta defining changed pixels\n"
            "\n"
            "Artifacts:\n"
            "  --heatmap FILE                      Optional grayscale difference TGA\n"
            "  --heatmap-scale F                   Code-difference visualization scale (4)\n"
            "  --reference-metadata FILE           Override the reference .json sidecar\n"
            "  --candidate-metadata FILE           Override the candidate .json sidecar\n"
            "  --allow-missing-metadata            Permit image-only comparison when both sidecars are absent\n"
            "  --help                              Show this help\n";
    }

    [[nodiscard]] uint8_t parseCodeValue(std::string_view text,
        std::string_view option) {
        uint32_t value = 0;
        const auto result = std::from_chars(text.data(),
            text.data() + text.size(), value);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
            value > 255) {
            throw std::invalid_argument(std::string(option) +
                " requires an integer in [0, 255].");
        }
        return static_cast<uint8_t>(value);
    }

    [[nodiscard]] double parseFiniteDouble(std::string_view text,
        std::string_view option) {
        double value = 0.0;
        const auto result = std::from_chars(text.data(),
            text.data() + text.size(), value);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
            !std::isfinite(value)) {
            throw std::invalid_argument(std::string(option) +
                " requires a finite number.");
        }
        return value;
    }

    [[nodiscard]] CommandLineOptions parseCommandLine(int argc, char** argv) {
        CommandLineOptions options{};
        std::unordered_set<std::string> seen;
        for (int argument = 1; argument < argc; ++argument) {
            const std::string option = argv[argument];
            if (option == "--help" || option == "-h") {
                options.showHelp = true;
                continue;
            }
            if (!seen.insert(option).second) {
                throw std::invalid_argument("Duplicate option: " + option);
            }
            if (option == "--allow-missing-metadata") {
                options.allowMissingMetadata = true;
                continue;
            }
            if (argument + 1 >= argc) {
                throw std::invalid_argument("Missing value for option: " + option);
            }
            const std::string_view value = argv[++argument];
            if (option == "--reference") options.reference = value;
            else if (option == "--candidate") options.candidate = value;
            else if (option == "--report") options.report = value;
            else if (option == "--heatmap") options.heatmap = value;
            else if (option == "--reference-metadata") {
                options.referenceMetadata = value;
            }
            else if (option == "--candidate-metadata") {
                options.candidateMetadata = value;
            }
            else if (option == "--max-abs-code") {
                options.thresholds.maximumAbsoluteErrorCode =
                    parseCodeValue(value, option);
            }
            else if (option == "--max-changed-pixel-fraction") {
                options.thresholds.maximumChangedPixelFraction =
                    parseFiniteDouble(value, option);
            }
            else if (option == "--min-luma-ssim") {
                options.thresholds.minimumMeanLumaSsim =
                    parseFiniteDouble(value, option);
            }
            else if (option == "--changed-pixel-code-threshold") {
                options.comparison.changedPixelCodeThreshold =
                    parseCodeValue(value, option);
            }
            else if (option == "--heatmap-scale") {
                options.heatmapScale = parseFiniteDouble(value, option);
            }
            else {
                throw std::invalid_argument("Unknown option: " + option);
            }
        }

        if (options.showHelp) return options;
        if (options.reference.empty()) {
            throw std::invalid_argument("--reference is required.");
        }
        if (options.candidate.empty()) {
            throw std::invalid_argument("--candidate is required.");
        }
        if (options.report.empty() || options.report.filename().empty()) {
            throw std::invalid_argument("--report must name an output file.");
        }
        if (options.thresholds.maximumChangedPixelFraction < 0.0 ||
            options.thresholds.maximumChangedPixelFraction > 1.0) {
            throw std::invalid_argument(
                "--max-changed-pixel-fraction must be in [0, 1].");
        }
        if (options.thresholds.minimumMeanLumaSsim < -1.0 ||
            options.thresholds.minimumMeanLumaSsim > 1.0) {
            throw std::invalid_argument("--min-luma-ssim must be in [-1, 1].");
        }
        if (options.heatmapScale < 0.0) {
            throw std::invalid_argument("--heatmap-scale must be nonnegative.");
        }
        if (options.heatmap && options.heatmap->filename().empty()) {
            throw std::invalid_argument("--heatmap must name an output file.");
        }
        if (options.referenceMetadata.has_value() !=
            options.candidateMetadata.has_value()) {
            throw std::invalid_argument(
                "Reference and candidate metadata overrides must be specified together.");
        }
        return options;
    }

    [[nodiscard]] nlohmann::ordered_json readMetadata(
        const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Failed to open capture metadata: " +
                path.generic_string());
        }
        nlohmann::ordered_json document;
        try {
            input >> document;
        }
        catch (const nlohmann::json::exception& error) {
            throw std::runtime_error("Invalid capture metadata '" +
                path.generic_string() + "': " + error.what());
        }
        if (document.value("schema", std::string{}) != "iridium.frame_capture" ||
            document.value("schema_version", 0) != 1) {
            throw std::runtime_error("Unsupported capture metadata schema: " +
                path.generic_string());
        }
        return document;
    }

    [[nodiscard]] nlohmann::ordered_json comparisonContext(
        const nlohmann::ordered_json& document) {
        return {
            { "source", document.at("source") },
            { "run", document.at("run") },
            { "benchmark", document.at("benchmark") },
            { "debug_view", document.at("debug_view") },
            { "capture", document.at("capture") },
            { "image", document.at("image") },
        };
    }

    void validateImageHash(const nlohmann::ordered_json& document,
        const std::filesystem::path& imagePath, std::string_view role) {
        std::string expectedSha256;
        try {
            expectedSha256 = document.at(
                nlohmann::json::json_pointer("/image/sha256")).get<std::string>();
        }
        catch (const nlohmann::json::exception& error) {
            throw std::runtime_error("Capture metadata is missing a valid image SHA-256 for " +
                std::string(role) + ": " + error.what());
        }
        const std::string actualSha256 = sha256File(imagePath);
        if (expectedSha256 != actualSha256) {
            throw std::runtime_error("Capture metadata image SHA-256 does not match the " +
                std::string(role) + " image.");
        }
    }

    [[nodiscard]] MetadataCompatibility validateMetadataCompatibility(
        const CommandLineOptions& options) {
        MetadataCompatibility result{};
        result.referencePath = options.referenceMetadata.value_or(
            std::filesystem::path(options.reference).replace_extension(".json"));
        result.candidatePath = options.candidateMetadata.value_or(
            std::filesystem::path(options.candidate).replace_extension(".json"));
        const bool referenceExists = std::filesystem::exists(result.referencePath);
        const bool candidateExists = std::filesystem::exists(result.candidatePath);
        if (!referenceExists || !candidateExists) {
            if (options.allowMissingMetadata && !referenceExists && !candidateExists) {
                return result;
            }
            throw std::runtime_error(
                "Both capture metadata sidecars are required for threshold evaluation. "
                "Use --allow-missing-metadata only for an explicit image-only comparison.");
        }

        const nlohmann::ordered_json reference = readMetadata(result.referencePath);
        const nlohmann::ordered_json candidate = readMetadata(result.candidatePath);
        validateImageHash(reference, options.reference, "reference");
        validateImageHash(candidate, options.candidate, "candidate");
        constexpr std::string_view fields[] = {
            "/image/encoding",
            "/image/source_pixel_format",
            "/image/width",
            "/image/height",
            "/capture/point",
            "/capture/color_domain",
            "/capture/transfer",
            "/capture/primaries",
            "/capture/range",
            "/capture/output_operator",
            "/benchmark/fixture_id",
            "/benchmark/fixture_revision",
            "/benchmark/camera_id",
            "/benchmark/manifest_sha256",
            "/benchmark/content_hashes",
            "/run/measured_frame_index",
            "/run/benchmark_state_frame_index",
            "/debug_view/name",
            "/debug_view/semantics",
        };
        for (const std::string_view field : fields) {
            const nlohmann::json::json_pointer pointer{ std::string(field) };
            nlohmann::ordered_json referenceValue;
            nlohmann::ordered_json candidateValue;
            try {
                referenceValue = reference.at(pointer);
                candidateValue = candidate.at(pointer);
            }
            catch (const nlohmann::json::exception& error) {
                throw std::runtime_error("Capture metadata is missing required field '" +
                    std::string(field) + "': " + error.what());
            }
            if (referenceValue != candidateValue) {
                throw std::runtime_error("Capture metadata mismatch at '" +
                    std::string(field) + "'.");
            }
            result.checkedFields.emplace_back(field);
        }
        result.checked = true;
        result.referenceContext = comparisonContext(reference);
        result.candidateContext = comparisonContext(candidate);
        return result;
    }

    [[nodiscard]] nlohmann::ordered_json channelJson(
        const ImageChannelMetrics& metrics) {
        return {
            { "mae_code", metrics.meanAbsoluteErrorCode },
            { "rmse_code", metrics.rootMeanSquareErrorCode },
            { "max_abs_code",
                static_cast<uint32_t>(metrics.maximumAbsoluteErrorCode) },
        };
    }

    [[nodiscard]] nlohmann::ordered_json makeReport(
        const CommandLineOptions& options,
        const ImageComparisonResult& result,
        const MetadataCompatibility& metadata) {
        const ImageComparisonMetrics& metrics = result.metrics;
        nlohmann::ordered_json document{
            { "schema", "iridium.image_comparison.v1" },
            { "reference", options.reference.generic_string() },
            { "candidate", options.candidate.generic_string() },
            { "width", metrics.width },
            { "height", metrics.height },
            { "pixel_count", metrics.pixelCount },
            { "settings", {
                { "changed_pixel_code_threshold",
                    static_cast<uint32_t>(metrics.changedPixelCodeThreshold) },
                { "ssim_window_size", metrics.lumaSsim.windowSize },
                { "ssim_color_domain", "srgb_decoded_linear_rec709_luma" },
                { "ssim_window_layout", "anchored_non_overlapping_with_partial_edges" },
                { "heatmap_scale", options.heatmapScale },
            } },
            { "thresholds", {
                { "max_abs_code", static_cast<uint32_t>(
                    result.thresholds.maximumAbsoluteErrorCode) },
                { "max_changed_pixel_fraction",
                    result.thresholds.maximumChangedPixelFraction },
                { "min_mean_luma_ssim",
                    result.thresholds.minimumMeanLumaSsim },
            } },
            { "metrics", {
                { "rgba", {
                    { "r", channelJson(metrics.rgba[0]) },
                    { "g", channelJson(metrics.rgba[1]) },
                    { "b", channelJson(metrics.rgba[2]) },
                    { "a", channelJson(metrics.rgba[3]) },
                } },
                { "max_abs_code", static_cast<uint32_t>(
                    metrics.maximumAbsoluteErrorCode) },
                { "max_rgb_pixel_diff_p95_code",
                    metrics.maximumRgbPixelDifferencePercentile95Code },
                { "max_rgb_pixel_diff_p99_code",
                    metrics.maximumRgbPixelDifferencePercentile99Code },
                { "changed_pixel_count", metrics.changedPixelCount },
                { "changed_pixel_fraction", metrics.changedPixelFraction },
                { "luma_ssim", {
                    { "mean", metrics.lumaSsim.mean },
                    { "minimum", metrics.lumaSsim.minimum },
                    { "p5", metrics.lumaSsim.percentile5 },
                    { "window_count", metrics.lumaSsim.windowCount },
                } },
            } },
            { "checks", {
                { "max_abs_code", result.maximumAbsoluteErrorPassed },
                { "changed_pixel_fraction", result.changedPixelFractionPassed },
                { "mean_luma_ssim", result.meanLumaSsimPassed },
            } },
            { "passed", result.passed() },
        };
        document["metadata_compatibility"] = {
            { "checked", metadata.checked },
            { "reference_path", metadata.referencePath.generic_string() },
            { "candidate_path", metadata.candidatePath.generic_string() },
            { "checked_fields", metadata.checkedFields },
        };
        if (metadata.checked) {
            document["metadata_compatibility"]["reference_context"] =
                metadata.referenceContext;
            document["metadata_compatibility"]["candidate_context"] =
                metadata.candidateContext;
        }
        if (options.heatmap) {
            document["heatmap"] = options.heatmap->generic_string();
        }
        else {
            document["heatmap"] = nullptr;
        }
        return document;
    }

    void writeJsonReport(const std::filesystem::path& path,
        const nlohmann::ordered_json& report) {
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        std::ofstream output(path, std::ios::out | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Failed to open JSON report output: " +
                path.generic_string());
        }
        output << report.dump(2) << '\n';
        output.flush();
        if (!output) {
            throw std::runtime_error("Failed while writing JSON report: " +
                path.generic_string());
        }
    }

    void writeComparisonArtifacts(const CommandLineOptions& options,
        const TgaImage& reference, const TgaImage& candidate,
        const nlohmann::ordered_json& report) {
        std::filesystem::path temporaryReport = options.report;
        temporaryReport += ".tmp";
        std::optional<std::filesystem::path> temporaryHeatmap;
        if (options.heatmap) {
            temporaryHeatmap = *options.heatmap;
            *temporaryHeatmap += ".tmp";
        }
        if (std::filesystem::exists(temporaryReport) ||
            (temporaryHeatmap && std::filesystem::exists(*temporaryHeatmap))) {
            throw std::runtime_error(
                "Refusing to overwrite a temporary comparison artifact.");
        }

        bool heatmapCommitted = false;
        try {
            if (temporaryHeatmap) {
                writeTga(*temporaryHeatmap, makeImageDifferenceHeatmap(
                    reference, candidate, options.heatmapScale));
            }
            writeJsonReport(temporaryReport, report);
            if (temporaryHeatmap) {
                std::filesystem::rename(*temporaryHeatmap, *options.heatmap);
                heatmapCommitted = true;
            }
            std::filesystem::rename(temporaryReport, options.report);
        }
        catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temporaryReport, ignored);
            if (temporaryHeatmap) {
                std::filesystem::remove(*temporaryHeatmap, ignored);
            }
            if (heatmapCommitted) {
                std::filesystem::remove(*options.heatmap, ignored);
            }
            throw;
        }
    }

} // namespace

int main(int argc, char** argv) {
    try {
        const CommandLineOptions options = parseCommandLine(argc, argv);
        if (options.showHelp) {
            printUsage(std::cout);
            return 0;
        }
        if (options.heatmap && std::filesystem::exists(*options.heatmap)) {
            throw std::runtime_error("Refusing to overwrite heatmap: " +
                options.heatmap->generic_string());
        }
        if (std::filesystem::exists(options.report)) {
            throw std::runtime_error("Refusing to overwrite report: " +
                options.report.generic_string());
        }
        if (options.heatmap &&
            std::filesystem::absolute(*options.heatmap).lexically_normal() ==
                std::filesystem::absolute(options.report).lexically_normal()) {
            throw std::invalid_argument(
                "--heatmap and --report must name different output paths.");
        }

        const MetadataCompatibility metadata =
            validateMetadataCompatibility(options);

        const TgaImage reference = readTga(options.reference);
        const TgaImage candidate = readTga(options.candidate);
        const ImageComparisonResult result = compareImages(reference, candidate,
            options.comparison, options.thresholds);

        writeComparisonArtifacts(options, reference, candidate,
            makeReport(options, result, metadata));
        std::cout << (result.passed() ? "PASS" : "FAIL")
            << ": max RGBA delta "
            << static_cast<uint32_t>(result.metrics.maximumAbsoluteErrorCode)
            << ", changed pixels " << result.metrics.changedPixelFraction
            << ", mean luma SSIM " << result.metrics.lumaSsim.mean << '\n';
        return result.passed() ? 0 : 1;
    }
    catch (const std::exception& exception) {
        std::cerr << "IridiumImageCompare: " << exception.what() << '\n';
        printUsage(std::cerr);
        return 2;
    }
}
