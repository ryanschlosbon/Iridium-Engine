#include "renderer/lighting/LightingReference.h"
#include "material/StandardMaterialShading.h"
#include "scene/lighting/LightPhotometry.h"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>

namespace {

    namespace Reference = Iridium::LightingReference;

    #define CHECK(condition) do { if (!(condition)) { \
        std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
        return false; } } while (false)

    bool near(double actual, long double expected, double absolute = 1.0e-5,
        double relative = 1.0e-4) {
        if (!std::isfinite(actual) || !std::isfinite(static_cast<double>(expected))) {
            return false;
        }
        const long double difference = std::abs(
            static_cast<long double>(actual) - expected);
        return difference <= absolute || difference <=
            relative * std::max(std::abs(expected), 1.0L);
    }

    bool testPhotometricConversions() {
        const long double pi = std::numbers::pi_v<long double>;
        CHECK(near(Reference::pointCandelaFromLumens(4.0 *
            std::numbers::pi), 1.0L));
        CHECK(near(Reference::spotEffectiveSolidAngle(0.0,
            std::numbers::pi / 3.0), pi / 2.0L));
        CHECK(near(Reference::spotCandelaFromLumens(
            static_cast<double>(pi), 0.0, std::numbers::pi / 3.0), 2.0L));
        CHECK(Reference::pointCandelaFromLumens(-1.0) == 0.0);
        CHECK(Reference::pointCandelaFromLumens(
            std::numeric_limits<double>::infinity()) == 0.0);
        CHECK(Reference::spotEffectiveSolidAngle(1.0, 0.5) == 0.0);
        CHECK(Reference::spotEffectiveSolidAngle(0.0,
            std::numeric_limits<double>::quiet_NaN()) == 0.0);
        CHECK(near(Iridium::pointLumensFromCandela(1.0f), 4.0L * pi));
        CHECK(near(Iridium::pointCandelaFromLumens(
            4.0f * std::numbers::pi_v<float>), 1.0L));
        const float spotLumens = Iridium::spotLumensFromCandela(
            2.0f, 0.0f, 60.0f);
        CHECK(near(spotLumens, pi));
        CHECK(near(Iridium::spotCandelaFromLumens(
            spotLumens, 0.0f, 60.0f), 2.0L));
        CHECK(near(100000.0f * Iridium::kPhotometricToSceneScale, 10.0L));
        const glm::vec3 white = Iridium::normalizedAp1LightChromaticity(
            glm::vec3(1.0f));
        CHECK(near(white.y, 1.0L));
        CHECK(glm::all(glm::greaterThanEqual(white, glm::vec3(0.0f))));
        const glm::vec3 srgb = Iridium::linearRec709ToSrgb(glm::vec3(0.18f));
        const glm::vec3 linear = Iridium::srgbToLinearRec709(srgb);
        CHECK(near(linear.x, 0.18L));
        return true;
    }

    bool testDistanceAndRangeAttenuation() {
        CHECK(near(Reference::smoothRangeWindow(0.0, 10.0), 1.0L));
        CHECK(near(Reference::smoothRangeWindow(5.0, 10.0),
            (15.0L / 16.0L) * (15.0L / 16.0L)));
        CHECK(Reference::smoothRangeWindow(10.0, 10.0) == 0.0);
        CHECK(Reference::smoothRangeWindow(11.0, 10.0) == 0.0);
        CHECK(near(Reference::inverseSquareRangeAttenuation(0.0, 10.0, 0.25),
            16.0L));
        CHECK(near(Reference::inverseSquareRangeAttenuation(5.0, 10.0, 0.0),
            (15.0L / 16.0L) * (15.0L / 16.0L) / 25.0L));
        CHECK(std::isfinite(Reference::inverseSquareRangeAttenuation(
            0.0, 10.0, 0.0)));
        CHECK(Reference::inverseSquareRangeAttenuation(-1.0, 10.0, 0.0) == 0.0);
        CHECK(Reference::inverseSquareRangeAttenuation(1.0, 0.0, 0.0) == 0.0);
        return true;
    }

    bool testSpotConeBoundaries() {
        const double inner = std::cos(std::numbers::pi / 12.0);
        const double outer = std::cos(std::numbers::pi / 6.0);
        CHECK(Reference::spotConeAttenuation(inner, inner, outer) == 1.0);
        CHECK(Reference::spotConeAttenuation(outer, inner, outer) == 0.0);
        CHECK(near(Reference::spotConeAttenuation(
            (inner + outer) * 0.5, inner, outer), 0.5L));
        CHECK(Reference::spotConeAttenuation(0.0, inner, outer) == 0.0);
        CHECK(Reference::spotConeAttenuation(1.0, outer, inner) == 0.0);
        CHECK(Reference::spotConeAttenuation(0.5, 0.5, 0.5) == 1.0);
        return true;
    }

    bool testDirectRadianceVectors() {
        CHECK(near(Reference::directionalRadiance(1.0, 100'000.0), 10.0L));
        CHECK(near(Reference::directionalRadiance(0.5, 50'000.0), 2.5L));
        CHECK(Reference::directionalRadiance(-1.0, 100'000.0) == 0.0);

        const long double window =
            (1.0L - 16.0L / 10'000.0L) *
            (1.0L - 16.0L / 10'000.0L);
        CHECK(near(Reference::localRadiance(
            1.0, 1'000.0, 2.0, 10.0, 0.0), 0.025L * window));
        CHECK(near(Reference::localRadiance(
            1.0, 1'000.0, 0.0, 10.0, 0.25), 1.6L));
        CHECK(near(Reference::localRadiance(
            0.5, 1'000.0, 2.0, 10.0, 0.0, 0.5),
            0.00625L * window));
        CHECK(Reference::localRadiance(
            1.0, 1'000.0, 10.0, 10.0, 0.0) == 0.0);
        CHECK(Reference::localRadiance(
            1.0, 1'000.0, 2.0, 10.0, 0.0, -1.0) == 0.0);
        return true;
    }

    bool testBsdfAndIblEdges() {
        CHECK(near(Reference::lambertianDiffuse(1.0),
            1.0L / std::numbers::pi_v<long double>));
        CHECK(Reference::lambertianDiffuse(-1.0) == 0.0);
        CHECK(near(Reference::iblSplitSum(0.04, 1.0, 0.8, 0.1), 0.132L));
        CHECK(near(Reference::iblSplitSum(0.0, 1.0, 0.0, 1.0), 1.0L));
        CHECK(Reference::iblSplitSum(std::numeric_limits<double>::quiet_NaN(),
            1.0, 1.0, 1.0) == 0.0);

        const glm::vec3 normal(0.0f, 0.0f, 1.0f);
        const glm::vec3 value = Iridium::materialEvaluateStandardBrdf(
            glm::vec3(0.5f), glm::vec3(0.04f), glm::vec3(1.0f), 0.0f, 0.5f,
            normal, normal, normal);
        CHECK(std::isfinite(value.x) && std::isfinite(value.y) &&
            std::isfinite(value.z));
        CHECK(glm::all(glm::greaterThanEqual(value, glm::vec3(0.0f))));
        return true;
    }

}

int main() {
    struct Test { const char* name; bool (*run)(); };
    constexpr std::array tests{
        Test{ "photometric conversions", testPhotometricConversions },
        Test{ "distance and range attenuation", testDistanceAndRangeAttenuation },
        Test{ "spot cone boundaries", testSpotConeBoundaries },
        Test{ "direct radiance vectors", testDirectRadianceVectors },
        Test{ "BSDF and IBL edges", testBsdfAndIblEdges },
    };
    for (const Test& test : tests) {
        std::cout << test.name << '\n';
        if (!test.run()) return 1;
    }
    std::cout << "M5 lighting reference tests passed\n";
    return 0;
}
