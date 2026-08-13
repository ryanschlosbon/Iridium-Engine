#include "renderer/color/SceneColor.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

    using namespace Iridium::Color;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    [[nodiscard]] bool near(double actual, double expected,
        double tolerance = 1.0e-9) {
        return std::abs(actual - expected) <= tolerance;
    }

    bool testSrgbTransferReferenceValues() {
        CHECK(near(decodeSrgb(0.0), 0.0));
        CHECK(near(decodeSrgb(0.04045), 0.00313080495356037, 1.0e-12));
        CHECK(near(decodeSrgb(0.5), 0.214041140482233, 1.0e-12));
        CHECK(near(decodeSrgb(1.0), 1.0));
        constexpr std::array<double, 6> samples{ 0.0, 0.01, 0.18, 0.5, 0.9, 1.0 };
        for (const double sample : samples) {
            CHECK(near(encodeSrgb(decodeSrgb(sample)), sample, 1.0e-12));
        }
        return true;
    }

    bool testLinearPrimaryVectors() {
        const Rgb red = linearSrgbToAcesCg({ 1.0, 0.0, 0.0 });
        CHECK(near(red.r, 0.6130974024));
        CHECK(near(red.g, 0.0701937225));
        CHECK(near(red.b, 0.0206155929));
        const Rgb green = linearSrgbToAcesCg({ 0.0, 1.0, 0.0 });
        CHECK(near(green.r, 0.3395231462));
        CHECK(near(green.g, 0.9163538791));
        CHECK(near(green.b, 0.1095697729));
        const Rgb blue = linearSrgbToAcesCg({ 0.0, 0.0, 1.0 });
        CHECK(near(blue.r, 0.0473794514));
        CHECK(near(blue.g, 0.0134523984));
        CHECK(near(blue.b, 0.8698146342));
        return true;
    }

    bool testWhiteAndRoundTrip() {
        const Rgb white = linearSrgbToAcesCg({ 1.0, 1.0, 1.0 });
        CHECK(near(white.r, 1.0));
        CHECK(near(white.g, 1.0));
        CHECK(near(white.b, 1.0));
        constexpr std::array<Rgb, 4> samples{
            Rgb{ 0.0, 0.0, 0.0 }, Rgb{ 0.18, 0.18, 0.18 },
            Rgb{ 0.02, 0.5, 4.0 }, Rgb{ -0.1, 2.0, 16.0 },
        };
        for (const Rgb sample : samples) {
            const Rgb restored = acesCgToLinearSrgb(linearSrgbToAcesCg(sample));
            CHECK(near(restored.r, sample.r, 2.0e-10));
            CHECK(near(restored.g, sample.g, 2.0e-10));
            CHECK(near(restored.b, sample.b, 2.0e-10));
        }
        return true;
    }

    bool testHalfFloatDecode() {
        CHECK(halfToFloat(0x0000u) == 0.0f);
        CHECK(halfToFloat(0x3c00u) == 1.0f);
        CHECK(halfToFloat(0x3800u) == 0.5f);
        CHECK(halfToFloat(0xc000u) == -2.0f);
        CHECK(halfToFloat(0x7bffu) == 65504.0f);
        CHECK(std::isinf(halfToFloat(0x7c00u)));
        CHECK(std::isnan(halfToFloat(0x7e00u)));
        return true;
    }

	bool testSt2084ReferenceVectors() {
		CHECK(near(encodeSt2084FromNits(0.0), 7.309559025783966e-7,
			1.0e-15));
		CHECK(near(encodeSt2084FromNits(0.1), 0.06233686566269587, 1.0e-12));
		CHECK(near(encodeSt2084FromNits(1.0), 0.14994573210018022, 1.0e-12));
		CHECK(near(encodeSt2084FromNits(100.0), 0.508078421517399, 1.0e-12));
		CHECK(near(encodeSt2084FromNits(1000.0), 0.751827096247041, 1.0e-12));
		CHECK(near(encodeSt2084FromNits(10000.0), 1.0));
		for (const double nits : { 0.0, 0.1, 1.0, 100.0, 203.0, 1000.0, 10000.0 }) {
			CHECK(near(decodeSt2084ToNits(encodeSt2084FromNits(nits)),
				nits, 2.0e-8 * std::max(1.0, nits)));
		}
		return true;
	}

	bool testScRgbAbsoluteLuminanceAndGamut() {
		const double pq203 = encodeSt2084FromNits(203.0);
		const Rgb neutral = rec2100PqToScRgb({ pq203, pq203, pq203 });
		CHECK(near(neutral.r, 203.0 / 80.0, 2.0e-9));
		CHECK(near(neutral.g, 203.0 / 80.0, 2.0e-9));
		CHECK(near(neutral.b, 203.0 / 80.0, 2.0e-9));
		const Rgb red = linearRec2020ToLinearSrgb({ 1.0, 0.0, 0.0 });
		CHECK(near(red.r, 1.6604910021));
		CHECK(red.g < 0.0);
		CHECK(red.b < 0.0);
		const Rgb uiWhite = srgbUiToScRgb({ 1.0, 1.0, 1.0 }, 203.0);
		CHECK(near(uiWhite.r, 2.5375));
		CHECK(near(uiWhite.g, 2.5375));
		CHECK(near(uiWhite.b, 2.5375));
		const Rgb dimmedUiWhite = srgbUiToScRgb({ 1.0, 1.0, 1.0 }, 120.0);
		CHECK(near(dimmedUiWhite.r, 1.5));
		CHECK(near(dimmedUiWhite.g, 1.5));
		CHECK(near(dimmedUiWhite.b, 1.5));
		return true;
	}

    [[nodiscard]] std::string readText(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>() };
    }

    bool testShaderColorBoundary() {
        const std::filesystem::path shaders =
            std::filesystem::path(PROJECT_ROOT_DIR) / "assets/shaders";
        const std::string gbuffer = readText(
            shaders / "include/canonical_gbuffer_body.glsl");
        const std::string lighting = readText(
            shaders / "include/canonical_lighting_body.glsl");
        const std::string forward = readText(
            shaders / "include/complex_material_body.glsl");
        const std::string output = readText(shaders / "output.frag");
        const std::string ui = readText(shaders / "imgui_color_managed.frag");
        const std::string selectionMask = readText(shaders / "canonical_mask.frag");
        const std::string shared = readText(shaders / "include/scene_color.glsl");
        CHECK(gbuffer.find("linearSrgbToAcesCg") != std::string::npos);
        CHECK(forward.find("linearSrgbToAcesCg") != std::string::npos);
        CHECK(lighting.find("linearSrgbToAcesCg") == std::string::npos);
        CHECK(lighting.find("include/environment_ibl.glsl") !=
            std::string::npos);
        CHECK(lighting.find("ACESFilm") == std::string::npos);
        CHECK(forward.find("ACESFilm") == std::string::npos);
        CHECK(ui.find("decodeSrgb(In.Color.rgb)") != std::string::npos);
        CHECK(ui.find("decodeSrgb(sampled.rgb)") == std::string::npos);
        CHECK(output.find("acesCgToLinearSrgb") != std::string::npos);
        CHECK(output.find("2.51") != std::string::npos);
        CHECK(output.find("push.selectionActive == 0u") != std::string::npos);
        CHECK(output.find("selectionMask") != std::string::npos);
        CHECK(shared.find("0.6130974024") != std::string::npos);
        CHECK(shared.find("1.705050992697") != std::string::npos);
        const std::string application = readText(
            std::filesystem::path(PROJECT_ROOT_DIR) / "src/core/Application.cpp");
        const std::string backend = readText(
            std::filesystem::path(PROJECT_ROOT_DIR) /
            "src/renderer/vulkan/VulkanVertexBackend.cpp");
        const std::string settings = readText(
            std::filesystem::path(PROJECT_ROOT_DIR) /
            "src/editor/panels/windows/ProjectSettingsPanel.cpp");
        CHECK(application.find("setOutputSettings") != std::string::npos);
        CHECK(backend.find("ImGui_ImplVulkan_SetDisplayColorConfiguration") !=
            std::string::npos);
        CHECK(settings.find("UI / paper white (nits)") != std::string::npos);
        CHECK(settings.find("currently outputting SDR") != std::string::npos);
        CHECK(selectionMask.find("outDiffuseAo = vec4(0.0);") != std::string::npos);
        CHECK(selectionMask.find("outEmissive = vec4(0.0, 0.0, 0.0, -1.0)") !=
            std::string::npos);
        const size_t framePasses = application.find("// Pass 3:");
        const size_t sceneCapture = application.find("captureCurrentFrame", framePasses);
        const size_t outputPass = application.find("submitOutputPass", sceneCapture);
        const size_t finalCapture = application.find("captureCurrentFrame", outputPass);
        const size_t uiPass = application.find("submitUIPass", finalCapture);
        CHECK(framePasses != std::string::npos);
        CHECK(sceneCapture < outputPass);
        CHECK(outputPass < finalCapture);
        CHECK(finalCapture < uiPass);
        return true;
    }

} // namespace

int main() {
    const bool transfer = testSrgbTransferReferenceValues();
    const bool primaries = testLinearPrimaryVectors();
    const bool roundTrip = testWhiteAndRoundTrip();
    const bool half = testHalfFloatDecode();
	const bool pq = testSt2084ReferenceVectors();
	const bool scRgb = testScRgbAbsoluteLuminanceAndGamut();
    const bool shaders = testShaderColorBoundary();
    std::cout << (transfer + primaries + roundTrip + half + pq + scRgb + shaders)
        << "/7 tests passed\n";
    return transfer && primaries && roundTrip && half && pq && scRgb && shaders
		? 0 : 1;
}
