#include "renderer/color/AcesOutputLut.h"

#include <cmath>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>

namespace {
    bool near(float actual, float expected, float tolerance = 0.0003f) {
        return std::abs(actual - expected) <= tolerance;
    }
}

int main() {
    const std::filesystem::path path = std::filesystem::path(PROJECT_ROOT_DIR) /
        "assets/color/aces2_rec709_100nit_srgb_128.irlt";
    const Iridium::Color::AcesOutputLut lut =
        Iridium::Color::loadAcesOutputLut(path);
    bool valid = lut.size == 128 && lut.width() == 16384 &&
        lut.height() == 128 && lut.minimumLog2 == -10.0f &&
        lut.maximumLog2 == 16.0f &&
        lut.rgba32f.size() == 128ull * 128ull * 128ull * 4ull &&
        lut.rgba32f[0] == 0.0f && lut.rgba32f[1] == 0.0f &&
        lut.rgba32f[2] == 0.0f && lut.rgba32f[3] == 1.0f;
    struct Vector { std::array<float, 3> input; std::array<float, 3> expected; };
    constexpr std::array vectors{
        Vector{{0.18f, 0.18f, 0.18f}, {0.3491874f, 0.3491881f, 0.3491878f}},
        Vector{{1.0f, 1.0f, 1.0f}, {0.7066830f, 0.7066836f, 0.7066833f}},
        Vector{{4.0f, 4.0f, 4.0f}, {0.8998693f, 0.8998691f, 0.8998693f}},
        Vector{{1.0f, 0.0f, 0.0f}, {0.8771549f, 0.0f, 0.0402514f}},
        Vector{{0.0f, 1.0f, 0.0f}, {0.0f, 0.7144404f, 0.2748624f}},
        Vector{{0.0f, 0.0f, 1.0f}, {0.0f, 0.2105445f, 0.6467322f}},
    };
    for (const Vector& vector : vectors) {
        const auto actual = Iridium::Color::sampleAcesOutputLutEncoded(
            lut, vector.input);
        for (size_t channel = 0; channel < 3; ++channel) {
            if (!near(actual[channel], vector.expected[channel])) {
                std::cerr << "vector mismatch channel " << channel << ": "
                    << actual[channel] << " expected " << vector.expected[channel]
                    << '\n';
                valid = false;
            }
        }
    }
    float previous = -1.0f;
    for (float neutral : { 0.0f, 0.001f, 0.01f, 0.18f, 1.0f, 4.0f,
        64.0f, 65536.0f }) {
        const auto actual = Iridium::Color::sampleAcesOutputLutEncoded(
            lut, { neutral, neutral, neutral });
        if (!std::isfinite(actual[0]) || actual[0] < previous ||
            actual[0] < 0.0f || actual[0] > 1.0f) {
            std::cerr << "neutral mismatch at " << neutral << ": " << actual[0]
                << " after " << previous << '\n';
            valid = false;
        }
        previous = actual[0];
    }
    const auto hostile = Iridium::Color::sampleAcesOutputLutEncoded(lut,
        { -1.0f, std::numeric_limits<float>::quiet_NaN(),
          std::numeric_limits<float>::infinity() });
    if (!std::isfinite(hostile[0]) || !std::isfinite(hostile[1]) ||
        !std::isfinite(hostile[2]) || hostile[0] < 0.0f || hostile[0] > 1.0f ||
        hostile[1] < 0.0f || hostile[1] > 1.0f || hostile[2] < 0.0f ||
        hostile[2] > 1.0f) {
        std::cerr << "hostile input mismatch: " << hostile[0] << ", "
            << hostile[1] << ", " << hostile[2] << '\n';
        valid = false;
    }

	const std::filesystem::path hdrPath = std::filesystem::path(PROJECT_ROOT_DIR) /
		"assets/color/aces2_p3d65_1000nit_rec2100_pq_128.irlt";
	const Iridium::Color::AcesOutputLut hdrLut =
		Iridium::Color::loadAcesOutputLut(hdrPath);
	valid = valid && hdrLut.size == 128 && hdrLut.width() == 16384 &&
		hdrLut.height() == 128 && hdrLut.minimumLog2 == -10.0f &&
		hdrLut.maximumLog2 == 16.0f;
	constexpr std::array hdrVectors{
		Vector{{0.18f, 0.18f, 0.18f}, {0.32983688f, 0.32983717f, 0.32983705f}},
		Vector{{1.0f, 1.0f, 1.0f}, {0.51447237f, 0.51447266f, 0.51447254f}},
		Vector{{4.0f, 4.0f, 4.0f}, {0.63980043f, 0.63980049f, 0.63980049f}},
		Vector{{1.0f, 0.0f, 0.0f}, {0.49310714f, 0.25107059f, 0.13869348f}},
		Vector{{0.0f, 1.0f, 0.0f}, {0.35394207f, 0.49764952f, 0.28495893f}},
		Vector{{0.0f, 0.0f, 1.0f}, {0.21925898f, 0.23239051f, 0.44679290f}},
	};
	for (const Vector& vector : hdrVectors) {
		const auto actual = Iridium::Color::sampleAcesOutputLutEncoded(
			hdrLut, vector.input);
		for (size_t channel = 0; channel < 3; ++channel) {
			if (!near(actual[channel], vector.expected[channel])) {
				std::cerr << "HDR vector mismatch channel " << channel << ": "
					<< actual[channel] << " expected " << vector.expected[channel]
					<< '\n';
				valid = false;
			}
		}
	}
	std::ifstream hdrMetadata(hdrPath.parent_path() /
		"aces2_p3d65_1000nit_rec2100_pq_128.json");
	const std::string metadataText((std::istreambuf_iterator<char>(hdrMetadata)),
		std::istreambuf_iterator<char>());
	if (metadataText.find("HDR-1000nit-P3-D65_2.0") == std::string::npos ||
		metadataText.find("DISPLAY - CIE-XYZ-D65_to_REC.2100-PQ") ==
			std::string::npos ||
		metadataText.find("Output.Academy.P3-D65_1000nit_in_Rec2100-D65_ST2084.a2.v1") ==
			std::string::npos ||
		metadataText.find("742a0fff9f8048adf56513f4760180d9d07fa28757628a7a051df084a62bac52") ==
			std::string::npos) {
		std::cerr << "HDR LUT metadata provenance mismatch\n";
		valid = false;
	}
    std::cout << (valid ? "[PASS]" : "[FAIL]")
        << " pinned ACES 2 SDR and HDR output LUTs\n";
    return valid ? 0 : 1;
}
