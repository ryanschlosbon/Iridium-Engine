#include "renderer/color/OutputTransformConfig.h"

#include <cmath>
#include <stdexcept>

namespace Iridium::Color {

    namespace {

        constexpr Chromaticities Rec709D65{
            0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290 };
        constexpr Chromaticities P3D65{
            0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290 };
        constexpr Chromaticities Rec2020D65{
            0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290 };

        [[nodiscard]] bool finite(double value) noexcept {
            return std::isfinite(value);
        }

        void applyProfile(OutputTransformConfig& config) {
            switch (config.profile) {
            case OutputProfile::WindowsSdrSrgb:
                config.transport = OutputTransport::SdrSrgb;
                config.transfer = OutputTransfer::Srgb;
                config.targetChromaticities = Rec709D65;
                config.encodingChromaticities = Rec709D65;
                config.effectivePaperWhiteNits = 100.0;
                config.effectivePeakNits = 100.0;
                break;
            case OutputProfile::WindowsScRgbP3D65:
                config.transport = OutputTransport::ScRgb;
                config.transfer = OutputTransfer::LinearScRgb;
                config.targetChromaticities = P3D65;
                config.encodingChromaticities = Rec709D65;
                config.effectivePaperWhiteNits = config.requestedPaperWhiteNits;
                config.effectivePeakNits = config.requestedPeakNits;
                break;
            case OutputProfile::Hdr10P3D65InRec2020:
                config.transport = OutputTransport::Hdr10Pq;
                config.transfer = OutputTransfer::St2084Pq;
                config.targetChromaticities = P3D65;
                config.encodingChromaticities = Rec2020D65;
                config.effectivePaperWhiteNits = config.requestedPaperWhiteNits;
                config.effectivePeakNits = config.requestedPeakNits;
                break;
            }
        }

        [[nodiscard]] std::string invalidReason(const OutputTransformConfig& config) {
            if (config.schemaVersion != 1) return "unsupported schema version";
            if (!finite(config.manualExposureEv) || config.manualExposureEv < -16.0 ||
                config.manualExposureEv > 16.0) return "manual exposure is outside [-16, 16] EV";
            if (!finite(config.requestedPaperWhiteNits) ||
                config.requestedPaperWhiteNits < 80.0) return "paper white is below 80 nits";
            if (!finite(config.requestedPeakNits) ||
                config.requestedPeakNits < config.requestedPaperWhiteNits ||
                config.requestedPeakNits > 10000.0) return "peak luminance is invalid";
            if (!finite(config.masteringMinimumNits) || config.masteringMinimumNits < 0.0 ||
                !finite(config.maxCllNits) || config.maxCllNits < 0.0 ||
                !finite(config.maxFallNits) || config.maxFallNits < 0.0) {
                return "optional luminance metadata is invalid";
            }
            if (config.acesPackage != "v2.0.0+2025.04.04") {
                return "ACES package does not match the frozen M1 release";
            }
            if (config.acesTransform != "ACES2.OutputTransform.Core") {
                return "ACES transform identity is unsupported";
            }
            return {};
        }

    } // namespace

    OutputTransformConfig makeDefaultOutputTransformConfig() {
        OutputTransformConfig result{};
        applyProfile(result);
        return result;
    }

    void requireValidOutputTransformConfig(const OutputTransformConfig& config) {
        if (const std::string reason = invalidReason(config); !reason.empty()) {
            throw std::invalid_argument(reason);
        }
    }

    OutputConfigValidation validatePersistedOutputTransformConfig(
        const OutputTransformConfig& persisted) {
        OutputConfigValidation result{};
        const std::string reason = invalidReason(persisted);
        if (!reason.empty()) {
            result.effective = makeDefaultOutputTransformConfig();
            result.diagnostics.push_back("Persisted output config reset: " + reason);
            result.usedFallback = true;
            return result;
        }
        result.effective = persisted;
        applyProfile(result.effective);
        return result;
    }

} // namespace Iridium::Color
