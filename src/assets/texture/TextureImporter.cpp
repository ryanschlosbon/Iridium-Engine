#include "assets/texture/TextureImporter.h"

#include "assets/AssetSourceValidation.h"
#include "assets/texture/TextureMipGenerator.h"
#include "assets/texture/TextureProduct.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <future>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXTex.h>
#include <objbase.h>
#endif

namespace Iridium {

    namespace {
        constexpr uint32_t ParsedTextureMagic = 0x31585449; // ITX1
        constexpr uint32_t ParsedTextureSchema = 1;

        void addError(std::vector<CookDiagnostic>& diagnostics,
            std::string code, std::string field, std::string message) {
            diagnostics.push_back({
                .severity = CookDiagnosticSeverity::Error,
                .code = std::move(code),
                .field = std::move(field),
                .message = std::move(message),
            });
        }

        std::string lowerExtension(const std::filesystem::path& path) {
            std::string extension = path.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                });
            return extension;
        }

        void appendU32(std::vector<std::byte>& bytes, uint32_t value) {
            for (uint32_t shift = 0; shift < 32; shift += 8) {
                bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
            }
        }

        void appendFloat(std::vector<std::byte>& bytes, float value) {
            appendU32(bytes, std::bit_cast<uint32_t>(value));
        }

        uint32_t readU32(std::span<const std::byte> bytes, size_t& cursor) {
            if (cursor + 4 > bytes.size()) {
                throw std::runtime_error("parsed texture document is truncated");
            }
            uint32_t value = 0;
            for (uint32_t shift = 0; shift < 32; shift += 8) {
                value |= static_cast<uint32_t>(
                    std::to_integer<uint8_t>(bytes[cursor++])) << shift;
            }
            return value;
        }

        float readFloat(std::span<const std::byte> bytes, size_t& cursor) {
            return std::bit_cast<float>(readU32(bytes, cursor));
        }

        std::vector<std::byte> serializeParsedImages(
            std::span<const FloatRgbaImage> images) {
            std::vector<std::byte> bytes;
            uint64_t floatCount = 0;
            for (const FloatRgbaImage& image : images) {
                floatCount += image.rgba.size();
            }
            if (floatCount >
                ((std::numeric_limits<size_t>::max)() - 16) / 4) {
                throw std::overflow_error("parsed texture document is too large");
            }
            bytes.reserve(static_cast<size_t>(16 + floatCount * 4 +
                images.size() * 8));
            appendU32(bytes, ParsedTextureMagic);
            appendU32(bytes, ParsedTextureSchema);
            appendU32(bytes, static_cast<uint32_t>(images.size()));
            appendU32(bytes, 0);
            for (const FloatRgbaImage& image : images) {
                appendU32(bytes, image.width);
                appendU32(bytes, image.height);
                for (float value : image.rgba) {
                    appendFloat(bytes, value);
                }
            }
            return bytes;
        }

        std::vector<FloatRgbaImage> deserializeParsedImages(
            std::span<const std::byte> bytes) {
            size_t cursor = 0;
            if (readU32(bytes, cursor) != ParsedTextureMagic ||
                readU32(bytes, cursor) != ParsedTextureSchema) {
                throw std::runtime_error(
                    "parsed texture document has an unsupported header");
            }
            const uint32_t imageCount = readU32(bytes, cursor);
            (void)readU32(bytes, cursor);
            if (imageCount == 0 || imageCount > 32) {
                throw std::runtime_error(
                    "parsed texture document has an invalid mip count");
            }
            std::vector<FloatRgbaImage> result;
            result.reserve(imageCount);
            for (uint32_t index = 0; index < imageCount; ++index) {
                FloatRgbaImage image;
                image.width = readU32(bytes, cursor);
                image.height = readU32(bytes, cursor);
                if (image.width == 0 || image.height == 0 ||
                    image.width > 65536 || image.height > 65536) {
                    throw std::runtime_error(
                        "parsed texture document has invalid dimensions");
                }
                const uint64_t count =
                    static_cast<uint64_t>(image.width) * image.height * 4;
                if (count > (bytes.size() - cursor) / 4) {
                    throw std::runtime_error(
                        "parsed texture pixels are truncated");
                }
                image.rgba.resize(static_cast<size_t>(count));
                for (float& value : image.rgba) {
                    value = readFloat(bytes, cursor);
                    if (!std::isfinite(value)) {
                        throw std::runtime_error(
                            "parsed texture contains non-finite pixels");
                    }
                }
                result.push_back(std::move(image));
            }
            if (cursor != bytes.size()) {
                throw std::runtime_error(
                    "parsed texture document contains trailing bytes");
            }
            return result;
        }

#if defined(_WIN32)
        std::string hresultMessage(HRESULT result) {
            std::ostringstream output;
            output << "DirectXTex failed with HRESULT 0x" << std::hex
                << static_cast<uint32_t>(result);
            return output.str();
        }

        struct ComApartment {
            HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            ~ComApartment() {
                if (result == S_OK || result == S_FALSE) {
                    CoUninitialize();
                }
            }
        };

        class WicFactoryLifetime {
        public:
            WicFactoryLifetime()
                : ready_(promise_.get_future().share()) {
                owner_ = std::jthread(
                    [this](std::stop_token stopToken) {
                        const HRESULT initialized =
                            CoInitializeEx(
                                nullptr,
                                COINIT_MULTITHREADED);
                        HRESULT result = initialized;
                        if (SUCCEEDED(initialized) ||
                            initialized ==
                                RPC_E_CHANGED_MODE) {
                            bool wic2 = false;
                            if (!DirectX::GetWICFactory(
                                    wic2)) {
                                result = E_FAIL;
                            }
                        }
                        promise_.set_value(result);
                        {
                            std::unique_lock lock(mutex_);
                            condition_.wait(
                                lock, stopToken,
                                [] { return false; });
                        }
                        if (initialized == S_OK ||
                            initialized == S_FALSE) {
                            CoUninitialize();
                        }
                    });
            }

            ~WicFactoryLifetime() {
                owner_.request_stop();
                condition_.notify_all();
                if (owner_.joinable()) {
                    owner_.join();
                }
            }

            [[nodiscard]] HRESULT result() const {
                return ready_.get();
            }

        private:
            std::promise<HRESULT> promise_;
            std::shared_future<HRESULT> ready_;
            std::mutex mutex_;
            std::condition_variable_any condition_;
            std::jthread owner_;
        };

        WicFactoryLifetime& wicFactoryLifetime() {
            static WicFactoryLifetime lifetime;
            return lifetime;
        }

        struct ComScope {
            ComScope()
                : result(apartment().result) {
                if (SUCCEEDED(result) ||
                    result ==
                        RPC_E_CHANGED_MODE) {
                    const HRESULT factory =
                        wicFactoryLifetime().result();
                    if (FAILED(factory)) {
                        result = factory;
                    }
                }
            }

            HRESULT result;

            [[nodiscard]] bool usable() const noexcept {
                return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
            }

        private:
            static ComApartment& apartment() {
                // DirectXTex/WIC retains process-level decoder state. Keep the
                // calling worker thread's COM apartment alive across repeated
                // imports instead of tearing it down after every image.
                static thread_local ComApartment value;
                return value;
            }
        };

        HRESULT decodeSource(const std::filesystem::path& relativePath,
            std::span<const std::byte> sourceBytes,
            DirectX::TexMetadata& metadata, DirectX::ScratchImage& image) {
            const std::string extension = lowerExtension(relativePath);
            if (extension == ".dds") {
                return DirectX::LoadFromDDSMemory(sourceBytes.data(),
                    sourceBytes.size(), DirectX::DDS_FLAGS_NONE, &metadata, image);
            }
            if (extension == ".hdr") {
                return DirectX::LoadFromHDRMemory(sourceBytes.data(),
                    sourceBytes.size(), &metadata, image);
            }
            if (extension == ".tga") {
                return DirectX::LoadFromTGAMemory(sourceBytes.data(),
                    sourceBytes.size(), DirectX::TGA_FLAGS_NONE, &metadata, image);
            }
            return DirectX::LoadFromWICMemory(sourceBytes.data(), sourceBytes.size(),
                static_cast<DirectX::WIC_FLAGS>(
                    DirectX::WIC_FLAGS_FORCE_RGB |
                    DirectX::WIC_FLAGS_IGNORE_SRGB),
                &metadata, image);
        }

        HRESULT convertToFloat(const DirectX::Image& source,
            DirectX::ScratchImage& converted) {
            if (DirectX::IsCompressed(source.format)) {
                return DirectX::Decompress(source,
                    DXGI_FORMAT_R32G32B32A32_FLOAT, converted);
            }
            return DirectX::Convert(source, DXGI_FORMAT_R32G32B32A32_FLOAT,
                DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT,
                converted);
        }

        DXGI_FORMAT compressionFormat(TextureFormat format) {
            switch (format) {
            case TextureFormat::BC4_UNorm: return DXGI_FORMAT_BC4_UNORM;
            case TextureFormat::BC5_UNorm: return DXGI_FORMAT_BC5_UNORM;
            case TextureFormat::BC6H_UFloat: return DXGI_FORMAT_BC6H_UF16;
            case TextureFormat::BC7_UNorm:
            case TextureFormat::BC7_sRGB:
                // BC7 UNORM and sRGB use identical block bits. Compressing the
                // already encoded float values as UNORM avoids an implicit second
                // transfer conversion; the runtime image view supplies sRGB decode.
                return DXGI_FORMAT_BC7_UNORM;
            default:
                return DXGI_FORMAT_UNKNOWN;
            }
        }

        DXGI_FORMAT uncompressedFormat(
            TextureFormat format) {
            switch (format) {
            case TextureFormat::RGBA8_UNorm:
            case TextureFormat::RGBA8_sRGB:
                // Float intermediates already contain the encoded view values.
                // The runtime sRGB image view supplies the transfer decode.
                return DXGI_FORMAT_R8G8B8A8_UNORM;
            case TextureFormat::RGBA16_SFloat:
                return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case TextureFormat::RGBA32_SFloat:
                return DXGI_FORMAT_R32G32B32A32_FLOAT;
            default:
                return DXGI_FORMAT_UNKNOWN;
            }
        }

        HRESULT encodeMip(const FloatRgbaImage& mip, TextureFormat format,
            TextureCompressionQuality quality, DirectX::ScratchImage& encoded) {
            DirectX::Image source{
                .width = mip.width,
                .height = mip.height,
                .format = DXGI_FORMAT_R32G32B32A32_FLOAT,
                .rowPitch = static_cast<size_t>(mip.width) * 4 * sizeof(float),
                .slicePitch = static_cast<size_t>(mip.width) * mip.height *
                    4 * sizeof(float),
                .pixels = reinterpret_cast<uint8_t*>(
                    const_cast<float*>(mip.rgba.data())),
            };
            if (!isBlockCompressed(format)) {
                return DirectX::Convert(
                    source,
                    uncompressedFormat(format),
                    DirectX::TEX_FILTER_DEFAULT,
                    DirectX::TEX_THRESHOLD_DEFAULT,
                    encoded);
            }
            DirectX::TEX_COMPRESS_FLAGS flags = DirectX::TEX_COMPRESS_DEFAULT;
            if ((format == TextureFormat::BC7_UNorm ||
                    format == TextureFormat::BC7_sRGB) &&
                quality == TextureCompressionQuality::Iteration) {
                flags = static_cast<DirectX::TEX_COMPRESS_FLAGS>(
                    flags | DirectX::TEX_COMPRESS_BC7_QUICK);
            }
            return DirectX::Compress(source, compressionFormat(format), flags,
                DirectX::TEX_THRESHOLD_DEFAULT, encoded);
        }
#endif

    } // namespace

    const ImporterDescriptor& TextureImporter::descriptor() const noexcept {
        static const ImporterDescriptor value{
            .id = "iridium.texture.directxtex",
            .implementationVersion = kDirectXTexCodecVersion,
            .currentSettingsSchemaVersion = 1,
            .assetTypes = { "iridium.texture" },
            .extensions = {
                ".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff",
                ".tga", ".dds",
            },
        };
        return value;
    }

    ImportProbeResult TextureImporter::probe(
        const std::filesystem::path& relativePath,
        std::span<const std::byte> sourceBytes) const {
        if (sourceBytes.empty()) {
            return ImportProbeResult::Unsupported;
        }
        const std::string extension = lowerExtension(relativePath);
        return std::find(descriptor().extensions.begin(),
            descriptor().extensions.end(), extension) !=
                descriptor().extensions.end()
            ? ImportProbeResult::Supported : ImportProbeResult::Unsupported;
    }

    NormalizedImportSettings TextureImporter::normalizeSettings(
        uint32_t sourceSchemaVersion, const nlohmann::json& settings,
        bool strict) const {
        NormalizedImportSettings result;
        result.schemaVersion = descriptor().currentSettingsSchemaVersion;
        if (sourceSchemaVersion != result.schemaVersion) {
            addError(result.diagnostics, "TEXTURE_SETTINGS_SCHEMA",
                "settings.schemaVersion",
                "Texture importer cannot migrate this settings schema version.");
            return result;
        }
        if (strict && settings.is_object()) {
            static const std::set<std::string> known{
                "alpha_coverage_threshold", "alpha_mode", "flip_green",
                "mip_policy", "quality", "reconstruct_normal_z", "semantic",
                "view_color_space",
            };
            for (const auto& [key, ignored] : settings.items()) {
                (void)ignored;
                if (!known.contains(key)) {
                    addError(result.diagnostics, "TEXTURE_SETTINGS_UNKNOWN",
                        "/" + key,
                        "Strict texture cooking rejects unknown settings.");
                }
            }
        }
        TextureSettingsResult canonical = canonicalizeTextureSettings(settings);
        result.diagnostics.insert(result.diagnostics.end(),
            canonical.diagnostics.begin(), canonical.diagnostics.end());
        if (!canonical.valid() || hasCookErrors(result.diagnostics)) {
            return result;
        }
        result.canonicalBytes = std::move(canonical.canonicalBytes);
        result.values = nlohmann::json::parse(
            reinterpret_cast<const char*>(result.canonicalBytes.data()),
            reinterpret_cast<const char*>(result.canonicalBytes.data() +
                result.canonicalBytes.size()));
        result.values.erase("schema");
        return result;
    }

    ParsedSourceAsset TextureImporter::parse(
        const ImportSource& input,
        const NormalizedImportSettings& settings) const {
        ParsedSourceAsset result;
        if (input.stopToken.stop_requested()) {
            addError(
                result.diagnostics,
                "TEXTURE_IMPORT_CANCELLED", "/",
                "Asset import cancelled.");
            return result;
        }
        if (!settings.valid()) {
            addError(result.diagnostics, "TEXTURE_SETTINGS_NOT_NORMALIZED", "/",
                "Texture source cannot parse with invalid settings.");
            return result;
        }
        if (isGitLfsPointer(input.bytes)) {
            addError(result.diagnostics,
                "TEXTURE_SOURCE_GIT_LFS_POINTER",
                input.relativePath.generic_string(),
                "The texture is an unresolved Git LFS pointer, not image data. "
                "Resolve the LFS object in the source package and import again.");
            return result;
        }
#if defined(_WIN32)
        ComScope com;
        if (!com.usable()) {
            addError(result.diagnostics, "TEXTURE_COM_INITIALIZATION", "/",
                hresultMessage(com.result));
            return result;
        }
        DirectX::TexMetadata metadata{};
        DirectX::ScratchImage decoded;
        const HRESULT decodeResult = decodeSource(
            input.relativePath, input.bytes, metadata, decoded);
        if (input.stopToken.stop_requested()) {
            addError(
                result.diagnostics,
                "TEXTURE_IMPORT_CANCELLED", "/",
                "Asset import cancelled.");
            return result;
        }
        if (FAILED(decodeResult)) {
            addError(result.diagnostics, "TEXTURE_SOURCE_DECODE",
                input.relativePath.generic_string(), hresultMessage(decodeResult));
            return result;
        }
        if (metadata.dimension != DirectX::TEX_DIMENSION_TEXTURE2D ||
            metadata.arraySize != 1 || metadata.depth != 1 ||
            metadata.width == 0 || metadata.height == 0 ||
            metadata.width > 65536 || metadata.height > 65536) {
            addError(result.diagnostics, "TEXTURE_SOURCE_SHAPE",
                input.relativePath.generic_string(),
                "Only one non-cube 2D texture up to 65536x65536 is supported.");
            return result;
        }

        try {
            std::vector<FloatRgbaImage> images;
            images.reserve(metadata.mipLevels);
            for (size_t mip = 0; mip < metadata.mipLevels; ++mip) {
                const DirectX::Image* source = decoded.GetImage(mip, 0, 0);
                if (!source) {
                    throw std::runtime_error(
                        "DirectXTex returned a missing source mip");
                }
                DirectX::ScratchImage converted;
                const HRESULT convertResult = convertToFloat(*source, converted);
                if (FAILED(convertResult)) {
                    throw std::runtime_error(hresultMessage(convertResult));
                }
                const DirectX::Image* pixels = converted.GetImage(0, 0, 0);
                if (!pixels || pixels->format !=
                    DXGI_FORMAT_R32G32B32A32_FLOAT) {
                    throw std::runtime_error(
                        "DirectXTex returned an unexpected float format");
                }
                FloatRgbaImage image{
                    .width = static_cast<uint32_t>(pixels->width),
                    .height = static_cast<uint32_t>(pixels->height),
                    .rgba = std::vector<float>(
                        pixels->width * pixels->height * 4),
                };
                for (size_t row = 0; row < pixels->height; ++row) {
                    const auto* sourceRow = reinterpret_cast<const float*>(
                        pixels->pixels + row * pixels->rowPitch);
                    std::copy_n(sourceRow, pixels->width * 4,
                        image.rgba.begin() + row * pixels->width * 4);
                }
                if (std::any_of(image.rgba.begin(), image.rgba.end(),
                    [](float value) { return !std::isfinite(value); })) {
                    throw std::runtime_error(
                        "decoded source contains non-finite pixels");
                }
                images.push_back(std::move(image));
            }
            result.documentBytes = serializeParsedImages(images);
            result.dependencies.push_back({
                .type = AssetDependencyType::Tool,
                .location = kDirectXTexCodecId,
                .contentHash =
                    kDirectXTexCodecContentHash,
            });
        } catch (const std::exception& exception) {
            addError(result.diagnostics, "TEXTURE_SOURCE_CONVERT",
                input.relativePath.generic_string(), exception.what());
        }
#else
        (void)input;
        addError(result.diagnostics, "TEXTURE_CODEC_PLATFORM", "/",
            "The pinned DirectXTex production importer is available on Windows.");
#endif
        return result;
    }

    CookProduct TextureImporter::cook(
        const ParsedSourceAsset& source,
        const NormalizedImportSettings& settings,
        const CookTarget& target,
        const AssetCookContext& context,
        std::stop_token stopToken) const {
        (void)target;
        (void)context;
        CookProduct failed{
            .artifactType = "iridium.texture",
            .artifactSchemaVersion = kCookedTextureSchemaVersion,
        };
        const auto cancelled =
            [&stopToken] {
                if (stopToken.stop_requested()) {
                    throw std::runtime_error(
                        "Texture cook was cancelled.");
                }
            };
        if (stopToken.stop_requested()) {
            addError(
                failed.diagnostics,
                "TEXTURE_COOK_CANCELLED", "/",
                "Texture cook was cancelled.");
            return failed;
        }
        if (hasCookErrors(source.diagnostics) || !settings.valid()) {
            addError(failed.diagnostics, "TEXTURE_COOK_INPUT", "/",
                "Texture cook input is invalid.");
            return failed;
        }
        const TextureSettingsResult canonical =
            canonicalizeTextureSettings(settings.values);
        if (!canonical.valid()) {
            failed.diagnostics = canonical.diagnostics;
            return failed;
        }
#if defined(_WIN32)
        try {
            cancelled();
            const std::vector<FloatRgbaImage> parsed =
                deserializeParsedImages(source.documentBytes);
            std::vector<FloatRgbaImage> mips;
            if (canonical.settings->mipPolicy == TextureMipPolicy::FullChain) {
                mips = generateTextureMipChain(parsed.front(),
                    *canonical.settings);
            } else if (canonical.settings->mipPolicy ==
                TextureMipPolicy::PreserveSource) {
                TextureImportSettings perMip = *canonical.settings;
                perMip.mipPolicy = TextureMipPolicy::None;
                for (const FloatRgbaImage& sourceMip : parsed) {
                    cancelled();
                    mips.push_back(generateTextureMipChain(
                        sourceMip, perMip).front());
                }
            } else {
                TextureImportSettings baseOnly = *canonical.settings;
                baseOnly.mipPolicy = TextureMipPolicy::None;
                mips = generateTextureMipChain(parsed.front(), baseOnly);
            }
            if (canonical.settings->quality ==
                    TextureCompressionQuality::Preview &&
                canonical.settings->mipPolicy ==
                    TextureMipPolicy::FullChain) {
                const auto previewBase =
                    std::ranges::find_if(
                        mips,
                        [](const FloatRgbaImage&
                            mip) {
                            return mip.width <=
                                    kEditorPreviewTextureMaxDimension &&
                                mip.height <=
                                    kEditorPreviewTextureMaxDimension;
                        });
                if (previewBase != mips.end()) {
                    mips.erase(
                        mips.begin(),
                        previewBase);
                }
            }

            const TextureFormat format =
                selectTextureProductFormat(*canonical.settings);
            std::vector<std::byte> payload;
            CookedTextureManifest manifest{
                .width = mips.front().width,
                .height = mips.front().height,
                .storageFormat = format,
                .viewColorSpace = canonical.settings->viewColorSpace,
                .semantic = canonical.settings->semantic,
                .quality = canonical.settings->quality,
                .alphaMode = canonical.settings->alphaMode,
                .codecId = kDirectXTexCodecId,
                .codecVersion = kDirectXTexCodecVersion,
            };
            for (const FloatRgbaImage& mip : mips) {
                cancelled();
                if (format ==
                        TextureFormat::BC6H_UFloat &&
                    std::any_of(mip.rgba.begin(), mip.rgba.end(),
                        [](float value) { return value < 0.0f; })) {
                    throw std::runtime_error(
                        "BC6H unsigned-float products reject negative HDR values");
                }
                DirectX::ScratchImage encoded;
                const HRESULT encodeResult = encodeMip(mip, format,
                    canonical.settings->quality, encoded);
                if (FAILED(encodeResult)) {
                    throw std::runtime_error(hresultMessage(encodeResult));
                }
                const DirectX::Image* output = encoded.GetImage(0, 0, 0);
                const uint64_t expected =
                    textureMipDataSize(format, mip.width, mip.height);
                if (!output || output->slicePitch != expected) {
                    throw std::runtime_error(
                        "DirectXTex returned an unexpected texture mip layout");
                }
                manifest.mips.push_back({
                    .width = mip.width,
                    .height = mip.height,
                    .byteOffset = payload.size(),
                    .byteSize = expected,
                });
                payload.insert(payload.end(),
                    reinterpret_cast<const std::byte*>(output->pixels),
                    reinterpret_cast<const std::byte*>(
                        output->pixels + output->slicePitch));
            }
            return makeCookedTextureProduct(manifest, payload);
        } catch (const std::exception& exception) {
            addError(failed.diagnostics, "TEXTURE_COOK_EXCEPTION", "/",
                exception.what());
        }
#else
        addError(failed.diagnostics, "TEXTURE_CODEC_PLATFORM", "/",
            "The pinned DirectXTex production cooker is available on Windows.");
#endif
        return failed;
    }

} // namespace Iridium
