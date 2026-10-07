// M9.8c: the persisted Project Settings file round-trips the authored subset,
// tolerates missing and unknown fields, and reports (without applying) a
// malformed file.
#include "app/ApplicationConfig.h"
#include "app/ProjectSettingsFile.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

    using namespace Iridium;

    int failures = 0;
#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            std::cerr << "check failed: " #condition " (" << __FILE__ << ":" << __LINE__ \
                      << ")\n";                                                       \
            ++failures;                                                               \
        }                                                                             \
    } while (false)

    std::filesystem::path scratch(const char* name) {
        const std::filesystem::path directory =
            std::filesystem::temp_directory_path() / "iridium-project-settings-tests";
        std::filesystem::create_directories(directory);
        return directory / name;
    }

    void testRoundTrip() {
        ApplicationConfig authored{};
        authored.antiAliasing = AntiAliasingMode::None;
        TemporalAntiAliasingTuning taa{};
        taa.minimumHistoryWeight = 0.85f;
        taa.motionPixelsForMinimum = 6.0f;
        authored.taaTuning = taa;
        authored.exposureMode = ExposureMode::Manual;
        AutoExposureSettings exposure{};
        exposure.adaptation = ExposureAdaptation::Linear;
        exposure.speedUpEvPerSecond = 0.75f;
        exposure.maximumEvPerSecond = 2.5f;
        exposure.minimumEv100 = 3.0f;
        authored.autoExposureSettings = exposure;
        authored.bloom.enabled = false;
        authored.bloom.intensity = 0.1f;
        authored.bloom.radius = 0.4f;
        authored.bloom.threshold = 1.5f;
        authored.bloom.tint = { 1.0f, 0.8f, 0.6f };
        authored.bloom.karis = BloomKarisMode::On;
        authored.manualExposureEv = 1.25;
        authored.paperWhiteNits = 240.0;
        authored.shadowSettings.qualityProfile = ShadowQualityProfile::High;
        authored.shadowSettings.maximumPenumbraTexels = 32.0f;
        authored.reflectionProbeSettings.prefilterSampleCount = 128;

        const std::filesystem::path path = scratch("roundtrip.json");
        std::string diagnostic;
        CHECK(saveProjectSettings(path, authored, diagnostic));
        CHECK(diagnostic.empty());

        ApplicationConfig loaded{};
        CHECK(loadProjectSettings(path, loaded, diagnostic));
        CHECK(loaded.antiAliasing == AntiAliasingMode::None);
        CHECK(loaded.taaTuning == authored.taaTuning);
        CHECK(loaded.exposureMode == ExposureMode::Manual);
        CHECK(loaded.autoExposureSettings == authored.autoExposureSettings);
        CHECK(loaded.bloom == authored.bloom);
        CHECK(loaded.manualExposureEv == authored.manualExposureEv);
        CHECK(loaded.paperWhiteNits == authored.paperWhiteNits);
        CHECK(loaded.shadowSettings.qualityProfile == ShadowQualityProfile::High);
        CHECK(loaded.shadowSettings.maximumPenumbraTexels == 32.0f);
        CHECK(loaded.reflectionProbeSettings.prefilterSampleCount == 128u);
        // Machine-specific state is not persisted.
        CHECK(loaded.outputTransport == ApplicationConfig{}.outputTransport);
    }

    void testTolerance() {
        const std::filesystem::path path = scratch("partial.json");
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << R"({ "schema_version": 1, "future_section": { "x": 1 },
                "bloom": { "intensity": 0.2, "radius": "not a number", "unknown": true },
                "exposure": { "mode": "sideways" } })";
        }
        ApplicationConfig config{};
        const ApplicationConfig defaults{};
        std::string diagnostic;
        CHECK(loadProjectSettings(path, config, diagnostic));
        CHECK(config.bloom.intensity == 0.2f);
        CHECK(config.bloom.radius == defaults.bloom.radius);       // wrong type: kept
        CHECK(config.exposureMode == defaults.exposureMode);       // unknown name: kept
        CHECK(config.antiAliasing == defaults.antiAliasing);       // missing: kept

        // A missing file is not an error and changes nothing.
        ApplicationConfig untouched{};
        CHECK(loadProjectSettings(scratch("does-not-exist.json"), untouched, diagnostic));
        CHECK(diagnostic.empty());
        CHECK(untouched.bloom == defaults.bloom);
    }

    void testMalformed() {
        const std::filesystem::path path = scratch("malformed.json");
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << "{ \"bloom\": { \"intensity\": 0.2 ";
        }
        ApplicationConfig config{};
        std::string diagnostic;
        CHECK(!loadProjectSettings(path, config, diagnostic));
        CHECK(!diagnostic.empty());
        CHECK(config.bloom == ApplicationConfig{}.bloom);
    }

} // namespace

int main() {
    testRoundTrip();
    testTolerance();
    testMalformed();
    if (failures == 0) std::cout << "[PASS] Project settings file\n";
    return failures == 0 ? 0 : 1;
}
