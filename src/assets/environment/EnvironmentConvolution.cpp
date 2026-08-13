#include "assets/environment/EnvironmentConvolution.h"

#include "material/MaterialRuntime.h"
#include "material/StandardMaterialShading.h"
#include "renderer/color/SceneColor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <execution>
#include <numbers>
#include <stdexcept>

namespace Iridium {
namespace {

    constexpr float Pi = std::numbers::pi_v<float>;

    uint32_t fullMipCount(uint32_t size) noexcept {
        uint32_t result = 0;
        do {
            ++result;
            if (size == 1u) break;
            size = (std::max)(size / 2u, 1u);
        } while (true);
        return result;
    }

    void validateInputs(const EnvironmentFloatImage& source,
        const EnvironmentConvolutionSettings& settings) {
        if (source.width == 0 || source.height == 0 ||
            source.pixels.size() != static_cast<size_t>(source.width) * source.height)
            throw std::invalid_argument("Environment source image is incomplete.");
        if (settings.radianceSize == 0 || settings.irradianceSize == 0 ||
            settings.prefilteredSize == 0 || settings.brdfLutSize == 0 ||
            settings.prefilteredSamples == 0 || settings.brdfSamples == 0 ||
            !std::isfinite(settings.sourceRadianceScale) ||
            settings.sourceRadianceScale < 0.0f)
            throw std::invalid_argument("Environment convolution settings are invalid.");
        if (settings.sourcePrimaries != "linear_rec709_d65" &&
            settings.sourcePrimaries != "acescg_ap1_d60")
            throw std::invalid_argument("Environment source primaries are unsupported.");
        for (const glm::vec4 pixel : source.pixels) {
            if (!std::isfinite(pixel.x) || !std::isfinite(pixel.y) ||
                !std::isfinite(pixel.z) || pixel.x < 0.0f || pixel.y < 0.0f ||
                pixel.z < 0.0f)
                throw std::invalid_argument(
                    "Environment source radiance must be finite and nonnegative.");
        }
    }

    glm::vec3 sourceToAp1(glm::vec3 value, std::string_view primaries) {
        if (primaries == "acescg_ap1_d60") return value;
        const Color::Rgb converted = Color::linearSrgbToAcesCg(
            { value.x, value.y, value.z });
        return { static_cast<float>(converted.r), static_cast<float>(converted.g),
            static_cast<float>(converted.b) };
    }

    using IrradianceSh = std::array<glm::dvec3, 9>;

    IrradianceSh projectDiffuseIrradiance(const EnvironmentFloatImage& source,
        const EnvironmentConvolutionSettings& settings,
        std::stop_token stopToken) {
        struct LongitudeIntegrals {
            double delta = 0.0;
            double cosine = 0.0;
            double sine = 0.0;
            double cosineSine = 0.0;
            double cosineSquared = 0.0;
            double sineSquared = 0.0;
        };
        struct LatitudeIntegrals {
            double solid = 0.0;
            double y = 0.0;
            double horizontal = 0.0;
            double yHorizontal = 0.0;
            double horizontalSquared = 0.0;
            double ySquared = 0.0;
        };
        std::vector<LongitudeIntegrals> longitude(source.width);
        for (uint32_t x = 0; x < source.width; ++x) {
            const double begin = -std::numbers::pi +
                2.0 * std::numbers::pi * x / source.width;
            const double end = -std::numbers::pi +
                2.0 * std::numbers::pi * (x + 1u) / source.width;
            const double delta = end - begin;
            longitude[x] = {
                .delta = delta,
                .cosine = std::sin(end) - std::sin(begin),
                .sine = std::cos(begin) - std::cos(end),
                .cosineSine = 0.5 *
                    (std::sin(end) * std::sin(end) -
                        std::sin(begin) * std::sin(begin)),
                .cosineSquared = 0.5 * delta +
                    0.25 * (std::sin(2.0 * end) - std::sin(2.0 * begin)),
                .sineSquared = 0.5 * delta -
                    0.25 * (std::sin(2.0 * end) - std::sin(2.0 * begin)),
            };
        }
        std::vector<LatitudeIntegrals> latitude(source.height);
        for (uint32_t y = 0; y < source.height; ++y) {
            const double beginLatitude = -0.5 * std::numbers::pi +
                std::numbers::pi * y / source.height;
            const double endLatitude = -0.5 * std::numbers::pi +
                std::numbers::pi * (y + 1u) / source.height;
            const double begin = std::sin(beginLatitude);
            const double end = std::sin(endLatitude);
            const auto horizontalPrimitive = [](double value) {
                const double root = std::sqrt((std::max)(0.0,
                    1.0 - value * value));
                return 0.5 * (value * root + std::asin(value));
            };
            const auto yHorizontalPrimitive = [](double value) {
                return -std::pow((std::max)(0.0, 1.0 - value * value), 1.5) /
                    3.0;
            };
            latitude[y] = {
                .solid = end - begin,
                .y = 0.5 * (end * end - begin * begin),
                .horizontal = horizontalPrimitive(end) -
                    horizontalPrimitive(begin),
                .yHorizontal = yHorizontalPrimitive(end) -
                    yHorizontalPrimitive(begin),
                .horizontalSquared = (end - end * end * end / 3.0) -
                    (begin - begin * begin * begin / 3.0),
                .ySquared = (end * end * end - begin * begin * begin) / 3.0,
            };
        }

        IrradianceSh coefficients{};
        for (uint32_t y = 0; y < source.height; ++y) {
            if (stopToken.stop_requested())
                throw std::runtime_error("Environment convolution cancelled.");
            const LatitudeIntegrals& lat = latitude[y];
            for (uint32_t x = 0; x < source.width; ++x) {
                const LongitudeIntegrals& lon = longitude[x];
                const double solidAngle = lat.solid * lon.delta;
                const double ix = lat.horizontal * lon.cosine;
                const double iy = lat.y * lon.delta;
                const double iz = lat.horizontal * lon.sine;
                const double ixy = lat.yHorizontal * lon.cosine;
                const double iyz = lat.yHorizontal * lon.sine;
                const double ixz = lat.horizontalSquared * lon.cosineSine;
                const double ix2 = lat.horizontalSquared * lon.cosineSquared;
                const double iy2 = lat.ySquared * lon.delta;
                const double iz2 = lat.horizontalSquared * lon.sineSquared;
                const std::array<double, 9> integratedBasis{
                    0.28209479177387814 * solidAngle,
                    0.4886025119029199 * iy,
                    0.4886025119029199 * iz,
                    0.4886025119029199 * ix,
                    1.0925484305920792 * ixy,
                    1.0925484305920792 * iyz,
                    0.31539156525252005 * (3.0 * iz2 - solidAngle),
                    1.0925484305920792 * ixz,
                    0.5462742152960396 * (ix2 - iy2),
                };
                const glm::vec3 ap1 = sourceToAp1(glm::vec3(
                    source.pixels[static_cast<size_t>(y) * source.width + x]),
                    settings.sourcePrimaries) * settings.sourceRadianceScale;
                const glm::dvec3 radiance(ap1);
                for (size_t coefficient = 0; coefficient < coefficients.size();
                    ++coefficient)
                    coefficients[coefficient] +=
                        radiance * integratedBasis[coefficient];
            }
        }
        return coefficients;
    }

    double cubeAreaElement(double x, double y) noexcept {
        return std::atan2(x * y, std::sqrt(x * x + y * y + 1.0));
    }

    double cubeTexelSolidAngle(uint32_t x, uint32_t y,
        uint32_t size) noexcept {
        const double inverseSize = 1.0 / static_cast<double>(size);
        const double u0 = 2.0 * static_cast<double>(x) * inverseSize - 1.0;
        const double v0 = 2.0 * static_cast<double>(y) * inverseSize - 1.0;
        const double u1 = 2.0 * static_cast<double>(x + 1u) * inverseSize - 1.0;
        const double v1 = 2.0 * static_cast<double>(y + 1u) * inverseSize - 1.0;
        return cubeAreaElement(u1, v1) - cubeAreaElement(u0, v1) -
            cubeAreaElement(u1, v0) + cubeAreaElement(u0, v0);
    }

    IrradianceSh projectCapturedCubeDiffuseIrradiance(
        std::span<const std::byte> radiance, uint32_t size) {
        const uint64_t expectedBytes = static_cast<uint64_t>(size) * size *
            6u * sizeof(uint16_t) * 4u;
        if (size == 0 || expectedBytes != radiance.size())
            throw std::invalid_argument(
                "Captured cube radiance does not match its dimensions.");
        IrradianceSh coefficients{};
        for (uint32_t face = 0; face < 6u; ++face)
            for (uint32_t y = 0; y < size; ++y)
                for (uint32_t x = 0; x < size; ++x) {
                    const size_t pixelIndex =
                        (static_cast<size_t>(face) * size * size +
                            static_cast<size_t>(y) * size + x) * 4u;
                    uint16_t packed[4]{};
                    std::memcpy(packed,
                        radiance.data() + pixelIndex * sizeof(uint16_t),
                        sizeof(packed));
                    const glm::dvec3 value{
                        Color::halfToFloat(packed[0]),
                        Color::halfToFloat(packed[1]),
                        Color::halfToFloat(packed[2]),
                    };
                    const float u = 2.0f *
                        (static_cast<float>(x) + 0.5f) / size - 1.0f;
                    const float v = 2.0f *
                        (static_cast<float>(y) + 0.5f) / size - 1.0f;
                    const glm::dvec3 direction(
                        environmentCubeDirection(face, u, v));
                    const double solidAngle =
                        cubeTexelSolidAngle(x, y, size);
                    const double dx = direction.x;
                    const double dy = direction.y;
                    const double dz = direction.z;
                    const std::array<double, 9> basis{
                        0.28209479177387814,
                        0.4886025119029199 * dy,
                        0.4886025119029199 * dz,
                        0.4886025119029199 * dx,
                        1.0925484305920792 * dx * dy,
                        1.0925484305920792 * dy * dz,
                        0.31539156525252005 * (3.0 * dz * dz - 1.0),
                        1.0925484305920792 * dx * dz,
                        0.5462742152960396 * (dx * dx - dy * dy),
                    };
                    for (size_t coefficient = 0;
                        coefficient < coefficients.size(); ++coefficient)
                        coefficients[coefficient] += value *
                            (basis[coefficient] * solidAngle);
                }
        return coefficients;
    }

    glm::vec3 evaluateDiffuseIrradiance(const IrradianceSh& coefficients,
        glm::vec3 direction) {
        const double x = direction.x;
        const double y = direction.y;
        const double z = direction.z;
        const std::array<double, 9> basis{
            0.28209479177387814,
            0.4886025119029199 * y,
            0.4886025119029199 * z,
            0.4886025119029199 * x,
            1.0925484305920792 * x * y,
            1.0925484305920792 * y * z,
            0.31539156525252005 * (3.0 * z * z - 1.0),
            1.0925484305920792 * x * z,
            0.5462742152960396 * (x * x - y * y),
        };
        constexpr std::array<double, 3> CosineKernel{
            std::numbers::pi, 2.0 * std::numbers::pi / 3.0,
            std::numbers::pi / 4.0,
        };
        glm::dvec3 result{};
        for (size_t coefficient = 0; coefficient < coefficients.size();
            ++coefficient) {
            const size_t band = coefficient == 0 ? 0 : coefficient <= 3 ? 1 : 2;
            result += coefficients[coefficient] * basis[coefficient] *
                CosineKernel[band];
        }
        return glm::max(glm::vec3(result), glm::vec3(0.0f));
    }

    glm::vec2 hammersley(uint32_t index, uint32_t count) noexcept {
        uint32_t bits = index;
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xaaaaaaaau) >> 1u);
        bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xccccccccu) >> 2u);
        bits = ((bits & 0x0f0f0f0fu) << 4u) | ((bits & 0xf0f0f0f0u) >> 4u);
        bits = ((bits & 0x00ff00ffu) << 8u) | ((bits & 0xff00ff00u) >> 8u);
        return { static_cast<float>(index) / static_cast<float>(count),
            static_cast<float>(bits) * 2.3283064365386963e-10f };
    }

    void tangentBasis(glm::vec3 normal, glm::vec3& tangent, glm::vec3& bitangent) {
        const glm::vec3 up = std::abs(normal.z) < 0.999f
            ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        tangent = glm::normalize(glm::cross(up, normal));
        bitangent = glm::cross(normal, tangent);
    }

    glm::vec3 importanceSampleGgx(glm::vec2 sample, glm::vec3 normal,
        float perceptualRoughness) {
        const float alpha = materialGgxAlpha(perceptualRoughness);
        const float alphaSquared = alpha * alpha;
        const float phi = 2.0f * Pi * sample.x;
        const float cosTheta = std::sqrt((1.0f - sample.y) /
            (1.0f + (alphaSquared - 1.0f) * sample.y));
        const float sinTheta = std::sqrt((std::max)(0.0f,
            1.0f - cosTheta * cosTheta));
        const glm::vec3 local{ std::cos(phi) * sinTheta,
            std::sin(phi) * sinTheta, cosTheta };
        glm::vec3 tangent{}, bitangent{};
        tangentBasis(normal, tangent, bitangent);
        return glm::normalize(tangent * local.x + bitangent * local.y +
            normal * local.z);
    }

    uint32_t mipSize(uint32_t base, uint32_t mip) noexcept {
        return (std::max)(base >> mip, 1u);
    }

    std::vector<glm::vec4> makeCubeMip(uint32_t size) {
        return std::vector<glm::vec4>(static_cast<size_t>(size) * size * 6u);
    }

    size_t cubeIndex(uint32_t size, uint32_t face, uint32_t x, uint32_t y) {
        return static_cast<size_t>(face) * size * size +
            static_cast<size_t>(y) * size + x;
    }

    struct CubeCoordinates {
        uint32_t face = 0;
        float u = 0.0f;
        float v = 0.0f;
    };

    CubeCoordinates cubeCoordinates(glm::vec3 direction) {
        direction = glm::normalize(direction);
        const glm::vec3 absolute = glm::abs(direction);
        if (absolute.x >= absolute.y && absolute.x >= absolute.z) {
            if (direction.x >= 0.0f)
                return { 0u, -direction.z / absolute.x,
                    -direction.y / absolute.x };
            return { 1u, direction.z / absolute.x,
                -direction.y / absolute.x };
        }
        if (absolute.y >= absolute.z) {
            if (direction.y >= 0.0f)
                return { 2u, direction.x / absolute.y,
                    direction.z / absolute.y };
            return { 3u, direction.x / absolute.y,
                -direction.z / absolute.y };
        }
        if (direction.z >= 0.0f)
            return { 4u, direction.x / absolute.z,
                -direction.y / absolute.z };
        return { 5u, -direction.x / absolute.z,
            -direction.y / absolute.z };
    }

    glm::vec4 sampleCubeMip(const EnvironmentFloatCube& cube,
        uint32_t mip, glm::vec3 direction) {
        mip = (std::min)(mip,
            static_cast<uint32_t>(cube.mips.size() - 1u));
        const uint32_t size = mipSize(cube.baseSize, mip);
        const CubeCoordinates coordinates = cubeCoordinates(direction);
        const float px = (coordinates.u * 0.5f + 0.5f) * size - 0.5f;
        const float py = (coordinates.v * 0.5f + 0.5f) * size - 0.5f;
        const int32_t x0 = static_cast<int32_t>(std::floor(px));
        const int32_t y0 = static_cast<int32_t>(std::floor(py));
        const float tx = px - std::floor(px);
        const float ty = py - std::floor(py);
        const auto texel = [&](int32_t x, int32_t y) {
            x = std::clamp(x, 0, static_cast<int32_t>(size) - 1);
            y = std::clamp(y, 0, static_cast<int32_t>(size) - 1);
            return cube.mips[mip][cubeIndex(size, coordinates.face,
                static_cast<uint32_t>(x), static_cast<uint32_t>(y))];
        };
        return glm::mix(glm::mix(texel(x0, y0), texel(x0 + 1, y0), tx),
            glm::mix(texel(x0, y0 + 1), texel(x0 + 1, y0 + 1), tx), ty);
    }

    glm::vec4 sampleCubeLod(const EnvironmentFloatCube& cube,
        float lod, glm::vec3 direction) {
        const float maximum = static_cast<float>(cube.mips.size() - 1u);
        lod = std::clamp(lod, 0.0f, maximum);
        const uint32_t lower = static_cast<uint32_t>(std::floor(lod));
        const uint32_t upper = (std::min)(lower + 1u,
            static_cast<uint32_t>(cube.mips.size() - 1u));
        return glm::mix(sampleCubeMip(cube, lower, direction),
            sampleCubeMip(cube, upper, direction), lod - lower);
    }

    template <typename Pixel>
    void appendHalf(std::vector<std::byte>& bytes, Pixel value);

    template <>
    void appendHalf<glm::vec4>(std::vector<std::byte>& bytes, glm::vec4 value) {
        if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
            !std::isfinite(value.z) || !std::isfinite(value.w))
            throw std::overflow_error(
                "Environment radiance is not finite at the FP16 storage boundary.");
        // Scene targets and cooked environment products are FP16. Preserve the
        // useful exposure of the image and saturate only channels outside that
        // representable range instead of rejecting or globally rescaling an HDRI
        // because of a handful of sun/fire outliers.
        value = glm::clamp(value, glm::vec4(0.0f), glm::vec4(65504.0f));
        const uint16_t packed[4]{ floatToHalf(value.x), floatToHalf(value.y),
            floatToHalf(value.z), floatToHalf(value.w) };
        const auto* first = reinterpret_cast<const std::byte*>(packed);
        bytes.insert(bytes.end(), first, first + sizeof(packed));
    }

    template <>
    void appendHalf<glm::vec2>(std::vector<std::byte>& bytes, glm::vec2 value) {
        if (!std::isfinite(value.x) || !std::isfinite(value.y))
            throw std::overflow_error(
                "Environment BRDF data is not finite at the FP16 storage boundary.");
        value = glm::clamp(value, glm::vec2(0.0f), glm::vec2(65504.0f));
        const uint16_t packed[2]{ floatToHalf(value.x), floatToHalf(value.y) };
        const auto* first = reinterpret_cast<const std::byte*>(packed);
        bytes.insert(bytes.end(), first, first + sizeof(packed));
    }

    std::vector<std::byte> packCube(const EnvironmentFloatCube& cube) {
        std::vector<std::byte> result;
        for (uint32_t face = 0; face < 6; ++face) {
            for (uint32_t mip = 0; mip < cube.mips.size(); ++mip) {
                const uint32_t size = mipSize(cube.baseSize, mip);
                const auto& pixels = cube.mips[mip];
                for (uint32_t y = 0; y < size; ++y)
                    for (uint32_t x = 0; x < size; ++x)
                        appendHalf(result, pixels[cubeIndex(size, face, x, y)]);
            }
        }
        return result;
    }

} // namespace

glm::vec3 environmentCubeDirection(uint32_t face, float u, float v) {
    if (face >= 6 || !std::isfinite(u) || !std::isfinite(v))
        throw std::invalid_argument("Cube direction coordinates are invalid.");
    const glm::vec3 directions[6]{
        { 1.0f, -v, -u }, { -1.0f, -v, u }, { u, 1.0f, v },
        { u, -1.0f, -v }, { u, -v, 1.0f }, { -u, -v, -1.0f },
    };
    return glm::normalize(directions[face]);
}

glm::vec4 sampleEnvironmentEquirect(const EnvironmentFloatImage& image,
    glm::vec3 direction) {
    if (image.width == 0 || image.height == 0 ||
        image.pixels.size() != static_cast<size_t>(image.width) * image.height ||
        glm::dot(direction, direction) <= 0.0f)
        throw std::invalid_argument("Environment sample input is invalid.");
    direction = glm::normalize(direction);
    float u = std::atan2(direction.z, direction.x) / (2.0f * Pi) + 0.5f;
    u -= std::floor(u);
    const float v = std::clamp(std::asin(std::clamp(direction.y, -1.0f, 1.0f)) /
        Pi + 0.5f, 0.0f, 1.0f);
    const float px = u * static_cast<float>(image.width) - 0.5f;
    const float py = v * static_cast<float>(image.height) - 0.5f;
    const int32_t x0 = static_cast<int32_t>(std::floor(px));
    const int32_t y0 = static_cast<int32_t>(std::floor(py));
    const float tx = px - std::floor(px);
    const float ty = py - std::floor(py);
    const auto pixel = [&](int32_t x, int32_t y) {
        x %= static_cast<int32_t>(image.width);
        if (x < 0) x += static_cast<int32_t>(image.width);
        y = std::clamp(y, 0, static_cast<int32_t>(image.height) - 1);
        return image.pixels[static_cast<size_t>(y) * image.width + x];
    };
    return glm::mix(glm::mix(pixel(x0, y0), pixel(x0 + 1, y0), tx),
        glm::mix(pixel(x0, y0 + 1), pixel(x0 + 1, y0 + 1), tx), ty);
}

ConvolvedEnvironment convolveEnvironmentReference(
    const EnvironmentFloatImage& source,
    const EnvironmentConvolutionSettings& settings,
    std::stop_token stopToken) {
    validateInputs(source, settings);
    const auto cancel = [&] {
        if (stopToken.stop_requested())
            throw std::runtime_error("Environment convolution was cancelled.");
    };
    const auto radiance = [&](glm::vec3 direction) {
        glm::vec4 sample = sampleEnvironmentEquirect(source, direction);
        const glm::vec3 converted = glm::clamp(
            sourceToAp1(glm::vec3(sample), settings.sourcePrimaries) *
                settings.sourceRadianceScale,
            glm::vec3(0.0f), glm::vec3(65504.0f));
        sample = glm::vec4(converted, 1.0f);
        return sample;
    };

    ConvolvedEnvironment result;
    result.radiance.baseSize = settings.radianceSize;
    const uint32_t radianceMips = fullMipCount(settings.radianceSize);
    for (uint32_t mip = 0; mip < radianceMips; ++mip) {
        cancel();
        const uint32_t size = mipSize(settings.radianceSize, mip);
        auto pixels = makeCubeMip(size);
        std::for_each(std::execution::par, pixels.begin(), pixels.end(),
            [&](glm::vec4& output) {
                if (stopToken.stop_requested()) return;
                const size_t index = static_cast<size_t>(&output - pixels.data());
                const size_t faceArea = static_cast<size_t>(size) * size;
                const uint32_t face = static_cast<uint32_t>(index / faceArea);
                const size_t local = index % faceArea;
                const uint32_t y = static_cast<uint32_t>(local / size);
                const uint32_t x = static_cast<uint32_t>(local % size);
                if (mip == 0u) {
                    const float u = 2.0f *
                        (static_cast<float>(x) + 0.5f) / size - 1.0f;
                    const float v = 2.0f *
                        (static_cast<float>(y) + 0.5f) / size - 1.0f;
                    output = radiance(
                        environmentCubeDirection(face, u, v));
                } else {
                    const uint32_t previousSize = size * 2u;
                    const auto& previous = result.radiance.mips.back();
                    const uint32_t px = x * 2u;
                    const uint32_t py = y * 2u;
                    output = 0.25f *
                        (previous[cubeIndex(previousSize, face, px, py)] +
                         previous[cubeIndex(previousSize, face, px + 1u, py)] +
                         previous[cubeIndex(previousSize, face, px, py + 1u)] +
                         previous[cubeIndex(previousSize, face, px + 1u, py + 1u)]);
                }
            });
        cancel();
        result.radiance.mips.push_back(std::move(pixels));
    }

    result.irradiance.baseSize = settings.irradianceSize;
    const IrradianceSh irradianceSh =
        projectDiffuseIrradiance(source, settings, stopToken);
    auto irradiance = makeCubeMip(settings.irradianceSize);
    std::for_each(std::execution::par, irradiance.begin(), irradiance.end(),
        [&](glm::vec4& output) {
            if (stopToken.stop_requested()) return;
            const uint32_t size = settings.irradianceSize;
            const size_t index = static_cast<size_t>(&output - irradiance.data());
            const size_t faceArea = static_cast<size_t>(size) * size;
            const uint32_t face = static_cast<uint32_t>(index / faceArea);
            const size_t local = index % faceArea;
            const uint32_t y = static_cast<uint32_t>(local / size);
            const uint32_t x = static_cast<uint32_t>(local % size);
            const float u = 2.0f * (static_cast<float>(x) + 0.5f) /
                size - 1.0f;
            const float v = 2.0f * (static_cast<float>(y) + 0.5f) /
                size - 1.0f;
            const glm::vec3 normal = environmentCubeDirection(face, u, v);
            output = glm::vec4(
                evaluateDiffuseIrradiance(irradianceSh, normal), 1.0f);
        });
    cancel();
    result.irradiance.mips.push_back(std::move(irradiance));

    result.prefilteredSpecular.baseSize = settings.prefilteredSize;
    const uint32_t prefilteredMips = fullMipCount(settings.prefilteredSize);
    for (uint32_t mip = 0; mip < prefilteredMips; ++mip) {
        cancel();
        const uint32_t size = mipSize(settings.prefilteredSize, mip);
        const float roughness = prefilteredMips > 1
            ? static_cast<float>(mip) / (prefilteredMips - 1u) : 0.0f;
        auto pixels = makeCubeMip(size);
        // At roughness zero every GGX sample collapses to the same reflected
        // direction. Evaluating hundreds of identical samples only multiplies
        // cold-cook time. One sample is mathematically equivalent and makes a
        // high-resolution mip zero practical.
        const uint32_t sampleCount = mip == 0u
            ? 1u : settings.prefilteredSamples;
        const float sourceTexelSolidAngle = 4.0f * Pi /
            (6.0f * settings.radianceSize * settings.radianceSize);
        std::for_each(std::execution::par, pixels.begin(), pixels.end(),
            [&](glm::vec4& output) {
                if (stopToken.stop_requested()) return;
                const size_t index = static_cast<size_t>(&output - pixels.data());
                const size_t faceArea = static_cast<size_t>(size) * size;
                const uint32_t face = static_cast<uint32_t>(index / faceArea);
                const size_t local = index % faceArea;
                const uint32_t y = static_cast<uint32_t>(local / size);
                const uint32_t x = static_cast<uint32_t>(local % size);
                const float u = 2.0f * (static_cast<float>(x) + 0.5f) / size - 1.0f;
                const float v = 2.0f * (static_cast<float>(y) + 0.5f) / size - 1.0f;
                const glm::vec3 normal = environmentCubeDirection(face, u, v);
                glm::vec3 sum{};
                float weight = 0.0f;
                for (uint32_t sample = 0; sample < sampleCount; ++sample) {
                    const glm::vec3 halfVector = importanceSampleGgx(
                        hammersley(sample, sampleCount), normal, roughness);
                    const glm::vec3 light = glm::normalize(2.0f *
                        glm::dot(normal, halfVector) * halfVector - normal);
                    const float noL = (std::max)(glm::dot(normal, light), 0.0f);
                    if (noL > 0.0f) {
                        float sourceLod = 0.0f;
                        if (mip != 0u) {
                            const float noH = (std::max)(
                                glm::dot(normal, halfVector), 0.0f);
                            const float pdf = (std::max)(
                                materialDistributionGgx(noH, roughness) * 0.25f,
                                1.0e-6f);
                            const float sampleSolidAngle = 1.0f /
                                (static_cast<float>(sampleCount) * pdf);
                            sourceLod = 0.5f * std::log2((std::max)(
                                sampleSolidAngle / sourceTexelSolidAngle,
                                1.0f));
                        }
                        const glm::vec3 incoming = mip == 0u
                            ? glm::vec3(radiance(light))
                            : glm::vec3(sampleCubeLod(result.radiance,
                                sourceLod, light));
                        sum += incoming * noL;
                        weight += noL;
                    }
                }
                output = glm::vec4(
                    weight > 0.0f ? sum / weight : glm::vec3(0.0f), 1.0f);
            });
        cancel();
        result.prefilteredSpecular.mips.push_back(std::move(pixels));
    }

    result.brdfLutSize = settings.brdfLutSize;
    result.brdfLut.resize(static_cast<size_t>(settings.brdfLutSize) *
        settings.brdfLutSize);
    const glm::vec3 normal{ 0.0f, 0.0f, 1.0f };
    std::for_each(std::execution::par, result.brdfLut.begin(),
        result.brdfLut.end(), [&](glm::vec2& output) {
            if (stopToken.stop_requested()) return;
            const size_t index = static_cast<size_t>(
                &output - result.brdfLut.data());
            const uint32_t y = static_cast<uint32_t>(
                index / settings.brdfLutSize);
            const uint32_t x = static_cast<uint32_t>(
                index % settings.brdfLutSize);
            const float noV = (static_cast<float>(x) + 0.5f) / settings.brdfLutSize;
            const float roughness = (static_cast<float>(y) + 0.5f) /
                settings.brdfLutSize;
            const glm::vec3 view{ std::sqrt((std::max)(0.0f, 1.0f - noV * noV)),
                0.0f, noV };
            glm::vec2 integrated{};
            for (uint32_t sample = 0; sample < settings.brdfSamples; ++sample) {
                const glm::vec3 halfVector = importanceSampleGgx(
                    hammersley(sample, settings.brdfSamples), normal, roughness);
                const glm::vec3 light = glm::normalize(2.0f *
                    glm::dot(view, halfVector) * halfVector - view);
                const float noL = (std::max)(light.z, 0.0f);
                const float noH = (std::max)(halfVector.z, 0.0f);
                const float voH = (std::max)(glm::dot(view, halfVector), 0.0f);
                if (noL > 0.0f && noH > 0.0f && voH > 0.0f) {
                    const float geometry = materialGeometrySmith(noV, noL, roughness);
                    const float visibility = geometry * voH /
                        (std::max)(noH * noV, MaterialBsdfDenominatorEpsilon);
                    const float fresnelWeight = std::pow(1.0f - voH, 5.0f);
                    integrated += glm::vec2((1.0f - fresnelWeight) * visibility,
                        fresnelWeight * visibility);
                }
            }
            output = integrated / static_cast<float>(settings.brdfSamples);
        });
    cancel();
    return result;
}

std::vector<std::byte> makeCapturedCubeDiffuseIrradiance(
    std::span<const std::byte> radianceRgba16, uint32_t radianceSize,
    uint32_t irradianceSize) {
    if (irradianceSize == 0)
        throw std::invalid_argument(
            "Captured cube irradiance size must be nonzero.");
    const IrradianceSh coefficients = projectCapturedCubeDiffuseIrradiance(
        radianceRgba16, radianceSize);
    EnvironmentFloatCube cube{
        .baseSize = irradianceSize,
        .mips = { makeCubeMip(irradianceSize) },
    };
    for (uint32_t face = 0; face < 6u; ++face)
        for (uint32_t y = 0; y < irradianceSize; ++y)
            for (uint32_t x = 0; x < irradianceSize; ++x) {
                const float u = 2.0f *
                    (static_cast<float>(x) + 0.5f) / irradianceSize - 1.0f;
                const float v = 2.0f *
                    (static_cast<float>(y) + 0.5f) / irradianceSize - 1.0f;
                cube.mips[0][cubeIndex(irradianceSize, face, x, y)] =
                    glm::vec4(evaluateDiffuseIrradiance(coefficients,
                        environmentCubeDirection(face, u, v)), 1.0f);
            }
    return packCube(cube);
}

CookProduct makeConvolvedEnvironmentProduct(const AssetGuid& sourceTextureGuid,
    const ConvolvedEnvironment& environment,
    const EnvironmentConvolutionSettings& settings, std::string toolVersion) {
    if (environment.radiance.mips.empty() || environment.irradiance.mips.size() != 1 ||
        environment.prefilteredSpecular.mips.empty() || environment.brdfLut.empty())
        throw std::invalid_argument("Convolved environment is incomplete.");
    std::vector<std::byte> radiance = packCube(environment.radiance);
    std::vector<std::byte> irradiance = packCube(environment.irradiance);
    std::vector<std::byte> prefiltered = packCube(environment.prefilteredSpecular);
    std::vector<std::byte> brdf;
    brdf.reserve(environment.brdfLut.size() * sizeof(uint16_t) * 2u);
    for (glm::vec2 value : environment.brdfLut) appendHalf(brdf, value);

    CookedEnvironmentManifest manifest{
        .sourceTextureGuid = sourceTextureGuid,
        .sourcePrimaries = settings.sourcePrimaries,
        .sourceRadianceScale = settings.sourceRadianceScale,
        .convolutionImplementation = "iridium_cpu_reference_v3",
        .sampleSequence =
            "exact_source_texel_sh9_v1+hammersley_base2_vdc_pdf_mips_v2",
        .toolVersion = std::move(toolVersion),
        .radiance = { environment.radiance.baseSize, environment.radiance.baseSize,
            static_cast<uint32_t>(environment.radiance.mips.size()), 6,
            TextureFormat::RGBA16_SFloat },
        .irradiance = { environment.irradiance.baseSize, environment.irradiance.baseSize,
            1, 6, TextureFormat::RGBA16_SFloat },
        .prefilteredSpecular = { environment.prefilteredSpecular.baseSize,
            environment.prefilteredSpecular.baseSize,
            static_cast<uint32_t>(environment.prefilteredSpecular.mips.size()), 6,
            TextureFormat::RGBA16_SFloat },
        .brdfLut = { environment.brdfLutSize, environment.brdfLutSize, 1, 1,
            TextureFormat::RG16_SFloat },
    };
    return makeCookedEnvironmentProduct(manifest,
        { radiance, irradiance, prefiltered, brdf });
}

} // namespace Iridium
