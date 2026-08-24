#pragma once

#include "renderer/transparency/LayeredGlass.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include <glm/glm.hpp>

namespace Iridium {

    inline constexpr uint32_t kOrdinary2AtlasTileSize = 16u;

    struct Ordinary2ScreenRect {
        uint32_t minX = 0u;
        uint32_t minY = 0u;
        uint32_t maxX = 0u;
        uint32_t maxY = 0u;

        friend constexpr bool operator==(
            Ordinary2ScreenRect, Ordinary2ScreenRect) = default;
    };

    [[nodiscard]] constexpr bool validOrdinary2ScreenRect(
        Ordinary2ScreenRect rect, uint32_t sceneWidth,
        uint32_t sceneHeight) noexcept {
        return rect.minX < rect.maxX && rect.minY < rect.maxY &&
            rect.maxX <= sceneWidth && rect.maxY <= sceneHeight;
    }

    [[nodiscard]] constexpr uint32_t ordinary2AlignUp(
        uint32_t value) noexcept {
        return (value + kOrdinary2AtlasTileSize - 1u) &
            ~(kOrdinary2AtlasTileSize - 1u);
    }

    [[nodiscard]] constexpr uint32_t ordinary2AlignDown(
        uint32_t value) noexcept {
        return value & ~(kOrdinary2AtlasTileSize - 1u);
    }

    struct Ordinary2AtlasExtent {
        uint32_t width = 0u;
        uint32_t height = 0u;

        [[nodiscard]] constexpr bool empty() const noexcept {
            return width == 0u || height == 0u;
        }
        [[nodiscard]] constexpr uint64_t area() const noexcept {
            return static_cast<uint64_t>(width) * height;
        }

        friend constexpr bool operator==(
            Ordinary2AtlasExtent, Ordinary2AtlasExtent) = default;
    };

    // The Ordinary2 topology uses a stable, scene-derived maximum atlas rather
    // than resizing to transient content every frame. Full aligned scene width
    // plus the largest aligned height within 25% keeps topology changes sparse.
    [[nodiscard]] constexpr Ordinary2AtlasExtent ordinary2AtlasCapacityExtent(
        uint32_t sceneWidth, uint32_t sceneHeight) noexcept {
        const uint32_t width = ordinary2AlignDown(sceneWidth);
        if (width == 0u || sceneHeight < kOrdinary2AtlasTileSize)
            return {};
        const uint64_t maximumArea =
            static_cast<uint64_t>(sceneWidth) * sceneHeight / 4u;
        const uint32_t height = ordinary2AlignDown(static_cast<uint32_t>(
            (std::min)(maximumArea / width,
                static_cast<uint64_t>(sceneHeight))));
        return height == 0u ? Ordinary2AtlasExtent{} :
            Ordinary2AtlasExtent{ width, height };
    }

    struct Ordinary2AtlasRequest {
        TransparentWorkIdentity work;
        Ordinary2ScreenRect screenRect;
        uint32_t packetIndex = 0u;
        int32_t priority = 0;
    };

    enum class Ordinary2ProjectionStatus : uint8_t {
        Accepted,
        Culled,
        InvalidSceneExtent,
        InvalidBounds,
        NearPlaneFallback,
        UnsafeProjection,
    };

    struct Ordinary2ProjectionResult {
        Ordinary2ProjectionStatus status =
            Ordinary2ProjectionStatus::InvalidBounds;
        Ordinary2ScreenRect screenRect;

        [[nodiscard]] constexpr bool accepted() const noexcept {
            return status == Ordinary2ProjectionStatus::Accepted;
        }
    };

    // Projects a finite world AABB whose full depth interval is in front of the
    // near plane. A one-pixel guard band makes the integer result conservative
    // for raster coverage. Near-clipped/camera-intersecting work deliberately
    // falls back instead of attempting an unbounded homogeneous clip here.
    [[nodiscard]] inline Ordinary2ProjectionResult
        projectOrdinary2WorldBounds(const glm::vec3& boundsMinWorld,
            const glm::vec3& boundsMaxWorld, uint32_t transparentWorkFlags,
            const glm::mat4& viewProjection, uint32_t sceneWidth,
            uint32_t sceneHeight) noexcept {
        if (sceneWidth == 0u || sceneHeight == 0u) {
            return { Ordinary2ProjectionStatus::InvalidSceneExtent, {} };
        }
        const auto finite = [](float value) { return std::isfinite(value); };
        for (uint32_t component = 0; component < 3u; ++component) {
            if (!finite(boundsMinWorld[component]) ||
                !finite(boundsMaxWorld[component]) ||
                boundsMinWorld[component] > boundsMaxWorld[component]) {
                return { Ordinary2ProjectionStatus::InvalidBounds, {} };
            }
        }
        if ((transparentWorkFlags & TransparentWorkCulled) != 0u) {
            return { Ordinary2ProjectionStatus::Culled, {} };
        }
        if ((transparentWorkFlags & TransparentWorkIntervalValid) == 0u ||
            (transparentWorkFlags &
                TransparentWorkInvalidBoundsFallback) != 0u) {
            return { Ordinary2ProjectionStatus::InvalidBounds, {} };
        }
        if ((transparentWorkFlags & (TransparentWorkNearClipped |
                TransparentWorkCameraIntersecting)) != 0u) {
            return { Ordinary2ProjectionStatus::NearPlaneFallback, {} };
        }
        for (uint32_t column = 0; column < 4u; ++column) {
            for (uint32_t row = 0; row < 4u; ++row) {
                if (!finite(viewProjection[column][row])) {
                    return { Ordinary2ProjectionStatus::UnsafeProjection, {} };
                }
            }
        }

        glm::vec2 ndcMin(std::numeric_limits<float>::max());
        glm::vec2 ndcMax(std::numeric_limits<float>::lowest());
        for (uint32_t corner = 0; corner < 8u; ++corner) {
            const glm::vec3 world{
                (corner & 1u) != 0u ? boundsMaxWorld.x : boundsMinWorld.x,
                (corner & 2u) != 0u ? boundsMaxWorld.y : boundsMinWorld.y,
                (corner & 4u) != 0u ? boundsMaxWorld.z : boundsMinWorld.z,
            };
            const glm::vec4 clip = viewProjection * glm::vec4(world, 1.0f);
            if (!finite(clip.x) || !finite(clip.y) || !finite(clip.w) ||
                clip.w <= 1.0e-6f) {
                return { Ordinary2ProjectionStatus::UnsafeProjection, {} };
            }
            const glm::vec2 ndc = glm::vec2(clip) / clip.w;
            if (!finite(ndc.x) || !finite(ndc.y)) {
                return { Ordinary2ProjectionStatus::UnsafeProjection, {} };
            }
            ndcMin = glm::min(ndcMin, ndc);
            ndcMax = glm::max(ndcMax, ndc);
        }
        if (ndcMax.x < -1.0f || ndcMin.x > 1.0f ||
            ndcMax.y < -1.0f || ndcMin.y > 1.0f) {
            return { Ordinary2ProjectionStatus::Culled, {} };
        }
        ndcMin = glm::clamp(ndcMin, glm::vec2(-1.0f), glm::vec2(1.0f));
        ndcMax = glm::clamp(ndcMax, glm::vec2(-1.0f), glm::vec2(1.0f));
        const glm::vec2 sceneExtent{
            static_cast<float>(sceneWidth), static_cast<float>(sceneHeight) };
        const glm::vec2 pixelMin = (ndcMin * 0.5f + 0.5f) * sceneExtent;
        const glm::vec2 pixelMax = (ndcMax * 0.5f + 0.5f) * sceneExtent;
        const auto guardedMin = [](float value) {
            return static_cast<int64_t>(std::floor(value)) - 1;
        };
        const auto guardedMax = [](float value) {
            return static_cast<int64_t>(std::ceil(value)) + 1;
        };
        const auto clampPixel = [](int64_t value, uint32_t maximum) {
            return static_cast<uint32_t>((std::clamp)(value, int64_t{ 0 },
                static_cast<int64_t>(maximum)));
        };
        const Ordinary2ScreenRect screenRect{
            clampPixel(guardedMin(pixelMin.x), sceneWidth),
            clampPixel(guardedMin(pixelMin.y), sceneHeight),
            clampPixel(guardedMax(pixelMax.x), sceneWidth),
            clampPixel(guardedMax(pixelMax.y), sceneHeight),
        };
        if (!validOrdinary2ScreenRect(screenRect, sceneWidth, sceneHeight)) {
            return { Ordinary2ProjectionStatus::Culled, {} };
        }
        return { Ordinary2ProjectionStatus::Accepted, screenRect };
    }

    struct Ordinary2RequestCollectionStats {
        uint32_t inspectedPacketCount = 0u;
        uint32_t candidatePacketCount = 0u;
        uint32_t projectedPacketCount = 0u;
        uint32_t culledPacketCount = 0u;
        uint32_t invalidBoundsFallbackCount = 0u;
        uint32_t nearPlaneFallbackCount = 0u;
        uint32_t unsafeProjectionFallbackCount = 0u;
        uint32_t requestCapacityFallbackCount = 0u;
    };

    [[nodiscard]] constexpr bool isOrdinary2LayeredGlassPacket(
        const DrawPacket& packet) noexcept {
        return packet.transparencyExecutionMode ==
                TransparencyExecutionMode::Classified &&
            packet.transparency.resolvedClass ==
                TransparencyClass::LayeredGlass &&
            packet.transparency.quality ==
                TransparencyQuality::Ordinary2;
    }

    // Profiler-facing fixed-capacity request collection. If more projected
    // candidates arrive than fit, a fixed heap retains higher author priority,
    // then lower stable work identity, independent of extraction order.
    template <size_t MaximumRequests>
    class BoundedOrdinary2RequestCollector final {
    public:
        static_assert(MaximumRequests > 0u);
        static_assert(MaximumRequests <= kOrdinary2MaximumWorkCount);

        void collect(std::span<const DrawPacket> packets,
            const glm::mat4& viewProjection, uint32_t sceneWidth,
            uint32_t sceneHeight) noexcept {
            requestCount_ = 0u;
            stats_ = {};
            stats_.inspectedPacketCount = static_cast<uint32_t>((std::min)(
                packets.size(), static_cast<size_t>(
                    (std::numeric_limits<uint32_t>::max)())));
            for (size_t packetIndex = 0; packetIndex < packets.size();
                    ++packetIndex) {
                const DrawPacket& packet = packets[packetIndex];
                if (!isOrdinary2LayeredGlassPacket(packet)) {
                    continue;
                }
                ++stats_.candidatePacketCount;
                const Ordinary2ProjectionResult projected =
                    projectOrdinary2WorldBounds(packet.boundsMinWorld,
                        packet.boundsMaxWorld, packet.transparentWorkFlags,
                        viewProjection, sceneWidth, sceneHeight);
                switch (projected.status) {
                case Ordinary2ProjectionStatus::Accepted:
                    break;
                case Ordinary2ProjectionStatus::Culled:
                    ++stats_.culledPacketCount;
                    continue;
                case Ordinary2ProjectionStatus::InvalidSceneExtent:
                case Ordinary2ProjectionStatus::InvalidBounds:
                    ++stats_.invalidBoundsFallbackCount;
                    continue;
                case Ordinary2ProjectionStatus::NearPlaneFallback:
                    ++stats_.nearPlaneFallbackCount;
                    continue;
                case Ordinary2ProjectionStatus::UnsafeProjection:
                    ++stats_.unsafeProjectionFallbackCount;
                    continue;
                }
                ++stats_.projectedPacketCount;
                const Ordinary2AtlasRequest request{
                    .work = transparentWorkIdentity(packet),
                    .screenRect = projected.screenRect,
                    .packetIndex = static_cast<uint32_t>((std::min)(packetIndex,
                        static_cast<size_t>(
                            (std::numeric_limits<uint32_t>::max)()))),
                    .priority = packet.transparency.priority,
                };
                if (requestCount_ < MaximumRequests) {
                    requests_[requestCount_++] = request;
                    std::push_heap(requests_.begin(),
                        requests_.begin() + requestCount_, requestBetter);
                    continue;
                }
                ++stats_.requestCapacityFallbackCount;
                if (!requestBetter(request, requests_.front()))
                    continue;
                std::pop_heap(requests_.begin(),
                    requests_.begin() + requestCount_, requestBetter);
                requests_[requestCount_ - 1u] = request;
                std::push_heap(requests_.begin(),
                    requests_.begin() + requestCount_, requestBetter);
            }
        }

        [[nodiscard]] std::span<const Ordinary2AtlasRequest> requests()
            const noexcept {
            return { requests_.data(), requestCount_ };
        }
        [[nodiscard]] const Ordinary2RequestCollectionStats& stats()
            const noexcept {
            return stats_;
        }

    private:
        [[nodiscard]] static bool requestBetter(
            const Ordinary2AtlasRequest& lhs,
            const Ordinary2AtlasRequest& rhs) noexcept {
            if (lhs.priority != rhs.priority)
                return lhs.priority > rhs.priority;
            if (lhs.work != rhs.work)
                return lhs.work < rhs.work;
            return lhs.packetIndex < rhs.packetIndex;
        }

        std::array<Ordinary2AtlasRequest, MaximumRequests> requests_{};
        size_t requestCount_ = 0u;
        Ordinary2RequestCollectionStats stats_{};
    };

    struct Ordinary2AtlasPlacement {
        Ordinary2ScreenRect screenRect;
        uint32_t atlasX = 0u;
        uint32_t atlasY = 0u;
        uint32_t width = 0u;
        uint32_t height = 0u;
        int32_t viewportOffsetX = 0;
        int32_t viewportOffsetY = 0;
        uint32_t workTableIndex = 0u;
        uint32_t packetIndex = 0u;
    };

    enum class Ordinary2AtlasDecisionStatus : uint8_t {
        Accepted,
        InvalidScreenRect,
        AtlasCapacityExceeded,
        ViewportOffsetExceeded,
    };

    struct Ordinary2AtlasDecision {
        Ordinary2AtlasDecisionStatus status =
            Ordinary2AtlasDecisionStatus::InvalidScreenRect;
        Ordinary2AtlasPlacement placement;

        [[nodiscard]] constexpr bool accepted() const noexcept {
            return status == Ordinary2AtlasDecisionStatus::Accepted;
        }
    };

    struct Ordinary2CaptureDraw {
        uint32_t packetIndex = 0u;
        uint32_t workTableIndex = 0u;
        uint32_t atlasX = 0u;
        uint32_t atlasY = 0u;
        uint32_t width = 0u;
        uint32_t height = 0u;
        int32_t viewportOffsetX = 0;
        int32_t viewportOffsetY = 0;
    };

    enum class Ordinary2CaptureDrawPreparationStatus : uint8_t {
        Prepared,
        Empty,
        DecisionCapacityExceeded,
    };

    struct Ordinary2CaptureDrawStats {
        uint32_t acceptedDecisionCount = 0u;
        uint32_t preparedDrawCount = 0u;
        uint32_t invalidPacketIndexCount = 0u;
        uint32_t incompatiblePacketCount = 0u;
        uint32_t invalidPlacementCount = 0u;
    };

    // Converts atlas decisions into a stable draw sequence without retaining
    // pointers into frontend-owned packet storage. Entry and exit capture reuse
    // this exact order; only their push flag and descriptor set differ.
    template <size_t MaximumDraws>
    class BoundedOrdinary2CaptureDrawPlan final {
    public:
        static_assert(MaximumDraws > 0u);
        static_assert(MaximumDraws <= kOrdinary2MaximumWorkCount);

        Ordinary2CaptureDrawPreparationStatus prepare(
            std::span<const Ordinary2AtlasDecision> decisions,
            std::span<const DrawPacket> packets,
            Ordinary2AtlasExtent atlasExtent) noexcept {
            drawCount_ = 0u;
            stats_ = {};
            status_ = Ordinary2CaptureDrawPreparationStatus::Empty;
            if (decisions.empty())
                return status_;
            if (decisions.size() > MaximumDraws) {
                status_ = Ordinary2CaptureDrawPreparationStatus::
                    DecisionCapacityExceeded;
                return status_;
            }
            for (const Ordinary2AtlasDecision& decision : decisions) {
                if (!decision.accepted())
                    continue;
                ++stats_.acceptedDecisionCount;
                const Ordinary2AtlasPlacement& placement = decision.placement;
                if (placement.packetIndex >= packets.size()) {
                    ++stats_.invalidPacketIndexCount;
                    continue;
                }
                const DrawPacket& packet = packets[placement.packetIndex];
                if (packet.transparencyExecutionMode !=
                        TransparencyExecutionMode::Classified ||
                    packet.transparency.resolvedClass !=
                        TransparencyClass::LayeredGlass ||
                    packet.transparency.quality !=
                        TransparencyQuality::Ordinary2) {
                    ++stats_.incompatiblePacketCount;
                    continue;
                }
                if (placement.width == 0u || placement.height == 0u ||
                    placement.atlasX >= atlasExtent.width ||
                    placement.atlasY >= atlasExtent.height ||
                    placement.width > atlasExtent.width - placement.atlasX ||
                    placement.height > atlasExtent.height - placement.atlasY ||
                    !validLayeredViewportOffset(placement.viewportOffsetX,
                        placement.viewportOffsetY)) {
                    ++stats_.invalidPlacementCount;
                    continue;
                }
                draws_[drawCount_++] = {
                    .packetIndex = placement.packetIndex,
                    .workTableIndex = placement.workTableIndex,
                    .atlasX = placement.atlasX,
                    .atlasY = placement.atlasY,
                    .width = placement.width,
                    .height = placement.height,
                    .viewportOffsetX = placement.viewportOffsetX,
                    .viewportOffsetY = placement.viewportOffsetY,
                };
            }
            std::sort(draws_.begin(), draws_.begin() + drawCount_,
                [](const Ordinary2CaptureDraw& lhs,
                    const Ordinary2CaptureDraw& rhs) {
                    if (lhs.atlasY != rhs.atlasY)
                        return lhs.atlasY < rhs.atlasY;
                    if (lhs.atlasX != rhs.atlasX)
                        return lhs.atlasX < rhs.atlasX;
                    if (lhs.workTableIndex != rhs.workTableIndex)
                        return lhs.workTableIndex < rhs.workTableIndex;
                    return lhs.packetIndex < rhs.packetIndex;
                });
            stats_.preparedDrawCount = static_cast<uint32_t>(drawCount_);
            status_ = drawCount_ == 0u
                ? Ordinary2CaptureDrawPreparationStatus::Empty
                : Ordinary2CaptureDrawPreparationStatus::Prepared;
            return status_;
        }

        [[nodiscard]] std::span<const Ordinary2CaptureDraw> draws()
            const noexcept {
            return { draws_.data(), drawCount_ };
        }
        [[nodiscard]] const Ordinary2CaptureDrawStats& stats() const noexcept {
            return stats_;
        }
        [[nodiscard]] Ordinary2CaptureDrawPreparationStatus status()
            const noexcept {
            return status_;
        }

    private:
        std::array<Ordinary2CaptureDraw, MaximumDraws> draws_{};
        size_t drawCount_ = 0u;
        Ordinary2CaptureDrawStats stats_{};
        Ordinary2CaptureDrawPreparationStatus status_ =
            Ordinary2CaptureDrawPreparationStatus::Empty;
    };

    enum class Ordinary2AtlasPreparationStatus : uint8_t {
        Prepared,
        Empty,
        InvalidSceneExtent,
        RequestCapacityExceeded,
        AtlasUnavailable,
    };

    struct Ordinary2AtlasStats {
        uint32_t requestCount = 0u;
        uint32_t validRequestCount = 0u;
        uint32_t acceptedPacketCount = 0u;
        uint32_t acceptedIslandCount = 0u;
        uint32_t duplicatePacketCount = 0u;
        uint32_t invalidRectCount = 0u;
        uint32_t atlasRejectedPacketCount = 0u;
        uint32_t viewportOffsetRejectedPacketCount = 0u;
        uint32_t requestCapacityRejectedCount = 0u;
        uint64_t allocatedTexelCount = 0u;
    };

    // Fixed-capacity deterministic CPU preparation for the two-interface atlas.
    // It uses std::sort over fixed index arrays (no heap allocation), unions
    // duplicate packet bounds by stable work identity, retains higher author
    // priority first, and returns an explicit decision for every input packet.
    template <size_t MaximumRequests>
    class BoundedOrdinary2AtlasPlan final {
    public:
        static_assert(MaximumRequests > 0u);
        static_assert(MaximumRequests <= kOrdinary2MaximumWorkCount);

        Ordinary2AtlasPreparationStatus prepare(
            std::span<const Ordinary2AtlasRequest> requests,
            uint32_t sceneWidth, uint32_t sceneHeight) noexcept {
            workTable_.reset();
            requestCount_ = 0u;
            groupCount_ = 0u;
            atlasExtent_ = {};
            stats_ = {};
            status_ = Ordinary2AtlasPreparationStatus::Empty;
            if (requests.empty())
                return status_;
            if (sceneWidth == 0u || sceneHeight == 0u) {
                status_ = Ordinary2AtlasPreparationStatus::InvalidSceneExtent;
                return status_;
            }
            sceneWidthForValidation_ = sceneWidth;
            sceneHeightForValidation_ = sceneHeight;
            if (requests.size() > MaximumRequests) {
                stats_.requestCount = static_cast<uint32_t>(requests.size());
                stats_.requestCapacityRejectedCount =
                    static_cast<uint32_t>(requests.size());
                status_ =
                    Ordinary2AtlasPreparationStatus::RequestCapacityExceeded;
                return status_;
            }
            const Ordinary2AtlasExtent capacity =
                ordinary2AtlasCapacityExtent(sceneWidth, sceneHeight);
            if (capacity.empty()) {
                status_ = Ordinary2AtlasPreparationStatus::AtlasUnavailable;
                return status_;
            }

            requestCount_ = requests.size();
            stats_.requestCount = static_cast<uint32_t>(requestCount_);
            for (size_t index = 0; index < requestCount_; ++index) {
                requestOrder_[index] = static_cast<uint32_t>(index);
                decisions_[index] = {};
            }
            std::sort(requestOrder_.begin(),
                requestOrder_.begin() + requestCount_,
                [&requests](uint32_t lhsIndex, uint32_t rhsIndex) {
                    const Ordinary2AtlasRequest& lhs = requests[lhsIndex];
                    const Ordinary2AtlasRequest& rhs = requests[rhsIndex];
                    if (lhs.work != rhs.work)
                        return lhs.work < rhs.work;
                    return lhs.packetIndex < rhs.packetIndex;
                });

            size_t sorted = 0u;
            while (sorted < requestCount_) {
                const size_t first = sorted;
                const TransparentWorkIdentity work =
                    requests[requestOrder_[sorted]].work;
                Group group{};
                group.work = work;
                group.firstSortedRequest = static_cast<uint32_t>(first);
                group.priority = (std::numeric_limits<int32_t>::min)();
                bool hasValidRect = false;
                while (sorted < requestCount_ &&
                    requests[requestOrder_[sorted]].work == work) {
                    const uint32_t requestIndex = requestOrder_[sorted];
                    const Ordinary2AtlasRequest& request =
                        requests[requestIndex];
                    ++group.sortedRequestCount;
                    group.priority = (std::max)(group.priority,
                        request.priority);
                    if (!validOrdinary2ScreenRect(request.screenRect,
                            sceneWidth, sceneHeight)) {
                        decisions_[requestIndex].status =
                            Ordinary2AtlasDecisionStatus::InvalidScreenRect;
                        ++stats_.invalidRectCount;
                    }
                    else {
                        ++group.validRequestCount;
                        ++stats_.validRequestCount;
                        if (!hasValidRect) {
                            group.screenRect = request.screenRect;
                            hasValidRect = true;
                        }
                        else {
                            group.screenRect.minX = (std::min)(
                                group.screenRect.minX, request.screenRect.minX);
                            group.screenRect.minY = (std::min)(
                                group.screenRect.minY, request.screenRect.minY);
                            group.screenRect.maxX = (std::max)(
                                group.screenRect.maxX, request.screenRect.maxX);
                            group.screenRect.maxY = (std::max)(
                                group.screenRect.maxY, request.screenRect.maxY);
                        }
                    }
                    ++sorted;
                }
                if (hasValidRect) {
                    groups_[groupCount_] = group;
                    groupOrder_[groupCount_] =
                        static_cast<uint32_t>(groupCount_);
                    ++groupCount_;
                }
            }

            std::sort(groupOrder_.begin(), groupOrder_.begin() + groupCount_,
                [this](uint32_t lhsIndex, uint32_t rhsIndex) {
                    const Group& lhs = groups_[lhsIndex];
                    const Group& rhs = groups_[rhsIndex];
                    if (lhs.priority != rhs.priority)
                        return lhs.priority > rhs.priority;
                    return lhs.work < rhs.work;
                });

            uint32_t cursorX = 0u;
            uint32_t cursorY = 0u;
            uint32_t rowHeight = 0u;
            for (size_t ordered = 0; ordered < groupCount_; ++ordered) {
                Group& group = groups_[groupOrder_[ordered]];
                const uint32_t width = ordinary2AlignUp(
                    group.screenRect.maxX - group.screenRect.minX);
                const uint32_t height = ordinary2AlignUp(
                    group.screenRect.maxY - group.screenRect.minY);
                uint32_t candidateX = cursorX;
                uint32_t candidateY = cursorY;
                uint32_t candidateRowHeight = rowHeight;
                if (candidateX + width > capacity.width) {
                    candidateX = 0u;
                    candidateY += candidateRowHeight;
                    candidateRowHeight = 0u;
                }
                if (width > capacity.width || height > capacity.height ||
                    candidateY + height > capacity.height) {
                    rejectGroup(requests, group,
                        Ordinary2AtlasDecisionStatus::AtlasCapacityExceeded);
                    stats_.atlasRejectedPacketCount +=
                        group.validRequestCount;
                    continue;
                }
                const int64_t offsetX =
                    static_cast<int64_t>(group.screenRect.minX) - candidateX;
                const int64_t offsetY =
                    static_cast<int64_t>(group.screenRect.minY) - candidateY;
                if (!validLayeredViewportOffset(offsetX, offsetY)) {
                    rejectGroup(requests, group,
                        Ordinary2AtlasDecisionStatus::ViewportOffsetExceeded);
                    stats_.viewportOffsetRejectedPacketCount +=
                        group.validRequestCount;
                    continue;
                }

                uint32_t workTableIndex = 0u;
                bool firstWorkPacket = true;
                forEachValidRequest(requests, group,
                    [&](uint32_t, const Ordinary2AtlasRequest& request) {
                        const LayeredWorkTableInsertResult inserted =
                            workTable_.insert(request.work);
                        if (firstWorkPacket) {
                            workTableIndex = inserted.workTableIndex;
                            firstWorkPacket = false;
                        }
                    });
                const Ordinary2AtlasPlacement base{
                    .screenRect = group.screenRect,
                    .atlasX = candidateX,
                    .atlasY = candidateY,
                    .width = width,
                    .height = height,
                    .viewportOffsetX = static_cast<int32_t>(offsetX),
                    .viewportOffsetY = static_cast<int32_t>(offsetY),
                    .workTableIndex = workTableIndex,
                };
                forEachValidRequest(requests, group,
                    [&](uint32_t requestIndex,
                        const Ordinary2AtlasRequest& request) {
                        decisions_[requestIndex].status =
                            Ordinary2AtlasDecisionStatus::Accepted;
                        decisions_[requestIndex].placement = base;
                        decisions_[requestIndex].placement.packetIndex =
                            request.packetIndex;
                    });
                cursorX = candidateX + width;
                cursorY = candidateY;
                rowHeight = (std::max)(candidateRowHeight, height);
                ++stats_.acceptedIslandCount;
                stats_.acceptedPacketCount += group.validRequestCount;
                stats_.duplicatePacketCount += group.validRequestCount - 1u;
                stats_.allocatedTexelCount +=
                    static_cast<uint64_t>(width) * height;
            }
            if (stats_.acceptedIslandCount != 0u)
                atlasExtent_ = capacity;
            status_ = Ordinary2AtlasPreparationStatus::Prepared;
            return status_;
        }

        [[nodiscard]] Ordinary2AtlasPreparationStatus status() const noexcept {
            return status_;
        }
        [[nodiscard]] Ordinary2AtlasExtent atlasExtent() const noexcept {
            return atlasExtent_;
        }
        [[nodiscard]] std::span<const Ordinary2AtlasDecision> decisions()
            const noexcept {
            return { decisions_.data(), requestCount_ };
        }
        [[nodiscard]] std::span<const TransparentWorkIdentity> workIdentities()
            const noexcept {
            return workTable_.identities();
        }
        [[nodiscard]] const LayeredWorkTableStats& workTableStats()
            const noexcept {
            return workTable_.stats();
        }
        [[nodiscard]] const Ordinary2AtlasStats& stats() const noexcept {
            return stats_;
        }

    private:
        struct Group {
            TransparentWorkIdentity work;
            Ordinary2ScreenRect screenRect;
            int32_t priority = 0;
            uint32_t firstSortedRequest = 0u;
            uint32_t sortedRequestCount = 0u;
            uint32_t validRequestCount = 0u;
        };

        template <typename Callback>
        void forEachValidRequest(
            std::span<const Ordinary2AtlasRequest> requests,
            const Group& group, Callback&& callback) noexcept {
            for (uint32_t offset = 0; offset <
                group.sortedRequestCount; ++offset) {
                const uint32_t requestIndex = requestOrder_[
                    group.firstSortedRequest + offset];
                if (validOrdinary2ScreenRect(requests[requestIndex].screenRect,
                        sceneWidthForValidation_, sceneHeightForValidation_))
                    callback(requestIndex, requests[requestIndex]);
            }
        }

        void rejectGroup(std::span<const Ordinary2AtlasRequest> requests,
            const Group& group,
            Ordinary2AtlasDecisionStatus status) noexcept {
            forEachValidRequest(requests, group,
                [&](uint32_t requestIndex, const Ordinary2AtlasRequest&) {
                    decisions_[requestIndex].status = status;
                });
        }

        std::array<uint32_t, MaximumRequests> requestOrder_{};
        std::array<uint32_t, MaximumRequests> groupOrder_{};
        std::array<Group, MaximumRequests> groups_{};
        std::array<Ordinary2AtlasDecision, MaximumRequests> decisions_{};
        BoundedLayeredWorkTable<MaximumRequests> workTable_;
        size_t requestCount_ = 0u;
        size_t groupCount_ = 0u;
        uint32_t sceneWidthForValidation_ = 0u;
        uint32_t sceneHeightForValidation_ = 0u;
        Ordinary2AtlasExtent atlasExtent_{};
        Ordinary2AtlasStats stats_{};
        Ordinary2AtlasPreparationStatus status_ =
            Ordinary2AtlasPreparationStatus::Empty;
    };

    using Ordinary2AtlasPlan =
        BoundedOrdinary2AtlasPlan<kOrdinary2MaximumWorkCount>;
    using Ordinary2RequestCollector =
        BoundedOrdinary2RequestCollector<kOrdinary2MaximumWorkCount>;
    using Ordinary2CaptureDrawPlan =
        BoundedOrdinary2CaptureDrawPlan<kOrdinary2MaximumWorkCount>;

} // namespace Iridium
