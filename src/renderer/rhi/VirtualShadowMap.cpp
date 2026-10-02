#include "renderer/rhi/VirtualShadowMap.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

#include <glm/vec4.hpp>
#include <glm/glm.hpp>

namespace Iridium {
    PackedVirtualShadowResidencyRequest packVirtualShadowResidencyRequest(const VirtualShadowPageRequest& request) {
        if (!request.receiverSamples || !request.requiredLayers || (request.requiredLayers & ~VirtualShadowPageLayerAll))
            throw std::invalid_argument("Residency input requires unique nonempty valid-layer requests");
        const auto split = [](uint64_t x) { return std::array<uint32_t, 2>{uint32_t(x), uint32_t(x >> 32)}; };
        return {packVirtualShadowPageKey(request.address), split(request.staticCasterRevision), split(request.dynamicCasterRevision),
            request.receiverSamples, request.priority, request.requiredLayers, 0};
    }

    VirtualShadowPageMapping unpackVirtualShadowPageMapping(const PackedVirtualShadowPageMapping& mapping, uint32_t capacity) {
        if (!capacity || mapping.reserved0 || mapping.reserved1 || mapping.state > 2 || mapping.missingAction > 1 ||
            !mapping.receiverSamples || !mapping.requiredLayers || mapping.requiredLayers > VirtualShadowPageLayerAll ||
            (mapping.updateLayers & ~mapping.requiredLayers) ||
            (mapping.physicalPage != InvalidVirtualShadowPhysicalPage && mapping.physicalPage >= capacity) ||
            (mapping.state != uint32_t(VirtualShadowPageState::Missing) && mapping.physicalPage == InvalidVirtualShadowPhysicalPage) ||
            (mapping.state == uint32_t(VirtualShadowPageState::CachedSampleable) && mapping.updateLayers) ||
            (mapping.state == uint32_t(VirtualShadowPageState::PendingRaster) && !mapping.updateLayers))
            throw std::invalid_argument("Invalid GPU virtual-shadow mapping");
        const auto join = [](const std::array<uint32_t, 2>& x) { return uint64_t(x[0]) | (uint64_t(x[1]) << 32); };
        return {.address = unpackVirtualShadowPageKey(mapping.key),
            .staticCasterRevision = join(mapping.staticRevisionWords), .dynamicCasterRevision = join(mapping.dynamicRevisionWords),
            .physicalPage = mapping.physicalPage, .receiverSamples = mapping.receiverSamples,
            .state = VirtualShadowPageState(mapping.state), .missingAction = VirtualShadowMissingPageAction(mapping.missingAction),
            .requiredLayers = uint8_t(mapping.requiredLayers), .updateLayers = uint8_t(mapping.updateLayers)};
    }

    PackedVirtualShadowPageKey packVirtualShadowPageKey(const VirtualShadowPageAddress& address) {
        if (!address.lightOwner.isSupported() || !address.projectionRevision ||
            uint8_t(address.projection) > uint8_t(VirtualShadowProjection::PointFace) ||
            (address.projection == VirtualShadowProjection::PointFace && address.levelOrFace >= 6) ||
            (address.projection == VirtualShadowProjection::Spot && address.levelOrFace != 0))
            throw std::invalid_argument("Invalid virtual-shadow residency identity");
        PackedVirtualShadowPageKey packed{};
        const auto& bytes = address.lightOwner.bytes();
        for (size_t i = 0; i < bytes.size(); ++i)
            packed.ownerWords[i / 4] |= uint32_t{bytes[i]} << ((i % 4) * 8);
        const auto x = std::bit_cast<uint64_t>(address.pageX), y = std::bit_cast<uint64_t>(address.pageY);
        packed.signedPageWords = {uint32_t(x), uint32_t(x >> 32), uint32_t(y), uint32_t(y >> 32)};
        packed.projectionRevisionWords = {uint32_t(address.projectionRevision), uint32_t(address.projectionRevision >> 32)};
        packed.projectionDescriptor = uint32_t(address.mip) | (uint32_t(address.levelOrFace) << 16) |
            (uint32_t(address.projection) << 24);
        packed.abiVersion = VirtualShadowResidencyAbiVersion;
        return packed;
    }

    VirtualShadowPageAddress unpackVirtualShadowPageKey(const PackedVirtualShadowPageKey& key) {
        if (key.abiVersion != VirtualShadowResidencyAbiVersion)
            throw std::invalid_argument("Virtual-shadow residency key ABI mismatch or unused entry");
        SceneEntityUuid::Bytes bytes{};
        for (size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = uint8_t(key.ownerWords[i / 4] >> ((i % 4) * 8));
        const auto join = [](uint32_t low, uint32_t high) { return uint64_t(low) | (uint64_t(high) << 32); };
        VirtualShadowPageAddress address{
            .lightOwner = SceneEntityUuid(bytes),
            .projectionRevision = join(key.projectionRevisionWords[0], key.projectionRevisionWords[1]),
            .pageX = std::bit_cast<int64_t>(join(key.signedPageWords[0], key.signedPageWords[1])),
            .pageY = std::bit_cast<int64_t>(join(key.signedPageWords[2], key.signedPageWords[3])),
            .mip = uint16_t(key.projectionDescriptor),
            .levelOrFace = uint8_t(key.projectionDescriptor >> 16),
            .projection = VirtualShadowProjection(key.projectionDescriptor >> 24)};
        (void)packVirtualShadowPageKey(address);
        return address;
    }

    PackedVirtualShadowResidentPage packVirtualShadowResidentPage(const VirtualShadowResidentPage& page,
        uint32_t capacity) {
        if (!capacity || page.physicalPage >= capacity || (page.validLayers & ~VirtualShadowPageLayerAll))
            throw std::invalid_argument("Invalid virtual-shadow physical residency record");
        const auto split = [](uint64_t value) { return std::array<uint32_t, 2>{uint32_t(value), uint32_t(value >> 32)}; };
        return {packVirtualShadowPageKey(page.address), split(page.staticCasterRevision), split(page.dynamicCasterRevision),
            split(page.lastUsedFrame), split(page.lastRenderedFrame), page.physicalPage, page.lastPriority, page.validLayers, 0};
    }

    VirtualShadowResidentPage unpackVirtualShadowResidentPage(const PackedVirtualShadowResidentPage& page,
        uint32_t capacity) {
        if (page.reserved || page.validLayers > VirtualShadowPageLayerAll)
            throw std::invalid_argument("Invalid virtual-shadow residency record flags");
        const auto join = [](const std::array<uint32_t, 2>& words) { return uint64_t(words[0]) | (uint64_t(words[1]) << 32); };
        VirtualShadowResidentPage result{
            .address = unpackVirtualShadowPageKey(page.key),
            .staticCasterRevision = join(page.staticRevisionWords), .dynamicCasterRevision = join(page.dynamicRevisionWords),
            .lastUsedFrame = join(page.lastUsedFrameWords), .lastRenderedFrame = join(page.lastRenderedFrameWords),
            .physicalPage = page.physicalPage, .lastPriority = page.lastPriority, .validLayers = uint8_t(page.validLayers)};
        (void)packVirtualShadowResidentPage(result, capacity);
        return result;
    }

    VirtualShadowPageRequest unpackDirectionalVirtualShadowGpuRequest(const PackedDirectionalVirtualShadowGpuRequest& request,
        std::span<const DirectionalVirtualShadowClipLevel> clips, uint32_t pageSize) {
        if (request.selectedLevelIndex >= clips.size() || !pageSize || !std::has_single_bit(pageSize))
            throw std::invalid_argument("GPU virtual-shadow request selects an unpublished clip or invalid page size");
        const auto& clip = clips[request.selectedLevelIndex];
        (void)packDirectionalVirtualShadowClipLevel(clip);
        const uint32_t axis = clip.virtualResolutionTexels / pageSize;
        if (!axis || clip.virtualResolutionTexels % pageSize || !request.receiverSamples ||
            request.level != clip.level || request.mip != clip.mip || request.priority != clip.priority ||
            request.requiredLayers != clip.requiredLayers || request.pageX < clip.worldPageOriginX ||
            request.pageY < clip.worldPageOriginY ||
            uint64_t(request.pageX) - uint64_t(clip.worldPageOriginX) >= axis ||
            uint64_t(request.pageY) - uint64_t(clip.worldPageOriginY) >= axis)
            throw std::invalid_argument("GPU virtual-shadow request disagrees with its owning clip snapshot");
        VirtualShadowPageRequest result{
            .address = {.lightOwner = clip.lightOwner, .projectionRevision = clip.projectionRevision,
                .pageX = request.pageX, .pageY = request.pageY, .mip = clip.mip, .levelOrFace = clip.level},
            .staticCasterRevision = clip.staticCasterRevision, .dynamicCasterRevision = clip.dynamicCasterRevision,
            .receiverSamples = request.receiverSamples, .priority = request.priority, .requiredLayers = clip.requiredLayers};
        (void)packVirtualShadowPageKey(result.address);
        return result;
    }

    VirtualShadowAtlasLayout buildVirtualShadowAtlasLayout(
        uint32_t pageSize, uint32_t border, uint32_t capacity, uint32_t maximumDimension) {
        if (pageSize < 32 || pageSize > 512 || !std::has_single_bit(pageSize) ||
            !border || border >= pageSize / 2 || !capacity || !maximumDimension)
            throw std::invalid_argument("Invalid virtual-shadow atlas dimensions");
        uint32_t axis = 1;
        while (uint64_t{axis} * axis < capacity) ++axis;
        const uint64_t footprint = uint64_t{pageSize} + 2u * border;
        const uint64_t dimension = axis * footprint;
        if (dimension > maximumDimension)
            throw std::invalid_argument("Virtual-shadow atlas exceeds the dimension limit");
        return {static_cast<uint32_t>(dimension), static_cast<uint32_t>(dimension),
            axis, static_cast<uint32_t>(footprint), capacity, pageSize, border};
    }

    VirtualShadowPageRasterRegion buildDirectionalVirtualShadowPageRasterRegion(
        const VirtualShadowAtlasLayout& atlas,
        const DirectionalVirtualShadowClipLevel& clip,
        const VirtualShadowPageMapping& mapping) {
        const auto checked = buildVirtualShadowAtlasLayout(atlas.pageSizeTexels,
            atlas.borderTexels, atlas.physicalPageCapacity, std::numeric_limits<uint32_t>::max());
        if (atlas.widthTexels != checked.widthTexels || atlas.heightTexels != checked.heightTexels ||
            atlas.tilesPerRow != checked.tilesPerRow || atlas.tileFootprintTexels != checked.tileFootprintTexels)
            throw std::invalid_argument("Incoherent virtual-shadow atlas layout");
        (void)packDirectionalVirtualShadowClipLevel(clip);
        const auto& address = mapping.address;
        if (mapping.state != VirtualShadowPageState::PendingRaster ||
            mapping.physicalPage >= atlas.physicalPageCapacity ||
            !mapping.updateLayers || (mapping.updateLayers & ~mapping.requiredLayers) ||
            (mapping.requiredLayers & ~clip.requiredLayers) ||
            address.lightOwner != clip.lightOwner || address.projectionRevision != clip.projectionRevision ||
            address.projection != VirtualShadowProjection::DirectionalClip ||
            address.levelOrFace != clip.level || address.mip != clip.mip ||
            ((mapping.requiredLayers & VirtualShadowPageLayerStatic) &&
                mapping.staticCasterRevision != clip.staticCasterRevision) ||
            ((mapping.requiredLayers & VirtualShadowPageLayerDynamic) &&
                mapping.dynamicCasterRevision != clip.dynamicCasterRevision) ||
            clip.virtualResolutionTexels % atlas.pageSizeTexels)
            throw std::invalid_argument("Page raster mapping is not current pending directional work");
        const uint32_t axis = clip.virtualResolutionTexels / atlas.pageSizeTexels;
        // Unsigned difference is defined even across the signed origin boundary.
        const uint64_t x = uint64_t(address.pageX) - uint64_t(clip.worldPageOriginX);
        const uint64_t y = uint64_t(address.pageY) - uint64_t(clip.worldPageOriginY);
        if (!axis || address.pageX < clip.worldPageOriginX || address.pageY < clip.worldPageOriginY ||
            x >= axis || y >= axis)
            throw std::invalid_argument("Page raster mapping is outside its clip window");
        const float scale = float(clip.virtualResolutionTexels) / atlas.tileFootprintTexels;
        const glm::vec2 center = (glm::vec2(float(x), float(y)) + 0.5f) * (2.0f / axis) - 1.0f;
        glm::mat4 crop{1.0f};
        crop[0][0] = crop[1][1] = scale;
        crop[3][0] = -center.x * scale; crop[3][1] = -center.y * scale;
        VirtualShadowPageRasterRegion region{};
        region.worldToPageClip = crop * clip.worldToShadowClip;
        for (uint32_t c = 0; c < 4; ++c) for (uint32_t r = 0; r < 4; ++r)
            if (!std::isfinite(region.worldToPageClip[c][r]))
                throw std::invalid_argument("Page raster crop exceeds finite projection range");
        region.physicalPage = mapping.physicalPage;
        region.updateLayers = mapping.updateLayers;
        region.extentTexels = atlas.tileFootprintTexels;
        region.originXTexels = (mapping.physicalPage % atlas.tilesPerRow) * atlas.tileFootprintTexels;
        region.originYTexels = (mapping.physicalPage / atlas.tilesPerRow) * atlas.tileFootprintTexels;
        region.pageUvToAtlasUv = {
            float(atlas.pageSizeTexels) / atlas.widthTexels, float(atlas.pageSizeTexels) / atlas.heightTexels,
            float(region.originXTexels + atlas.borderTexels) / atlas.widthTexels,
            float(region.originYTexels + atlas.borderTexels) / atlas.heightTexels};
        return region;
    }

    bool virtualShadowPageMayContainCaster(const VirtualShadowPageRasterRegion& region,
        const VirtualShadowCasterBounds& caster) noexcept {
        if (!std::isfinite(caster.radiusWorld) || caster.radiusWorld < 0 ||
            !std::isfinite(caster.centerWorld.x) || !std::isfinite(caster.centerWorld.y) ||
            !std::isfinite(caster.centerWorld.z)) return true;
        const auto& matrix = region.worldToPageClip;
        glm::vec4 rows[4];
        for (uint32_t r = 0; r < 4; ++r)
            rows[r] = {matrix[0][r], matrix[1][r], matrix[2][r], matrix[3][r]};
        const std::array planes{rows[3] + rows[0], rows[3] - rows[0],
            rows[3] + rows[1], rows[3] - rows[1], rows[2], rows[3] - rows[2]};
        // Validate every plane before rejecting, so an unsafe matrix fails visible.
        std::array<float, 6> distances{}, radii{};
        for (size_t i = 0; i < planes.size(); ++i) {
            const auto& plane = planes[i];
            const float length = glm::length(glm::vec3(plane));
            if (!std::isfinite(plane.x) || !std::isfinite(plane.y) ||
                !std::isfinite(plane.z) || !std::isfinite(plane.w) ||
                !std::isfinite(length) || !(length > 0)) return true;
            distances[i] = glm::dot(glm::vec3(plane), caster.centerWorld) + plane.w;
            radii[i] = caster.radiusWorld * length;
            if (!std::isfinite(distances[i]) || !std::isfinite(radii[i])) return true;
        }
        for (size_t i = 0; i < planes.size(); ++i) {
            const float distance = distances[i], radius = radii[i];
            const float tolerance = 1e-5f * (1.0f + std::abs(distance) + radius);
            if (distance < -radius - tolerance) return false;
        }
        return true;
    }

namespace {

    [[nodiscard]] bool validLayers(uint8_t layers) noexcept {
        return layers != VirtualShadowPageLayerNone &&
            (layers & ~VirtualShadowPageLayerAll) == 0u;
    }

    [[nodiscard]] VirtualShadowMissingPageAction missingAction(
        const VirtualShadowPagePolicy& policy) noexcept {
        return policy.deterministicConventionalFallback
            ? VirtualShadowMissingPageAction::ConventionalShadowFallback
            : VirtualShadowMissingPageAction::FullyLitFailVisible;
    }

    [[nodiscard]] uint8_t requiredUpdates(
        const VirtualShadowPagePolicy& policy,
        const VirtualShadowPageRequest& request,
        const VirtualShadowResidentPage& resident) noexcept {
        uint8_t result = VirtualShadowPageLayerNone;
        if ((request.requiredLayers & VirtualShadowPageLayerStatic) != 0u &&
            (!policy.cacheStaticCasters ||
                (resident.validLayers & VirtualShadowPageLayerStatic) == 0u ||
                resident.staticCasterRevision !=
                    request.staticCasterRevision)) {
            result |= VirtualShadowPageLayerStatic;
        }
        if ((request.requiredLayers & VirtualShadowPageLayerDynamic) != 0u &&
            ((resident.validLayers & VirtualShadowPageLayerDynamic) == 0u ||
                resident.dynamicCasterRevision !=
                    request.dynamicCasterRevision)) {
            result |= VirtualShadowPageLayerDynamic;
        }
        return result;
    }

    [[nodiscard]] bool finiteMatrix(const glm::mat4& matrix) noexcept {
        for (uint32_t column = 0; column < 4; ++column)
            for (uint32_t row = 0; row < 4; ++row)
                if (!std::isfinite(matrix[column][row])) return false;
        return true;
    }

    [[nodiscard]] uint64_t saturatingAdd(
        uint64_t left, uint64_t right) noexcept {
        return right > std::numeric_limits<uint64_t>::max() - left
            ? std::numeric_limits<uint64_t>::max()
            : left + right;
    }

} // namespace

    PackedDirectionalVirtualShadowClipLevel packDirectionalVirtualShadowClipLevel(
        const DirectionalVirtualShadowClipLevel& source) {
        if (!finiteMatrix(source.worldToShadowClip) || !validLayers(source.requiredLayers) ||
            !source.virtualResolutionTexels || source.virtualResolutionTexels > INT32_MAX ||
            source.worldPageOriginX < INT32_MIN || source.worldPageOriginX > INT32_MAX ||
            source.worldPageOriginY < INT32_MIN || source.worldPageOriginY > INT32_MAX)
            throw std::invalid_argument("Virtual-shadow clip cannot be packed for the GPU ABI");
        return {source.worldToShadowClip, static_cast<int32_t>(source.worldPageOriginX),
            static_cast<int32_t>(source.worldPageOriginY), source.virtualResolutionTexels,
            uint32_t{source.level} | (uint32_t{source.mip} << 8u), source.priority,
            source.requiredLayers, 0, 0};
    }

    std::optional<DirectionalVirtualShadowClipPlan> DirectionalVirtualShadowClipPublisher::publish(
        DirectionalVirtualShadowClipConfig config,
        std::span<const VirtualShadowCasterBounds> casters,
        double depthQuantum, double padding) {
        if (!std::isfinite(depthQuantum) || depthQuantum <= 0 ||
            !std::isfinite(padding) || padding < 0)
            throw std::invalid_argument("Invalid virtual-shadow depth publication policy");
        // Validate the supplied projection shape before using its density or basis.
        config.projectionRevision = 1;
        (void)buildDirectionalVirtualShadowClips(config);
        const glm::dvec3 forward = glm::normalize(glm::dvec3(config.lightForward));
        config.lightForward = glm::vec3(forward);
        const double focusDepth = glm::dot(forward, glm::dvec3(config.focusWorld));
        const double receiverExtent = std::ldexp(config.finestWorldSpan, int(config.levelCount - 1));
        double minimum = focusDepth - receiverExtent;
        double maximum = focusDepth + receiverExtent;
        for (const auto& caster : casters) {
            if (!std::isfinite(caster.radiusWorld) || caster.radiusWorld < 0 ||
                !std::isfinite(caster.centerWorld.x) || !std::isfinite(caster.centerWorld.y) ||
                !std::isfinite(caster.centerWorld.z)) return std::nullopt;
            const double center = glm::dot(forward, glm::dvec3(caster.centerWorld));
            minimum = (std::min)(minimum, center - caster.radiusWorld);
            maximum = (std::max)(maximum, center + caster.radiusWorld);
        }
        minimum = std::floor((minimum - padding) / depthQuantum) * depthQuantum;
        maximum = std::ceil((maximum + padding) / depthQuantum) * depthQuantum;
        const bool shapeChanged = !initialized_ || config.lightOwner != previous_.lightOwner ||
            config.lightForward != previous_.lightForward ||
            config.finestWorldSpan != previous_.finestWorldSpan ||
            config.levelCount != previous_.levelCount ||
            config.virtualResolutionTexels != previous_.virtualResolutionTexels ||
            config.pageSizeTexels != previous_.pageSizeTexels ||
            config.requiredLayers != previous_.requiredLayers;
        if (!shapeChanged) {
            minimum = (std::min)(minimum, previous_.lightDepthMinimum);
            maximum = (std::max)(maximum, previous_.lightDepthMaximum);
        }
        const bool changed = shapeChanged || minimum != previous_.lightDepthMinimum ||
            maximum != previous_.lightDepthMaximum;
        if (changed && revision_ == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("Virtual-shadow projection revision exhausted");
        config.projectionRevision = revision_ + (changed ? 1u : 0u);
        config.lightDepthMinimum = minimum; config.lightDepthMaximum = maximum;
        // Commit only after complete validation/build; failures retain prior state.
        auto plan = buildDirectionalVirtualShadowClips(config);
        previous_ = config; revision_ = config.projectionRevision; initialized_ = true;
        return plan;
    }

    DirectionalVirtualShadowClipPlan buildDirectionalVirtualShadowClips(
        const DirectionalVirtualShadowClipConfig& config) {
        const auto finiteVector = [](glm::vec3 value) {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        };
        if (!config.lightOwner.isSupported() || !config.projectionRevision ||
            !finiteVector(config.focusWorld) || !finiteVector(config.lightForward) ||
            !std::isfinite(config.finestWorldSpan) || config.finestWorldSpan <= 0 ||
            !std::isfinite(config.lightDepthMinimum) || !std::isfinite(config.lightDepthMaximum) ||
            config.lightDepthMaximum <= config.lightDepthMinimum ||
            config.levelCount == 0 || config.levelCount > 16 ||
            !std::has_single_bit(config.pageSizeTexels) ||
            config.virtualResolutionTexels < config.pageSizeTexels ||
            config.virtualResolutionTexels % config.pageSizeTexels ||
            !validLayers(config.requiredLayers))
            throw std::invalid_argument("Invalid directional virtual-shadow clip policy");
        const glm::dvec3 direction(config.lightForward);
        const double directionLength = glm::length(direction);
        if (directionLength <= 1e-12)
            throw std::invalid_argument("Directional virtual shadows require a nonzero light direction");
        const glm::dvec3 forward = direction / directionLength;
        const glm::dvec3 upHint = std::abs(forward.y) < 0.99 ?
            glm::dvec3(0, 1, 0) : glm::dvec3(0, 0, 1);
        const glm::dvec3 right = glm::normalize(glm::cross(upHint, forward));
        const glm::dvec3 up = glm::cross(forward, right);
        const uint32_t axis = config.virtualResolutionTexels / config.pageSizeTexels;
        // Even page grids center symmetrically on the snapped focus page.
        if (axis < 2 || axis % 2)
            throw std::invalid_argument("Directional clip windows require an even page axis >= 2");
        const double depthRange = config.lightDepthMaximum - config.lightDepthMinimum;
        DirectionalVirtualShadowClipPlan plan{};
        plan.levelCount = config.levelCount;
        for (uint32_t i = 0; i < config.levelCount; ++i) {
            const double span = std::ldexp(config.finestWorldSpan, static_cast<int>(i));
            const double pageSpan = span / axis;
            const double originX = std::floor(glm::dot(right, glm::dvec3(config.focusWorld)) / pageSpan) - axis / 2;
            const double originY = std::floor(glm::dot(up, glm::dvec3(config.focusWorld)) / pageSpan) - axis / 2;
            if (!std::isfinite(span) || !std::isfinite(pageSpan) || pageSpan <= 0 ||
                !std::isfinite(originX) || !std::isfinite(originY) ||
                originX < INT32_MIN || originY < INT32_MIN ||
                originX > double(INT32_MAX) - (axis - 1) ||
                originY > double(INT32_MAX) - (axis - 1))
                throw std::invalid_argument("Directional clip window exceeds signed GPU page range");
            glm::dmat4 matrix(1.0);
            for (uint32_t c = 0; c < 3; ++c) {
                matrix[c][0] = right[c] * 2.0 / span;
                matrix[c][1] = up[c] * 2.0 / span;
                matrix[c][2] = forward[c] / depthRange;
            }
            matrix[3][0] = -1.0 - originX * 2.0 / axis;
            matrix[3][1] = -1.0 - originY * 2.0 / axis;
            matrix[3][2] = -config.lightDepthMinimum / depthRange;
            auto& clip = plan.clips[i];
            clip.lightOwner = config.lightOwner;
            clip.projectionRevision = config.projectionRevision;
            clip.staticCasterRevision = config.staticCasterRevision;
            clip.dynamicCasterRevision = config.dynamicCasterRevision;
            clip.worldToShadowClip = glm::mat4(matrix);
            const auto nonzeroRow = [&](uint32_t row) {
                return clip.worldToShadowClip[0][row] != 0.0f ||
                    clip.worldToShadowClip[1][row] != 0.0f ||
                    clip.worldToShadowClip[2][row] != 0.0f;
            };
            if (!finiteMatrix(clip.worldToShadowClip) || !nonzeroRow(0) ||
                !nonzeroRow(1) || !nonzeroRow(2))
                throw std::invalid_argument("Directional clip projection cannot be represented by GPU floats");
            clip.worldPageOriginX = static_cast<int64_t>(originX);
            clip.worldPageOriginY = static_cast<int64_t>(originY);
            clip.virtualResolutionTexels = config.virtualResolutionTexels;
            clip.level = static_cast<uint8_t>(i);
            clip.priority = static_cast<int32_t>(config.levelCount - i);
            clip.requiredLayers = config.requiredLayers;
            plan.worldUnitsPerTexel[i] = span / config.virtualResolutionTexels;
            plan.worldUnitsPerPage[i] = pageSpan;
        }
        return plan;
    }

    std::string validateVirtualShadowResourceConfig(
        const VirtualShadowResourceConfig& config) {
        if (config.pageSizeTexels < 64u || config.pageSizeTexels > 256u ||
            !std::has_single_bit(config.pageSizeTexels))
            return "virtual shadow page size must be a power of two in [64, 256]";
        if (config.borderTexels == 0u ||
            config.borderTexels > config.pageSizeTexels / 4u)
            return "virtual shadow page border must be nonzero and no more than one quarter of the page";
        if (config.physicalPageCapacity == 0u)
            return "virtual shadow physical page capacity must be nonzero";
        if (config.pageTableEntryCapacity < config.physicalPageCapacity)
            return "virtual shadow page table cannot be smaller than the physical pool";
        if (config.receiverMarkCapacity < 256u ||
            config.receiverMarkCapacity > 65'536u ||
            !std::has_single_bit(config.receiverMarkCapacity))
            return "virtual shadow receiver-mark capacity must be a power of two in [256, 65536]";
        if (config.compactedRequestCapacity == 0u ||
            config.compactedRequestCapacity > config.receiverMarkCapacity)
            return "virtual shadow compacted-request capacity must be nonzero and no larger than receiver-mark capacity";
        return {};
    }

    VirtualShadowGpuWorkingSetLayout buildVirtualShadowGpuWorkingSetLayout(
        const VirtualShadowResourceConfig& config,
        uint64_t storageBufferOffsetAlignment) {
        if (const std::string error =
                validateVirtualShadowResourceConfig(config); !error.empty())
            throw std::invalid_argument(error);
        if (storageBufferOffsetAlignment == 0u ||
            !std::has_single_bit(storageBufferOffsetAlignment))
            throw std::invalid_argument(
                "storage-buffer alignment must be a nonzero power of two");

        VirtualShadowGpuWorkingSetLayout layout{};
        layout.scratchCapacity = config.receiverMarkCapacity;
        uint64_t cursor = 0;
        const auto append = [&](uint64_t size,
            VirtualShadowGpuBufferRange& range) {
            if (cursor > std::numeric_limits<uint64_t>::max() -
                    (storageBufferOffsetAlignment - 1u))
                throw std::overflow_error(
                    "virtual shadow GPU working set alignment overflow");
            cursor = (cursor + storageBufferOffsetAlignment - 1u) &
                ~(storageBufferOffsetAlignment - 1u);
            range = { .offset = cursor, .size = size };
            if (size > std::numeric_limits<uint64_t>::max() - cursor)
                throw std::overflow_error(
                    "virtual shadow GPU working set size overflow");
            cursor += size;
        };
        append(16u * sizeof(PackedDirectionalVirtualShadowClipLevel),
            layout.clipLevels);
        append(uint64_t{ config.receiverMarkCapacity } *
                sizeof(PackedDirectionalVirtualShadowReceiver),
            layout.receivers);
        append(uint64_t{ config.receiverMarkCapacity } *
                sizeof(PackedDirectionalVirtualShadowGpuMark),
            layout.rawMarks);
        append(uint64_t{ layout.scratchCapacity } *
                sizeof(PackedDirectionalVirtualShadowGpuRequest),
            layout.alternateRequests);
        append(uint64_t{ layout.scratchCapacity } *
                sizeof(PackedDirectionalVirtualShadowGpuRequest),
            layout.denseRequests);
        append(uint64_t{ config.compactedRequestCapacity } *
                sizeof(PackedDirectionalVirtualShadowGpuRequest),
            layout.outputRequests);
        append(sizeof(PackedDirectionalVirtualShadowGpuCompactionTelemetry),
            layout.telemetry);
        layout.totalBytes = cursor;
        return layout;
    }

    uint32_t validateVirtualShadowDepthReceiverRegion(
        const VirtualShadowDepthReceiverRegion& region, uint32_t capacity) {
        if (!region.sourceWidth || !region.sourceHeight || !region.width || !region.height ||
            region.originX >= region.sourceWidth || region.originY >= region.sourceHeight ||
            region.width > region.sourceWidth - region.originX ||
            region.height > region.sourceHeight - region.originY ||
            uint64_t{region.width} * region.height > capacity)
            throw std::invalid_argument("Virtual shadow depth region exceeds source or receiver capacity");
        return region.width * region.height;
    }

    std::vector<PackedDirectionalVirtualShadowReceiver> buildVirtualShadowDepthReceivers(
        const VirtualShadowDepthReceiverRegion& region,
        const glm::mat4& inverseViewProjection, std::span<const float> depth,
        uint32_t capacity) {
        const auto count = validateVirtualShadowDepthReceiverRegion(region, capacity);
        if (uint64_t{region.sourceWidth} * region.sourceHeight != depth.size() ||
            !finiteMatrix(inverseViewProjection))
            throw std::invalid_argument("Invalid virtual shadow depth source or inverse matrix");
        std::vector<PackedDirectionalVirtualShadowReceiver> output(count);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t x = region.originX + i % region.width;
            const uint32_t y = region.originY + i / region.width;
            const float z = depth[size_t{y} * region.sourceWidth + x];
            if (!std::isfinite(z) || z < 0 || z > 1 ||
                z == (region.reverseDepth ? 0.0f : 1.0f)) continue;
            const glm::vec4 homogeneous = inverseViewProjection * glm::vec4(
                (float(x) + 0.5f) / float(region.sourceWidth) * 2.0f - 1.0f,
                (float(y) + 0.5f) / float(region.sourceHeight) * 2.0f - 1.0f, z, 1.0f);
            if (!std::isfinite(homogeneous.w) || std::abs(homogeneous.w) <= 1e-7f) continue;
            const glm::vec3 world = glm::vec3(homogeneous) / homogeneous.w;
            if (!std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z)) continue;
            output[i].worldPosition = glm::vec4(world, 1.0f);
            output[i].receiverSamples = 1;
        }
        return output;
    }

    VirtualShadowFullViewPageGrid buildVirtualShadowFullViewPageGrid(
        const DirectionalVirtualShadowMarkConfig& config,
        std::span<const DirectionalVirtualShadowClipLevel> levels,
        uint32_t cellCapacity) {
        // Reuse the marking oracle's clip-stack coherence checks.
        (void)buildDirectionalVirtualShadowReceiverMarks(config, levels, {});
        VirtualShadowFullViewPageGrid grid{};
        for (const auto& level : levels) {
            const uint64_t axis = level.virtualResolutionTexels / config.pageSizeTexels;
            const uint64_t cells = axis * axis;
            if (cells > cellCapacity || grid.cellCount > cellCapacity - cells ||
                level.worldPageOriginX < std::numeric_limits<int32_t>::min() ||
                level.worldPageOriginY < std::numeric_limits<int32_t>::min() ||
                level.worldPageOriginX > int64_t{std::numeric_limits<int32_t>::max()} - static_cast<int64_t>(axis - 1) ||
                level.worldPageOriginY > int64_t{std::numeric_limits<int32_t>::max()} - static_cast<int64_t>(axis - 1))
                throw std::invalid_argument("Full-view clip page grid exceeds cell capacity or signed GPU address range");
            grid.levelOffsets.push_back(grid.cellCount);
            grid.cellCount += static_cast<uint32_t>(cells);
        }
        return grid;
    }

    std::string validateVirtualShadowPagePolicy(
        const VirtualShadowPagePolicy& policy) {
        if (policy.pageSizeTexels < 32u || policy.pageSizeTexels > 512u ||
            !std::has_single_bit(policy.pageSizeTexels))
            return "pageSizeTexels must be a power of two in [32, 512]";
        if (policy.borderTexels == 0u ||
            policy.borderTexels * 2u >= policy.pageSizeTexels)
            return "borderTexels must be nonzero and leave an interior page";
        if (policy.maximumPhysicalPages == 0u)
            return "maximumPhysicalPages must be nonzero";
        if (policy.maximumPageUpdatesPerFrame == 0u ||
            policy.maximumPageUpdatesPerFrame > policy.maximumPhysicalPages)
            return "maximumPageUpdatesPerFrame must be in the physical pool";
        return {};
    }

    DirectionalVirtualShadowMarkPlan
        buildDirectionalVirtualShadowReceiverMarks(
            const DirectionalVirtualShadowMarkConfig& config,
            std::span<const DirectionalVirtualShadowClipLevel> levels,
            std::span<const DirectionalVirtualShadowReceiverSample> receivers) {
        if (config.pageSizeTexels == 0u ||
            !std::has_single_bit(config.pageSizeTexels) ||
            config.maximumUniquePageRequests == 0u)
            throw std::invalid_argument(
                "invalid directional virtual-shadow marking config");
        if (levels.empty() || levels.size() > 16u)
            throw std::invalid_argument(
                "directional virtual shadows require 1..16 clip levels");

        std::set<uint8_t> levelIndices;
        const SceneEntityUuid owner = levels.front().lightOwner;
        const uint64_t projectionRevision =
            levels.front().projectionRevision;
        uint8_t coarsestLevel = 0;
        for (const DirectionalVirtualShadowClipLevel& level : levels) {
            if (!owner.isSupported() || level.lightOwner != owner ||
                projectionRevision == 0u ||
                level.projectionRevision != projectionRevision ||
                !levelIndices.insert(level.level).second ||
                !finiteMatrix(level.worldToShadowClip) ||
                level.virtualResolutionTexels < config.pageSizeTexels ||
                level.virtualResolutionTexels % config.pageSizeTexels != 0u ||
                !validLayers(level.requiredLayers))
                throw std::invalid_argument(
                    "invalid directional virtual-shadow clip level");
            coarsestLevel = (std::max)(coarsestLevel, level.level);
        }

        std::map<VirtualShadowPageAddress, VirtualShadowPageRequest> unique;
        DirectionalVirtualShadowMarkPlan plan{};
        for (const DirectionalVirtualShadowReceiverSample& receiver : receivers) {
            if (receiver.receiverSamples == 0u) continue;
            plan.inputReceiverSamples = saturatingAdd(
                plan.inputReceiverSamples, receiver.receiverSamples);
            if (!std::isfinite(receiver.worldPosition.x) ||
                !std::isfinite(receiver.worldPosition.y) ||
                !std::isfinite(receiver.worldPosition.z)) {
                ++plan.invalidReceivers;
                plan.unmappedReceiverSamples = saturatingAdd(
                    plan.unmappedReceiverSamples, receiver.receiverSamples);
                continue;
            }

            const DirectionalVirtualShadowClipLevel* selected = nullptr;
            glm::vec3 selectedNdc{ 0.0f };
            for (const DirectionalVirtualShadowClipLevel& level : levels) {
                const glm::vec4 clip = level.worldToShadowClip *
                    glm::vec4(receiver.worldPosition, 1.0f);
                if (!std::isfinite(clip.x) || !std::isfinite(clip.y) ||
                    !std::isfinite(clip.z) || !std::isfinite(clip.w) ||
                    std::abs(clip.w) <= 1e-7f)
                    continue;
                const glm::vec3 ndc = glm::vec3(clip) / clip.w;
                const float guard = level.level == coarsestLevel
                    ? 0.0f
                    : 2.0f * static_cast<float>(
                        uint64_t{ config.finerLevelGuardBandPages } *
                        config.pageSizeTexels) /
                        static_cast<float>(level.virtualResolutionTexels);
                if (ndc.x < -1.0f + guard || ndc.x > 1.0f - guard ||
                    ndc.y < -1.0f + guard || ndc.y > 1.0f - guard ||
                    ndc.z < 0.0f || ndc.z > 1.0f)
                    continue;
                if (selected == nullptr || level.level < selected->level) {
                    selected = &level;
                    selectedNdc = ndc;
                }
            }
            if (selected == nullptr) {
                plan.unmappedReceiverSamples = saturatingAdd(
                    plan.unmappedReceiverSamples, receiver.receiverSamples);
                continue;
            }

            const uint32_t pagesPerAxis =
                selected->virtualResolutionTexels / config.pageSizeTexels;
            const float unitX = (selectedNdc.x + 1.0f) * 0.5f;
            const float unitY = (selectedNdc.y + 1.0f) * 0.5f;
            const uint32_t localPageX = (std::min)(
                static_cast<uint32_t>(unitX * pagesPerAxis),
                pagesPerAxis - 1u);
            const uint32_t localPageY = (std::min)(
                static_cast<uint32_t>(unitY * pagesPerAxis),
                pagesPerAxis - 1u);
            const VirtualShadowPageAddress address{
                .lightOwner = selected->lightOwner,
                .projectionRevision = selected->projectionRevision,
                .pageX = selected->worldPageOriginX + localPageX,
                .pageY = selected->worldPageOriginY + localPageY,
                .mip = selected->mip,
                .levelOrFace = selected->level,
                .projection = VirtualShadowProjection::DirectionalClip,
            };
            auto [iterator, inserted] = unique.emplace(address,
                VirtualShadowPageRequest{
                    .address = address,
                    .staticCasterRevision = selected->staticCasterRevision,
                    .dynamicCasterRevision = selected->dynamicCasterRevision,
                    .receiverSamples = receiver.receiverSamples,
                    .priority = selected->priority,
                    .requiredLayers = selected->requiredLayers,
                });
            if (!inserted) {
                VirtualShadowPageRequest& request = iterator->second;
                request.receiverSamples = receiver.receiverSamples >
                        std::numeric_limits<uint32_t>::max() -
                            request.receiverSamples
                    ? std::numeric_limits<uint32_t>::max()
                    : request.receiverSamples + receiver.receiverSamples;
            }
            plan.markedReceiverSamples = saturatingAdd(
                plan.markedReceiverSamples, receiver.receiverSamples);
        }

        std::vector<VirtualShadowPageRequest> ordered;
        ordered.reserve(unique.size());
        for (const auto& [address, request] : unique)
            ordered.push_back(request);
        std::ranges::sort(ordered,
            [](const VirtualShadowPageRequest& left,
                const VirtualShadowPageRequest& right) {
                if (left.priority != right.priority)
                    return left.priority > right.priority;
                if (left.receiverSamples != right.receiverSamples)
                    return left.receiverSamples > right.receiverSamples;
                return left.address < right.address;
            });
        plan.uniquePagesBeforeCapacity = static_cast<uint32_t>(ordered.size());
        if (ordered.size() > config.maximumUniquePageRequests) {
            plan.requestCapacityOverflow = static_cast<uint32_t>(
                ordered.size() - config.maximumUniquePageRequests);
            for (size_t index = config.maximumUniquePageRequests;
                index < ordered.size(); ++index)
                plan.requestCapacityDroppedSamples = saturatingAdd(
                    plan.requestCapacityDroppedSamples,
                    ordered[index].receiverSamples);
            ordered.resize(config.maximumUniquePageRequests);
        }
        plan.requests = std::move(ordered);
        return plan;
    }

    VirtualShadowPagePlan buildVirtualShadowPagePlan(
        const VirtualShadowPagePolicy& policy,
        std::span<const VirtualShadowPageRequest> requests,
        std::span<const VirtualShadowResidentPage> currentResidency,
        uint64_t frameSerial) {
        if (const std::string error = validateVirtualShadowPagePolicy(policy);
            !error.empty())
            throw std::invalid_argument(error);

        std::map<VirtualShadowPageAddress, VirtualShadowPageRequest> unique;
        for (const VirtualShadowPageRequest& source : requests) {
            if (source.receiverSamples == 0u) continue;
            if (!source.address.lightOwner.isSupported() ||
                source.address.projectionRevision == 0u ||
                !validLayers(source.requiredLayers) ||
                (source.address.projection == VirtualShadowProjection::PointFace &&
                    source.address.levelOrFace >= 6u))
                throw std::invalid_argument("invalid virtual-shadow page request");
            auto [iterator, inserted] = unique.emplace(source.address, source);
            if (inserted) continue;
            VirtualShadowPageRequest& merged = iterator->second;
            if (merged.staticCasterRevision != source.staticCasterRevision ||
                merged.dynamicCasterRevision != source.dynamicCasterRevision)
                throw std::invalid_argument(
                    "conflicting revisions for one virtual-shadow page");
            merged.receiverSamples = source.receiverSamples >
                    std::numeric_limits<uint32_t>::max() - merged.receiverSamples
                ? std::numeric_limits<uint32_t>::max()
                : merged.receiverSamples + source.receiverSamples;
            merged.priority = (std::max)(merged.priority, source.priority);
            merged.requiredLayers |= source.requiredLayers;
        }

        std::vector<VirtualShadowPageRequest> ordered;
        ordered.reserve(unique.size());
        for (const auto& [address, request] : unique) ordered.push_back(request);
        std::ranges::sort(ordered,
            [](const VirtualShadowPageRequest& left,
                const VirtualShadowPageRequest& right) {
                if (left.priority != right.priority)
                    return left.priority > right.priority;
                if (left.receiverSamples != right.receiverSamples)
                    return left.receiverSamples > right.receiverSamples;
                return left.address < right.address;
            });

        VirtualShadowPagePlan plan{};
        plan.uniqueRequests = static_cast<uint32_t>(ordered.size());
        plan.nextResidency.assign(currentResidency.begin(),
            currentResidency.end());

        std::map<VirtualShadowPageAddress, size_t> residentByAddress;
        std::set<uint32_t> occupied;
        for (size_t index = 0; index < plan.nextResidency.size(); ++index) {
            const VirtualShadowResidentPage& resident =
                plan.nextResidency[index];
            if (resident.physicalPage >= policy.maximumPhysicalPages ||
                !resident.address.lightOwner.isSupported() ||
                resident.address.projectionRevision == 0u ||
                !residentByAddress.emplace(resident.address, index).second ||
                !occupied.insert(resident.physicalPage).second ||
                (resident.validLayers & ~VirtualShadowPageLayerAll) != 0u)
                throw std::invalid_argument(
                    "invalid virtual-shadow physical residency");
        }

        std::set<VirtualShadowPageAddress> requestedAddresses;
        for (const VirtualShadowPageRequest& request : ordered)
            requestedAddresses.insert(request.address);

        uint32_t updateSlots = policy.maximumPageUpdatesPerFrame;
        for (const VirtualShadowPageRequest& request : ordered) {
            VirtualShadowPageMapping mapping{
                .address = request.address,
                .staticCasterRevision = request.staticCasterRevision,
                .dynamicCasterRevision = request.dynamicCasterRevision,
                .receiverSamples = request.receiverSamples,
                .missingAction = missingAction(policy),
                .requiredLayers = request.requiredLayers,
            };

            auto residentIterator = residentByAddress.find(request.address);
            if (residentIterator != residentByAddress.end()) {
                VirtualShadowResidentPage& resident =
                    plan.nextResidency[residentIterator->second];
                resident.lastUsedFrame = frameSerial;
                resident.lastPriority = request.priority;
                mapping.physicalPage = resident.physicalPage;
                mapping.updateLayers = requiredUpdates(policy, request, resident);
                if (mapping.updateLayers == VirtualShadowPageLayerNone) {
                    mapping.state = VirtualShadowPageState::CachedSampleable;
                    ++plan.cacheHits;
                }
                else if (updateSlots > 0u) {
                    --updateSlots;
                    ++plan.pagesToRender;
                    mapping.state = VirtualShadowPageState::PendingRaster;
                    resident.validLayers &= ~mapping.updateLayers;
                    resident.staticCasterRevision =
                        request.staticCasterRevision;
                    resident.dynamicCasterRevision =
                        request.dynamicCasterRevision;
                }
                else {
                    mapping.state = VirtualShadowPageState::Missing;
                    ++plan.missingPages;
                    ++plan.updateBudgetOverflow;
                    resident.validLayers &= ~mapping.updateLayers;
                }
                plan.mappings.push_back(mapping);
                continue;
            }

            if (updateSlots == 0u) {
                ++plan.missingPages;
                ++plan.updateBudgetOverflow;
                plan.mappings.push_back(mapping);
                continue;
            }

            uint32_t physicalPage = InvalidVirtualShadowPhysicalPage;
            for (uint32_t candidate = 0;
                candidate < policy.maximumPhysicalPages; ++candidate) {
                if (!occupied.contains(candidate)) {
                    physicalPage = candidate;
                    break;
                }
            }
            if (physicalPage == InvalidVirtualShadowPhysicalPage) {
                auto victim = plan.nextResidency.end();
                for (auto iterator = plan.nextResidency.begin();
                    iterator != plan.nextResidency.end(); ++iterator) {
                    if (requestedAddresses.contains(iterator->address)) continue;
                    if (victim == plan.nextResidency.end() ||
                        std::tie(iterator->lastPriority,
                            iterator->lastUsedFrame, iterator->physicalPage) <
                        std::tie(victim->lastPriority,
                            victim->lastUsedFrame, victim->physicalPage))
                        victim = iterator;
                }
                if (victim == plan.nextResidency.end()) {
                    ++plan.missingPages;
                    ++plan.physicalPoolOverflow;
                    plan.mappings.push_back(mapping);
                    continue;
                }
                physicalPage = victim->physicalPage;
                residentByAddress.erase(victim->address);
                *victim = VirtualShadowResidentPage{
                    .address = request.address,
                    .staticCasterRevision = request.staticCasterRevision,
                    .dynamicCasterRevision = request.dynamicCasterRevision,
                    .lastUsedFrame = frameSerial,
                    .physicalPage = physicalPage,
                    .lastPriority = request.priority,
                };
                residentByAddress.emplace(request.address,
                    static_cast<size_t>(std::distance(
                        plan.nextResidency.begin(), victim)));
                ++plan.pagesEvicted;
            }
            else {
                occupied.insert(physicalPage);
                plan.nextResidency.push_back({
                    .address = request.address,
                    .staticCasterRevision = request.staticCasterRevision,
                    .dynamicCasterRevision = request.dynamicCasterRevision,
                    .lastUsedFrame = frameSerial,
                    .physicalPage = physicalPage,
                    .lastPriority = request.priority,
                });
                residentByAddress.emplace(request.address,
                    plan.nextResidency.size() - 1u);
                ++plan.pagesAllocated;
            }
            --updateSlots;
            ++plan.pagesToRender;
            mapping.physicalPage = physicalPage;
            mapping.updateLayers = request.requiredLayers;
            mapping.state = VirtualShadowPageState::PendingRaster;
            plan.mappings.push_back(mapping);
        }

        std::ranges::sort(plan.mappings,
            [](const VirtualShadowPageMapping& left,
                const VirtualShadowPageMapping& right) {
                return left.address < right.address;
            });
        std::ranges::sort(plan.nextResidency,
            [](const VirtualShadowResidentPage& left,
                const VirtualShadowResidentPage& right) {
                return left.physicalPage < right.physicalPage;
            });
        return plan;
    }

    void commitVirtualShadowPageUpdates(VirtualShadowPagePlan& plan,
        std::span<const uint32_t> completedPhysicalPages,
        uint64_t frameSerial) {
        const std::set<uint32_t> completed(completedPhysicalPages.begin(),
            completedPhysicalPages.end());
        for (VirtualShadowPageMapping& mapping : plan.mappings) {
            if (mapping.state != VirtualShadowPageState::PendingRaster ||
                !completed.contains(mapping.physicalPage))
                continue;
            const auto resident = std::ranges::find_if(plan.nextResidency,
                [&](const VirtualShadowResidentPage& candidate) {
                    return candidate.physicalPage == mapping.physicalPage &&
                        candidate.address == mapping.address;
                });
            if (resident == plan.nextResidency.end()) continue;
            resident->validLayers |= mapping.updateLayers;
            resident->lastRenderedFrame = frameSerial;
            mapping.updateLayers = VirtualShadowPageLayerNone;
            mapping.state = VirtualShadowPageState::CachedSampleable;
        }
    }

} // namespace Iridium
