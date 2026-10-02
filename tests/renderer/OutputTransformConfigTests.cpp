#include "renderer/color/OutputTransformConfig.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace {
    using namespace Iridium::Color;
    #define CHECK(x) do { if (!(x)) { std::cerr << "check failed: " #x \
        << " line " << __LINE__ << '\n'; return false; } } while (false)

    bool testDefaults() {
        const OutputTransformConfig config = makeDefaultOutputTransformConfig();
        CHECK(config.outputOperator == OutputOperator::Aces2);
        CHECK(config.profile == OutputProfile::WindowsSdrSrgb);
        CHECK(config.transport == OutputTransport::SdrSrgb);
        CHECK(config.transfer == OutputTransfer::Srgb);
        CHECK(config.manualExposureEv == 0.0);
        CHECK(config.requestedPaperWhiteNits == 203.0);
        CHECK(config.requestedPeakNits == 1000.0);
        CHECK(config.effectivePaperWhiteNits == 100.0);
        CHECK(config.effectivePeakNits == 100.0);
        CHECK(config.acesPackage == "v2.0.0+2025.04.04");
        return true;
    }

    bool testHdrProfiles() {
        OutputTransformConfig config = makeDefaultOutputTransformConfig();
        config.profile = OutputProfile::WindowsScRgbP3D65;
        auto validated = validatePersistedOutputTransformConfig(config);
        CHECK(!validated.usedFallback);
        CHECK(validated.effective.transport == OutputTransport::ScRgb);
        CHECK(validated.effective.transfer == OutputTransfer::LinearScRgb);
        CHECK(validated.effective.effectivePaperWhiteNits == 203.0);
        CHECK(validated.effective.effectivePeakNits == 1000.0);
        CHECK(validated.effective.scRgbNitsPerUnit == 80.0);
        config.profile = OutputProfile::Hdr10P3D65InRec2020;
        validated = validatePersistedOutputTransformConfig(config);
        CHECK(validated.effective.transport == OutputTransport::Hdr10Pq);
        CHECK(validated.effective.transfer == OutputTransfer::St2084Pq);
        CHECK(validated.effective.encodingChromaticities.redX == 0.708);
        return true;
    }

    bool testBoundsAndFallback() {
        OutputTransformConfig config = makeDefaultOutputTransformConfig();
        config.manualExposureEv = -16.0;
        requireValidOutputTransformConfig(config);
        config.manualExposureEv = 16.0;
        requireValidOutputTransformConfig(config);
        config.manualExposureEv = std::numeric_limits<double>::quiet_NaN();
        const auto fallback = validatePersistedOutputTransformConfig(config);
        CHECK(fallback.usedFallback);
        CHECK(!fallback.diagnostics.empty());
        CHECK(fallback.effective.manualExposureEv == 0.0);
        bool rejected = false;
        try { requireValidOutputTransformConfig(config); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
        return true;
    }
}

int main() {
    const bool defaults = testDefaults();
    const bool profiles = testHdrProfiles();
    const bool bounds = testBoundsAndFallback();
    std::cout << (defaults + profiles + bounds) << "/3 tests passed\n";
    return defaults && profiles && bounds ? 0 : 1;
}
