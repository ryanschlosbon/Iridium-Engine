#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Iridium::Color {

    enum class OutputOperator : uint8_t {
        Aces2,
        AcesFittedLegacy,
        IdentityClampDiagnostic,
    };

    enum class OutputTransport : uint8_t {
        SdrSrgb,
        ScRgb,
        Hdr10Pq,
    };

    enum class OutputProfile : uint8_t {
        WindowsSdrSrgb,
        WindowsScRgbP3D65,
        Hdr10P3D65InRec2020,
    };

    enum class OutputTransfer : uint8_t {
        Srgb,
        LinearScRgb,
        St2084Pq,
    };

    struct Chromaticities {
        double redX = 0.0;
        double redY = 0.0;
        double greenX = 0.0;
        double greenY = 0.0;
        double blueX = 0.0;
        double blueY = 0.0;
        double whiteX = 0.0;
        double whiteY = 0.0;
    };

    struct OutputTransformConfig {
        uint32_t schemaVersion = 1;
        OutputOperator outputOperator = OutputOperator::Aces2;
        OutputTransport transport = OutputTransport::SdrSrgb;
        OutputProfile profile = OutputProfile::WindowsSdrSrgb;
        OutputTransfer transfer = OutputTransfer::Srgb;
        Chromaticities targetChromaticities{};
        Chromaticities encodingChromaticities{};
        double manualExposureEv = 0.0;
        double requestedPaperWhiteNits = 203.0;
        double requestedPeakNits = 1000.0;
        double masteringMinimumNits = 0.0;
        double maxCllNits = 0.0;
        double maxFallNits = 0.0;
        double effectivePaperWhiteNits = 100.0;
        double effectivePeakNits = 100.0;
        double scRgbNitsPerUnit = 80.0;
        std::string acesPackage = "v2.0.0+2025.04.04";
        std::string acesTransform = "ACES2.OutputTransform.Core";
    };

    struct OutputConfigValidation {
        OutputTransformConfig effective;
        std::vector<std::string> diagnostics;
        bool usedFallback = false;
    };

    [[nodiscard]] OutputTransformConfig makeDefaultOutputTransformConfig();
    [[nodiscard]] OutputConfigValidation validatePersistedOutputTransformConfig(
        const OutputTransformConfig& persisted);
    void requireValidOutputTransformConfig(const OutputTransformConfig& config);

} // namespace Iridium::Color

namespace Iridium {
    using OutputTransformOperator = Color::OutputOperator;
}
