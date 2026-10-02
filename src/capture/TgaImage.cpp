#include "capture/TgaImage.h"

#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace Iridium {

    namespace {

        constexpr size_t TgaHeaderSize = 18;
        constexpr uint8_t TgaTrueColorImage = 2;
        constexpr uint8_t TgaTopLeftWithEightAlphaBits = 0x28;

        [[nodiscard]] size_t checkedByteCount(uint32_t width, uint32_t height) {
            if (width == 0 || height == 0 ||
                width > std::numeric_limits<uint16_t>::max() ||
                height > std::numeric_limits<uint16_t>::max()) {
                throw std::invalid_argument(
                    "TGA dimensions must be between 1 and 65535 pixels.");
            }
            const uint64_t byteCount = static_cast<uint64_t>(width) * height * 4;
            if (byteCount > std::numeric_limits<size_t>::max()) {
                throw std::overflow_error("TGA pixel storage exceeds addressable memory.");
            }
            return static_cast<size_t>(byteCount);
        }

        void storeLittleEndian16(std::array<std::byte, TgaHeaderSize>& header,
            size_t offset, uint16_t value) noexcept {
            header[offset] = static_cast<std::byte>(value & 0xff);
            header[offset + 1] = static_cast<std::byte>((value >> 8) & 0xff);
        }

        [[nodiscard]] uint16_t loadLittleEndian16(
            const std::array<std::byte, TgaHeaderSize>& header,
            size_t offset) noexcept {
            return static_cast<uint16_t>(std::to_integer<uint8_t>(header[offset])) |
                static_cast<uint16_t>(
                    std::to_integer<uint8_t>(header[offset + 1]) << 8);
        }

        void requireWritablePath(const std::filesystem::path& path) {
            if (path.empty() || path.filename().empty()) {
                throw std::invalid_argument("TGA output path must name a file.");
            }
            if (std::filesystem::exists(path)) {
                throw std::runtime_error("Refusing to overwrite capture image: " +
                    path.generic_string());
            }
            const std::filesystem::path parent = path.parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent);
        }

    } // namespace

    void writeTga(const std::filesystem::path& path, const TgaImage& image) {
        const size_t byteCount = checkedByteCount(image.width, image.height);
        if (image.bgra8.size() != byteCount) {
            throw std::invalid_argument(
                "TGA images must contain tightly packed BGRA8 pixels.");
        }
        requireWritablePath(path);

        std::array<std::byte, TgaHeaderSize> header{};
        header[2] = static_cast<std::byte>(TgaTrueColorImage);
        storeLittleEndian16(header, 12, static_cast<uint16_t>(image.width));
        storeLittleEndian16(header, 14, static_cast<uint16_t>(image.height));
        header[16] = static_cast<std::byte>(32);
        header[17] = static_cast<std::byte>(TgaTopLeftWithEightAlphaBits);

        std::ofstream output(path, std::ios::binary | std::ios::out);
        if (!output) {
            throw std::runtime_error("Failed to open TGA output: " +
                path.generic_string());
        }
        output.write(reinterpret_cast<const char*>(header.data()), header.size());
        output.write(reinterpret_cast<const char*>(image.bgra8.data()),
            static_cast<std::streamsize>(image.bgra8.size()));
        output.flush();
        if (!output) {
            throw std::runtime_error("Failed while writing TGA output: " +
                path.generic_string());
        }
    }

    void writeFrameCaptureTga(const std::filesystem::path& path,
        const FrameCapture& capture) {
        const size_t byteCount = checkedByteCount(capture.width, capture.height);
        const uint64_t minimumRowPitch = static_cast<uint64_t>(capture.width) * 4;
        if (capture.rowPitchBytes < minimumRowPitch ||
            capture.pixels.size() <
                static_cast<uint64_t>(capture.rowPitchBytes) * capture.height) {
            throw std::invalid_argument("Frame capture pixel storage is truncated.");
        }

        TgaImage image{};
        image.width = capture.width;
        image.height = capture.height;
        image.bgra8.resize(byteCount);
        const bool sourceIsRgba =
            capture.pixelFormat == FrameCapturePixelFormat::Rgba8Srgb ||
            capture.pixelFormat == FrameCapturePixelFormat::Rgba8Unorm;
        for (uint32_t y = 0; y < capture.height; ++y) {
            const std::byte* source = capture.pixels.data() +
                static_cast<size_t>(y) * capture.rowPitchBytes;
            std::byte* destination = image.bgra8.data() +
                static_cast<size_t>(y) * capture.width * 4;
            for (uint32_t x = 0; x < capture.width; ++x) {
                const size_t pixel = static_cast<size_t>(x) * 4;
                if (sourceIsRgba) {
                    destination[pixel] = source[pixel + 2];
                    destination[pixel + 1] = source[pixel + 1];
                    destination[pixel + 2] = source[pixel];
                    destination[pixel + 3] = source[pixel + 3];
                }
                else {
                    destination[pixel] = source[pixel];
                    destination[pixel + 1] = source[pixel + 1];
                    destination[pixel + 2] = source[pixel + 2];
                    destination[pixel + 3] = source[pixel + 3];
                }
            }
        }
        writeTga(path, image);
    }

    TgaImage readTga(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary | std::ios::in);
        if (!input) {
            throw std::runtime_error("Failed to open TGA input: " +
                path.generic_string());
        }
        std::array<std::byte, TgaHeaderSize> header{};
        input.read(reinterpret_cast<char*>(header.data()), header.size());
        if (input.gcount() != static_cast<std::streamsize>(header.size())) {
            throw std::runtime_error("TGA header is truncated: " +
                path.generic_string());
        }
        if (std::to_integer<uint8_t>(header[0]) != 0 ||
            std::to_integer<uint8_t>(header[1]) != 0 ||
            std::to_integer<uint8_t>(header[2]) != TgaTrueColorImage ||
            std::to_integer<uint8_t>(header[16]) != 32 ||
            std::to_integer<uint8_t>(header[17]) !=
                TgaTopLeftWithEightAlphaBits) {
            throw std::runtime_error(
                "Only canonical top-left uncompressed 32-bit BGRA TGA is supported: " +
                path.generic_string());
        }
        for (size_t offset = 3; offset <= 11; ++offset) {
            if (header[offset] != std::byte{ 0 }) {
                throw std::runtime_error(
                    "Canonical capture TGA requires zero color-map and origin fields: " +
                    path.generic_string());
            }
        }

        TgaImage image{};
        image.width = loadLittleEndian16(header, 12);
        image.height = loadLittleEndian16(header, 14);
        image.bgra8.resize(checkedByteCount(image.width, image.height));
        input.read(reinterpret_cast<char*>(image.bgra8.data()),
            static_cast<std::streamsize>(image.bgra8.size()));
        if (input.gcount() != static_cast<std::streamsize>(image.bgra8.size())) {
            throw std::runtime_error("TGA pixel payload is truncated: " +
                path.generic_string());
        }
        char trailing = 0;
        if (input.read(&trailing, 1)) {
            throw std::runtime_error("Canonical capture TGA contains trailing bytes: " +
                path.generic_string());
        }
        return image;
    }

} // namespace Iridium
