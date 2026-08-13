#include "renderer/color/SceneColor.h"

#include <bit>
#include <algorithm>
#include <cmath>

namespace Iridium::Color {

    namespace {

        // Linear Rec.709/sRGB D65 to ACEScg/AP1 D60, including Bradford white
        // adaptation. These frozen coefficients are shared with the M1 shader
        // implementation and must change only with new reference-vector evidence.
        constexpr double SrgbToAcesCg[3][3] = {
            { 0.6130974024, 0.3395231462, 0.0473794514 },
            { 0.0701937225, 0.9163538791, 0.0134523984 },
            { 0.0206155929, 0.1095697729, 0.8698146342 },
        };
        constexpr double AcesCgToSrgb[3][3] = {
            { 1.705050992697, -0.621792120673, -0.083258872024 },
            { -0.130256417562, 1.140804736533, -0.010548318971 },
            { -0.024003356839, -0.128968975993, 1.152972332832 },
        };
		constexpr double Rec2020ToSrgb[3][3] = {
			{ 1.6604910021, -0.5876411388, -0.0728498633 },
			{ -0.1245504745, 1.1328998971, -0.0083494226 },
			{ -0.0181507634, -0.1005788980, 1.1187296614 },
		};

        [[nodiscard]] Rgb transform(const double matrix[3][3], Rgb value) noexcept {
            return {
                matrix[0][0] * value.r + matrix[0][1] * value.g +
                    matrix[0][2] * value.b,
                matrix[1][0] * value.r + matrix[1][1] * value.g +
                    matrix[1][2] * value.b,
                matrix[2][0] * value.r + matrix[2][1] * value.g +
                    matrix[2][2] * value.b,
            };
        }

    } // namespace

    double decodeSrgb(double encoded) noexcept {
        return encoded <= 0.04045
            ? encoded / 12.92
            : std::pow((encoded + 0.055) / 1.055, 2.4);
    }

    double encodeSrgb(double linear) noexcept {
        return linear <= 0.0031308
            ? linear * 12.92
            : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    }

    Rgb linearSrgbToAcesCg(Rgb linearSrgb) noexcept {
        return transform(SrgbToAcesCg, linearSrgb);
    }

    Rgb acesCgToLinearSrgb(Rgb acesCg) noexcept {
        return transform(AcesCgToSrgb, acesCg);
    }

    Rgb srgbToAcesCg(Rgb encodedSrgb) noexcept {
        return linearSrgbToAcesCg({ decodeSrgb(encodedSrgb.r),
            decodeSrgb(encodedSrgb.g), decodeSrgb(encodedSrgb.b) });
    }

	double encodeSt2084FromNits(double nits) noexcept {
		if (!std::isfinite(nits)) return nits > 0.0 ? 1.0 : 0.0;
		const double normalized = std::clamp(nits / 10000.0, 0.0, 1.0);
		constexpr double m1 = 2610.0 / 16384.0;
		constexpr double m2 = 2523.0 / 32.0;
		constexpr double c1 = 3424.0 / 4096.0;
		constexpr double c2 = 2413.0 / 128.0;
		constexpr double c3 = 2392.0 / 128.0;
		const double powered = std::pow(normalized, m1);
		return std::pow((c1 + c2 * powered) / (1.0 + c3 * powered), m2);
	}

	double decodeSt2084ToNits(double encoded) noexcept {
		if (!std::isfinite(encoded)) return encoded > 0.0 ? 10000.0 : 0.0;
		const double safe = std::clamp(encoded, 0.0, 1.0);
		constexpr double m1 = 2610.0 / 16384.0;
		constexpr double m2 = 2523.0 / 32.0;
		constexpr double c1 = 3424.0 / 4096.0;
		constexpr double c2 = 2413.0 / 128.0;
		constexpr double c3 = 2392.0 / 128.0;
		const double powered = std::pow(safe, 1.0 / m2);
		const double numerator = std::max(powered - c1, 0.0);
		const double denominator = c2 - c3 * powered;
		return 10000.0 * std::pow(numerator / denominator, 1.0 / m1);
	}

	Rgb linearRec2020ToLinearSrgb(Rgb linearRec2020) noexcept {
		return transform(Rec2020ToSrgb, linearRec2020);
	}

	Rgb rec2100PqToScRgb(Rgb encodedRec2100, double nitsPerUnit) noexcept {
		if (!std::isfinite(nitsPerUnit) || nitsPerUnit <= 0.0) return {};
		const Rgb rec2020Nits{ decodeSt2084ToNits(encodedRec2100.r),
			decodeSt2084ToNits(encodedRec2100.g),
			decodeSt2084ToNits(encodedRec2100.b) };
		const Rgb rec709Nits = linearRec2020ToLinearSrgb(rec2020Nits);
		return { rec709Nits.r / nitsPerUnit, rec709Nits.g / nitsPerUnit,
			rec709Nits.b / nitsPerUnit };
	}

	Rgb srgbUiToScRgb(Rgb encodedSrgb, double paperWhiteNits,
		double nitsPerUnit) noexcept {
		if (!std::isfinite(paperWhiteNits) || paperWhiteNits < 0.0 ||
			!std::isfinite(nitsPerUnit) || nitsPerUnit <= 0.0) return {};
		const double scale = paperWhiteNits / nitsPerUnit;
		return { decodeSrgb(encodedSrgb.r) * scale,
			decodeSrgb(encodedSrgb.g) * scale,
			decodeSrgb(encodedSrgb.b) * scale };
	}

    float halfToFloat(uint16_t bits) noexcept {
        const uint32_t sign = static_cast<uint32_t>(bits & 0x8000u) << 16u;
        int32_t exponent = static_cast<int32_t>((bits >> 10u) & 0x1fu);
        uint32_t mantissa = bits & 0x03ffu;
        uint32_t result = 0;
        if (exponent == 0) {
            if (mantissa == 0) {
                result = sign;
            }
            else {
                exponent = 1;
                while ((mantissa & 0x0400u) == 0) {
                    mantissa <<= 1u;
                    --exponent;
                }
                mantissa &= 0x03ffu;
                result = sign | (static_cast<uint32_t>(exponent + 112) << 23u) |
                    (mantissa << 13u);
            }
        }
        else if (exponent == 0x1f) {
            result = sign | 0x7f800000u | (mantissa << 13u);
        }
        else {
            result = sign | (static_cast<uint32_t>(exponent + 112) << 23u) |
                (mantissa << 13u);
        }
        return std::bit_cast<float>(result);
    }

} // namespace Iridium::Color
