#include "core/ApplicationConfig.h"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

    using namespace Iridium;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    template <size_t Size>
    bool rejects(const std::array<std::string_view, Size>& arguments) {
        try {
            (void)parseApplicationConfig(arguments);
        }
        catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    }

    bool testExplicitValidationAndProfiling() {
        constexpr std::array arguments{
            std::string_view("--no-validation"),
            std::string_view("--validation"),
            std::string_view("--profile-cpu-output"),
            std::string_view("profiles/run.jsonl"),
            std::string_view("--profile-gpu"),
            std::string_view("--profile-transparent-overdraw"),
            std::string_view("--validate-texture-residency-churn"),
            std::string_view("--validate-reflection-probes"),
            std::string_view("--validate-ordinary2-capture"),
            std::string_view("--validate-ordinary2-fallback"),
            std::string_view("--validate-ordinary2-resize"),
            std::string_view("--validate-deep-layered-capture"),
            std::string_view("--validate-deep-layered-lifecycle"),
            std::string_view("--deep-layered-validation-quality"),
            std::string_view("cinematic8"),
            std::string_view("--validate-texture-table-scale"),
            std::string_view("8192"),
            std::string_view("--validate-material-table-scale"),
            std::string_view("65536"),
            std::string_view("--validate-light-table-scale"),
            std::string_view("4096"),
            std::string_view("--cache-state"),
            std::string_view("warm-steady-state"),
        };
        const ApplicationConfig config = parseApplicationConfig(arguments);
        CHECK(config.enableValidation);
        CHECK(config.enableCpuProfiling);
        CHECK(config.enableGpuProfiling);
        CHECK(config.enableTransparentPipelineStatistics);
        CHECK(config.validateTextureResidencyChurn);
        CHECK(config.validateReflectionProbes);
        CHECK(config.validateOrdinary2Capture);
        CHECK(config.validateOrdinary2Fallback);
        CHECK(config.validateOrdinary2Resize);
        CHECK(config.validateDeepLayeredCapture);
        CHECK(config.validateDeepLayeredLifecycle);
        CHECK(config.deepLayeredCaptureQuality ==
            TransparencyQuality::Cinematic8);
        CHECK(config.validateTextureTableScale == 8192);
        CHECK(config.validateMaterialTableScale == 65'536);
        CHECK(config.validateLightTableScale == 4'096);
        CHECK(config.cacheState == "warm-steady-state");
        CHECK(config.cpuProfileOutput.generic_string() == "profiles/run.jsonl");
        return true;
    }

    bool testM2ProductionDefaults() {
        constexpr std::array<std::string_view, 0> defaultArguments{};
        const ApplicationConfig defaultConfig = parseApplicationConfig(defaultArguments);
        CHECK(defaultConfig.gBufferLayout == GBufferLayout::CanonicalReference);
        CHECK(defaultConfig.clusterTileSize == 32);
        CHECK(defaultConfig.clusterDepthSlices == 24);
        CHECK(rejects(std::array{ std::string_view("--render-graph") }));
        CHECK(rejects(std::array{ std::string_view("--render-graph-shadow") }));
        CHECK(rejects(std::array{ std::string_view("--canonical-materials") }));
        CHECK(parseApplicationConfig(std::array{
            std::string_view("--wireframe") }).forceWireframe);
        return true;
    }

    bool testBoundedRunOptions() {
        constexpr std::array arguments{
            std::string_view("--frame-limit"),
            std::string_view("10000"),
            std::string_view("--warmup-frames"),
            std::string_view("500"),
            std::string_view("--window-size"),
            std::string_view("3840x2160"),
            std::string_view("--hidden-window"),
            std::string_view("--show-profiler"),
            std::string_view("--show-material-diagnostics"),
            std::string_view("--select-benchmark-entity"),
            std::string_view("--gbuffer-layout"),
            std::string_view("reference"),
            std::string_view("--exposure-ev"),
            std::string_view("-2.5"),
            std::string_view("--output-operator"),
            std::string_view("aces2"),
			std::string_view("--output-transport"),
			std::string_view("scrgb"),
            std::string_view("--paper-white-nits"),
            std::string_view("250"),
            std::string_view("--peak-nits"),
            std::string_view("1200"),
            std::string_view("--cluster-tile-size"),
            std::string_view("32"),
            std::string_view("--cluster-depth-slices"),
            std::string_view("32"),
            std::string_view("--cluster-stress-lights"),
            std::string_view("512"),
            std::string_view("--shadow-directional-resolution"),
            std::string_view("4096"),
            std::string_view("--shadow-directional-lights"),
            std::string_view("1"),
            std::string_view("--shadow-directional-source-diameter"),
            std::string_view("1.25"),
            std::string_view("--shadow-filter"),
            std::string_view("fixed"),
            std::string_view("--shadow-spot-atlas-resolution"),
            std::string_view("8192"),
            std::string_view("--benchmark-disable-local-shadows"),
            std::string_view("--debug-view"),
            std::string_view("cluster-occupancy"),
            std::string_view("--benchmark"),
            std::string_view("material_lab_v1"),
            std::string_view("--benchmark-manifest"),
            std::string_view("assets/benchmarks/m0/manifest.v1.json"),
            std::string_view("--cooked-model-artifact"),
            std::string_view("out/ddc/model.irartifact"),
            std::string_view("--cooked-environment-artifact"),
            std::string_view("out/ddc/environment.irartifact"),
            std::string_view("--open-asset-viewer"),
            std::string_view("019fb73d-5a26-7326-8688-ea55a972179c"),
            std::string_view("--capture-frame"),
            std::string_view("0"),
            std::string_view("--capture-directory"),
            std::string_view("out/captures/run-a"),
            std::string_view("--capture-point"),
            std::string_view("final-output"),
        };
        const ApplicationConfig config = parseApplicationConfig(arguments);
        CHECK(config.frameLimit == 10000);
        CHECK(config.frameLimitSpecified);
        CHECK(config.warmupFrameCount == 500);
        CHECK(config.warmupFrameCountSpecified);
        CHECK(config.windowWidth == 3840);
        CHECK(config.windowHeight == 2160);
        CHECK(!config.windowVisible);
        CHECK(!config.windowDecorated);
        CHECK(config.showProfiler);
        CHECK(config.showMaterialDiagnostics);
        CHECK(config.selectBenchmarkEntity);
        CHECK(config.gBufferLayout == GBufferLayout::CanonicalReference);
        CHECK(config.manualExposureEv == -2.5);
        CHECK(config.outputOperator == OutputTransformOperator::Aces2);
		CHECK(config.outputTransport == Color::OutputTransport::ScRgb);
        CHECK(config.paperWhiteNits == 250.0);
        CHECK(config.peakNits == 1200.0);
        CHECK(config.clusterTileSize == 32);
        CHECK(config.clusterDepthSlices == 32);
        CHECK(config.clusterStressLightCount == 512);
        CHECK(config.shadowSettings.directionalResolution == 4096);
        CHECK(config.shadowSettings.maximumDirectionalLights == 1);
        CHECK(config.shadowSettings.directionalSourceAngularDiameterDegrees ==
            1.25f);
        CHECK(config.shadowSettings.filterMode == ShadowFilterMode::FixedPcf);
        CHECK(config.shadowSettings.spotAtlasResolution == 8192);
        CHECK(config.disableBenchmarkLocalShadows);
        CHECK(config.debugView == RenderDebugView::ClusterOccupancy);
        CHECK(config.benchmarkId == "material_lab_v1");
        CHECK(config.benchmarkManifest.generic_string() ==
            "assets/benchmarks/m0/manifest.v1.json");
        CHECK(config.cookedModelArtifact.generic_string() ==
            "out/ddc/model.irartifact");
        CHECK(config.cookedEnvironmentArtifact.generic_string() ==
            "out/ddc/environment.irartifact");
        CHECK(config.editorAssetViewerGuid.has_value());
        CHECK(config.editorAssetViewerGuid->toString() ==
            "019fb73d-5a26-7326-8688-ea55a972179c");
        CHECK(config.captureFrameIndex == 0);
        CHECK(config.captureDirectory.generic_string() == "out/captures/run-a");
        CHECK(config.capturePoint == FrameCapturePoint::FinalOutput);
        return true;
    }

    bool testHelp() {
        constexpr std::array arguments{ std::string_view("--help") };
        CHECK(parseApplicationConfig(arguments).showHelp);
        CHECK(applicationUsage().find("--profile-cpu-output") != std::string::npos);
        CHECK(applicationUsage().find("--debug-view") != std::string::npos);
        CHECK(applicationUsage().find("--benchmark") != std::string::npos);
        CHECK(applicationUsage().find("--cooked-model-artifact") !=
            std::string::npos);
        CHECK(applicationUsage().find("--cooked-environment-artifact") !=
            std::string::npos);
        CHECK(applicationUsage().find("--open-asset-viewer") !=
            std::string::npos);
        CHECK(applicationUsage().find("--capture-frame") != std::string::npos);
        CHECK(applicationUsage().find("--capture-point") != std::string::npos);
        CHECK(applicationUsage().find("--cache-state") != std::string::npos);
        CHECK(applicationUsage().find("--render-graph-shadow") == std::string::npos);
        CHECK(applicationUsage().find("--canonical-materials") == std::string::npos);
        CHECK(applicationUsage().find("--show-material-diagnostics") != std::string::npos);
        CHECK(applicationUsage().find("--select-benchmark-entity") != std::string::npos);
        CHECK(applicationUsage().find("--benchmark-disable-local-shadows") !=
            std::string::npos);
        CHECK(applicationUsage().find("--wireframe") != std::string::npos);
        CHECK(applicationUsage().find("--gbuffer-layout") != std::string::npos);
        CHECK(applicationUsage().find("--material-descriptors") == std::string::npos);
        CHECK(applicationUsage().find("--validate-texture-residency-churn") !=
            std::string::npos);
        CHECK(applicationUsage().find("--validate-reflection-probes") !=
            std::string::npos);
        CHECK(applicationUsage().find("--validate-ordinary2-capture") !=
            std::string::npos);
        CHECK(applicationUsage().find("--validate-ordinary2-fallback") !=
            std::string::npos);
        CHECK(applicationUsage().find("--validate-ordinary2-resize") !=
            std::string::npos);
        CHECK(applicationUsage().find("--validate-deep-layered-capture") !=
            std::string::npos);
        CHECK(applicationUsage().find("--validate-deep-layered-lifecycle") !=
            std::string::npos);
        CHECK(applicationUsage().find("--deep-layered-validation-quality") !=
            std::string::npos);
        CHECK(applicationUsage().find("--validate-texture-table-scale") !=
            std::string::npos);
        CHECK(applicationUsage().find("--validate-material-table-scale") !=
            std::string::npos);
        CHECK(applicationUsage().find("--validate-light-table-scale") !=
            std::string::npos);
        CHECK(applicationUsage().find("--cluster-tile-size") != std::string::npos);
        CHECK(applicationUsage().find("--cluster-depth-slices") != std::string::npos);
        CHECK(applicationUsage().find("--cluster-stress-lights") != std::string::npos);
        CHECK(applicationUsage().find("--shadow-directional-resolution") !=
            std::string::npos);
        CHECK(applicationUsage().find("--shadow-directional-lights") !=
            std::string::npos);
        CHECK(applicationUsage().find("--shadow-spot-atlas-resolution") !=
            std::string::npos);
        CHECK(applicationUsage().find("--exposure-ev") != std::string::npos);
        CHECK(applicationUsage().find("--output-operator") != std::string::npos);
		CHECK(applicationUsage().find("--output-transport") != std::string::npos);
        CHECK(applicationUsage().find("--paper-white-nits") != std::string::npos);
        CHECK(applicationUsage().find("--peak-nits") != std::string::npos);
        return true;
    }

    bool testExplicitZeroRunOptions() {
        constexpr std::array arguments{
            std::string_view("--frame-limit"), std::string_view("0"),
            std::string_view("--warmup-frames"), std::string_view("0"),
        };
        const ApplicationConfig config = parseApplicationConfig(arguments);
        CHECK(config.frameLimit == 0);
        CHECK(config.warmupFrameCount == 0);
        CHECK(config.frameLimitSpecified);
        CHECK(config.warmupFrameCountSpecified);
        return true;
    }

    bool testInvalidOptions() {
        CHECK(rejects(std::array{ std::string_view("--unknown") }));
        CHECK(rejects(std::array{
            std::string_view("--shadow-directional-resolution"),
            std::string_view("1536") }));
        CHECK(rejects(std::array{
            std::string_view("--shadow-directional-lights"),
            std::string_view("3") }));
        CHECK(rejects(std::array{
            std::string_view("--shadow-directional-source-diameter"),
            std::string_view("5.1") }));
        CHECK(rejects(std::array{
            std::string_view("--shadow-filter"),
            std::string_view("variance") }));
        CHECK(rejects(std::array{
            std::string_view("--shadow-spot-atlas-resolution"),
            std::string_view("1024") }));
        CHECK(rejects(std::array{ std::string_view("--frame-limit") }));
        CHECK(rejects(std::array{ std::string_view("--warmup-frames") }));
        CHECK(rejects(std::array{
            std::string_view("--window-size"), std::string_view("0x2160") }));
        CHECK(rejects(std::array{
            std::string_view("--window-size"), std::string_view("3840") }));
        CHECK(rejects(std::array{ std::string_view("--profile-cpu-output") }));
        CHECK(rejects(std::array{ std::string_view("--debug-view") }));
        CHECK(rejects(std::array{ std::string_view("--cluster-tile-size"),
            std::string_view("8") }));
        CHECK(rejects(std::array{ std::string_view("--cluster-depth-slices"),
            std::string_view("16") }));
        CHECK(rejects(std::array{
            std::string_view("--wireframe"),
            std::string_view("--debug-view"),
            std::string_view("depth") }));
        CHECK(parseApplicationConfig(std::array{
            std::string_view("--debug-view"), std::string_view("material-id") }).debugView ==
            RenderDebugView::MaterialId);
        CHECK(parseApplicationConfig(std::array{
            std::string_view("--debug-view"),
            std::string_view("direct-lighting") }).debugView ==
            RenderDebugView::DirectLighting);
        CHECK(rejects(std::array{ std::string_view("--benchmark") }));
        CHECK(rejects(std::array{ std::string_view("--benchmark-manifest") }));
        CHECK(rejects(std::array{
            std::string_view("--cooked-model-artifact") }));
        CHECK(rejects(std::array{
            std::string_view("--cooked-environment-artifact") }));
        CHECK(rejects(std::array{
            std::string_view("--open-asset-viewer") }));
        CHECK(rejects(std::array{
            std::string_view("--open-asset-viewer"),
            std::string_view("not-a-guid") }));
        CHECK(rejects(std::array{
            std::string_view("--open-asset-viewer"),
            std::string_view("00000000-0000-0000-0000-000000000000") }));
        CHECK(rejects(std::array{ std::string_view("--capture-frame") }));
        CHECK(rejects(std::array{
            std::string_view("--capture-frame"), std::string_view("0") }));
        CHECK(rejects(std::array{
            std::string_view("--capture-directory"), std::string_view("out/captures") }));
        CHECK(rejects(std::array{ std::string_view("--capture-directory") }));
        CHECK(rejects(std::array{ std::string_view("--capture-point") }));
        CHECK(rejects(std::array{ std::string_view("--capture-point"),
            std::string_view("swapchain") }));
        CHECK(rejects(std::array{ std::string_view("--cache-state") }));
        CHECK(rejects(std::array{
            std::string_view("--cache-state"), std::string_view("cold") }));
        CHECK(rejects(std::array{ std::string_view("--gbuffer-layout"),
            std::string_view("legacy") }));
        CHECK(rejects(std::array{ std::string_view("--material-descriptors") }));
        CHECK(rejects(std::array{ std::string_view("--material-descriptors"),
            std::string_view("indexed") }));
        CHECK(rejects(std::array{
            std::string_view("--validate-texture-residency-churn"),
            std::string_view("--material-descriptors"),
            std::string_view("compatibility") }));
        CHECK(rejects(std::array{
            std::string_view("--validate-texture-table-scale") }));
        CHECK(rejects(std::array{
            std::string_view("--validate-texture-table-scale"),
            std::string_view("0") }));
        CHECK(rejects(std::array{
            std::string_view("--validate-texture-table-scale"),
            std::string_view("65536") }));
        CHECK(rejects(std::array{
            std::string_view("--validate-material-table-scale") }));
        CHECK(rejects(std::array{
            std::string_view("--validate-material-table-scale"),
            std::string_view("0") }));
        CHECK(rejects(std::array{
            std::string_view("--validate-material-table-scale"),
            std::string_view("65537") }));
        CHECK(rejects(std::array{
            std::string_view("--validate-light-table-scale") }));
        CHECK(rejects(std::array{
            std::string_view("--validate-light-table-scale"),
            std::string_view("0") }));
        CHECK(rejects(std::array{
            std::string_view("--validate-light-table-scale"),
            std::string_view("65537") }));
        CHECK(rejects(std::array{
            std::string_view("--deep-layered-validation-quality") }));
        CHECK(rejects(std::array{
            std::string_view("--deep-layered-validation-quality"),
            std::string_view("ordinary2") }));
        CHECK(rejects(std::array{ std::string_view("--exposure-ev") }));
        CHECK(rejects(std::array{
            std::string_view("--exposure-ev"), std::string_view("17") }));
        CHECK(rejects(std::array{
            std::string_view("--exposure-ev"), std::string_view("nan") }));
        CHECK(rejects(std::array{ std::string_view("--output-operator") }));
        CHECK(rejects(std::array{ std::string_view("--output-operator"),
            std::string_view("reinhard") }));
		CHECK(rejects(std::array{ std::string_view("--output-transport") }));
		CHECK(rejects(std::array{ std::string_view("--output-transport"),
			std::string_view("dolby-vision") }));
        CHECK(rejects(std::array{ std::string_view("--output-transport"),
            std::string_view("scrgb"), std::string_view("--output-operator"),
            std::string_view("legacy") }));
        CHECK(rejects(std::array{ std::string_view("--paper-white-nits"),
            std::string_view("20") }));
        CHECK(rejects(std::array{ std::string_view("--paper-white-nits"),
            std::string_view("500"), std::string_view("--peak-nits"),
            std::string_view("400") }));
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*run)();
    };

    constexpr TestCase tests[] = {
        { "Explicit validation/profiling", testExplicitValidationAndProfiling },
        { "M2 production defaults", testM2ProductionDefaults },
        { "Bounded run options", testBoundedRunOptions },
        { "Help", testHelp },
        { "Explicit zero run options", testExplicitZeroRunOptions },
        { "Invalid options", testInvalidOptions },
    };

    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.run()) {
                std::cout << "[PASS] " << test.name << '\n';
            }
            else {
                ++failures;
                std::cerr << "[FAIL] " << test.name << '\n';
            }
        }
        catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << exception.what() << '\n';
        }
    }

    constexpr size_t testCount = sizeof(tests) / sizeof(tests[0]);
    std::cout << testCount - failures << '/' << testCount << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
