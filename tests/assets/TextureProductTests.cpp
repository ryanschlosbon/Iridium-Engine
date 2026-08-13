#include "assets/environment/EnvironmentConvolution.h"
#include "assets/environment/BakedProbeEnvironmentImporter.h"
#include "assets/environment/EnvironmentImporter.h"
#include "assets/environment/EnvironmentProduct.h"
#include "assets/texture/TextureMipGenerator.h"
#include "assets/texture/TextureImporter.h"
#include "assets/texture/TextureProduct.h"
#include "renderer/rhi/TextureResidency.h"
#include "material/MaterialRuntime.h"
#include "renderer/color/SceneColor.h"

#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {

    using namespace Iridium;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    bool settingsAreCanonicalAndSemantic() {
        const auto color = canonicalizeTextureSettings({
            { "semantic", "color" },
            { "view_color_space", "srgb" },
            { "quality", "iteration" },
        });
        CHECK(color.valid());
        CHECK(selectTextureProductFormat(*color.settings) == TextureFormat::BC7_sRGB);

        const auto previewColor =
            canonicalizeTextureSettings({
                { "semantic", "color" },
                { "view_color_space", "srgb" },
                { "quality", "preview" },
            });
        CHECK(previewColor.valid());
        CHECK(selectTextureProductFormat(
            *previewColor.settings) ==
            TextureFormat::RGBA8_sRGB);
        const auto previewNormal =
            canonicalizeTextureSettings({
                { "semantic", "normal" },
                { "view_color_space", "linear" },
                { "quality", "preview" },
            });
        CHECK(previewNormal.valid());
        CHECK(selectTextureProductFormat(
            *previewNormal.settings) ==
            TextureFormat::RGBA8_UNorm);
        const auto previewHdr =
            canonicalizeTextureSettings({
                { "semantic", "hdr_color" },
                { "view_color_space", "linear" },
                { "quality", "preview" },
            });
        CHECK(previewHdr.valid());
        CHECK(selectTextureProductFormat(
            *previewHdr.settings) ==
            TextureFormat::RGBA16_SFloat);

        const auto reordered = canonicalizeTextureSettings({
            { "quality", "iteration" },
            { "view_color_space", "srgb" },
            { "semantic", "color" },
        });
        CHECK(reordered.valid());
        CHECK(color.canonicalBytes == reordered.canonicalBytes);

        const auto normal = canonicalizeTextureSettings({
            { "semantic", "normal" },
            { "view_color_space", "linear" },
        });
        CHECK(normal.valid());
        CHECK(selectTextureProductFormat(*normal.settings) == TextureFormat::BC5_UNorm);

        const auto invalid = canonicalizeTextureSettings({
            { "semantic", "normal" },
            { "view_color_space", "srgb" },
        });
        CHECK(!invalid.valid());
        CHECK(invalid.diagnostics.front().code == "TEXTURE_SEMANTIC_COLOR_SPACE");
        return true;
    }

    bool blockLayoutsAreExact() {
        CHECK(static_cast<uint8_t>(TextureFormat::BC4_UNorm) == 4);
        CHECK(static_cast<uint8_t>(TextureFormat::BC7_sRGB) == 8);
        CHECK(static_cast<uint8_t>(TextureFormat::RG16_SFloat) == 9);
        CHECK(textureMipDataSize(TextureFormat::BC4_UNorm, 7, 5) == 32);
        CHECK(textureMipDataSize(TextureFormat::BC5_UNorm, 7, 5) == 64);
        CHECK(textureMipDataSize(TextureFormat::BC7_sRGB, 1, 1) == 16);
        CHECK(textureMipDataSize(TextureFormat::RGBA16_SFloat, 2, 3) == 48);

        TextureDesc desc{
            .width = 7,
            .height = 5,
            .format = TextureFormat::BC7_UNorm,
            .mipLevels = 3,
        };
        CHECK(textureDataSize(desc) == 64 + 16 + 16);
        desc.topology = TextureTopology::Cube;
        desc.arrayLayers = 6;
        desc.width = desc.height = 8;
        CHECK(validTextureTopology(desc));
        CHECK(textureDataSize(desc) == 6 * (64 + 16 + 16));
        desc.height = 4;
        CHECK(!validTextureTopology(desc));
        desc.topology = TextureTopology::Texture2DArray;
        CHECK(validTextureTopology(desc));
        desc.arrayLayers = 0;
        CHECK(!validTextureTopology(desc));
        return true;
    }

    bool mipGenerationRespectsColorNormalAndCoverage() {
        FloatRgbaImage color{
            .width = 2,
            .height = 2,
            .rgba = {
                0.0f, 0.0f, 0.0f, 1.0f,
                1.0f, 1.0f, 1.0f, 1.0f,
                0.0f, 0.0f, 0.0f, 1.0f,
                1.0f, 1.0f, 1.0f, 1.0f,
            },
        };
        const auto colorMips = generateTextureMipChain(color, {
            .semantic = TextureSemantic::Color,
            .viewColorSpace = TextureViewColorSpace::sRGB,
        });
        CHECK(colorMips.size() == 2);
        // Linear-light average encoded back to sRGB, rather than a gamma-space 0.5.
        CHECK(colorMips[1].rgba[0] > 0.73f && colorMips[1].rgba[0] < 0.74f);
        const auto linearMips = generateTextureMipChain(color, {
            .semantic = TextureSemantic::Color,
            .viewColorSpace = TextureViewColorSpace::Linear,
        });
        CHECK(std::abs(linearMips[1].rgba[0] - 0.5f) < 1e-6f);
        CHECK(linearMips[1].rgba[0] != colorMips[1].rgba[0]);

        FloatRgbaImage normal{
            .width = 2,
            .height = 2,
            .rgba = {
                1.0f, 0.5f, 0.5f, 1.0f,
                0.5f, 1.0f, 0.5f, 1.0f,
                1.0f, 0.5f, 0.5f, 1.0f,
                0.5f, 1.0f, 0.5f, 1.0f,
            },
        };
        const auto normalMips = generateTextureMipChain(normal, {
            .semantic = TextureSemantic::Normal,
            .viewColorSpace = TextureViewColorSpace::Linear,
        });
        const float nx = normalMips[1].rgba[0] * 2.0f - 1.0f;
        const float ny = normalMips[1].rgba[1] * 2.0f - 1.0f;
        const float nz = normalMips[1].rgba[2] * 2.0f - 1.0f;
        CHECK(std::abs(std::sqrt(nx * nx + ny * ny + nz * nz) - 1.0f) < 1e-5f);

        FloatRgbaImage flippedNormal{
            .width = 1,
            .height = 1,
            .rgba = { 0.5f, 0.75f, 1.0f, 0.25f },
        };
        const auto flipped = generateTextureMipChain(flippedNormal, {
            .semantic = TextureSemantic::Normal,
            .mipPolicy = TextureMipPolicy::None,
            .alphaMode = TextureAlphaMode::Opaque,
            .viewColorSpace = TextureViewColorSpace::Linear,
            .flipGreen = true,
            .reconstructNormalZ = true,
        });
        CHECK(flipped.size() == 1);
        CHECK(flipped[0].rgba[1] < 0.5f);
        CHECK(flipped[0].rgba[3] == 1.0f);

        FloatRgbaImage coverage{
            .width = 4,
            .height = 4,
            .rgba = std::vector<float>(4 * 4 * 4, 1.0f),
        };
        for (size_t texel = 0; texel < 16; ++texel) {
            coverage.rgba[texel * 4 + 3] = texel < 8 ? 1.0f : 0.0f;
        }
        const TextureImportSettings coverageSettings{
            .semantic = TextureSemantic::Color,
            .alphaMode = TextureAlphaMode::Coverage,
            .viewColorSpace = TextureViewColorSpace::sRGB,
            .alphaCoverageThreshold = 0.5f,
        };
        const auto coverageA = generateTextureMipChain(coverage, coverageSettings);
        const auto coverageB = generateTextureMipChain(coverage, coverageSettings);
        CHECK(coverageA == coverageB);
        CHECK(std::abs(alphaCoverage(coverageA[1], 0.5f) - 0.5) <= 0.25);
        return true;
    }

    bool manifestRoundTripsAndRejectsWrongLayout() {
        CookedTextureManifest source{
            .width = 8,
            .height = 4,
            .storageFormat = TextureFormat::BC5_UNorm,
            .viewColorSpace = TextureViewColorSpace::Linear,
            .semantic = TextureSemantic::Normal,
            .codecId = "directxtex-cpu",
            .codecVersion = 20260508,
            .mips = {
                { 8, 4, 0, 32 },
                { 4, 2, 32, 16 },
                { 2, 1, 48, 16 },
                { 1, 1, 64, 16 },
            },
        };
        const auto bytes = serializeTextureManifest(source);
        std::vector<CookDiagnostic> diagnostics;
        const auto decoded = readTextureManifest(bytes, diagnostics);
        CHECK(decoded.has_value());
        CHECK(diagnostics.empty());
        CHECK(*decoded == source);
        CHECK(validateTextureProduct(*decoded, 80).empty());
        const std::vector<std::byte> payload(80);
        const CookProduct product = makeCookedTextureProduct(*decoded, payload);
        CHECK(!hasCookErrors(product.diagnostics));
        CHECK(product.artifactType == "iridium.texture");
        CHECK(product.sections.size() == 2);
        CHECK(product.sections[0].id == kCookedTextureManifestSection);
        CHECK(product.sections[1].id == kCookedTexturePayloadSection);

        auto invalid = *decoded;
        invalid.mips[1].byteOffset = 31;
        CHECK(hasCookErrors(validateTextureProduct(invalid, 80)));
        return true;
    }

    bool residencyUsesFallbackGrowthAndDelayedReuse() {
        TextureViewResidencyTable table(2, 8);
        table.setFallback(99);
        const TextureViewHandle first = table.allocate(100);
        CHECK(table.shaderIndex(first) == first.getIndex());
        CHECK(table.backendToken(first.getIndex()) == 100);

        const TextureViewHandle second = table.allocate();
        CHECK(table.stats().growthCount == 1);
        CHECK(table.shaderIndex(second) == 0);
        CHECK(table.backendToken(second.getIndex()) == 99);
        CHECK(table.makeResident(second, 200));
        CHECK(table.shaderIndex(second) == second.getIndex());
        CHECK(table.evict(second));
        CHECK(table.shaderIndex(second) == 0);

        CHECK(table.release(first, 12));
        table.collect(11);
        const TextureViewHandle third = table.allocate(300);
        CHECK(third.getIndex() != first.getIndex());
        table.collect(12);
        const TextureViewHandle recycled = table.allocate(400);
        CHECK(recycled.getIndex() == first.getIndex());
        CHECK(recycled.getGeneration() != first.getGeneration());
        CHECK(table.shaderIndex(first) == 0);
        CHECK(!table.evict(first));
        CHECK(table.stats().staleHandleRejections != 0);
        return true;
    }

    bool samplersDeduplicateByFullSemantics() {
        SamplerRegistry registry;
        SamplerDesc linear;
        const auto first = registry.acquire(linear);
        const auto second = registry.acquire(linear);
        CHECK(first.handle == second.handle);
        CHECK(first.shaderIndex == second.shaderIndex);

        SamplerDesc clamped = linear;
        clamped.addressU = SamplerAddressMode::ClampToEdge;
        const auto third = registry.acquire(clamped);
        CHECK(third.handle != first.handle);
        SamplerDesc shadow = clamped;
        shadow.compareEnable = true;
        shadow.compareOp = SamplerCompareOp::LessOrEqual;
        const auto fourth = registry.acquire(shadow);
        CHECK(fourth.handle != third.handle);
        CHECK(registry.descriptor(fourth.shaderIndex).compareEnable);
        CHECK(registry.size() == 4);
        CHECK(registry.release(first.handle));
        CHECK(registry.shaderIndex(second.handle) == second.shaderIndex);
        CHECK(registry.release(second.handle));
        CHECK(registry.shaderIndex(second.handle) == 0);
        return true;
    }

    std::vector<std::byte> makeBmp(
        uint32_t width,
        uint32_t height) {
        const uint32_t pixelBytes =
            width * height * 4;
        std::vector<std::byte> bytes(
            54ull + pixelBytes);
        const auto put16 = [&bytes](size_t offset, uint16_t value) {
            bytes[offset] = static_cast<std::byte>(value & 0xffu);
            bytes[offset + 1] = static_cast<std::byte>(value >> 8u);
        };
        const auto put32 = [&bytes](size_t offset, uint32_t value) {
            for (uint32_t shift = 0; shift < 32; shift += 8) {
                bytes[offset++] =
                    static_cast<std::byte>((value >> shift) & 0xffu);
            }
        };
        bytes[0] = std::byte{ 'B' };
        bytes[1] = std::byte{ 'M' };
        put32(2, static_cast<uint32_t>(
            bytes.size()));
        put32(10, 54);
        put32(14, 40);
        put32(18, width);
        put32(22, height);
        put16(26, 1);
        put16(28, 32);
        put32(34, pixelBytes);
        constexpr std::array<uint8_t, 16> bgra{
            0, 0, 255, 255, 0, 255, 0, 255,
            255, 0, 0, 255, 255, 255, 255, 255,
        };
        for (size_t index = 0;
            index < pixelBytes; ++index) {
            bytes[54 + index] =
                static_cast<std::byte>(
                    bgra[index %
                        bgra.size()]);
        }
        return bytes;
    }

    std::vector<std::byte> makeBmp2x2() {
        return makeBmp(2, 2);
    }

    bool previewProductsCapResidentExtent() {
        TextureImporter importer;
        const NormalizedImportSettings settings =
            importer.normalizeSettings(1, {
                { "semantic", "color" },
                { "view_color_space", "srgb" },
                { "quality", "preview" },
                { "mip_policy", "full_chain" },
                { "alpha_mode", "opaque" },
            }, true);
        CHECK(settings.valid());
        const std::vector<std::byte> source =
            makeBmp(1024, 1024);
        const ParsedSourceAsset parsed =
            importer.parse({
                .relativePath =
                    "large-preview.bmp",
                .resolvedPath =
                    "large-preview.bmp",
                .bytes = source,
            }, settings);
        CHECK(!hasCookErrors(
            parsed.diagnostics));
        const CookProduct product =
            importer.cook(
                parsed, settings, {
                    .platform =
                        "windows-x64",
                    .profile = "editor",
                    .qualityPolicy =
                        "reference",
                }, {});
        CHECK(!hasCookErrors(
            product.diagnostics));
        std::vector<CookDiagnostic>
            diagnostics;
        const auto manifest =
            readTextureManifest(
                product.sections[0].bytes,
                diagnostics);
        CHECK(manifest.has_value());
        CHECK(diagnostics.empty());
        CHECK(manifest->width ==
            kEditorPreviewTextureMaxDimension);
        CHECK(manifest->height ==
            kEditorPreviewTextureMaxDimension);
        CHECK(manifest->mips.size() == 10);
        CHECK(product.sections[1].bytes.size() ==
            textureDataSize({
                .width = manifest->width,
                .height = manifest->height,
                .format =
                    manifest->storageFormat,
                .mipLevels =
                    static_cast<uint32_t>(
                        manifest->mips.size()),
            }));
        return true;
    }

    bool productionImporterDecodesMipsAndCompressesDeterministically() {
        TextureImporter importer;
        CHECK(importer.descriptor().implementationVersion ==
            kDirectXTexCodecVersion);
        const std::vector<std::byte> source = makeBmp2x2();
        CHECK(importer.probe("fixture.bmp", source) ==
            ImportProbeResult::Supported);
        CHECK(importer.probe("fixture.txt", source) ==
            ImportProbeResult::Unsupported);

        const NormalizedImportSettings settings = importer.normalizeSettings(1, {
            { "semantic", "color" },
            { "view_color_space", "srgb" },
            { "quality", "iteration" },
            { "mip_policy", "full_chain" },
            { "alpha_mode", "opaque" },
        }, true);
        CHECK(settings.valid());
        const ParsedSourceAsset parsed =
            importer.parse({
                .relativePath = "fixture.bmp",
                .resolvedPath = "fixture.bmp",
                .bytes = source,
            }, settings);
        CHECK(!hasCookErrors(parsed.diagnostics));
        CHECK(!parsed.documentBytes.empty());
        CHECK(parsed.dependencies.size() == 1);
        CHECK(parsed.dependencies[0].type == AssetDependencyType::Tool);
        CHECK(parsed.dependencies[0].contentHash ==
            "9e1ad29041db6629ccab0d9d465a3e1a24a8ffb6e3d8edcc24e6a30545d0e71e");

        const CookTarget target{
            .platform = "windows-x64",
            .profile = "release",
            .qualityPolicy = "reference",
        };
        const CookProduct first = importer.cook(
            parsed, settings, target, {});
        const CookProduct second = importer.cook(
            parsed, settings, target, {});
        CHECK(!hasCookErrors(first.diagnostics));
        CHECK(first.artifactType == "iridium.texture");
        CHECK(first.sections == second.sections);
        CHECK(first.sections.size() == 2);
        CHECK(first.sections[1].bytes.size() == 32);

        std::vector<CookDiagnostic> diagnostics;
        const auto manifest = readTextureManifest(
            first.sections[0].bytes, diagnostics);
        CHECK(manifest.has_value());
        CHECK(diagnostics.empty());
        CHECK(manifest->width == 2 && manifest->height == 2);
        CHECK(manifest->mips.size() == 2);
        CHECK(manifest->storageFormat == TextureFormat::BC7_sRGB);
        CHECK(manifest->codecVersion == kDirectXTexCodecVersion);

        const NormalizedImportSettings previewSettings =
            importer.normalizeSettings(1, {
                { "semantic", "color" },
                { "view_color_space", "srgb" },
                { "quality", "preview" },
                { "mip_policy", "full_chain" },
                { "alpha_mode", "opaque" },
            }, true);
        CHECK(previewSettings.valid());
        const CookProduct preview = importer.cook(
            parsed, previewSettings, target, {});
        CHECK(!hasCookErrors(preview.diagnostics));
        CHECK(preview.sections[1].bytes.size() == 20);
        std::vector<CookDiagnostic>
            previewDiagnostics;
        const auto previewManifest =
            readTextureManifest(
                preview.sections[0].bytes,
                previewDiagnostics);
        CHECK(previewManifest.has_value());
        CHECK(previewDiagnostics.empty());
        CHECK(previewManifest->quality ==
            TextureCompressionQuality::Preview);
        CHECK(previewManifest->storageFormat ==
            TextureFormat::RGBA8_sRGB);
        CHECK(validateTextureProduct(
            *previewManifest,
            preview.sections[1].bytes.size()).empty());

        struct SemanticCase {
            const char* semantic;
            TextureFormat format;
            size_t expectedBytes;
        };
        constexpr std::array semanticCases{
            SemanticCase{ "data", TextureFormat::BC7_UNorm, 32 },
            SemanticCase{ "normal", TextureFormat::BC5_UNorm, 32 },
            SemanticCase{ "scalar", TextureFormat::BC4_UNorm, 16 },
            SemanticCase{ "hdr_color", TextureFormat::BC6H_UFloat, 32 },
        };
        for (const SemanticCase& semanticCase : semanticCases) {
            const NormalizedImportSettings semanticSettings =
                importer.normalizeSettings(1, {
                    { "semantic", semanticCase.semantic },
                    { "view_color_space", "linear" },
                    { "quality", "iteration" },
                    { "mip_policy", "full_chain" },
                    { "alpha_mode", "opaque" },
                }, true);
            CHECK(semanticSettings.valid());
            const CookProduct semanticProduct = importer.cook(
                parsed, semanticSettings, target, {});
            CHECK(!hasCookErrors(semanticProduct.diagnostics));
            CHECK(semanticProduct.sections[1].bytes.size() ==
                semanticCase.expectedBytes);
            std::vector<CookDiagnostic> semanticDiagnostics;
            const auto semanticManifest = readTextureManifest(
                semanticProduct.sections[0].bytes, semanticDiagnostics);
            CHECK(semanticManifest.has_value());
            CHECK(semanticDiagnostics.empty());
            CHECK(semanticManifest->storageFormat == semanticCase.format);
        }

        const NormalizedImportSettings preservedSettings =
            importer.normalizeSettings(1, {
                { "semantic", "color" },
                { "view_color_space", "srgb" },
                { "mip_policy", "preserve_source" },
            }, true);
        CHECK(preservedSettings.valid());
        const CookProduct preserved = importer.cook(
            parsed, preservedSettings, target, {});
        CHECK(!hasCookErrors(preserved.diagnostics));
        std::vector<CookDiagnostic> preservedDiagnostics;
        const auto preservedManifest = readTextureManifest(
            preserved.sections[0].bytes, preservedDiagnostics);
        CHECK(preservedManifest.has_value());
        CHECK(preservedManifest->mips.size() == 1);

        const auto unknown = importer.normalizeSettings(1, {
            { "semantic", "color" },
            { "view_color_space", "srgb" },
            { "untracked_workflow_knob", true },
        }, true);
        CHECK(!unknown.valid());
        CHECK(unknown.diagnostics.front().code ==
            "TEXTURE_SETTINGS_UNKNOWN");
        return true;
    }

    bool productionImporterReportsUnresolvedGitLfsPointers() {
        TextureImporter importer;
        const NormalizedImportSettings settings =
            importer.normalizeSettings(1, {
                { "semantic", "color" },
                { "view_color_space", "srgb" },
            }, true);
        CHECK(settings.valid());
        const std::string pointer =
            "version https://git-lfs.github.com/spec/v1\n"
            "oid sha256:e2ab2939dd0b2c771b68c418987fb9fdfb60e7f2a5e3f9a3a646e2571ec20b15\n"
            "size 70\n";
        const auto bytes =
            std::as_bytes(std::span(
                pointer.data(),
                pointer.size()));
        const ParsedSourceAsset parsed =
            importer.parse({
                .relativePath = "white.png",
                .resolvedPath = "white.png",
                .bytes = bytes,
            }, settings);
        CHECK(hasCookErrors(
            parsed.diagnostics));
        CHECK(parsed.diagnostics.front().code ==
            "TEXTURE_SOURCE_GIT_LFS_POINTER");
        CHECK(parsed.diagnostics.front()
            .message.find("Git LFS") !=
            std::string::npos);
        return true;
    }

    bool hdriImporterOwnsHdrAndValidatesProductionSettings() {
        EnvironmentImporter environment;
        TextureImporter texture;
        const std::array<std::byte, 1> bytes{ std::byte{ 1 } };
        CHECK(environment.probe("studio.HDR", bytes) ==
            ImportProbeResult::Supported);
        CHECK(environment.probe("studio.exr", bytes) ==
            ImportProbeResult::Unsupported);
        CHECK(texture.probe("studio.hdr", bytes) ==
            ImportProbeResult::Unsupported);
        CHECK(environment.descriptor().assetTypes ==
            std::vector<std::string>{ "iridium.environment" });

        const auto defaults = environment.normalizeSettings(
            1, nlohmann::json::object(), true);
        CHECK(defaults.valid());
        CHECK(defaults.values.at("radiance_size") == 1024u);
        CHECK(defaults.values.at("irradiance_size") == 32u);
        CHECK(defaults.values.at("prefiltered_size") == 1024u);
        CHECK(defaults.values.at("brdf_lut_size") == 256u);
        CHECK(defaults.values.at("prefiltered_samples") == 1024u);
        CHECK(defaults.values.at("brdf_samples") == 1024u);
        CHECK(defaults.values.at("source_primaries") ==
            "linear_rec709_d65");

        const auto ap1 = environment.normalizeSettings(1, {
            { "radiance_size", 2048u },
            { "irradiance_size", 64u },
            { "prefiltered_size", 1024u },
            { "brdf_lut_size", 512u },
            { "prefiltered_samples", 4096u },
            { "brdf_samples", 4096u },
            { "source_primaries", "acescg_ap1_d60" },
            { "radiance_scale", 1.5f },
        }, true);
        CHECK(ap1.valid());
        CHECK(ap1.values.at("radiance_scale") == 1.5f);
        CHECK(!environment.normalizeSettings(1, {
            { "radiance_size", 3000u },
        }, true).valid());
        CHECK(!environment.normalizeSettings(1, {
            { "untracked_hdri_knob", true },
        }, true).valid());
        return true;
    }

    bool environmentProductIsDeterministicAndStrict() {
        const auto sourceGuid = AssetGuid::parse(
            "019fb73d-5a26-7326-8688-ea55a972179c");
        CHECK(sourceGuid.has_value());
        CookedEnvironmentManifest manifest{
            .sourceTextureGuid = *sourceGuid,
            .sourcePrimaries = "linear_rec709_d65",
            .sourceRadianceScale = 1.25f,
            .convolutionImplementation = "iridium-cpu-reference-v1",
            .sampleSequence = "hammersley-base2-v1",
            .toolVersion = "m5.5-test-v1",
            .radiance = { 4, 4, 3, 6, TextureFormat::RGBA16_SFloat },
            .irradiance = { 2, 2, 1, 6, TextureFormat::RGBA16_SFloat },
            .prefilteredSpecular = {
                4, 4, 3, 6, TextureFormat::RGBA16_SFloat },
            .brdfLut = { 4, 4, 1, 1, TextureFormat::RG16_SFloat },
        };
        std::vector<std::byte> radiance(
            environmentProductByteSize(manifest.radiance));
        std::vector<std::byte> irradiance(
            environmentProductByteSize(manifest.irradiance));
        std::vector<std::byte> prefiltered(
            environmentProductByteSize(manifest.prefilteredSpecular));
        std::vector<std::byte> brdf(
            environmentProductByteSize(manifest.brdfLut));
        const EnvironmentPayloads payloads{
            radiance, irradiance, prefiltered, brdf };

        const auto first = serializeEnvironmentManifest(manifest);
        const auto second = serializeEnvironmentManifest(manifest);
        CHECK(first == second);
        std::vector<CookDiagnostic> readDiagnostics;
        const auto decoded = readEnvironmentManifest(first, readDiagnostics);
        CHECK(decoded.has_value());
        CHECK(readDiagnostics.empty());
        CHECK(*decoded == manifest);
        CHECK(validateEnvironmentProduct(manifest, payloads).empty());
        const CookProduct product = makeCookedEnvironmentProduct(
            manifest, payloads);
        CHECK(!hasCookErrors(product.diagnostics));
        CHECK(product.artifactType == "iridium.environment");
        CHECK(product.sections.size() == 5);
        CookedArtifact artifact{
            .assetGuid = *sourceGuid,
            .artifactType = product.artifactType,
            .artifactSchemaVersion = product.artifactSchemaVersion,
            .sections = product.sections,
        };
        const CookedEnvironmentReadResult productRead =
            readCookedEnvironmentProduct(artifact);
        CHECK(productRead.valid());
        CHECK(productRead.data->manifest == manifest);
        artifact.sections.push_back({ 0x12345678u, 1, 1, {} });
        CHECK(!readCookedEnvironmentProduct(artifact).valid());
        artifact.sections.pop_back();
        artifact.sections.back().bytes.pop_back();
        CHECK(!readCookedEnvironmentProduct(artifact).valid());

        irradiance.pop_back();
        const EnvironmentPayloads shortPayloads{
            radiance, irradiance, prefiltered, brdf };
        CHECK(hasCookErrors(validateEnvironmentProduct(
            manifest, shortPayloads)));
        manifest.sourcePrimaries = "unspecified";
        CHECK(hasCookErrors(validateEnvironmentProduct(manifest, shortPayloads)));
        manifest.sourcePrimaries = "linear_rec709_d65";
        manifest.sourceRadianceScale = -1.0f;
        CHECK(hasCookErrors(validateEnvironmentProduct(
            manifest, shortPayloads)));
        return true;
    }

    bool environmentConvolutionIsPhysicalAndDeterministic() {
        const std::array<glm::vec3, 6> expectedDirections{
            glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0),
            glm::vec3(0, 1, 0), glm::vec3(0, -1, 0),
            glm::vec3(0, 0, 1), glm::vec3(0, 0, -1),
        };
        for (uint32_t face = 0; face < expectedDirections.size(); ++face)
            CHECK(glm::length(environmentCubeDirection(face, 0.0f, 0.0f) -
                expectedDirections[face]) < 1.0e-6f);

        // Every face boundary must describe the same direction as exactly one
        // neighboring boundary (corners excluded). This freezes a seamless
        // cube orientation without relying on a particular face adjacency table.
        const auto edgeDirection = [](uint32_t face, uint32_t edge, float t) {
            switch (edge) {
            case 0: return environmentCubeDirection(face, -1.0f, t);
            case 1: return environmentCubeDirection(face, 1.0f, t);
            case 2: return environmentCubeDirection(face, t, -1.0f);
            default: return environmentCubeDirection(face, t, 1.0f);
            }
        };
        for (uint32_t face = 0; face < 6; ++face) {
            for (uint32_t edge = 0; edge < 4; ++edge) {
                uint32_t matches = 0;
                const glm::vec3 direction = edgeDirection(face, edge, 0.375f);
                for (uint32_t otherFace = 0; otherFace < 6; ++otherFace) {
                    if (otherFace == face) continue;
                    for (uint32_t otherEdge = 0; otherEdge < 4; ++otherEdge) {
                        const float same = glm::length(direction -
                            edgeDirection(otherFace, otherEdge, 0.375f));
                        const float reversed = glm::length(direction -
                            edgeDirection(otherFace, otherEdge, -0.375f));
                        if ((std::min)(same, reversed) < 1.0e-6f) ++matches;
                    }
                }
                CHECK(matches == 1);
            }
        }

        constexpr glm::vec3 ConstantRadiance{ 0.25f, 0.5f, 1.0f };
        EnvironmentFloatImage source{
            .width = 8,
            .height = 4,
            .pixels = std::vector<glm::vec4>(32, glm::vec4(ConstantRadiance, 1.0f)),
        };
        EnvironmentConvolutionSettings settings{
            .radianceSize = 4,
            .irradianceSize = 2,
            .prefilteredSize = 4,
            .brdfLutSize = 4,
            .prefilteredSamples = 64,
            .brdfSamples = 128,
            .sourcePrimaries = "acescg_ap1_d60",
        };
        const ConvolvedEnvironment first =
            convolveEnvironmentReference(source, settings);
        const ConvolvedEnvironment second =
            convolveEnvironmentReference(source, settings);
        EnvironmentConvolutionSettings singleSample = settings;
        singleSample.prefilteredSamples = 1;
        const ConvolvedEnvironment singleSampleEnvironment =
            convolveEnvironmentReference(source, singleSample);
        CHECK(first.radiance.mips == second.radiance.mips);
        CHECK(first.irradiance.mips == second.irradiance.mips);
        CHECK(first.prefilteredSpecular.mips ==
            second.prefilteredSpecular.mips);
        CHECK(first.prefilteredSpecular.mips.front() ==
            singleSampleEnvironment.prefilteredSpecular.mips.front());
        CHECK(first.brdfLut == second.brdfLut);
        CHECK(first.radiance.mips.size() == 3);
        CHECK(first.prefilteredSpecular.mips.size() == 3);
        for (const auto& mip : first.radiance.mips)
            for (glm::vec4 value : mip)
                CHECK(glm::length(glm::vec3(value) - ConstantRadiance) < 1.0e-6f);
        for (glm::vec4 value : first.irradiance.mips.front())
            CHECK(glm::length(glm::vec3(value) -
                ConstantRadiance * 3.14159265358979323846f) < 1.0e-5f);
        for (const auto& mip : first.prefilteredSpecular.mips)
            for (glm::vec4 value : mip)
                CHECK(glm::length(glm::vec3(value) - ConstantRadiance) < 1.0e-5f);
        for (glm::vec2 value : first.brdfLut) {
            CHECK(std::isfinite(value.x));
            CHECK(std::isfinite(value.y));
            CHECK(value.x >= 0.0f);
            CHECK(value.y >= 0.0f);
        }
        std::stop_source cancelled;
        cancelled.request_stop();
        bool cancellationObserved = false;
        try {
            (void)convolveEnvironmentReference(source, settings,
                cancelled.get_token());
        } catch (const std::runtime_error&) {
            cancellationObserved = true;
        }
        CHECK(cancellationObserved);

        EnvironmentFloatImage outlierSource{
            .width = 2,
            .height = 1,
            .pixels = {
                glm::vec4(90000.0f, 4.0f, 2.0f, 1.0f),
                glm::vec4(1.0f),
            },
        };
        EnvironmentConvolutionSettings outlierSettings{
            .radianceSize = 2,
            .irradianceSize = 1,
            .prefilteredSize = 1,
            .brdfLutSize = 1,
            .prefilteredSamples = 1,
            .brdfSamples = 1,
            .sourcePrimaries = "acescg_ap1_d60",
        };
        const ConvolvedEnvironment clamped =
            convolveEnvironmentReference(outlierSource, outlierSettings);
        bool reachedFp16Maximum = false;
        for (const auto& mip : clamped.radiance.mips) {
            for (const glm::vec4 value : mip) {
                CHECK(glm::all(glm::lessThanEqual(
                    glm::vec3(value), glm::vec3(65504.0f))));
                CHECK(glm::all(glm::greaterThanEqual(
                    glm::vec3(value), glm::vec3(0.0f))));
                reachedFp16Maximum |= value.x == 65504.0f;
            }
        }
        CHECK(reachedFp16Maximum);

        const auto sourceGuid = AssetGuid::parse(
            "019fb73d-5a26-7326-8688-ea55a972179c");
        CHECK(sourceGuid.has_value());
        const CookProduct firstProduct = makeConvolvedEnvironmentProduct(
            *sourceGuid, first, settings, "m5.5-test-v1");
        const CookProduct secondProduct = makeConvolvedEnvironmentProduct(
            *sourceGuid, second, settings, "m5.5-test-v1");
        CHECK(!hasCookErrors(firstProduct.diagnostics));
        CHECK(firstProduct.sections.size() == secondProduct.sections.size());
        for (size_t index = 0; index < firstProduct.sections.size(); ++index) {
            CHECK(firstProduct.sections[index].id ==
                secondProduct.sections[index].id);
            CHECK(firstProduct.sections[index].bytes ==
                secondProduct.sections[index].bytes);
        }
        return true;
    }

    bool capturedCubeBakeIsCompleteAndReimportable() {
        constexpr uint32_t CaptureSize = 4;
        constexpr uint32_t IrradianceSize = 2;
        constexpr glm::vec3 ConstantRadiance{ 0.25f, 0.5f, 1.0f };
        const uint16_t pixel[4]{
            floatToHalf(ConstantRadiance.x),
            floatToHalf(ConstantRadiance.y),
            floatToHalf(ConstantRadiance.z),
            floatToHalf(1.0f),
        };
        std::vector<std::byte> radiance(
            6u * CaptureSize * CaptureSize * sizeof(pixel));
        for (size_t offset = 0; offset < radiance.size();
            offset += sizeof(pixel))
            std::memcpy(radiance.data() + offset, pixel, sizeof(pixel));
        const std::vector<std::byte> irradiance =
            makeCapturedCubeDiffuseIrradiance(radiance, CaptureSize,
                IrradianceSize);
        CHECK(irradiance.size() ==
            6u * IrradianceSize * IrradianceSize * sizeof(pixel));
        const auto* irradianceHalf = reinterpret_cast<const uint16_t*>(
            irradiance.data());
        for (size_t index = 0; index < irradiance.size() / sizeof(pixel);
            ++index) {
            const glm::vec3 value{
                Color::halfToFloat(irradianceHalf[index * 4u]),
                Color::halfToFloat(irradianceHalf[index * 4u + 1u]),
                Color::halfToFloat(irradianceHalf[index * 4u + 2u]),
            };
            CHECK(glm::length(value - ConstantRadiance *
                3.14159265358979323846f) < 0.004f);
        }

        const auto assetGuid = AssetGuid::parse(
            "019fb73d-5a26-7326-8688-ea55a972179c");
        CHECK(assetGuid.has_value());
        std::vector<std::byte> prefiltered = radiance;
        std::vector<std::byte> brdf(4u, std::byte{});
        const CookedEnvironmentManifest manifest{
            .sourceTextureGuid = *assetGuid,
            .sourcePrimaries = "acescg_ap1_d60",
            .sourceRadianceScale = 1.0f,
            .convolutionImplementation = "iridium_gpu_scene_capture_ggx_v1",
            .sampleSequence = "test_sequence",
            .toolVersion = "m5.8-test-v1",
            .radiance = { CaptureSize, CaptureSize, 1, 6,
                TextureFormat::RGBA16_SFloat },
            .irradiance = { IrradianceSize, IrradianceSize, 1, 6,
                TextureFormat::RGBA16_SFloat },
            .prefilteredSpecular = { CaptureSize, CaptureSize, 1, 6,
                TextureFormat::RGBA16_SFloat },
            .brdfLut = { 1, 1, 1, 1, TextureFormat::RG16_SFloat },
        };
        const CookProduct product = makeCookedEnvironmentProduct(manifest,
            { radiance, irradiance, prefiltered, brdf });
        CHECK(!hasCookErrors(product.diagnostics));
        const CookedArtifact sourceArtifact{
            .assetGuid = *assetGuid,
            .artifactType = product.artifactType,
            .artifactSchemaVersion = product.artifactSchemaVersion,
            .target = { "windows-x64", "editor", "reference", 1, 2 },
            .cookKey = std::string(64, '1'),
            .dependencies = { {
                .type = AssetDependencyType::Asset,
                .assetGuid = *assetGuid,
                .location = "scene.iridium.scene.json",
            } },
            .sections = product.sections,
        };
        const CookedArtifactBlob sourceBlob =
            serializeCookedArtifact(sourceArtifact);
        BakedProbeEnvironmentImporter importer;
        CHECK(importer.probe("capture.irprobe", sourceBlob.bytes) ==
            ImportProbeResult::Supported);
        const NormalizedImportSettings settings = importer.normalizeSettings(
            1, nlohmann::json::object(), true);
        CHECK(settings.valid());
        const ParsedSourceAsset parsed = importer.parse({
            .relativePath = "capture.irprobe",
            .resolvedPath = "capture.irprobe",
            .bytes = sourceBlob.bytes,
        }, settings);
        CHECK(!hasCookErrors(parsed.diagnostics));
        CHECK(parsed.dependencies == sourceArtifact.dependencies);
        const CookProduct recooked = importer.cook(parsed, settings,
            sourceArtifact.target, { .assetGuid = *assetGuid });
        CHECK(!hasCookErrors(recooked.diagnostics));
        CHECK(recooked.sections == product.sections);
        return true;
    }

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<bool()>>> tests = {
        { "settings are canonical and semantic", settingsAreCanonicalAndSemantic },
        { "block layouts are exact", blockLayoutsAreExact },
        { "mips respect color normal and coverage", mipGenerationRespectsColorNormalAndCoverage },
        { "manifest round trips and validates layout", manifestRoundTripsAndRejectsWrongLayout },
        { "residency fallback growth and retirement", residencyUsesFallbackGrowthAndDelayedReuse },
        { "samplers deduplicate", samplersDeduplicateByFullSemantics },
        { "editor preview extent is resident-budget bounded",
            previewProductsCapResidentExtent },
        { "production texture importer is deterministic",
            productionImporterDecodesMipsAndCompressesDeterministically },
        { "production texture importer reports Git LFS pointers",
            productionImporterReportsUnresolvedGitLfsPointers },
        { "HDRI importer owns HDR and validates production settings",
            hdriImporterOwnsHdrAndValidatesProductionSettings },
        { "environment product is deterministic and strict",
            environmentProductIsDeterministicAndStrict },
        { "environment convolution is physical and deterministic",
            environmentConvolutionIsPhysicalAndDeterministic },
        { "captured cube bake is complete and reimportable",
            capturedCubeBakeIsCompleteAndReimportable },
    };

    for (const auto& [name, test] : tests) {
        std::cout << "[ RUN      ] " << name << '\n';
        if (!test()) {
            std::cout << "[  FAILED  ] " << name << '\n';
            return 1;
        }
        std::cout << "[       OK ] " << name << '\n';
    }
    return 0;
}
