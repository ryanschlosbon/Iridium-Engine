#pragma once

#include <cstdint>

namespace Iridium::Color {

    struct Rgb {
        double r = 0.0;
        double g = 0.0;
        double b = 0.0;
    };

    [[nodiscard]] double decodeSrgb(double encoded) noexcept;
    [[nodiscard]] double encodeSrgb(double linear) noexcept;
    [[nodiscard]] Rgb linearSrgbToAcesCg(Rgb linearSrgb) noexcept;
    [[nodiscard]] Rgb acesCgToLinearSrgb(Rgb acesCg) noexcept;
    [[nodiscard]] Rgb srgbToAcesCg(Rgb encodedSrgb) noexcept;
	[[nodiscard]] double encodeSt2084FromNits(double nits) noexcept;
	[[nodiscard]] double decodeSt2084ToNits(double encoded) noexcept;
	[[nodiscard]] Rgb linearRec2020ToLinearSrgb(Rgb linearRec2020) noexcept;
	[[nodiscard]] Rgb rec2100PqToScRgb(Rgb encodedRec2100,
		double nitsPerUnit = 80.0) noexcept;
	[[nodiscard]] Rgb srgbUiToScRgb(Rgb encodedSrgb,
		double paperWhiteNits, double nitsPerUnit = 80.0) noexcept;
    [[nodiscard]] float halfToFloat(uint16_t bits) noexcept;

} // namespace Iridium::Color
