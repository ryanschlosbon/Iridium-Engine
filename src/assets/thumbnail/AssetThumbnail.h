#pragma once

#include "assets/AssetCatalog.h"
#include "assets/model/ModelProduct.h"
#include "assets/environment/EnvironmentProduct.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Iridium {

    inline constexpr uint32_t kAssetThumbnailExtent = 96;
    inline constexpr uint32_t kAssetDetailThumbnailExtent = 256;

    enum class AssetThumbnailPurpose : uint8_t {
        Browser,
        Detail,
    };

    struct AssetThumbnailPixels {
        AssetGuid assetGuid;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<std::byte> rgba8;
        std::string diagnostic;
        AssetThumbnailPurpose purpose =
            AssetThumbnailPurpose::Browser;

        [[nodiscard]] bool valid() const noexcept {
            return !assetGuid.isNil() &&
                width != 0 && height != 0 &&
                rgba8.size() ==
                    static_cast<size_t>(width) *
                    height * 4 &&
                diagnostic.empty();
        }
    };

    // Produces a renderer-independent preview from the validated cooked CPU
    // product. Model/primitive previews rasterize real cooked triangles,
    // material previews use the compiled closure, and texture previews decode
    // the exact cooked texture view.
    [[nodiscard]] AssetThumbnailPixels makeAssetThumbnail(
        const CookedModelProductData& product,
        const AssetCatalogRecord& record,
        uint32_t extent = kAssetThumbnailExtent);
    [[nodiscard]] AssetThumbnailPixels
        makeCookedTextureThumbnail(
            AssetGuid assetGuid,
            const CookedTextureManifest& manifest,
            std::span<const std::byte> payload,
            uint32_t extent =
                kAssetThumbnailExtent);
    [[nodiscard]] AssetThumbnailPixels
        makeCookedEnvironmentThumbnail(
            AssetGuid assetGuid,
            const CookedEnvironmentProductData& product,
            uint32_t extent = kAssetThumbnailExtent);

} // namespace Iridium
