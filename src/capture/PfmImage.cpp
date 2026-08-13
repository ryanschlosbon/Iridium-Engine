#include "capture/PfmImage.h"

#include <bit>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace Iridium {

    namespace {

        [[nodiscard]] uint64_t checkedFloatCount(uint32_t width, uint32_t height,
            uint32_t channels) {
            const uint64_t count = static_cast<uint64_t>(width) * height * channels;
            if (width == 0 || height == 0 ||
                count > std::numeric_limits<size_t>::max() / sizeof(float)) {
                throw std::invalid_argument("PFM dimensions are invalid.");
            }
            return count;
        }

        void writeFloatLittleEndian(std::ostream& output, float value) {
            uint32_t bits = std::bit_cast<uint32_t>(value);
            const char bytes[4] = {
                static_cast<char>(bits & 0xffu),
                static_cast<char>((bits >> 8u) & 0xffu),
                static_cast<char>((bits >> 16u) & 0xffu),
                static_cast<char>((bits >> 24u) & 0xffu),
            };
            output.write(bytes, sizeof(bytes));
        }

        [[nodiscard]] float readFloatLittleEndian(std::istream& input) {
            unsigned char bytes[4]{};
            input.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
            if (!input) throw std::runtime_error("PFM pixel storage is truncated.");
            const uint32_t bits = static_cast<uint32_t>(bytes[0]) |
                (static_cast<uint32_t>(bytes[1]) << 8u) |
                (static_cast<uint32_t>(bytes[2]) << 16u) |
                (static_cast<uint32_t>(bytes[3]) << 24u);
            return std::bit_cast<float>(bits);
        }

        [[nodiscard]] float captureFloat(const std::byte* source) {
            float value = 0.0f;
            std::memcpy(&value, source, sizeof(value));
            return value;
        }

    } // namespace

    void writeFrameCapturePfm(const std::filesystem::path& path,
        const FrameCapture& capture) {
        if (capture.pixelFormat != FrameCapturePixelFormat::Rgba32Float ||
            (capture.colorDomain != FrameCaptureColorDomain::SceneLinearAcesCg &&
                capture.colorDomain != FrameCaptureColorDomain::DisplayLinearHdr)) {
            throw std::invalid_argument(
                "PFM capture requires linear RGBA32 float pixels.");
        }
        (void)checkedFloatCount(capture.width, capture.height, 3);
        const uint64_t minimumPitch = static_cast<uint64_t>(capture.width) * 4u *
            sizeof(float);
        if (capture.rowPitchBytes < minimumPitch ||
            capture.pixels.size() < static_cast<uint64_t>(capture.rowPitchBytes) *
                capture.height) {
            throw std::invalid_argument("PFM capture pixel storage is truncated.");
        }

        std::ofstream output(path, std::ios::binary);
        if (!output) {
            throw std::runtime_error("Failed to open PFM output: " +
                path.generic_string());
        }
        output << "PF\n" << capture.width << ' ' << capture.height << "\n-1.0\n";
        // PFM stores the bottom row first. The in-memory capture remains top-left.
        for (uint32_t storedY = 0; storedY < capture.height; ++storedY) {
            const uint32_t sourceY = capture.height - 1u - storedY;
            const std::byte* row = capture.pixels.data() +
                static_cast<size_t>(sourceY) * capture.rowPitchBytes;
            for (uint32_t x = 0; x < capture.width; ++x) {
                const std::byte* rgba = row + static_cast<size_t>(x) * 4u *
                    sizeof(float);
                writeFloatLittleEndian(output, captureFloat(rgba));
                writeFloatLittleEndian(output, captureFloat(rgba + sizeof(float)));
                writeFloatLittleEndian(output, captureFloat(rgba + 2u * sizeof(float)));
            }
        }
        if (!output) {
            throw std::runtime_error("Failed while writing PFM output: " +
                path.generic_string());
        }
    }

    PfmImage readPfm(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Failed to open PFM input: " +
                path.generic_string());
        }
        std::string magic;
        uint32_t width = 0;
        uint32_t height = 0;
        float scale = 0.0f;
        input >> magic >> width >> height >> scale;
        if (magic != "PF" || scale >= 0.0f || scale != -1.0f) {
            throw std::runtime_error(
                "Only little-endian RGB float PFM files with scale -1 are supported.");
        }
        const int separator = input.get();
        if (separator != '\n') {
            throw std::runtime_error("PFM header is not canonical.");
        }

        PfmImage result{};
        result.width = width;
        result.height = height;
        result.rgb32f.resize(static_cast<size_t>(checkedFloatCount(width, height, 3)));
        for (uint32_t storedY = 0; storedY < height; ++storedY) {
            const uint32_t targetY = height - 1u - storedY;
            float* row = result.rgb32f.data() +
                static_cast<size_t>(targetY) * width * 3u;
            for (uint32_t x = 0; x < width * 3u; ++x) {
                row[x] = readFloatLittleEndian(input);
            }
        }
        if (input.peek() != std::char_traits<char>::eof()) {
            throw std::runtime_error("Canonical PFM contains trailing bytes.");
        }
        return result;
    }

} // namespace Iridium
