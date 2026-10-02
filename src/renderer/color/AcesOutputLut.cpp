#include "renderer/color/AcesOutputLut.h"

#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace Iridium::Color {
namespace {

    constexpr std::array<char, 8> Magic{ 'I', 'R', 'A', 'C', '2', 'L', 'U', 'T' };
    constexpr uint32_t SchemaVersion = 1;
    constexpr uint64_t HeaderBytes = 32;

    template <typename T>
    T readLittleEndian(std::istream& input) {
        static_assert(std::is_trivially_copyable_v<T>);
        std::array<std::byte, sizeof(T)> bytes{};
        input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        if (!input) throw std::runtime_error("ACES output LUT header is truncated.");
        if constexpr (std::endian::native == std::endian::big) {
            std::reverse(bytes.begin(), bytes.end());
        }
        T value{};
        std::memcpy(&value, bytes.data(), sizeof(T));
        return value;
    }

} // namespace

AcesOutputLut loadAcesOutputLut(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to open ACES output LUT: " +
            path.generic_string());
    }
    std::array<char, Magic.size()> magic{};
    input.read(magic.data(), magic.size());
    if (!input || magic != Magic) {
        throw std::runtime_error("ACES output LUT has an invalid magic value.");
    }
    const uint32_t schemaVersion = readLittleEndian<uint32_t>(input);
    AcesOutputLut result{};
    result.size = readLittleEndian<uint32_t>(input);
    result.minimumLog2 = readLittleEndian<float>(input);
    result.maximumLog2 = readLittleEndian<float>(input);
    const uint64_t payloadBytes = readLittleEndian<uint64_t>(input);
    if (schemaVersion != SchemaVersion || result.size < 2 || result.size > 257 ||
        !std::isfinite(result.minimumLog2) || !std::isfinite(result.maximumLog2) ||
        result.minimumLog2 >= result.maximumLog2) {
        throw std::runtime_error("ACES output LUT header values are invalid.");
    }
    const uint64_t texelCount = static_cast<uint64_t>(result.size) * result.size *
        result.size;
    if (texelCount > std::numeric_limits<size_t>::max() / (4u * sizeof(float)) ||
        payloadBytes != texelCount * 4u * sizeof(float)) {
        throw std::runtime_error("ACES output LUT payload size is invalid.");
    }
    const uint64_t fileBytes = std::filesystem::file_size(path);
    if (fileBytes != HeaderBytes + payloadBytes) {
        throw std::runtime_error("ACES output LUT file length is invalid.");
    }
    result.rgba32f.resize(static_cast<size_t>(texelCount) * 4u);
    input.read(reinterpret_cast<char*>(result.rgba32f.data()),
        static_cast<std::streamsize>(payloadBytes));
    if (!input) throw std::runtime_error("ACES output LUT payload is truncated.");
    if constexpr (std::endian::native == std::endian::big) {
        for (float& value : result.rgba32f) {
            std::array<std::byte, sizeof(float)> bytes{};
            std::memcpy(bytes.data(), &value, sizeof(float));
            std::reverse(bytes.begin(), bytes.end());
            std::memcpy(&value, bytes.data(), sizeof(float));
        }
    }
    for (const float value : result.rgba32f) {
        if (!std::isfinite(value)) {
            throw std::runtime_error("ACES output LUT contains a non-finite value.");
        }
    }
    return result;
}

std::array<float, 3> sampleAcesOutputLutEncoded(const AcesOutputLut& lut,
    std::array<float, 3> sceneAcesCg) noexcept {
    if (lut.size < 2 || lut.rgba32f.size() !=
        static_cast<size_t>(lut.size) * lut.size * lut.size * 4u ||
        !(lut.minimumLog2 < lut.maximumLog2)) {
        return {};
    }

    std::array<uint32_t, 3> lower{};
    std::array<uint32_t, 3> upper{};
    std::array<float, 3> fraction{};
    const float offset = std::exp2(lut.minimumLog2);
    const float logRange = std::log2(std::exp2(lut.maximumLog2) + offset) -
        lut.minimumLog2;
    for (size_t channel = 0; channel < 3; ++channel) {
        float value = sceneAcesCg[channel];
        if (std::isnan(value) || value < 0.0f) value = 0.0f;
        if (std::isinf(value)) value = value > 0.0f
            ? std::exp2(lut.maximumLog2) - offset : 0.0f;
        const float coordinate = std::clamp(
            (std::log2(value + offset) - lut.minimumLog2) / logRange,
            0.0f, 1.0f) * static_cast<float>(lut.size - 1u);
        lower[channel] = static_cast<uint32_t>(std::floor(coordinate));
        upper[channel] = std::min(lower[channel] + 1u, lut.size - 1u);
        fraction[channel] = coordinate - static_cast<float>(lower[channel]);
    }

    const auto texel = [&lut](uint32_t x, uint32_t y, uint32_t z) {
        const size_t index = (static_cast<size_t>(y) * lut.size * lut.size +
            static_cast<size_t>(z) * lut.size + x) * 4u;
        return std::array<float, 3>{ lut.rgba32f[index], lut.rgba32f[index + 1u],
            lut.rgba32f[index + 2u] };
    };
    const auto addScaled = [](std::array<float, 3>& result,
        const std::array<float, 3>& value, float weight) {
        for (size_t channel = 0; channel < 3; ++channel) {
            result[channel] += value[channel] * weight;
        }
    };

    std::array<float, 3> result{};
    const auto add = [&](uint32_t x, uint32_t y, uint32_t z, float weight) {
        addScaled(result, texel(x, y, z), weight);
    };
    const float r = fraction[0];
    const float g = fraction[1];
    const float b = fraction[2];
    if (r >= g) {
        if (g >= b) {
            add(lower[0], lower[1], lower[2], 1.0f - r);
            add(upper[0], lower[1], lower[2], r - g);
            add(upper[0], upper[1], lower[2], g - b);
            add(upper[0], upper[1], upper[2], b);
        }
        else if (r >= b) {
            add(lower[0], lower[1], lower[2], 1.0f - r);
            add(upper[0], lower[1], lower[2], r - b);
            add(upper[0], lower[1], upper[2], b - g);
            add(upper[0], upper[1], upper[2], g);
        }
        else {
            add(lower[0], lower[1], lower[2], 1.0f - b);
            add(lower[0], lower[1], upper[2], b - r);
            add(upper[0], lower[1], upper[2], r - g);
            add(upper[0], upper[1], upper[2], g);
        }
    }
    else if (b >= g) {
        add(lower[0], lower[1], lower[2], 1.0f - b);
        add(lower[0], lower[1], upper[2], b - g);
        add(lower[0], upper[1], upper[2], g - r);
        add(upper[0], upper[1], upper[2], r);
    }
    else if (b >= r) {
        add(lower[0], lower[1], lower[2], 1.0f - g);
        add(lower[0], upper[1], lower[2], g - b);
        add(lower[0], upper[1], upper[2], b - r);
        add(upper[0], upper[1], upper[2], r);
    }
    else {
        add(lower[0], lower[1], lower[2], 1.0f - g);
        add(lower[0], upper[1], lower[2], g - r);
        add(upper[0], upper[1], lower[2], r - b);
        add(upper[0], upper[1], upper[2], b);
    }
    for (float& channel : result) channel = std::clamp(channel, 0.0f, 1.0f);
    return result;
}

} // namespace Iridium::Color
