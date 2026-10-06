// IridiumTemporalMetrics (M9 G6d): temporal image-quality metrics over engine
// captures (scene-linear PFM, final-SDR TGA). See tools/m9/Temporal-Metrics.md.

#include "temporal/TemporalMetrics.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

    namespace tmx = Iridium::TemporalMetrics;
    using Clock = std::chrono::steady_clock;
    using Json = nlohmann::ordered_json;
    namespace fs = std::filesystem;

    constexpr const char* Schema = "iridium.temporal_metrics.v1";

    void printUsage(std::ostream& output) {
        output <<
            "Usage: IridiumTemporalMetrics <command> [options]\n"
            "\n"
            "Commands:\n"
            "  reference-error --reference R --test T [--domain scene|sdr] [--mask M]\n"
            "  stability       --frames F... | --list FILE [--domain D] [--mask M]\n"
            "                  [--flicker-threshold T] [--heatmap OUT.pfm|OUT.tga] [--heatmap-scale S]\n"
            "  ghosting        --frames F... --references R... [--mask-dir DIR | --trail-mask auto]\n"
            "                  [--trail-k K] [--trail-threshold T] [--mask M]\n"
            "  recovery        --frames F... --references R... --region X0 Y0 X1 Y1\n"
            "                  [--threshold T] [--steady-multiplier M]\n"
            "  accumulate      --frames F.pfm... --out MEAN.pfm [--report FILE]\n"
            "  error-flicker   --frames F... --references R... [--flicker-threshold T] [--mask M]\n"
            "                  (frame-to-frame change of the error: shimmer in motion)\n"
            "\n"
            "Common options:\n"
            "  --out FILE            JSON report path (default stdout; accumulate: the mean PFM)\n"
            "  --force               Overwrite existing outputs\n"
            "  --threads N           Worker threads (default: hardware concurrency)\n"
            "  --exposure-ev EV      Scene domain: scale linear RGB by 2^EV before tone mapping (0)\n"
            "  --log2-epsilon E      Scene reference-error: luminance floor for log2 (1e-4)\n"
            "  --tm-threshold T      Scene reference-error: tone-mapped error threshold (1/64)\n"
            "  --changed-threshold N SDR reference-error: changed-pixel code threshold (0)\n"
            "  --list FILE           Frame paths, one per line (# comments; relative to FILE)\n"
            "  --reference-list FILE Reference paths, one per line\n"
            "\n"
            "Paths may contain * and ? in the file name; matches are sorted naturally.\n"
            "Scene-domain inputs are .pfm, SDR-domain inputs are .tga.\n";
    }

    // ------------------------------------------------------------ arguments

    struct Options {
        std::string command;
        std::optional<fs::path> out;
        std::optional<fs::path> report;
        bool force = false;
        std::optional<tmx::Domain> domain;
        std::optional<fs::path> mask;
        std::optional<fs::path> reference;
        std::optional<fs::path> test;
        std::vector<fs::path> frames;
        std::vector<fs::path> references;
        double exposureEv = 0.0;
        double log2Epsilon = 1.0e-4;
        double toneMappedThreshold = 1.0 / 64.0;
        uint32_t changedThreshold = 0;
        double flickerThreshold = tmx::DefaultFlickerThreshold;
        std::optional<fs::path> heatmap;
        std::optional<double> heatmapScale;
        std::optional<fs::path> maskDirectory;
        bool trailMaskAuto = false;
        uint32_t trailK = 4;
        double trailThreshold = 0.02;
        std::optional<std::array<uint32_t, 4>> region;
        std::optional<double> threshold;
        double steadyMultiplier = 2.0;
        unsigned threads = 0;
    };

    [[nodiscard]] double parseDouble(std::string_view text, std::string_view option) {
        double value = 0.0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
            !std::isfinite(value)) {
            throw std::invalid_argument(std::string(option) + " requires a finite number.");
        }
        return value;
    }

    [[nodiscard]] uint32_t parseUnsigned(std::string_view text, std::string_view option) {
        uint32_t value = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
            throw std::invalid_argument(std::string(option) +
                " requires a nonnegative integer.");
        }
        return value;
    }

    [[nodiscard]] bool hasWildcard(const std::string& text) {
        return text.find_first_of("*?") != std::string::npos;
    }

    [[nodiscard]] char foldCase(char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    [[nodiscard]] bool wildcardMatch(std::string_view pattern, std::string_view text) {
        size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
        while (t < text.size()) {
            if (p < pattern.size() && (pattern[p] == '?' ||
                foldCase(pattern[p]) == foldCase(text[t]))) {
                ++p;
                ++t;
            }
            else if (p < pattern.size() && pattern[p] == '*') {
                star = p++;
                mark = t;
            }
            else if (star != std::string_view::npos) {
                p = star + 1;
                t = ++mark;
            }
            else {
                return false;
            }
        }
        while (p < pattern.size() && pattern[p] == '*') ++p;
        return p == pattern.size();
    }

    // Natural order: digit runs compare numerically ("f2" < "f10").
    [[nodiscard]] bool naturalLess(const std::string& a, const std::string& b) {
        size_t i = 0, j = 0;
        while (i < a.size() && j < b.size()) {
            if (std::isdigit(static_cast<unsigned char>(a[i])) &&
                std::isdigit(static_cast<unsigned char>(b[j]))) {
                size_t ie = i, je = j;
                while (ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie]))) ++ie;
                while (je < b.size() && std::isdigit(static_cast<unsigned char>(b[je]))) ++je;
                std::string_view da(a.data() + i, ie - i), db(b.data() + j, je - j);
                while (da.size() > 1 && da.front() == '0') da.remove_prefix(1);
                while (db.size() > 1 && db.front() == '0') db.remove_prefix(1);
                if (da.size() != db.size()) return da.size() < db.size();
                if (da != db) return da < db;
                i = ie;
                j = je;
                continue;
            }
            const char ca = foldCase(a[i]), cb = foldCase(b[j]);
            if (ca != cb) return ca < cb;
            ++i;
            ++j;
        }
        if ((a.size() - i) != (b.size() - j)) return (a.size() - i) < (b.size() - j);
        return a < b;
    }

    void appendExpanded(std::vector<fs::path>& paths, const std::string& argument) {
        if (!hasWildcard(argument)) {
            paths.emplace_back(argument);
            return;
        }
        const fs::path pattern(argument);
        const fs::path directory = pattern.has_parent_path() ? pattern.parent_path() :
            fs::path(".");
        if (hasWildcard(directory.string())) {
            throw std::invalid_argument("Wildcards are only supported in file names: " +
                argument);
        }
        const std::string filePattern = pattern.filename().string();
        std::vector<fs::path> matches;
        for (const fs::directory_entry& entry : fs::directory_iterator(directory)) {
            if (entry.is_regular_file() &&
                wildcardMatch(filePattern, entry.path().filename().string())) {
                matches.push_back(entry.path());
            }
        }
        if (matches.empty()) throw std::invalid_argument("No files match: " + argument);
        std::sort(matches.begin(), matches.end(), [](const fs::path& a, const fs::path& b) {
            return naturalLess(a.filename().string(), b.filename().string());
        });
        paths.insert(paths.end(), matches.begin(), matches.end());
    }

    void appendList(std::vector<fs::path>& paths, const fs::path& listFile) {
        std::ifstream input(listFile);
        if (!input) throw std::runtime_error("Failed to open list: " + listFile.generic_string());
        std::string line;
        while (std::getline(input, line)) {
            const size_t begin = line.find_first_not_of(" \t\r");
            if (begin == std::string::npos || line[begin] == '#') continue;
            const size_t end = line.find_last_not_of(" \t\r");
            fs::path entry(line.substr(begin, end - begin + 1));
            if (entry.is_relative()) entry = listFile.parent_path() / entry;
            appendExpanded(paths, entry.string());
        }
    }

    [[nodiscard]] Options parseCommandLine(int argc, char** argv) {
        if (argc < 2) throw std::invalid_argument("Missing command.");
        Options options{};
        options.command = argv[1];
        if (options.command == "--help" || options.command == "-h" ||
            options.command == "help") {
            options.command = "help";
            return options;
        }
        int index = 2;
        const auto value = [&](const std::string& option) -> std::string {
            if (index >= argc) throw std::invalid_argument("Missing value for " + option);
            return argv[index++];
        };
        const auto values = [&](std::vector<fs::path>& target) {
            const size_t before = target.size();
            while (index < argc && std::string_view(argv[index]).rfind("--", 0) != 0) {
                appendExpanded(target, argv[index++]);
            }
            if (target.size() == before) {
                throw std::invalid_argument("Expected at least one path.");
            }
        };
        while (index < argc) {
            const std::string option = argv[index++];
            if (option == "--out") options.out = value(option);
            else if (option == "--report") options.report = value(option);
            else if (option == "--force") options.force = true;
            else if (option == "--domain") {
                const std::string domain = value(option);
                if (domain == "scene") options.domain = tmx::Domain::Scene;
                else if (domain == "sdr") options.domain = tmx::Domain::Sdr;
                else throw std::invalid_argument("--domain must be scene or sdr.");
            }
            else if (option == "--reference" || option == "--test" || option == "--mask") {
                std::vector<fs::path> expanded;
                appendExpanded(expanded, value(option));
                if (expanded.size() != 1) {
                    throw std::invalid_argument(option + " must name exactly one file (" +
                        std::to_string(expanded.size()) + " matched).");
                }
                if (option == "--reference") options.reference = expanded.front();
                else if (option == "--test") options.test = expanded.front();
                else options.mask = expanded.front();
            }
            else if (option == "--frames") values(options.frames);
            else if (option == "--references") values(options.references);
            else if (option == "--list") appendList(options.frames, value(option));
            else if (option == "--reference-list") appendList(options.references, value(option));
            else if (option == "--exposure-ev") options.exposureEv = parseDouble(value(option), option);
            else if (option == "--log2-epsilon") options.log2Epsilon = parseDouble(value(option), option);
            else if (option == "--tm-threshold") options.toneMappedThreshold = parseDouble(value(option), option);
            else if (option == "--changed-threshold") {
                options.changedThreshold = parseUnsigned(value(option), option);
                if (options.changedThreshold > 255) {
                    throw std::invalid_argument("--changed-threshold must be in [0, 255].");
                }
            }
            else if (option == "--flicker-threshold") options.flickerThreshold = parseDouble(value(option), option);
            else if (option == "--heatmap") options.heatmap = value(option);
            else if (option == "--heatmap-scale") options.heatmapScale = parseDouble(value(option), option);
            else if (option == "--mask-dir") options.maskDirectory = value(option);
            else if (option == "--trail-mask") {
                if (value(option) != "auto") throw std::invalid_argument("--trail-mask supports only 'auto'.");
                options.trailMaskAuto = true;
            }
            else if (option == "--trail-k") options.trailK = parseUnsigned(value(option), option);
            else if (option == "--trail-threshold") options.trailThreshold = parseDouble(value(option), option);
            else if (option == "--region") {
                std::array<uint32_t, 4> region{};
                for (uint32_t& coordinate : region) coordinate = parseUnsigned(value(option), option);
                options.region = region;
            }
            else if (option == "--threshold") options.threshold = parseDouble(value(option), option);
            else if (option == "--steady-multiplier") options.steadyMultiplier = parseDouble(value(option), option);
            else if (option == "--threads") options.threads = parseUnsigned(value(option), option);
            else throw std::invalid_argument("Unknown option: " + option);
        }
        if (options.maskDirectory && options.trailMaskAuto) {
            throw std::invalid_argument("--mask-dir and --trail-mask are exclusive.");
        }
        if (options.trailK == 0) throw std::invalid_argument("--trail-k must be at least 1.");
        return options;
    }

    // ---------------------------------------------------------------- utils

    [[nodiscard]] double millisecondsSince(Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    [[nodiscard]] Json number(double value) {
        if (std::isfinite(value)) return value;
        if (std::isnan(value)) return nullptr;
        return value > 0 ? "inf" : "-inf";
    }

    template <typename T>
    [[nodiscard]] Json optionalJson(const std::optional<T>& value) {
        return value ? Json(*value) : Json(nullptr);
    }

    void requireWritable(const fs::path& path, bool force) {
        if (!force && fs::exists(path)) {
            throw std::runtime_error("Refusing to overwrite " + path.generic_string() +
                " (use --force).");
        }
    }

    [[nodiscard]] tmx::Domain resolveDomain(const Options& options,
        const std::vector<fs::path>& paths) {
        if (paths.empty()) throw std::invalid_argument("No input images.");
        const tmx::Domain domain = options.domain.value_or(tmx::domainForPath(paths.front()));
        for (const fs::path& path : paths) {
            if (tmx::domainForPath(path) != domain) {
                throw std::invalid_argument(std::string("Input does not match the ") +
                    tmx::domainName(domain) + " domain (scene: .pfm, sdr: .tga): " +
                    path.generic_string());
            }
        }
        return domain;
    }

    [[nodiscard]] tmx::Plane loadLuma(const fs::path& path, tmx::Domain domain,
        double exposureScale) {
        if (domain == tmx::Domain::Scene) {
            return tmx::toneMappedLuma(tmx::readPfm(path), exposureScale);
        }
        return tmx::encodedLuma(tmx::readTga(path));
    }

    [[nodiscard]] std::optional<tmx::Mask> loadOptionalMask(const Options& options) {
        if (!options.mask) return std::nullopt;
        return tmx::readMask(*options.mask);
    }

    [[nodiscard]] Json baseReport(const Options& options) {
        Json report{
            { "schema", Schema },
            { "command", options.command },
        };
        return report;
    }

    void emitReport(const Options& options, const Json& report) {
        const std::optional<fs::path>& path = options.command == "accumulate" ?
            options.report : options.out;
        if (!path) {
            std::cout << report.dump(2) << '\n';
            return;
        }
        requireWritable(*path, options.force);
        const fs::path parent = path->parent_path();
        if (!parent.empty()) fs::create_directories(parent);
        std::ofstream output(*path, std::ios::trunc);
        if (!output) throw std::runtime_error("Failed to open report: " + path->generic_string());
        output << report.dump(2) << '\n';
        if (!output) throw std::runtime_error("Failed to write report: " + path->generic_string());
    }

    [[nodiscard]] Json pathList(const std::vector<fs::path>& paths) {
        Json list = Json::array();
        for (const fs::path& path : paths) list.push_back(path.generic_string());
        return list;
    }

    // ------------------------------------------------------------- commands

    Json runReferenceError(const Options& options) {
        if (!options.reference || !options.test) {
            throw std::invalid_argument("reference-error requires --reference and --test.");
        }
        const tmx::Domain domain = resolveDomain(options, { *options.reference, *options.test });
        const std::optional<tmx::Mask> mask = loadOptionalMask(options);
        const tmx::Mask* maskPointer = mask ? &*mask : nullptr;
        Json report = baseReport(options);
        report["domain"] = tmx::domainName(domain);
        report["reference"] = options.reference->generic_string();
        report["test"] = options.test->generic_string();
        report["mask"] = options.mask ? Json(options.mask->generic_string()) : Json(nullptr);

        const Clock::time_point loadStart = Clock::now();
        if (domain == tmx::Domain::Scene) {
            auto referenceFuture = std::async(std::launch::async,
                [&] { return tmx::readPfm(*options.reference); });
            const tmx::FloatImage test = tmx::readPfm(*options.test);
            const tmx::FloatImage reference = referenceFuture.get();
            const double loadMs = millisecondsSince(loadStart);
            const Clock::time_point computeStart = Clock::now();
            tmx::SceneErrorOptions sceneOptions{};
            sceneOptions.exposureScale = std::exp2(options.exposureEv);
            sceneOptions.log2Epsilon = options.log2Epsilon;
            sceneOptions.toneMappedThreshold = options.toneMappedThreshold;
            const tmx::SceneErrorMetrics m = tmx::computeSceneError(reference, test,
                maskPointer, sceneOptions);
            const double computeMs = millisecondsSince(computeStart);
            report["width"] = reference.width;
            report["height"] = reference.height;
            report["settings"] = {
                { "exposure_ev", options.exposureEv },
                { "luminance_weights", "acescg_ap1 (0.2722287, 0.6740818, 0.0536895)" },
                { "log2_epsilon", options.log2Epsilon },
                { "tone_map", "reinhard c/(1+c) per channel, negatives clamped to 0" },
                { "tone_mapped_threshold", options.toneMappedThreshold },
            };
            report["metrics"] = {
                { "pixel_count", m.pixelCount },
                { "non_finite_pixel_count", m.nonFinitePixelCount },
                { "luminance_rmse", number(m.luminanceRmse) },
                { "log2_luminance_mae", number(m.log2LuminanceMae) },
                { "log2_luminance_rmse", number(m.log2LuminanceRmse) },
                { "tone_mapped_rmse", number(m.toneMappedRmse) },
                { "max_abs_linear", number(m.maxAbsLinear) },
                { "max_abs_tone_mapped", number(m.maxAbsToneMapped) },
                { "above_threshold_pixel_count", m.aboveThresholdPixelCount },
                { "above_threshold_fraction", number(m.aboveThresholdFraction) },
            };
            report["timing_ms"] = { { "load", loadMs }, { "compute", computeMs } };
            std::cerr << std::setprecision(6) << "reference-error (scene): log2-Y MAE "
                << m.log2LuminanceMae << ", log2-Y RMSE " << m.log2LuminanceRmse
                << ", tone-mapped RMSE " << m.toneMappedRmse << ", max |tm| "
                << m.maxAbsToneMapped << ", >1/64 fraction " << m.aboveThresholdFraction
                << " (" << m.pixelCount << " px; load " << loadMs << " ms, compute "
                << computeMs << " ms)\n";
        }
        else {
            auto referenceFuture = std::async(std::launch::async,
                [&] { return tmx::readTga(*options.reference); });
            const tmx::Rgb8Image test = tmx::readTga(*options.test);
            const tmx::Rgb8Image reference = referenceFuture.get();
            const double loadMs = millisecondsSince(loadStart);
            const Clock::time_point computeStart = Clock::now();
            tmx::SdrErrorOptions sdrOptions{};
            sdrOptions.changedPixelCodeThreshold = static_cast<uint8_t>(options.changedThreshold);
            const tmx::SdrErrorMetrics m = tmx::computeSdrError(reference, test, maskPointer,
                sdrOptions);
            const double computeMs = millisecondsSince(computeStart);
            report["width"] = reference.width;
            report["height"] = reference.height;
            report["settings"] = {
                { "changed_pixel_code_threshold", options.changedThreshold },
                { "ssim", "gaussian 11x11 sigma 1.5, K1 0.01, K2 0.03, valid region" },
                { "ssim_signal", "rec709-weighted display-encoded luma, codes / 255" },
            };
            report["metrics"] = {
                { "pixel_count", m.pixelCount },
                { "mse_code", number(m.mse) },
                { "psnr_db", number(m.psnrDb) },
                { "max_abs_code", m.maxAbsCode },
                { "changed_pixel_count", m.changedPixelCount },
                { "changed_pixel_fraction", number(m.changedPixelFraction) },
                { "luma_ssim_mean", number(m.lumaSsim.mean) },
                { "luma_ssim_window_count", m.lumaSsim.windowCount },
            };
            report["timing_ms"] = { { "load", loadMs }, { "compute", computeMs } };
            std::cerr << std::setprecision(6) << "reference-error (sdr): PSNR "
                << (std::isinf(m.psnrDb) ? std::string("inf") : std::to_string(m.psnrDb))
                << " dB, luma SSIM " << m.lumaSsim.mean << ", changed "
                << m.changedPixelFraction << ", max code " << m.maxAbsCode << " (load "
                << loadMs << " ms, compute " << computeMs << " ms)\n";
        }
        return report;
    }

    // Loads luma planes in order, one frame ahead of the consumer.
    class LumaStream {
    public:
        LumaStream(const std::vector<fs::path>& paths, tmx::Domain domain, double scale)
            : m_paths(paths), m_domain(domain), m_scale(scale) {
            if (!m_paths.empty()) schedule(0);
        }
        [[nodiscard]] tmx::Plane take(size_t index) {
            if (index != m_next) throw std::logic_error("LumaStream is sequential.");
            tmx::Plane plane = m_pending.get();
            if (index + 1 < m_paths.size()) schedule(index + 1);
            return plane;
        }

    private:
        void schedule(size_t index) {
            m_next = index;
            const fs::path path = m_paths[index];
            const tmx::Domain domain = m_domain;
            const double scale = m_scale;
            m_pending = std::async(std::launch::async,
                [path, domain, scale] { return loadLuma(path, domain, scale); });
        }
        std::vector<fs::path> m_paths;
        tmx::Domain m_domain;
        double m_scale;
        size_t m_next = 0;
        std::future<tmx::Plane> m_pending;
    };

    Json runStability(const Options& options) {
        if (options.frames.size() < 2) {
            throw std::invalid_argument("stability requires at least two frames.");
        }
        const tmx::Domain domain = resolveDomain(options, options.frames);
        if (options.heatmap) requireWritable(*options.heatmap, options.force);
        const std::optional<tmx::Mask> mask = loadOptionalMask(options);
        const Clock::time_point start = Clock::now();
        LumaStream stream(options.frames, domain, std::exp2(options.exposureEv));
        std::optional<tmx::StabilityAccumulator> accumulator;
        uint64_t nonFinite = 0;
        for (size_t i = 0; i < options.frames.size(); ++i) {
            const tmx::Plane luma = stream.take(i);
            if (!accumulator) {
                accumulator.emplace(luma.width, luma.height, mask ? &*mask : nullptr,
                    options.flickerThreshold);
            }
            nonFinite += luma.nonFiniteCount;
            accumulator->addFrame(luma);
        }
        const tmx::StabilityMetrics m = accumulator->metrics();
        const double totalMs = millisecondsSince(start);

        Json report = baseReport(options);
        report["domain"] = tmx::domainName(domain);
        report["frames"] = pathList(options.frames);
        report["mask"] = options.mask ? Json(options.mask->generic_string()) : Json(nullptr);
        report["settings"] = {
            { "signal", domain == tmx::Domain::Scene ?
                "tone-mapped AP1 luma Y/(1+Y)" : "rec709-weighted display-encoded luma" },
            { "exposure_ev", options.exposureEv },
            { "flicker_threshold", m.flickerThreshold },
            { "std_dev", "population (divide by K)" },
            { "p99_delta_resolution", 1.0 / 65535.0 },
        };
        Json metrics{
            { "frame_count", m.frameCount },
            { "pixel_count", m.pixelCount },
            { "non_finite_sample_count", nonFinite },
            { "mean_frame_delta", m.meanFrameDelta },
            { "p99_frame_delta", m.p99FrameDelta },
            { "max_frame_delta", m.maxFrameDelta },
            { "mean_temporal_std", m.meanTemporalStdDev },
            { "p99_temporal_std", m.p99TemporalStdDev },
            { "max_temporal_std", m.maxTemporalStdDev },
            { "flicker_pixel_count", m.flickerPixelCount },
            { "flicker_pixel_fraction", m.flickerPixelFraction },
            { "flicker_energy", m.flickerEnergy },
        };
        metrics["pair_mean_delta"] = m.pairMeanDelta;
        report["metrics"] = metrics;
        if (options.heatmap) {
            const double scale = options.heatmapScale.value_or(
                m.maxTemporalStdDev > 0.0 ? 1.0 / m.maxTemporalStdDev : 1.0);
            tmx::writeHeatmap(*options.heatmap, accumulator->temporalStdDev(), scale);
            report["heatmap"] = { { "path", options.heatmap->generic_string() },
                { "signal", "temporal std dev" }, { "scale", scale } };
        }
        else {
            report["heatmap"] = nullptr;
        }
        report["timing_ms"] = { { "total", totalMs },
            { "per_frame", totalMs / static_cast<double>(options.frames.size()) } };
        std::cerr << std::setprecision(6) << "stability (" << tmx::domainName(domain) << ", "
            << m.frameCount << " frames): mean delta " << m.meanFrameDelta << ", p99 delta "
            << m.p99FrameDelta << ", mean std " << m.meanTemporalStdDev
            << ", flicker energy " << m.flickerEnergy << " over " << m.flickerPixelFraction
            << " of pixels (" << totalMs << " ms)\n";
        return report;
    }

    struct FramePairInputs {
        tmx::Domain domain;
        std::vector<fs::path> frames;
        std::vector<fs::path> references;
    };

    [[nodiscard]] FramePairInputs pairedInputs(const Options& options) {
        if (options.frames.empty()) throw std::invalid_argument("--frames is required.");
        if (options.frames.size() != options.references.size()) {
            throw std::invalid_argument("--frames and --references must have the same count (" +
                std::to_string(options.frames.size()) + " vs " +
                std::to_string(options.references.size()) + ").");
        }
        FramePairInputs inputs{};
        inputs.domain = resolveDomain(options, options.frames);
        if (resolveDomain(options, options.references) != inputs.domain) {
            throw std::invalid_argument("Frames and references are in different domains.");
        }
        inputs.frames = options.frames;
        inputs.references = options.references;
        return inputs;
    }

    [[nodiscard]] std::vector<fs::path> maskDirectoryFiles(const fs::path& directory) {
        std::vector<fs::path> files;
        for (const fs::directory_entry& entry : fs::directory_iterator(directory)) {
            if (!entry.is_regular_file()) continue;
            std::string extension = entry.path().extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (extension == ".tga" || extension == ".pfm") files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
            return naturalLess(a.filename().string(), b.filename().string());
        });
        return files;
    }

    // Shimmer under motion: for consecutive frames t-1, t (each with its own
    // reference, e.g. held accumulation references), the change of the
    // tone-mapped luma error e = test - reference. Content that legitimately
    // changes is in both test and reference and cancels; what remains is the
    // frame-to-frame instability of the test (jitter shimmer, crawl).
    Json runErrorFlicker(const Options& options) {
        const FramePairInputs inputs = pairedInputs(options);
        if (inputs.frames.size() < 2)
            throw std::invalid_argument("error-flicker needs at least two frames.");
        const std::optional<tmx::Mask> limit = loadOptionalMask(options);
        const double scale = std::exp2(options.exposureEv);
        LumaStream frameStream(inputs.frames, inputs.domain, scale);
        LumaStream referenceStream(inputs.references, inputs.domain, scale);
        std::vector<float> previousError;
        Json perPair = Json::array();
        double sumAll = 0.0;
        uint64_t countAll = 0;
        uint64_t aboveAll = 0;
        for (size_t i = 0; i < inputs.frames.size(); ++i) {
            const tmx::Plane frame = frameStream.take(i);
            const tmx::Plane reference = referenceStream.take(i);
            if (frame.values.size() != reference.values.size())
                throw std::invalid_argument("Frame and reference sizes differ.");
            std::vector<float> error(frame.values.size());
            for (size_t p = 0; p < error.size(); ++p) error[p] = frame.values[p] - reference.values[p];
            if (!previousError.empty()) {
                double sum = 0.0;
                uint64_t above = 0;
                std::vector<float> deltas;
                deltas.reserve(error.size());
                for (size_t p = 0; p < error.size(); ++p) {
                    if (limit && limit->include[p] == 0) continue;
                    const float d = std::abs(error[p] - previousError[p]);
                    sum += d;
                    if (d > options.flickerThreshold) ++above;
                    deltas.push_back(d);
                }
                const uint64_t count = deltas.size();
                double p99 = 0.0;
                if (count != 0) {
                    const auto nth = deltas.begin() + static_cast<std::ptrdiff_t>(count * 99 / 100);
                    std::nth_element(deltas.begin(), nth, deltas.end());
                    p99 = *nth;
                }
                perPair.push_back(Json{ { "from", i - 1 }, { "to", i },
                    { "mean_error_delta", count ? sum / static_cast<double>(count) : 0.0 },
                    { "p99_error_delta", p99 },
                    { "shimmer_pixel_fraction",
                        count ? static_cast<double>(above) / static_cast<double>(count) : 0.0 } });
                sumAll += sum;
                countAll += count;
                aboveAll += above;
            }
            previousError = std::move(error);
        }
        return Json{
            { "schema", "iridium.temporal_metrics.v1" },
            { "command", "error-flicker" },
            { "flicker_threshold", options.flickerThreshold },
            { "pairs", perPair },
            { "summary", Json{
                { "mean_error_delta", countAll ? sumAll / static_cast<double>(countAll) : 0.0 },
                { "shimmer_pixel_fraction",
                    countAll ? static_cast<double>(aboveAll) / static_cast<double>(countAll) : 0.0 } } },
        };
    }

    Json runGhosting(const Options& options) {
        const FramePairInputs inputs = pairedInputs(options);
        const std::optional<tmx::Mask> limit = loadOptionalMask(options);
        const tmx::Mask* limitPointer = limit ? &*limit : nullptr;
        std::vector<fs::path> maskFiles;
        if (options.maskDirectory) {
            maskFiles = maskDirectoryFiles(*options.maskDirectory);
            if (maskFiles.size() != inputs.frames.size()) {
                throw std::invalid_argument("--mask-dir holds " +
                    std::to_string(maskFiles.size()) + " masks for " +
                    std::to_string(inputs.frames.size()) + " frames.");
            }
        }
        const double scale = std::exp2(options.exposureEv);
        const Clock::time_point start = Clock::now();
        LumaStream frameStream(inputs.frames, inputs.domain, scale);
        LumaStream referenceStream(inputs.references, inputs.domain, scale);
        std::deque<tmx::Plane> history; // the last trailK references
        const bool trail = options.trailMaskAuto || options.maskDirectory.has_value();

        Json perFrame = Json::array();
        std::vector<double> errors;
        std::vector<double> trailEnergies;
        double trailErrorSum = 0.0;
        uint64_t trailArea = 0;
        for (size_t i = 0; i < inputs.frames.size(); ++i) {
            tmx::Plane frame = frameStream.take(i);
            tmx::Plane reference = referenceStream.take(i);
            const tmx::LumaErrorStats full = tmx::lumaError(reference, frame, limitPointer);
            errors.push_back(full.meanAbs);
            Json entry{
                { "index", i },
                { "frame", inputs.frames[i].generic_string() },
                { "reference", inputs.references[i].generic_string() },
                { "mean_abs", full.meanAbs },
                { "rmse", full.rmse },
                { "max_abs", full.maxAbs },
            };
            if (trail) {
                std::optional<tmx::Mask> trailMask;
                if (options.maskDirectory) {
                    trailMask = tmx::readMask(maskFiles[i]);
                    if (limitPointer != nullptr) {
                        for (size_t p = 0; p < trailMask->include.size(); ++p) {
                            trailMask->include[p] &= limitPointer->include[p];
                        }
                    }
                }
                else if (history.size() == options.trailK) {
                    trailMask = tmx::trailMask(reference, history.front(),
                        options.trailThreshold, limitPointer);
                }
                if (trailMask) {
                    const tmx::LumaErrorStats inTrail = tmx::lumaError(reference, frame,
                        &*trailMask);
                    entry["trail_area"] = inTrail.pixelCount;
                    entry["trail_energy"] = inTrail.pixelCount > 0 ?
                        Json(inTrail.meanAbs) : Json(nullptr);
                    trailErrorSum += inTrail.sumAbs;
                    trailArea += inTrail.pixelCount;
                    if (inTrail.pixelCount > 0) trailEnergies.push_back(inTrail.meanAbs);
                }
                else {
                    entry["trail_area"] = nullptr;
                    entry["trail_energy"] = nullptr;
                }
                if (options.trailMaskAuto) {
                    history.push_back(std::move(reference));
                    if (history.size() > options.trailK) history.pop_front();
                }
            }
            perFrame.push_back(std::move(entry));
        }
        const double totalMs = millisecondsSince(start);
        const auto meanOf = [](const std::vector<double>& v) {
            double sum = 0.0;
            for (const double x : v) sum += x;
            return v.empty() ? 0.0 : sum / static_cast<double>(v.size());
        };
        const auto maxOf = [](const std::vector<double>& v) {
            return v.empty() ? 0.0 : *std::max_element(v.begin(), v.end());
        };

        Json report = baseReport(options);
        report["domain"] = tmx::domainName(inputs.domain);
        report["settings"] = {
            { "signal", inputs.domain == tmx::Domain::Scene ?
                "tone-mapped AP1 luma Y/(1+Y)" : "rec709-weighted display-encoded luma" },
            { "exposure_ev", options.exposureEv },
            { "mask", options.mask ? Json(options.mask->generic_string()) : Json(nullptr) },
            { "trail_mask", options.trailMaskAuto ? Json("auto") :
                options.maskDirectory ? Json(options.maskDirectory->generic_string()) :
                Json(nullptr) },
            { "trail_k", options.trailK },
            { "trail_threshold", options.trailThreshold },
        };
        Json summary{
            { "frame_count", inputs.frames.size() },
            { "mean_abs_mean", meanOf(errors) },
            { "mean_abs_max", maxOf(errors) },
        };
        if (trail) {
            summary["trail_frames"] = trailEnergies.size();
            summary["trail_energy_mean"] = trailEnergies.empty() ? Json(nullptr) :
                Json(meanOf(trailEnergies));
            summary["trail_energy_max"] = trailEnergies.empty() ? Json(nullptr) :
                Json(maxOf(trailEnergies));
            summary["trail_energy_pooled"] = trailArea > 0 ?
                Json(trailErrorSum / static_cast<double>(trailArea)) : Json(nullptr);
        }
        report["summary"] = summary;
        report["frames"] = perFrame;
        report["timing_ms"] = { { "total", totalMs },
            { "per_frame", totalMs / static_cast<double>(inputs.frames.size()) } };
        std::cerr << std::setprecision(6) << "ghosting (" << inputs.frames.size()
            << " frames): mean error " << meanOf(errors) << " (max " << maxOf(errors) << ")";
        if (trail) {
            std::cerr << ", trail energy mean " << meanOf(trailEnergies) << " max "
                << maxOf(trailEnergies) << " over " << trailEnergies.size() << " frames";
        }
        std::cerr << " (" << totalMs << " ms)\n";
        return report;
    }

    Json runRecovery(const Options& options) {
        const FramePairInputs inputs = pairedInputs(options);
        if (!options.region) {
            throw std::invalid_argument("recovery requires --region X0 Y0 X1 Y1.");
        }
        const std::array<uint32_t, 4> r = *options.region;
        const double scale = std::exp2(options.exposureEv);
        const Clock::time_point start = Clock::now();
        LumaStream frameStream(inputs.frames, inputs.domain, scale);
        LumaStream referenceStream(inputs.references, inputs.domain, scale);
        std::optional<tmx::Mask> region;
        std::vector<double> errors;
        Json perFrame = Json::array();
        for (size_t i = 0; i < inputs.frames.size(); ++i) {
            const tmx::Plane frame = frameStream.take(i);
            const tmx::Plane reference = referenceStream.take(i);
            if (!region) region = tmx::regionMask(frame.width, frame.height, r[0], r[1], r[2], r[3]);
            const tmx::LumaErrorStats stats = tmx::lumaError(reference, frame, &*region);
            errors.push_back(stats.meanAbs);
            perFrame.push_back(Json{
                { "index", i },
                { "frame", inputs.frames[i].generic_string() },
                { "reference", inputs.references[i].generic_string() },
                { "region_mean_abs", stats.meanAbs },
                { "region_rmse", stats.rmse },
            });
        }
        const tmx::RecoveryResult result = tmx::evaluateRecovery(errors, options.threshold,
            options.steadyMultiplier);
        const double totalMs = millisecondsSince(start);
        Json report = baseReport(options);
        report["domain"] = tmx::domainName(inputs.domain);
        report["settings"] = {
            { "signal", inputs.domain == tmx::Domain::Scene ?
                "tone-mapped AP1 luma Y/(1+Y)" : "rec709-weighted display-encoded luma" },
            { "exposure_ev", options.exposureEv },
            { "region", { r[0], r[1], r[2], r[3] } },
            { "region_area", region->area() },
            { "steady_multiplier", options.steadyMultiplier },
        };
        report["summary"] = {
            { "frame_count", errors.size() },
            { "threshold", result.threshold },
            { "threshold_from_steady_state", result.thresholdFromSteadyState },
            { "steady_state_median", result.steadyStateMedian },
            { "steady_state_frame_count", result.steadyStateFrameCount },
            { "recovery_frames", optionalJson(result.firstBelowFrame) },
            { "settled_frame", optionalJson(result.settledFrame) },
            { "initial_error", errors.front() },
        };
        report["frames"] = perFrame;
        report["timing_ms"] = { { "total", totalMs } };
        std::cerr << std::setprecision(6) << "recovery (" << errors.size()
            << " frames): threshold " << result.threshold << ", recovery frames "
            << (result.firstBelowFrame ? std::to_string(*result.firstBelowFrame) : "never")
            << ", settled at "
            << (result.settledFrame ? std::to_string(*result.settledFrame) : "never")
            << " (" << totalMs << " ms)\n";
        return report;
    }

    Json runAccumulate(const Options& options) {
        if (options.frames.empty()) throw std::invalid_argument("--frames is required.");
        if (!options.out) throw std::invalid_argument("accumulate requires --out MEAN.pfm.");
        if (resolveDomain(options, options.frames) != tmx::Domain::Scene ||
            tmx::domainForPath(*options.out) != tmx::Domain::Scene) {
            throw std::invalid_argument("accumulate reads and writes scene-linear PFM only.");
        }
        requireWritable(*options.out, options.force);
        const Clock::time_point start = Clock::now();
        tmx::MeanAccumulator accumulator;
        std::future<tmx::FloatImage> pending = std::async(std::launch::async,
            [path = options.frames.front()] { return tmx::readPfm(path); });
        for (size_t i = 0; i < options.frames.size(); ++i) {
            const tmx::FloatImage image = pending.get();
            if (i + 1 < options.frames.size()) {
                pending = std::async(std::launch::async,
                    [path = options.frames[i + 1]] { return tmx::readPfm(path); });
            }
            accumulator.add(image);
        }
        const tmx::FloatImage mean = accumulator.mean();
        tmx::writePfm(*options.out, mean);
        const double totalMs = millisecondsSince(start);
        Json report = baseReport(options);
        report["frames"] = pathList(options.frames);
        report["output"] = options.out->generic_string();
        report["width"] = mean.width;
        report["height"] = mean.height;
        report["count"] = accumulator.count();
        report["non_finite_sample_count"] = accumulator.nonFiniteSampleCount();
        report["timing_ms"] = { { "total", totalMs } };
        std::cerr << "accumulate: mean of " << accumulator.count() << " frames -> "
            << options.out->generic_string() << " (" << totalMs << " ms)\n";
        return report;
    }

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseCommandLine(argc, argv);
        if (options.command == "help") {
            printUsage(std::cout);
            return 0;
        }
        tmx::setWorkerCount(options.threads);
        // Fail before any work when the JSON report would be refused.
        const std::optional<fs::path>& reportPath = options.command == "accumulate" ?
            options.report : options.out;
        if (reportPath) requireWritable(*reportPath, options.force);
        Json report;
        if (options.command == "reference-error") report = runReferenceError(options);
        else if (options.command == "stability") report = runStability(options);
        else if (options.command == "ghosting") report = runGhosting(options);
        else if (options.command == "error-flicker") report = runErrorFlicker(options);
        else if (options.command == "recovery") report = runRecovery(options);
        else if (options.command == "accumulate") report = runAccumulate(options);
        else throw std::invalid_argument("Unknown command: " + options.command);
        report["threads"] = tmx::workerCount();
        emitReport(options, report);
        return 0;
    }
    catch (const std::exception& exception) {
        std::cerr << "IridiumTemporalMetrics: " << exception.what() << '\n';
        if (dynamic_cast<const std::invalid_argument*>(&exception) != nullptr) {
            printUsage(std::cerr);
        }
        return 2;
    }
}
