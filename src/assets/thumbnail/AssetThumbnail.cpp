#include "assets/thumbnail/AssetThumbnail.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <numbers>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXTex.h>
#endif

namespace Iridium {

    namespace {

        struct Float3 {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
        };

        Float3 operator+(Float3 lhs, Float3 rhs) {
            return {
                lhs.x + rhs.x,
                lhs.y + rhs.y,
                lhs.z + rhs.z,
            };
        }

        Float3 operator-(Float3 lhs, Float3 rhs) {
            return {
                lhs.x - rhs.x,
                lhs.y - rhs.y,
                lhs.z - rhs.z,
            };
        }

        Float3 operator*(Float3 value, float scalar) {
            return {
                value.x * scalar,
                value.y * scalar,
                value.z * scalar,
            };
        }

        float dot(Float3 lhs, Float3 rhs) {
            return lhs.x * rhs.x +
                lhs.y * rhs.y +
                lhs.z * rhs.z;
        }

        Float3 cross(Float3 lhs, Float3 rhs) {
            return {
                lhs.y * rhs.z - lhs.z * rhs.y,
                lhs.z * rhs.x - lhs.x * rhs.z,
                lhs.x * rhs.y - lhs.y * rhs.x,
            };
        }

        Float3 normalized(Float3 value) {
            const float length =
                std::sqrt(dot(value, value));
            return length > 1.0e-8f
                ? value * (1.0f / length)
                : Float3{ 0.0f, 0.0f, 1.0f };
        }

        uint8_t toByte(float value) {
            return static_cast<uint8_t>(
                std::lround(std::clamp(
                    value, 0.0f, 1.0f) * 255.0f));
        }

        void setPixel(
            AssetThumbnailPixels& result,
            uint32_t x,
            uint32_t y,
            std::array<float, 4> color) {
            const size_t offset =
                (static_cast<size_t>(y) *
                    result.width + x) * 4;
            for (size_t channel = 0;
                channel < 4; ++channel) {
                result.rgba8[offset + channel] =
                    static_cast<std::byte>(
                        toByte(color[channel]));
            }
        }

        AssetThumbnailPixels baseImage(
            AssetGuid guid, uint32_t extent) {
            AssetThumbnailPixels result{
                .assetGuid = guid,
                .width = extent,
                .height = extent,
                .rgba8 = std::vector<std::byte>(
                    static_cast<size_t>(extent) *
                    extent * 4),
            };
            for (uint32_t y = 0; y < extent; ++y) {
                for (uint32_t x = 0;
                    x < extent; ++x) {
                    const bool light =
                        ((x / 12) + (y / 12)) % 2 == 0;
                    const float gradient =
                        static_cast<float>(y) /
                        std::max(1u, extent - 1);
                    const float value =
                        (light ? 0.105f : 0.075f) +
                        gradient * 0.035f;
                    setPixel(result, x, y,
                        { value, value * 1.04f,
                          value * 1.10f, 1.0f });
                }
            }
            return result;
        }

        const CookedModelMaterial* findMaterial(
            const CookedModelProductData& product,
            AssetGuid materialGuid) {
            const auto found = std::ranges::find_if(
                product.materials,
                [materialGuid](
                    const CookedModelMaterial& material) {
                    return material.materialGuid ==
                        materialGuid;
                });
            return found != product.materials.end()
                ? &*found : nullptr;
        }

        std::array<float, 3> displayBaseColor(
            const CompiledMaterial& material) {
            const auto& value =
                material.standard.baseColorFactor;
            return {
                std::pow(std::max(0.0f, value.r),
                    1.0f / 2.2f),
                std::pow(std::max(0.0f, value.g),
                    1.0f / 2.2f),
                std::pow(std::max(0.0f, value.b),
                    1.0f / 2.2f),
            };
        }

        AssetThumbnailPixels materialThumbnail(
            const CookedModelMaterial& material,
            uint32_t extent) {
            AssetThumbnailPixels result =
                baseImage(material.materialGuid, extent);
            const auto base =
                displayBaseColor(material.compiled);
            const float roughness =
                std::clamp(
                    material.compiled.standard
                        .roughnessFactor,
                    0.03f, 1.0f);
            const float metallic =
                std::clamp(
                    material.compiled.standard
                        .metallicFactor,
                    0.0f, 1.0f);
            const Float3 light =
                normalized({ -0.4f, 0.65f, 0.65f });
            const float radius =
                extent * 0.39f;
            const float center =
                (extent - 1) * 0.5f;
            for (uint32_t y = 0; y < extent; ++y) {
                for (uint32_t x = 0;
                    x < extent; ++x) {
                    const float nx =
                        (static_cast<float>(x) -
                            center) / radius;
                    const float ny =
                        (center -
                            static_cast<float>(y)) /
                        radius;
                    const float radiusSquared =
                        nx * nx + ny * ny;
                    if (radiusSquared > 1.0f) {
                        continue;
                    }
                    const Float3 normal{
                        nx, ny,
                        std::sqrt(std::max(
                            0.0f,
                            1.0f - radiusSquared)),
                    };
                    const float diffuse =
                        0.16f +
                        0.84f * std::max(
                            0.0f, dot(normal, light));
                    const Float3 halfVector =
                        normalized(light +
                            Float3{ 0.0f, 0.0f, 1.0f });
                    const float exponent =
                        std::max(2.0f,
                            2.0f /
                            (roughness * roughness));
                    const float specular =
                        std::pow(std::max(
                            0.0f,
                            dot(normal, halfVector)),
                            exponent) *
                        (0.18f + metallic * 0.72f);
                    std::array<float, 4> color{
                        base[0] * diffuse *
                            (1.0f - metallic * 0.35f) +
                            specular,
                        base[1] * diffuse *
                            (1.0f - metallic * 0.35f) +
                            specular,
                        base[2] * diffuse *
                            (1.0f - metallic * 0.35f) +
                            specular,
                        material.compiled.standard
                            .baseColorFactor.a,
                    };
                    setPixel(result, x, y, color);
                }
            }
            return result;
        }

        struct ProjectedVertex {
            float x = 0.0f;
            float y = 0.0f;
            float depth = 0.0f;
        };

        float edge(ProjectedVertex a,
            ProjectedVertex b,
            float x, float y) {
            return (x - a.x) * (b.y - a.y) -
                (y - a.y) * (b.x - a.x);
        }

        AssetThumbnailPixels geometryThumbnail(
            const CookedModelProductData& product,
            const AssetCatalogRecord& record,
            uint32_t extent) {
            AssetThumbnailPixels result =
                baseImage(record.guid, extent);
            std::vector<const CookedModelPrimitive*>
                primitives;
            for (const CookedModelPrimitive& primitive :
                product.manifest.primitives) {
                if (record.assetType ==
                        "iridium.model-primitive" &&
                    primitive.primitiveGuid !=
                        record.guid) {
                    continue;
                }
                primitives.push_back(&primitive);
            }
            if (primitives.empty()) {
                result.diagnostic =
                    "Cooked geometry thumbnail has no matching primitives.";
                return result;
            }

            Float3 boundsMin{
                std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max(),
            };
            Float3 boundsMax{
                std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::lowest(),
            };
            for (const CookedModelPrimitive* primitive :
                primitives) {
                boundsMin.x = std::min(boundsMin.x,
                    primitive->bounds.aabbMin[0]);
                boundsMin.y = std::min(boundsMin.y,
                    primitive->bounds.aabbMin[1]);
                boundsMin.z = std::min(boundsMin.z,
                    primitive->bounds.aabbMin[2]);
                boundsMax.x = std::max(boundsMax.x,
                    primitive->bounds.aabbMax[0]);
                boundsMax.y = std::max(boundsMax.y,
                    primitive->bounds.aabbMax[1]);
                boundsMax.z = std::max(boundsMax.z,
                    primitive->bounds.aabbMax[2]);
            }
            const Float3 center =
                (boundsMin + boundsMax) * 0.5f;
            const Float3 forward =
                normalized({ 0.75f, 0.45f, 1.0f });
            const Float3 right =
                normalized(cross(
                    { 0.0f, 1.0f, 0.0f },
                    forward));
            const Float3 up =
                normalized(cross(forward, right));

            float maxProjection = 1.0e-5f;
            const std::array<Float3, 8> corners{{
                { boundsMin.x, boundsMin.y, boundsMin.z },
                { boundsMax.x, boundsMin.y, boundsMin.z },
                { boundsMin.x, boundsMax.y, boundsMin.z },
                { boundsMax.x, boundsMax.y, boundsMin.z },
                { boundsMin.x, boundsMin.y, boundsMax.z },
                { boundsMax.x, boundsMin.y, boundsMax.z },
                { boundsMin.x, boundsMax.y, boundsMax.z },
                { boundsMax.x, boundsMax.y, boundsMax.z },
            }};
            for (Float3 corner : corners) {
                const Float3 relative =
                    corner - center;
                maxProjection = std::max(
                    maxProjection,
                    std::abs(dot(relative, right)));
                maxProjection = std::max(
                    maxProjection,
                    std::abs(dot(relative, up)));
            }
            const float scale =
                (extent * 0.42f) / maxProjection;
            const float imageCenter =
                (extent - 1) * 0.5f;
            std::vector<float> depths(
                static_cast<size_t>(extent) *
                    extent,
                std::numeric_limits<float>::lowest());
            const Float3 light =
                normalized({ -0.35f, 0.75f, 0.55f });

            const auto projected =
                [&](uint32_t vertexIndex) {
                    const auto& position =
                        product.vertices[vertexIndex]
                            .position;
                    const Float3 relative{
                        position[0] - center.x,
                        position[1] - center.y,
                        position[2] - center.z,
                    };
                    return ProjectedVertex{
                        imageCenter +
                            dot(relative, right) *
                                scale,
                        imageCenter -
                            dot(relative, up) *
                                scale,
                        dot(relative, forward),
                    };
                };

            for (const CookedModelPrimitive* primitive :
                primitives) {
                const CookedModelMaterial* material =
                    findMaterial(product,
                        primitive->materialGuid);
                const std::array<float, 3> base =
                    material
                    ? displayBaseColor(
                        material->compiled)
                    : std::array<float, 3>{
                        0.45f, 0.55f, 0.65f };
                const uint64_t indexEnd =
                    primitive->firstIndex +
                    primitive->indexCount;
                for (uint64_t index =
                        primitive->firstIndex;
                    index + 2 < indexEnd;
                    index += 3) {
                    const uint32_t ia =
                        product.indices[
                            static_cast<size_t>(index)];
                    const uint32_t ib =
                        product.indices[
                            static_cast<size_t>(index + 1)];
                    const uint32_t ic =
                        product.indices[
                            static_cast<size_t>(index + 2)];
                    const ProjectedVertex a =
                        projected(ia);
                    const ProjectedVertex b =
                        projected(ib);
                    const ProjectedVertex c =
                        projected(ic);
                    const float area =
                        edge(a, b, c.x, c.y);
                    if (std::abs(area) < 1.0e-6f) {
                        continue;
                    }
                    const int minX = std::max(
                        0, static_cast<int>(
                            std::floor(std::min({
                                a.x, b.x, c.x }))));
                    const int maxX = std::min(
                        static_cast<int>(extent) - 1,
                        static_cast<int>(
                            std::ceil(std::max({
                                a.x, b.x, c.x }))));
                    const int minY = std::max(
                        0, static_cast<int>(
                            std::floor(std::min({
                                a.y, b.y, c.y }))));
                    const int maxY = std::min(
                        static_cast<int>(extent) - 1,
                        static_cast<int>(
                            std::ceil(std::max({
                                a.y, b.y, c.y }))));

                    const auto& pa =
                        product.vertices[ia].position;
                    const auto& pb =
                        product.vertices[ib].position;
                    const auto& pc =
                        product.vertices[ic].position;
                    const Float3 normal = normalized(
                        cross(
                            { pb[0] - pa[0],
                              pb[1] - pa[1],
                              pb[2] - pa[2] },
                            { pc[0] - pa[0],
                              pc[1] - pa[1],
                              pc[2] - pa[2] }));
                    const float lighting =
                        0.22f + 0.78f *
                        std::abs(dot(normal, light));
                    for (int y = minY;
                        y <= maxY; ++y) {
                        for (int x = minX;
                            x <= maxX; ++x) {
                            const float sampleX =
                                x + 0.5f;
                            const float sampleY =
                                y + 0.5f;
                            const float wa =
                                edge(b, c,
                                    sampleX, sampleY) /
                                area;
                            const float wb =
                                edge(c, a,
                                    sampleX, sampleY) /
                                area;
                            const float wc =
                                edge(a, b,
                                    sampleX, sampleY) /
                                area;
                            if (wa < 0.0f ||
                                wb < 0.0f ||
                                wc < 0.0f) {
                                continue;
                            }
                            const float depth =
                                wa * a.depth +
                                wb * b.depth +
                                wc * c.depth;
                            const size_t pixel =
                                static_cast<size_t>(y) *
                                extent + x;
                            if (depth <= depths[pixel]) {
                                continue;
                            }
                            depths[pixel] = depth;
                            setPixel(result,
                                static_cast<uint32_t>(x),
                                static_cast<uint32_t>(y),
                                {
                                    base[0] * lighting,
                                    base[1] * lighting,
                                    base[2] * lighting,
                                    1.0f,
                                });
                        }
                    }
                }
            }
            return result;
        }

#if defined(_WIN32)
        DXGI_FORMAT dxgiFormat(
            TextureFormat format) {
            switch (format) {
            case TextureFormat::RGBA8_UNorm:
                return DXGI_FORMAT_R8G8B8A8_UNORM;
            case TextureFormat::RGBA8_sRGB:
                return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            case TextureFormat::RGBA16_SFloat:
                return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case TextureFormat::RGBA32_SFloat:
                return DXGI_FORMAT_R32G32B32A32_FLOAT;
            case TextureFormat::BC4_UNorm:
                return DXGI_FORMAT_BC4_UNORM;
            case TextureFormat::BC5_UNorm:
                return DXGI_FORMAT_BC5_UNORM;
            case TextureFormat::BC6H_UFloat:
                return DXGI_FORMAT_BC6H_UF16;
            case TextureFormat::BC7_UNorm:
                return DXGI_FORMAT_BC7_UNORM;
            case TextureFormat::BC7_sRGB:
                return DXGI_FORMAT_BC7_UNORM;
            }
            return DXGI_FORMAT_UNKNOWN;
        }
#endif

        AssetThumbnailPixels textureThumbnail(
            AssetGuid textureGuid,
            const CookedTextureManifest& manifest,
            std::span<const std::byte> payload,
            uint32_t extent) {
            AssetThumbnailPixels result =
                baseImage(textureGuid, extent);
            if (manifest.mips.empty()) {
                result.diagnostic =
                    "Cooked texture thumbnail has no mip.";
                return result;
            }
#if defined(_WIN32)
            const CookedTextureMip& mip =
                manifest.mips.front();
            if (mip.byteOffset >
                    payload.size() ||
                mip.byteSize >
                    payload.size() -
                        mip.byteOffset) {
                result.diagnostic =
                    "Cooked texture thumbnail payload is truncated.";
                return result;
            }
            DirectX::Image source{
                .width = mip.width,
                .height = mip.height,
                .format = dxgiFormat(
                    manifest.storageFormat),
                .rowPitch =
                    isBlockCompressed(
                        manifest.storageFormat)
                    ? static_cast<size_t>(
                        (mip.width + 3) / 4) *
                        bytesPerBlock(
                            manifest.storageFormat)
                    : static_cast<size_t>(
                        mip.width) *
                        bytesPerTexel(
                            manifest.storageFormat),
                .slicePitch =
                    static_cast<size_t>(
                        mip.byteSize),
                .pixels =
                    reinterpret_cast<uint8_t*>(
                        const_cast<std::byte*>(
                            payload.data() +
                            static_cast<size_t>(
                                mip.byteOffset))),
            };
            DirectX::ScratchImage converted;
            HRESULT convertedResult = S_OK;
            if (DirectX::IsCompressed(
                    source.format)) {
                convertedResult =
                    DirectX::Decompress(
                        source,
                        DXGI_FORMAT_R32G32B32A32_FLOAT,
                        converted);
            }
            else {
                convertedResult =
                    DirectX::Convert(
                        source,
                        DXGI_FORMAT_R32G32B32A32_FLOAT,
                        DirectX::TEX_FILTER_DEFAULT,
                        DirectX::TEX_THRESHOLD_DEFAULT,
                        converted);
            }
            if (FAILED(convertedResult) ||
                !converted.GetImage(0, 0, 0)) {
                result.diagnostic =
                    "DirectXTex could not decode the cooked thumbnail view.";
                return result;
            }
            const DirectX::Image* image =
                converted.GetImage(0, 0, 0);
            const float fit = std::min(
                static_cast<float>(extent) /
                    image->width,
                static_cast<float>(extent) /
                    image->height);
            const uint32_t drawWidth =
                std::max(1u, static_cast<uint32_t>(
                    std::floor(
                        image->width * fit)));
            const uint32_t drawHeight =
                std::max(1u, static_cast<uint32_t>(
                    std::floor(
                        image->height * fit)));
            const uint32_t offsetX =
                (extent - drawWidth) / 2;
            const uint32_t offsetY =
                (extent - drawHeight) / 2;
            for (uint32_t y = 0;
                y < drawHeight; ++y) {
                const uint32_t sourceY =
                    std::min<uint32_t>(
                        static_cast<uint32_t>(
                            image->height - 1),
                        static_cast<uint32_t>(
                            static_cast<uint64_t>(y) *
                            image->height /
                            drawHeight));
                const auto* row =
                    reinterpret_cast<const float*>(
                        image->pixels +
                        sourceY * image->rowPitch);
                for (uint32_t x = 0;
                    x < drawWidth; ++x) {
                    const uint32_t sourceX =
                        std::min<uint32_t>(
                            static_cast<uint32_t>(
                                image->width - 1),
                            static_cast<uint32_t>(
                                static_cast<uint64_t>(x) *
                                image->width /
                                drawWidth));
                    const float* pixel =
                        row + sourceX * 4;
                    std::array<float, 4> color{
                        pixel[0], pixel[1],
                        pixel[2], pixel[3],
                    };
                    if (manifest.semantic ==
                        TextureSemantic::Normal) {
                        const float nx =
                            pixel[0] * 2.0f - 1.0f;
                        const float ny =
                            pixel[1] * 2.0f - 1.0f;
                        const float nz =
                            std::sqrt(std::max(
                                0.0f,
                                1.0f - nx * nx -
                                    ny * ny));
                        color = {
                            nx * 0.5f + 0.5f,
                            ny * 0.5f + 0.5f,
                            nz * 0.5f + 0.5f,
                            1.0f,
                        };
                    }
                    else if (manifest.semantic ==
                        TextureSemantic::HdrColor) {
                        for (size_t channel = 0;
                            channel < 3; ++channel) {
                            color[channel] =
                                color[channel] /
                                (1.0f +
                                    color[channel]);
                            color[channel] =
                                std::pow(std::max(
                                    0.0f,
                                    color[channel]),
                                    1.0f / 2.2f);
                        }
                    }
                    setPixel(result,
                        offsetX + x,
                        offsetY + y,
                        color);
                }
            }
#else
            result.diagnostic =
                "Cooked texture thumbnails are unavailable on this platform.";
#endif
            return result;
        }

        float halfToFloat(uint16_t value) {
            const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16u;
            int32_t exponent = static_cast<int32_t>((value >> 10u) & 0x1fu);
            uint32_t mantissa = value & 0x03ffu;
            uint32_t bits = 0;
            if (exponent == 0) {
                if (mantissa == 0) bits = sign;
                else {
                    exponent = 1;
                    while ((mantissa & 0x0400u) == 0) {
                        mantissa <<= 1u;
                        --exponent;
                    }
                    mantissa &= 0x03ffu;
                    bits = sign | (static_cast<uint32_t>(exponent + 112) << 23u) |
                        (mantissa << 13u);
                }
            }
            else if (exponent == 31) {
                bits = sign | 0x7f800000u | (mantissa << 13u);
            }
            else {
                bits = sign | (static_cast<uint32_t>(exponent + 112) << 23u) |
                    (mantissa << 13u);
            }
            float result = 0.0f;
            std::memcpy(&result, &bits, sizeof(result));
            return result;
        }

        std::array<float, 3> sampleEnvironmentPreview(
            const CookedEnvironmentProductData& product, Float3 direction) {
            const float ax = std::abs(direction.x);
            const float ay = std::abs(direction.y);
            const float az = std::abs(direction.z);
            uint32_t face = 0;
            float u = 0.0f;
            float v = 0.0f;
            if (ax >= ay && ax >= az) {
                if (direction.x >= 0.0f) {
                    face = 0; u = -direction.z / ax; v = -direction.y / ax;
                }
                else {
                    face = 1; u = direction.z / ax; v = -direction.y / ax;
                }
            }
            else if (ay >= az) {
                if (direction.y >= 0.0f) {
                    face = 2; u = direction.x / ay; v = direction.z / ay;
                }
                else {
                    face = 3; u = direction.x / ay; v = -direction.z / ay;
                }
            }
            else if (direction.z >= 0.0f) {
                face = 4; u = direction.x / az; v = -direction.y / az;
            }
            else {
                face = 5; u = -direction.x / az; v = -direction.y / az;
            }
            const auto& desc = product.manifest.radiance;
            uint64_t faceStride = 0;
            uint32_t mipSize = desc.width;
            for (uint32_t mip = 0; mip < desc.mipLevels; ++mip) {
                faceStride += static_cast<uint64_t>(mipSize) * mipSize * 8u;
                mipSize = std::max(1u, mipSize / 2u);
            }
            const uint32_t x = std::min(desc.width - 1u,
                static_cast<uint32_t>((u * 0.5f + 0.5f) * desc.width));
            const uint32_t y = std::min(desc.height - 1u,
                static_cast<uint32_t>((v * 0.5f + 0.5f) * desc.height));
            const uint64_t offset = face * faceStride +
                (static_cast<uint64_t>(y) * desc.width + x) * 8u;
            if (offset + 8u > product.radiance.size()) return {};
            std::array<float, 3> color{};
            for (size_t channel = 0; channel < 3; ++channel) {
                uint16_t half = 0;
                std::memcpy(&half, product.radiance.data() + offset +
                    channel * sizeof(uint16_t), sizeof(half));
                const float linear = std::max(0.0f, halfToFloat(half));
                color[channel] = std::pow(linear / (1.0f + linear), 1.0f / 2.2f);
            }
            return color;
        }

    } // namespace

    AssetThumbnailPixels makeAssetThumbnail(
        const CookedModelProductData& product,
        const AssetCatalogRecord& record,
        uint32_t extent) {
        if (record.guid.isNil() ||
            extent < 16 || extent > 512) {
            return {
                .assetGuid = record.guid,
                .diagnostic =
                    "Thumbnail request has invalid identity or extent.",
            };
        }
        if (record.assetType == "iridium.model" ||
            record.assetType ==
                "iridium.model-primitive") {
            return geometryThumbnail(
                product, record, extent);
        }
        if (record.assetType ==
            "iridium.material") {
            const CookedModelMaterial* material =
                findMaterial(product, record.guid);
            if (!material) {
                return {
                    .assetGuid = record.guid,
                    .diagnostic =
                        "Cooked material thumbnail identity is missing.",
                };
            }
            return materialThumbnail(
                *material, extent);
        }
        if (record.assetType ==
            "iridium.texture") {
            const auto view =
                std::ranges::find_if(
                    product.textureViews,
                    [&record](
                        const CookedModelTextureView&
                            candidate) {
                        return candidate.textureGuid ==
                            record.guid;
                    });
            if (view ==
                product.textureViews.end()) {
                return {
                    .assetGuid = record.guid,
                    .diagnostic =
                        "Cooked texture thumbnail identity is missing.",
                };
            }
            return textureThumbnail(
                view->textureGuid,
                view->manifest,
                view->payload,
                extent);
        }
        return {
            .assetGuid = record.guid,
            .diagnostic =
                "This asset type has no thumbnail producer.",
        };
    }

    AssetThumbnailPixels
        makeCookedTextureThumbnail(
            AssetGuid assetGuid,
            const CookedTextureManifest& manifest,
            std::span<const std::byte> payload,
            uint32_t extent) {
        if (assetGuid.isNil() ||
            extent < 16 || extent > 512) {
            return {
                .assetGuid = assetGuid,
                .diagnostic =
                    "Thumbnail request has invalid identity or extent.",
            };
        }
        return textureThumbnail(
            assetGuid, manifest,
            payload, extent);
    }

    AssetThumbnailPixels makeCookedEnvironmentThumbnail(
        AssetGuid assetGuid,
        const CookedEnvironmentProductData& product,
        uint32_t extent) {
        if (assetGuid.isNil() || extent < 16 || extent > 512 ||
            product.manifest.radiance.format != TextureFormat::RGBA16_SFloat ||
            product.manifest.radiance.arrayLayers != 6 ||
            product.radiance.empty()) {
            return { .assetGuid = assetGuid,
                .diagnostic = "Environment thumbnail input is invalid." };
        }
        AssetThumbnailPixels result = baseImage(assetGuid, extent);
        for (uint32_t y = 0; y < extent; ++y) {
            const float latitude = (static_cast<float>(y) + 0.5f) /
                extent * std::numbers::pi_v<float> -
                std::numbers::pi_v<float> * 0.5f;
            for (uint32_t x = 0; x < extent; ++x) {
                const float longitude = (static_cast<float>(x) + 0.5f) /
                    extent * std::numbers::pi_v<float> * 2.0f -
                    std::numbers::pi_v<float>;
                const Float3 direction{
                    std::cos(latitude) * std::cos(longitude),
                    std::sin(latitude),
                    std::cos(latitude) * std::sin(longitude),
                };
                const auto color = sampleEnvironmentPreview(product, direction);
                setPixel(result, x, y,
                    { color[0], color[1], color[2], 1.0f });
            }
        }
        return result;
    }

} // namespace Iridium
