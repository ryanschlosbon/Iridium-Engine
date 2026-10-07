#include "temporal/TemporalMetrics.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace Iridium::TemporalMetrics {

    namespace {

        std::atomic<unsigned> g_workerCount{ 0 };

        struct FileBytes {
            std::unique_ptr<uint8_t[]> data;
            size_t size = 0;
        };

        [[nodiscard]] FileBytes readFileBytes(const std::filesystem::path& path) {
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input) {
                throw std::runtime_error("Failed to open input: " + path.generic_string());
            }
            const std::streamoff end = input.tellg();
            if (end < 0) {
                throw std::runtime_error("Failed to size input: " + path.generic_string());
            }
            FileBytes bytes{};
            bytes.size = static_cast<size_t>(end);
            bytes.data.reset(new uint8_t[std::max<size_t>(bytes.size, 1)]);
            input.seekg(0, std::ios::beg);
            input.read(reinterpret_cast<char*>(bytes.data.get()),
                static_cast<std::streamsize>(bytes.size));
            if (!input || static_cast<size_t>(input.gcount()) != bytes.size) {
                throw std::runtime_error("Failed to read input: " + path.generic_string());
            }
            return bytes;
        }

        void writeFileBytes(const std::filesystem::path& path, const void* data,
            size_t size) {
            if (path.empty() || path.filename().empty()) {
                throw std::invalid_argument("Output path must name a file.");
            }
            const std::filesystem::path parent = path.parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent);
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error("Failed to open output: " + path.generic_string());
            }
            output.write(static_cast<const char*>(data),
                static_cast<std::streamsize>(size));
            output.flush();
            if (!output) {
                throw std::runtime_error("Failed while writing output: " +
                    path.generic_string());
            }
        }

        [[nodiscard]] bool isSpace(uint8_t c) {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        }

        [[nodiscard]] std::string nextToken(const FileBytes& bytes, size_t& position) {
            while (position < bytes.size && isSpace(bytes.data[position])) ++position;
            const size_t begin = position;
            while (position < bytes.size && !isSpace(bytes.data[position])) ++position;
            if (begin == position) throw std::runtime_error("PFM header is truncated.");
            return std::string(reinterpret_cast<const char*>(bytes.data.get()) + begin,
                position - begin);
        }

        [[nodiscard]] uint32_t parseDimension(const std::string& token) {
            size_t used = 0;
            unsigned long value = 0;
            try {
                value = std::stoul(token, &used);
            }
            catch (const std::exception&) {
                throw std::runtime_error("PFM dimension is invalid: " + token);
            }
            if (used != token.size() || value == 0 || value > 65535u) {
                throw std::runtime_error("PFM dimension is invalid: " + token);
            }
            return static_cast<uint32_t>(value);
        }

        [[nodiscard]] float loadFloat(const uint8_t* source, bool bigEndian) {
            uint32_t bits = 0;
            std::memcpy(&bits, source, sizeof(bits));
            const bool swap = bigEndian != (std::endian::native == std::endian::big);
            if (swap) {
                bits = ((bits & 0x000000ffu) << 24u) | ((bits & 0x0000ff00u) << 8u) |
                    ((bits & 0x00ff0000u) >> 8u) | ((bits & 0xff000000u) >> 24u);
            }
            return std::bit_cast<float>(bits);
        }

        void storeFloatLittleEndian(uint8_t* destination, float value) {
            const uint32_t bits = std::bit_cast<uint32_t>(value);
            destination[0] = static_cast<uint8_t>(bits & 0xffu);
            destination[1] = static_cast<uint8_t>((bits >> 8u) & 0xffu);
            destination[2] = static_cast<uint8_t>((bits >> 16u) & 0xffu);
            destination[3] = static_cast<uint8_t>((bits >> 24u) & 0xffu);
        }

        [[nodiscard]] std::string lowerExtension(const std::filesystem::path& path) {
            std::string extension = path.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return extension;
        }

        [[nodiscard]] size_t pixelCount(uint32_t width, uint32_t height) {
            return static_cast<size_t>(width) * height;
        }

        void requireSameSize(uint32_t aw, uint32_t ah, uint32_t bw, uint32_t bh,
            const char* what) {
            if (aw != bw || ah != bh) {
                throw std::invalid_argument(std::string(what) +
                    ": image dimensions do not match (" + std::to_string(aw) + "x" +
                    std::to_string(ah) + " vs " + std::to_string(bw) + "x" +
                    std::to_string(bh) + ").");
            }
        }

        void requireMask(const Mask* mask, uint32_t width, uint32_t height) {
            if (mask == nullptr) return;
            requireSameSize(mask->width, mask->height, width, height, "mask");
            if (mask->include.size() != pixelCount(width, height)) {
                throw std::invalid_argument("Mask storage does not match its size.");
            }
        }

        void validatePlane(const Plane& plane) {
            if (plane.values.size() != pixelCount(plane.width, plane.height)) {
                throw std::invalid_argument("Plane storage does not match its size.");
            }
        }

        [[nodiscard]] float toneMapLuminance(double y, uint64_t& nonFinite) {
            if (std::isnan(y)) {
                ++nonFinite;
                return 0.0f;
            }
            if (y <= 0.0) return 0.0f;
            if (std::isinf(y)) return 1.0f;
            return static_cast<float>(y / (1.0 + y));
        }

        [[nodiscard]] double toneMapChannel(double value) {
            const double v = std::max(value, 0.0);
            return v / (1.0 + v);
        }

        [[nodiscard]] std::array<double, SsimWindowSize> gaussianWindow() {
            std::array<double, SsimWindowSize> weights{};
            constexpr double sigma = 1.5;
            const int radius = static_cast<int>(SsimWindowSize / 2);
            double total = 0.0;
            std::array<double, SsimWindowSize> raw{};
            for (int i = -radius; i <= radius; ++i) {
                raw[static_cast<size_t>(i + radius)] =
                    std::exp(-(i * i) / (2.0 * sigma * sigma));
                total += raw[static_cast<size_t>(i + radius)];
            }
            for (size_t i = 0; i < weights.size(); ++i) {
                weights[i] = raw[i] / total;
            }
            return weights;
        }

    } // namespace

    // ---------------------------------------------------------------- images

    uint64_t Mask::area() const {
        uint64_t count = 0;
        for (const uint8_t value : include) count += value != 0 ? 1u : 0u;
        return count;
    }

    const char* domainName(Domain domain) {
        return domain == Domain::Scene ? "scene" : "sdr";
    }

    Domain domainForPath(const std::filesystem::path& path) {
        const std::string extension = lowerExtension(path);
        if (extension == ".pfm") return Domain::Scene;
        if (extension == ".tga") return Domain::Sdr;
        throw std::invalid_argument("Unsupported image extension (expected .pfm or .tga): " +
            path.generic_string());
    }

    // ------------------------------------------------------------------- I/O

    FloatImage readPfm(const std::filesystem::path& path) {
        const FileBytes bytes = readFileBytes(path);
        size_t position = 0;
        const std::string magic = nextToken(bytes, position);
        uint32_t channels = 0;
        if (magic == "PF") channels = 3;
        else if (magic == "Pf") channels = 1;
        else throw std::runtime_error("Not a PFM file: " + path.generic_string());
        const uint32_t width = parseDimension(nextToken(bytes, position));
        const uint32_t height = parseDimension(nextToken(bytes, position));
        const std::string scaleToken = nextToken(bytes, position);
        double scale = 0.0;
        try {
            scale = std::stod(scaleToken);
        }
        catch (const std::exception&) {
            throw std::runtime_error("PFM scale is invalid: " + path.generic_string());
        }
        if (scale == 0.0 || !std::isfinite(scale)) {
            throw std::runtime_error("PFM scale is invalid: " + path.generic_string());
        }
        // Exactly one whitespace byte separates the header from the raster.
        if (position >= bytes.size || !isSpace(bytes.data[position])) {
            throw std::runtime_error("PFM header is truncated: " + path.generic_string());
        }
        ++position;
        const bool bigEndian = scale > 0.0;
        const size_t rowBytes = static_cast<size_t>(width) * channels * sizeof(float);
        const size_t expected = rowBytes * height;
        if (bytes.size - position != expected) {
            throw std::runtime_error("PFM raster size mismatch (" +
                std::to_string(bytes.size - position) + " bytes, expected " +
                std::to_string(expected) + "): " + path.generic_string());
        }

        FloatImage image{};
        image.width = width;
        image.height = height;
        image.rgb.resize(pixelCount(width, height) * 3u);
        const uint8_t* raster = bytes.data.get() + position;
        const bool directCopy = channels == 3 && !bigEndian &&
            std::endian::native == std::endian::little;
        parallelForRows(height, [&](uint32_t, uint32_t rowBegin, uint32_t rowEnd,
            unsigned) {
            for (uint32_t y = rowBegin; y < rowEnd; ++y) {
                // PFM stores the bottom row first.
                const uint8_t* source = raster + static_cast<size_t>(height - 1u - y) *
                    rowBytes;
                float* destination = image.rgb.data() + static_cast<size_t>(y) * width * 3u;
                if (directCopy) {
                    std::memcpy(destination, source, rowBytes);
                    continue;
                }
                for (uint32_t x = 0; x < width; ++x) {
                    if (channels == 3) {
                        for (uint32_t c = 0; c < 3; ++c) {
                            destination[x * 3u + c] = loadFloat(
                                source + (static_cast<size_t>(x) * 3u + c) * 4u, bigEndian);
                        }
                    }
                    else {
                        const float value = loadFloat(source + static_cast<size_t>(x) * 4u,
                            bigEndian);
                        destination[x * 3u] = value;
                        destination[x * 3u + 1] = value;
                        destination[x * 3u + 2] = value;
                    }
                }
            }
        });
        return image;
    }

    void writePfm(const std::filesystem::path& path, const FloatImage& image) {
        if (image.width == 0 || image.height == 0 ||
            image.rgb.size() != pixelCount(image.width, image.height) * 3u) {
            throw std::invalid_argument("PFM image storage does not match its size.");
        }
        const std::string header = "PF\n" + std::to_string(image.width) + ' ' +
            std::to_string(image.height) + "\n-1.0\n";
        const size_t rowBytes = static_cast<size_t>(image.width) * 3u * sizeof(float);
        std::vector<uint8_t> buffer(header.size() + rowBytes * image.height);
        std::memcpy(buffer.data(), header.data(), header.size());
        uint8_t* raster = buffer.data() + header.size();
        parallelForRows(image.height, [&](uint32_t, uint32_t rowBegin, uint32_t rowEnd,
            unsigned) {
            for (uint32_t y = rowBegin; y < rowEnd; ++y) {
                const float* source = image.rgb.data() +
                    static_cast<size_t>(y) * image.width * 3u;
                uint8_t* destination = raster +
                    static_cast<size_t>(image.height - 1u - y) * rowBytes;
                for (size_t i = 0; i < static_cast<size_t>(image.width) * 3u; ++i) {
                    storeFloatLittleEndian(destination + i * 4u, source[i]);
                }
            }
        });
        writeFileBytes(path, buffer.data(), buffer.size());
    }

    Rgb8Image readTga(const std::filesystem::path& path) {
        const FileBytes bytes = readFileBytes(path);
        if (bytes.size < 18) {
            throw std::runtime_error("TGA header is truncated: " + path.generic_string());
        }
        const uint8_t* header = bytes.data.get();
        const uint8_t idLength = header[0];
        if (header[1] != 0 || header[2] != 2) {
            throw std::runtime_error(
                "Only uncompressed true-colour TGA is supported: " + path.generic_string());
        }
        const uint32_t width = static_cast<uint32_t>(header[12]) |
            (static_cast<uint32_t>(header[13]) << 8u);
        const uint32_t height = static_cast<uint32_t>(header[14]) |
            (static_cast<uint32_t>(header[15]) << 8u);
        const uint32_t bitsPerPixel = header[16];
        const uint8_t descriptor = header[17];
        if (width == 0 || height == 0 || (bitsPerPixel != 24 && bitsPerPixel != 32) ||
            (descriptor & 0x10u) != 0) {
            throw std::runtime_error("Unsupported TGA layout: " + path.generic_string());
        }
        const bool topOrigin = (descriptor & 0x20u) != 0;
        const uint32_t pixelBytes = bitsPerPixel / 8u;
        const size_t offset = 18u + idLength;
        const size_t rowBytes = static_cast<size_t>(width) * pixelBytes;
        if (bytes.size < offset + rowBytes * height) {
            throw std::runtime_error("TGA pixel payload is truncated: " +
                path.generic_string());
        }
        Rgb8Image image{};
        image.width = width;
        image.height = height;
        image.rgb.resize(pixelCount(width, height) * 3u);
        const uint8_t* raster = bytes.data.get() + offset;
        parallelForRows(height, [&](uint32_t, uint32_t rowBegin, uint32_t rowEnd,
            unsigned) {
            for (uint32_t y = rowBegin; y < rowEnd; ++y) {
                const uint32_t storedY = topOrigin ? y : height - 1u - y;
                const uint8_t* source = raster + static_cast<size_t>(storedY) * rowBytes;
                uint8_t* destination = image.rgb.data() + static_cast<size_t>(y) * width * 3u;
                for (uint32_t x = 0; x < width; ++x) {
                    const uint8_t* bgr = source + static_cast<size_t>(x) * pixelBytes;
                    destination[x * 3u] = bgr[2];
                    destination[x * 3u + 1] = bgr[1];
                    destination[x * 3u + 2] = bgr[0];
                }
            }
        });
        return image;
    }

    void writeTga(const std::filesystem::path& path, const Rgb8Image& image) {
        if (image.width == 0 || image.height == 0 || image.width > 65535u ||
            image.height > 65535u ||
            image.rgb.size() != pixelCount(image.width, image.height) * 3u) {
            throw std::invalid_argument("TGA image storage does not match its size.");
        }
        std::vector<uint8_t> buffer(18u + pixelCount(image.width, image.height) * 4u);
        buffer[2] = 2;
        buffer[12] = static_cast<uint8_t>(image.width & 0xffu);
        buffer[13] = static_cast<uint8_t>(image.width >> 8u);
        buffer[14] = static_cast<uint8_t>(image.height & 0xffu);
        buffer[15] = static_cast<uint8_t>(image.height >> 8u);
        buffer[16] = 32;
        buffer[17] = 0x28; // top-left origin, eight alpha bits (engine layout)
        for (size_t pixel = 0; pixel < pixelCount(image.width, image.height); ++pixel) {
            uint8_t* destination = buffer.data() + 18u + pixel * 4u;
            destination[0] = image.rgb[pixel * 3u + 2];
            destination[1] = image.rgb[pixel * 3u + 1];
            destination[2] = image.rgb[pixel * 3u];
            destination[3] = 255;
        }
        writeFileBytes(path, buffer.data(), buffer.size());
    }

    Mask readMask(const std::filesystem::path& path) {
        Mask mask{};
        if (domainForPath(path) == Domain::Sdr) {
            const Rgb8Image image = readTga(path);
            mask.width = image.width;
            mask.height = image.height;
            mask.include.resize(pixelCount(image.width, image.height));
            for (size_t pixel = 0; pixel < mask.include.size(); ++pixel) {
                const uint8_t value = std::max({ image.rgb[pixel * 3u],
                    image.rgb[pixel * 3u + 1], image.rgb[pixel * 3u + 2] });
                mask.include[pixel] = value > 127 ? 1 : 0;
            }
        }
        else {
            const FloatImage image = readPfm(path);
            mask.width = image.width;
            mask.height = image.height;
            mask.include.resize(pixelCount(image.width, image.height));
            for (size_t pixel = 0; pixel < mask.include.size(); ++pixel) {
                mask.include[pixel] = image.rgb[pixel * 3u] > 0.5f ? 1 : 0;
            }
        }
        return mask;
    }

    Mask regionMask(uint32_t width, uint32_t height, uint32_t x0, uint32_t y0,
        uint32_t x1, uint32_t y1) {
        x1 = std::min(x1, width);
        y1 = std::min(y1, height);
        if (x0 >= x1 || y0 >= y1) {
            throw std::invalid_argument("Region is empty after clipping to the image.");
        }
        Mask mask{};
        mask.width = width;
        mask.height = height;
        mask.include.assign(pixelCount(width, height), 0);
        for (uint32_t y = y0; y < y1; ++y) {
            std::fill_n(mask.include.begin() + static_cast<ptrdiff_t>(
                static_cast<size_t>(y) * width + x0), x1 - x0, uint8_t{ 1 });
        }
        return mask;
    }

    void writeHeatmap(const std::filesystem::path& path, const Plane& plane,
        double scale) {
        validatePlane(plane);
        if (domainForPath(path) == Domain::Scene) {
            FloatImage image{};
            image.width = plane.width;
            image.height = plane.height;
            image.rgb.resize(plane.values.size() * 3u);
            for (size_t pixel = 0; pixel < plane.values.size(); ++pixel) {
                const float value = static_cast<float>(plane.values[pixel] * scale);
                image.rgb[pixel * 3u] = value;
                image.rgb[pixel * 3u + 1] = value;
                image.rgb[pixel * 3u + 2] = value;
            }
            writePfm(path, image);
            return;
        }
        Rgb8Image image{};
        image.width = plane.width;
        image.height = plane.height;
        image.rgb.resize(plane.values.size() * 3u);
        for (size_t pixel = 0; pixel < plane.values.size(); ++pixel) {
            const double value = std::clamp(plane.values[pixel] * scale, 0.0, 1.0);
            const uint8_t code = static_cast<uint8_t>(std::lround(value * 255.0));
            image.rgb[pixel * 3u] = code;
            image.rgb[pixel * 3u + 1] = code;
            image.rgb[pixel * 3u + 2] = code;
        }
        writeTga(path, image);
    }

    // ------------------------------------------------------------- execution

    void setWorkerCount(unsigned count) { g_workerCount.store(count); }

    unsigned workerCount() {
        const unsigned configured = g_workerCount.load();
        if (configured != 0) return configured;
        return std::max(1u, std::thread::hardware_concurrency());
    }

    uint32_t chunkCount(uint32_t rows) {
        return (rows + RowsPerChunk - 1u) / RowsPerChunk;
    }

    void parallelForRows(uint32_t rows,
        const std::function<void(uint32_t, uint32_t, uint32_t, unsigned)>& body) {
        const uint32_t chunks = chunkCount(rows);
        if (chunks == 0) return;
        const unsigned workers = std::min<unsigned>(workerCount(), chunks);
        std::atomic<uint32_t> next{ 0 };
        std::exception_ptr failure;
        std::mutex failureMutex;
        const auto run = [&](unsigned worker) {
            try {
                for (;;) {
                    const uint32_t chunk = next.fetch_add(1);
                    if (chunk >= chunks) break;
                    const uint32_t begin = chunk * RowsPerChunk;
                    body(chunk, begin, std::min(rows, begin + RowsPerChunk), worker);
                }
            }
            catch (...) {
                std::lock_guard lock(failureMutex);
                if (!failure) failure = std::current_exception();
                next.store(chunks);
            }
        };
        if (workers <= 1) {
            run(0);
        }
        else {
            std::vector<std::thread> threads;
            threads.reserve(workers - 1u);
            for (unsigned worker = 1; worker < workers; ++worker) {
                threads.emplace_back(run, worker);
            }
            run(0);
            for (std::thread& thread : threads) thread.join();
        }
        if (failure) std::rethrow_exception(failure);
    }

    // ------------------------------------------------------------------ luma

    Plane toneMappedLuma(const FloatImage& image, double exposureScale) {
        if (image.rgb.size() != pixelCount(image.width, image.height) * 3u) {
            throw std::invalid_argument("Image storage does not match its size.");
        }
        Plane plane{};
        plane.width = image.width;
        plane.height = image.height;
        plane.values.resize(pixelCount(image.width, image.height));
        std::vector<uint64_t> nonFinite(chunkCount(image.height), 0);
        parallelForRows(image.height, [&](uint32_t chunk, uint32_t rowBegin,
            uint32_t rowEnd, unsigned) {
            uint64_t bad = 0;
            for (size_t pixel = static_cast<size_t>(rowBegin) * image.width;
                pixel < static_cast<size_t>(rowEnd) * image.width; ++pixel) {
                const float* rgb = image.rgb.data() + pixel * 3u;
                const double y = exposureScale * (Ap1LuminanceR * rgb[0] +
                    Ap1LuminanceG * rgb[1] + Ap1LuminanceB * rgb[2]);
                plane.values[pixel] = toneMapLuminance(y, bad);
            }
            nonFinite[chunk] = bad;
        });
        for (const uint64_t count : nonFinite) plane.nonFiniteCount += count;
        return plane;
    }

    Plane encodedLuma(const Rgb8Image& image) {
        if (image.rgb.size() != pixelCount(image.width, image.height) * 3u) {
            throw std::invalid_argument("Image storage does not match its size.");
        }
        std::array<float, 256> r{}, g{}, b{};
        for (uint32_t code = 0; code < 256; ++code) {
            const double v = code / 255.0;
            r[code] = static_cast<float>(Rec709LumaR * v);
            g[code] = static_cast<float>(Rec709LumaG * v);
            b[code] = static_cast<float>(Rec709LumaB * v);
        }
        Plane plane{};
        plane.width = image.width;
        plane.height = image.height;
        plane.values.resize(pixelCount(image.width, image.height));
        parallelForRows(image.height, [&](uint32_t, uint32_t rowBegin, uint32_t rowEnd,
            unsigned) {
            for (size_t pixel = static_cast<size_t>(rowBegin) * image.width;
                pixel < static_cast<size_t>(rowEnd) * image.width; ++pixel) {
                const uint8_t* rgb = image.rgb.data() + pixel * 3u;
                plane.values[pixel] = r[rgb[0]] + g[rgb[1]] + b[rgb[2]];
            }
        });
        return plane;
    }

    // ------------------------------------------------------- reference error

    SceneErrorMetrics computeSceneError(const FloatImage& reference,
        const FloatImage& test, const Mask* mask, const SceneErrorOptions& options) {
        requireSameSize(reference.width, reference.height, test.width, test.height,
            "reference-error");
        requireMask(mask, reference.width, reference.height);
        if (reference.rgb.size() != pixelCount(reference.width, reference.height) * 3u ||
            test.rgb.size() != reference.rgb.size()) {
            throw std::invalid_argument("Image storage does not match its size.");
        }
        if (!(options.log2Epsilon > 0.0)) {
            throw std::invalid_argument("log2 epsilon must be positive.");
        }
        struct Partial {
            uint64_t pixels = 0, nonFinite = 0, above = 0;
            double sumLuminance2 = 0, sumLog = 0, sumLog2 = 0, sumTone2 = 0;
            double maxLinear = 0, maxTone = 0;
        };
        std::vector<Partial> partials(chunkCount(reference.height));
        const double scale = options.exposureScale;
        const double epsilon = options.log2Epsilon;
        parallelForRows(reference.height, [&](uint32_t chunk, uint32_t rowBegin,
            uint32_t rowEnd, unsigned) {
            Partial p{};
            for (size_t pixel = static_cast<size_t>(rowBegin) * reference.width;
                pixel < static_cast<size_t>(rowEnd) * reference.width; ++pixel) {
                if (mask != nullptr && mask->include[pixel] == 0) continue;
                const float* r = reference.rgb.data() + pixel * 3u;
                const float* t = test.rgb.data() + pixel * 3u;
                if (!std::isfinite(r[0]) || !std::isfinite(r[1]) || !std::isfinite(r[2]) ||
                    !std::isfinite(t[0]) || !std::isfinite(t[1]) || !std::isfinite(t[2])) {
                    ++p.nonFinite;
                    continue;
                }
                ++p.pixels;
                const double rr = scale * r[0], rg = scale * r[1], rb = scale * r[2];
                const double tr = scale * t[0], tg = scale * t[1], tb = scale * t[2];
                const double yr = Ap1LuminanceR * rr + Ap1LuminanceG * rg +
                    Ap1LuminanceB * rb;
                const double yt = Ap1LuminanceR * tr + Ap1LuminanceG * tg +
                    Ap1LuminanceB * tb;
                const double dy = yt - yr;
                p.sumLuminance2 += dy * dy;
                const double dl = std::abs(std::log2(std::max(yt, epsilon)) -
                    std::log2(std::max(yr, epsilon)));
                p.sumLog += dl;
                p.sumLog2 += dl * dl;
                const double referenceChannels[3] = { rr, rg, rb };
                const double testChannels[3] = { tr, tg, tb };
                double pixelTone = 0.0;
                for (int c = 0; c < 3; ++c) {
                    const double e = std::abs(toneMapChannel(testChannels[c]) -
                        toneMapChannel(referenceChannels[c]));
                    p.sumTone2 += e * e;
                    pixelTone = std::max(pixelTone, e);
                    p.maxLinear = std::max(p.maxLinear,
                        std::abs(testChannels[c] - referenceChannels[c]));
                }
                p.maxTone = std::max(p.maxTone, pixelTone);
                if (pixelTone > options.toneMappedThreshold) ++p.above;
            }
            partials[chunk] = p;
        });
        Partial total{};
        for (const Partial& p : partials) {
            total.pixels += p.pixels;
            total.nonFinite += p.nonFinite;
            total.above += p.above;
            total.sumLuminance2 += p.sumLuminance2;
            total.sumLog += p.sumLog;
            total.sumLog2 += p.sumLog2;
            total.sumTone2 += p.sumTone2;
            total.maxLinear = std::max(total.maxLinear, p.maxLinear);
            total.maxTone = std::max(total.maxTone, p.maxTone);
        }
        SceneErrorMetrics metrics{};
        metrics.pixelCount = total.pixels;
        metrics.nonFinitePixelCount = total.nonFinite;
        if (total.pixels > 0) {
            const double n = static_cast<double>(total.pixels);
            metrics.luminanceRmse = std::sqrt(total.sumLuminance2 / n);
            metrics.log2LuminanceMae = total.sumLog / n;
            metrics.log2LuminanceRmse = std::sqrt(total.sumLog2 / n);
            metrics.toneMappedRmse = std::sqrt(total.sumTone2 / (3.0 * n));
            metrics.maxAbsLinear = total.maxLinear;
            metrics.maxAbsToneMapped = total.maxTone;
            metrics.aboveThresholdPixelCount = total.above;
            metrics.aboveThresholdFraction = static_cast<double>(total.above) / n;
        }
        return metrics;
    }

    SsimResult computeSsim(const Plane& reference, const Plane& test, const Mask* mask,
        double dynamicRange) {
        validatePlane(reference);
        validatePlane(test);
        requireSameSize(reference.width, reference.height, test.width, test.height, "SSIM");
        requireMask(mask, reference.width, reference.height);
        if (reference.width < SsimWindowSize || reference.height < SsimWindowSize) {
            throw std::invalid_argument("SSIM requires images of at least 11x11 pixels.");
        }
        const std::array<double, SsimWindowSize> w = gaussianWindow();
        const uint32_t width = reference.width;
        const uint32_t height = reference.height;
        const uint32_t outWidth = width - (SsimWindowSize - 1u);
        const uint32_t outHeight = height - (SsimWindowSize - 1u);
        const uint32_t radius = SsimWindowSize / 2u;
        const size_t planeSize = static_cast<size_t>(height) * outWidth;
        // Horizontal pass: five filtered moments per input row. Moments are kept in
        // double: float cancellation in E[x^2] - E[x]^2 is visible against C2.
        std::vector<double> hx(planeSize), hy(planeSize), hxx(planeSize),
            hyy(planeSize), hxy(planeSize);
        parallelForRows(height, [&](uint32_t, uint32_t rowBegin, uint32_t rowEnd,
            unsigned) {
            for (uint32_t y = rowBegin; y < rowEnd; ++y) {
                const float* a = reference.values.data() + static_cast<size_t>(y) * width;
                const float* b = test.values.data() + static_cast<size_t>(y) * width;
                const size_t row = static_cast<size_t>(y) * outWidth;
                double* ox = hx.data() + row;
                double* oy = hy.data() + row;
                double* oxx = hxx.data() + row;
                double* oyy = hyy.data() + row;
                double* oxy = hxy.data() + row;
                std::fill_n(ox, outWidth, 0.0);
                std::fill_n(oy, outWidth, 0.0);
                std::fill_n(oxx, outWidth, 0.0);
                std::fill_n(oyy, outWidth, 0.0);
                std::fill_n(oxy, outWidth, 0.0);
                for (uint32_t k = 0; k < SsimWindowSize; ++k) {
                    const double wk = w[k];
                    const float* ak = a + k;
                    const float* bk = b + k;
                    for (uint32_t x = 0; x < outWidth; ++x) {
                        const double av = ak[x];
                        const double bv = bk[x];
                        ox[x] += wk * av;
                        oy[x] += wk * bv;
                        oxx[x] += wk * av * av;
                        oyy[x] += wk * bv * bv;
                        oxy[x] += wk * av * bv;
                    }
                }
            }
        });
        const double c1 = (0.01 * dynamicRange) * (0.01 * dynamicRange);
        const double c2 = (0.03 * dynamicRange) * (0.03 * dynamicRange);
        struct Partial {
            double sum = 0.0;
            uint64_t count = 0;
        };
        std::vector<Partial> partials(chunkCount(outHeight));
        parallelForRows(outHeight, [&](uint32_t chunk, uint32_t rowBegin,
            uint32_t rowEnd, unsigned) {
            std::vector<double> mx(outWidth), my(outWidth), mxx(outWidth),
                myy(outWidth), mxy(outWidth);
            Partial p{};
            for (uint32_t y = rowBegin; y < rowEnd; ++y) {
                std::fill(mx.begin(), mx.end(), 0.0);
                std::fill(my.begin(), my.end(), 0.0);
                std::fill(mxx.begin(), mxx.end(), 0.0);
                std::fill(myy.begin(), myy.end(), 0.0);
                std::fill(mxy.begin(), mxy.end(), 0.0);
                for (uint32_t k = 0; k < SsimWindowSize; ++k) {
                    const double wk = w[k];
                    const size_t row = static_cast<size_t>(y + k) * outWidth;
                    const double* sx = hx.data() + row;
                    const double* sy = hy.data() + row;
                    const double* sxx = hxx.data() + row;
                    const double* syy = hyy.data() + row;
                    const double* sxy = hxy.data() + row;
                    for (uint32_t x = 0; x < outWidth; ++x) {
                        mx[x] += wk * sx[x];
                        my[x] += wk * sy[x];
                        mxx[x] += wk * sxx[x];
                        myy[x] += wk * syy[x];
                        mxy[x] += wk * sxy[x];
                    }
                }
                const uint8_t* centreMask = mask != nullptr ?
                    mask->include.data() + static_cast<size_t>(y + radius) * width + radius :
                    nullptr;
                for (uint32_t x = 0; x < outWidth; ++x) {
                    if (centreMask != nullptr && centreMask[x] == 0) continue;
                    const double ux = mx[x];
                    const double uy = my[x];
                    const double vx = mxx[x] - ux * ux;
                    const double vy = myy[x] - uy * uy;
                    const double cxy = mxy[x] - ux * uy;
                    const double numerator = (2.0 * ux * uy + c1) * (2.0 * cxy + c2);
                    const double denominator = (ux * ux + uy * uy + c1) * (vx + vy + c2);
                    p.sum += numerator / denominator;
                    ++p.count;
                }
            }
            partials[chunk] = p;
        });
        Partial total{};
        for (const Partial& p : partials) {
            total.sum += p.sum;
            total.count += p.count;
        }
        SsimResult result{};
        result.windowCount = total.count;
        result.mean = total.count > 0 ? total.sum / static_cast<double>(total.count) : 1.0;
        return result;
    }

    SdrErrorMetrics computeSdrError(const Rgb8Image& reference, const Rgb8Image& test,
        const Mask* mask, const SdrErrorOptions& options) {
        requireSameSize(reference.width, reference.height, test.width, test.height,
            "reference-error");
        requireMask(mask, reference.width, reference.height);
        if (reference.rgb.size() != pixelCount(reference.width, reference.height) * 3u ||
            test.rgb.size() != reference.rgb.size()) {
            throw std::invalid_argument("Image storage does not match its size.");
        }
        struct Partial {
            uint64_t pixels = 0, changed = 0, sumSq = 0;
            uint32_t maxAbs = 0;
        };
        std::vector<Partial> partials(chunkCount(reference.height));
        parallelForRows(reference.height, [&](uint32_t chunk, uint32_t rowBegin,
            uint32_t rowEnd, unsigned) {
            Partial p{};
            for (size_t pixel = static_cast<size_t>(rowBegin) * reference.width;
                pixel < static_cast<size_t>(rowEnd) * reference.width; ++pixel) {
                if (mask != nullptr && mask->include[pixel] == 0) continue;
                ++p.pixels;
                uint32_t pixelMax = 0;
                for (int c = 0; c < 3; ++c) {
                    const int d = static_cast<int>(test.rgb[pixel * 3u + c]) -
                        static_cast<int>(reference.rgb[pixel * 3u + c]);
                    const uint32_t ad = static_cast<uint32_t>(d < 0 ? -d : d);
                    p.sumSq += static_cast<uint64_t>(ad) * ad;
                    pixelMax = std::max(pixelMax, ad);
                }
                p.maxAbs = std::max(p.maxAbs, pixelMax);
                if (pixelMax > options.changedPixelCodeThreshold) ++p.changed;
            }
            partials[chunk] = p;
        });
        Partial total{};
        for (const Partial& p : partials) {
            total.pixels += p.pixels;
            total.changed += p.changed;
            total.sumSq += p.sumSq;
            total.maxAbs = std::max(total.maxAbs, p.maxAbs);
        }
        SdrErrorMetrics metrics{};
        metrics.pixelCount = total.pixels;
        metrics.maxAbsCode = total.maxAbs;
        metrics.changedPixelCount = total.changed;
        if (total.pixels > 0) {
            metrics.mse = static_cast<double>(total.sumSq) /
                (3.0 * static_cast<double>(total.pixels));
            metrics.changedPixelFraction = static_cast<double>(total.changed) /
                static_cast<double>(total.pixels);
        }
        metrics.psnrDb = metrics.mse == 0.0 ? std::numeric_limits<double>::infinity() :
            10.0 * std::log10(255.0 * 255.0 / metrics.mse);
        metrics.lumaSsim = computeSsim(encodedLuma(reference), encodedLuma(test), mask);
        return metrics;
    }

    // -------------------------------------------------------------- stability

    StabilityAccumulator::StabilityAccumulator(uint32_t width, uint32_t height,
        const Mask* mask, double flickerThreshold)
        : m_width(width), m_height(height), m_flickerThreshold(flickerThreshold) {
        if (width == 0 || height == 0) {
            throw std::invalid_argument("Stability requires nonzero dimensions.");
        }
        requireMask(mask, width, height);
        if (mask != nullptr) m_mask = mask->include;
        m_deltaHistogram.assign(static_cast<size_t>(HistogramBins) * workerCount(), 0);
    }

    void StabilityAccumulator::addFrame(const Plane& luma) {
        validatePlane(luma);
        requireSameSize(luma.width, luma.height, m_width, m_height, "stability");
        const size_t pixels = pixelCount(m_width, m_height);
        if (m_frameCount == 0) {
            m_first = luma.values;
            m_previous = luma.values;
            m_sum.assign(pixels, 0.0);
            m_sumSq.assign(pixels, 0.0);
            m_frameCount = 1;
            return;
        }
        struct Partial {
            double sum = 0.0, max = 0.0;
            uint64_t count = 0;
        };
        std::vector<Partial> partials(chunkCount(m_height));
        const unsigned workers = workerCount();
        if (m_deltaHistogram.size() < static_cast<size_t>(HistogramBins) * workers) {
            m_deltaHistogram.resize(static_cast<size_t>(HistogramBins) * workers, 0);
        }
        parallelForRows(m_height, [&](uint32_t chunk, uint32_t rowBegin, uint32_t rowEnd,
            unsigned worker) {
            Partial p{};
            uint64_t* histogram = m_deltaHistogram.data() +
                static_cast<size_t>(worker) * HistogramBins;
            for (size_t pixel = static_cast<size_t>(rowBegin) * m_width;
                pixel < static_cast<size_t>(rowEnd) * m_width; ++pixel) {
                const float value = luma.values[pixel];
                const double offset = static_cast<double>(value) - m_first[pixel];
                m_sum[pixel] += offset;
                m_sumSq[pixel] += offset * offset;
                if (included(pixel)) {
                    const double delta = std::abs(static_cast<double>(value) -
                        m_previous[pixel]);
                    p.sum += delta;
                    p.max = std::max(p.max, delta);
                    ++p.count;
                    uint32_t bin = 0;
                    if (delta > 0.0) {
                        bin = 1u + static_cast<uint32_t>(std::min(
                            std::floor(delta * (HistogramBins - 1u)),
                            static_cast<double>(HistogramBins - 2u)));
                    }
                    ++histogram[bin];
                }
                m_previous[pixel] = value;
            }
            partials[chunk] = p;
        });
        Partial total{};
        for (const Partial& p : partials) {
            total.sum += p.sum;
            total.count += p.count;
            total.max = std::max(total.max, p.max);
        }
        m_deltaSum += total.sum;
        m_deltaCount += total.count;
        m_deltaMax = std::max(m_deltaMax, total.max);
        m_pairMeanDelta.push_back(total.count > 0 ?
            total.sum / static_cast<double>(total.count) : 0.0);
        ++m_frameCount;
    }

    Plane StabilityAccumulator::temporalStdDev() const {
        Plane plane{};
        plane.width = m_width;
        plane.height = m_height;
        plane.values.assign(pixelCount(m_width, m_height), 0.0f);
        if (m_frameCount == 0) return plane;
        const double n = static_cast<double>(m_frameCount);
        parallelForRows(m_height, [&](uint32_t, uint32_t rowBegin, uint32_t rowEnd,
            unsigned) {
            for (size_t pixel = static_cast<size_t>(rowBegin) * m_width;
                pixel < static_cast<size_t>(rowEnd) * m_width; ++pixel) {
                if (!included(pixel)) continue;
                const double mean = m_sum[pixel] / n;
                const double variance = std::max(0.0, m_sumSq[pixel] / n - mean * mean);
                plane.values[pixel] = static_cast<float>(std::sqrt(variance));
            }
        });
        return plane;
    }

    StabilityMetrics StabilityAccumulator::metrics() const {
        StabilityMetrics metrics{};
        metrics.frameCount = m_frameCount;
        metrics.flickerThreshold = m_flickerThreshold;
        metrics.pairMeanDelta = m_pairMeanDelta;
        metrics.maxFrameDelta = m_deltaMax;
        if (m_deltaCount > 0) {
            metrics.meanFrameDelta = m_deltaSum / static_cast<double>(m_deltaCount);
            std::vector<uint64_t> merged(HistogramBins, 0);
            const size_t histograms = m_deltaHistogram.size() / HistogramBins;
            for (size_t h = 0; h < histograms; ++h) {
                for (uint32_t bin = 0; bin < HistogramBins; ++bin) {
                    merged[bin] += m_deltaHistogram[h * HistogramBins + bin];
                }
            }
            const uint64_t rank = std::max<uint64_t>(1, static_cast<uint64_t>(std::ceil(
                0.99 * static_cast<double>(m_deltaCount))));
            uint64_t cumulative = 0;
            for (uint32_t bin = 0; bin < HistogramBins; ++bin) {
                cumulative += merged[bin];
                if (cumulative >= rank) {
                    metrics.p99FrameDelta = bin == 0 ? 0.0 :
                        std::min(m_deltaMax, (bin - 0.5) / (HistogramBins - 1u));
                    break;
                }
            }
        }

        const Plane deviation = temporalStdDev();
        std::vector<double> values;
        values.reserve(m_mask.empty() ? deviation.values.size() : m_mask.size());
        double sum = 0.0;
        double flickerSum = 0.0;
        for (size_t pixel = 0; pixel < deviation.values.size(); ++pixel) {
            if (!included(pixel)) continue;
            const double value = deviation.values[pixel];
            values.push_back(value);
            sum += value;
            metrics.maxTemporalStdDev = std::max(metrics.maxTemporalStdDev, value);
            if (value > m_flickerThreshold) {
                ++metrics.flickerPixelCount;
                flickerSum += value;
            }
        }
        metrics.pixelCount = values.size();
        if (!values.empty()) {
            const double n = static_cast<double>(values.size());
            metrics.meanTemporalStdDev = sum / n;
            metrics.p99TemporalStdDev = nearestRankPercentile(values, 0.99);
            metrics.flickerPixelFraction =
                static_cast<double>(metrics.flickerPixelCount) / n;
            metrics.flickerEnergy = metrics.flickerPixelCount > 0 ?
                flickerSum / static_cast<double>(metrics.flickerPixelCount) : 0.0;
        }
        return metrics;
    }

    // --------------------------------------------------- ghosting / recovery

    LumaErrorStats lumaError(const Plane& reference, const Plane& test, const Mask* mask) {
        validatePlane(reference);
        validatePlane(test);
        requireSameSize(reference.width, reference.height, test.width, test.height,
            "luma error");
        requireMask(mask, reference.width, reference.height);
        struct Partial {
            uint64_t count = 0;
            double sumAbs = 0.0, sumSq = 0.0, max = 0.0;
        };
        std::vector<Partial> partials(chunkCount(reference.height));
        parallelForRows(reference.height, [&](uint32_t chunk, uint32_t rowBegin,
            uint32_t rowEnd, unsigned) {
            Partial p{};
            for (size_t pixel = static_cast<size_t>(rowBegin) * reference.width;
                pixel < static_cast<size_t>(rowEnd) * reference.width; ++pixel) {
                if (mask != nullptr && mask->include[pixel] == 0) continue;
                const double e = std::abs(static_cast<double>(test.values[pixel]) -
                    reference.values[pixel]);
                ++p.count;
                p.sumAbs += e;
                p.sumSq += e * e;
                p.max = std::max(p.max, e);
            }
            partials[chunk] = p;
        });
        Partial total{};
        for (const Partial& p : partials) {
            total.count += p.count;
            total.sumAbs += p.sumAbs;
            total.sumSq += p.sumSq;
            total.max = std::max(total.max, p.max);
        }
        LumaErrorStats stats{};
        stats.pixelCount = total.count;
        stats.sumAbs = total.sumAbs;
        stats.maxAbs = total.max;
        if (total.count > 0) {
            stats.meanAbs = total.sumAbs / static_cast<double>(total.count);
            stats.rmse = std::sqrt(total.sumSq / static_cast<double>(total.count));
        }
        return stats;
    }

    Mask trailMask(const Plane& referenceNow, const Plane& referencePast,
        double threshold, const Mask* limit) {
        validatePlane(referenceNow);
        validatePlane(referencePast);
        requireSameSize(referenceNow.width, referenceNow.height, referencePast.width,
            referencePast.height, "trail mask");
        requireMask(limit, referenceNow.width, referenceNow.height);
        Mask mask{};
        mask.width = referenceNow.width;
        mask.height = referenceNow.height;
        mask.include.resize(referenceNow.values.size());
        parallelForRows(mask.height, [&](uint32_t, uint32_t rowBegin, uint32_t rowEnd,
            unsigned) {
            for (size_t pixel = static_cast<size_t>(rowBegin) * mask.width;
                pixel < static_cast<size_t>(rowEnd) * mask.width; ++pixel) {
                const bool moved = std::abs(static_cast<double>(referenceNow.values[pixel]) -
                    referencePast.values[pixel]) > threshold;
                const bool allowed = limit == nullptr || limit->include[pixel] != 0;
                mask.include[pixel] = moved && allowed ? 1 : 0;
            }
        });
        return mask;
    }

    RecoveryResult evaluateRecovery(std::span<const double> errors,
        std::optional<double> absoluteThreshold, double steadyStateMultiplier) {
        if (errors.empty()) {
            throw std::invalid_argument("Recovery requires at least one frame.");
        }
        RecoveryResult result{};
        const size_t quarter = std::max<size_t>(1, errors.size() / 4u);
        result.steadyStateFrameCount = static_cast<uint32_t>(quarter);
        result.steadyStateMedian = median(std::vector<double>(
            errors.end() - static_cast<ptrdiff_t>(quarter), errors.end()));
        if (absoluteThreshold) {
            result.threshold = *absoluteThreshold;
        }
        else {
            result.threshold = steadyStateMultiplier * result.steadyStateMedian;
            result.thresholdFromSteadyState = true;
        }
        for (size_t i = 0; i < errors.size(); ++i) {
            if (errors[i] <= result.threshold) {
                result.firstBelowFrame = static_cast<uint32_t>(i);
                break;
            }
        }
        size_t settled = errors.size();
        while (settled > 0 && errors[settled - 1] <= result.threshold) --settled;
        if (settled < errors.size()) result.settledFrame = static_cast<uint32_t>(settled);
        return result;
    }

    // ------------------------------------------------------------ accumulate

    void MeanAccumulator::add(const FloatImage& image) {
        if (image.rgb.size() != pixelCount(image.width, image.height) * 3u ||
            image.width == 0 || image.height == 0) {
            throw std::invalid_argument("Image storage does not match its size.");
        }
        if (m_count == 0) {
            m_width = image.width;
            m_height = image.height;
            m_sum.assign(image.rgb.size(), 0.0);
        }
        requireSameSize(image.width, image.height, m_width, m_height, "accumulate");
        std::vector<uint64_t> nonFinite(chunkCount(m_height), 0);
        parallelForRows(m_height, [&](uint32_t chunk, uint32_t rowBegin, uint32_t rowEnd,
            unsigned) {
            uint64_t bad = 0;
            for (size_t i = static_cast<size_t>(rowBegin) * m_width * 3u;
                i < static_cast<size_t>(rowEnd) * m_width * 3u; ++i) {
                const float value = image.rgb[i];
                if (!std::isfinite(value)) ++bad;
                m_sum[i] += value;
            }
            nonFinite[chunk] = bad;
        });
        for (const uint64_t count : nonFinite) m_nonFinite += count;
        ++m_count;
    }

    FloatImage MeanAccumulator::mean() const {
        if (m_count == 0) throw std::logic_error("No images were accumulated.");
        FloatImage image{};
        image.width = m_width;
        image.height = m_height;
        image.rgb.resize(m_sum.size());
        const double n = static_cast<double>(m_count);
        for (size_t i = 0; i < m_sum.size(); ++i) {
            image.rgb[i] = static_cast<float>(m_sum[i] / n);
        }
        return image;
    }

    // ----------------------------------------------------------------- utils

    double nearestRankPercentile(std::vector<double>& values, double p) {
        if (values.empty()) throw std::invalid_argument("Percentile of no samples.");
        const size_t rank = std::max<size_t>(1, static_cast<size_t>(std::ceil(
            std::clamp(p, 0.0, 1.0) * static_cast<double>(values.size()))));
        std::nth_element(values.begin(), values.begin() + static_cast<ptrdiff_t>(rank - 1),
            values.end());
        return values[rank - 1];
    }

    double median(std::vector<double> values) {
        if (values.empty()) throw std::invalid_argument("Median of no samples.");
        const size_t middle = values.size() / 2u;
        std::nth_element(values.begin(), values.begin() + static_cast<ptrdiff_t>(middle),
            values.end());
        const double upper = values[middle];
        if (values.size() % 2u == 1u) return upper;
        const double lower = *std::max_element(values.begin(),
            values.begin() + static_cast<ptrdiff_t>(middle));
        return 0.5 * (lower + upper);
    }

} // namespace Iridium::TemporalMetrics
