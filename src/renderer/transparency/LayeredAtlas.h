#pragma once

#include "renderer/transparency/Ordinary2Atlas.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace Iridium {

    inline constexpr size_t kLayeredQualityTierCount = 3u;

    [[nodiscard]] constexpr size_t layeredQualityTierIndex(
        TransparencyQuality quality) noexcept {
        switch (quality) {
        case TransparencyQuality::Ordinary2: return 0u;
        case TransparencyQuality::Hero4: return 1u;
        case TransparencyQuality::Cinematic8: return 2u;
        }
        return kLayeredQualityTierCount;
    }

    [[nodiscard]] constexpr Ordinary2AtlasExtent layeredAtlasCapacityExtent(
        uint32_t sceneWidth, uint32_t sceneHeight,
        TransparencyQuality quality) noexcept {
        const LayeredQualityTierContract tier =
            layeredQualityTierContract(quality);
        const uint32_t width = ordinary2AlignDown(sceneWidth);
        if (!tier.valid() || width == 0u ||
            sceneHeight < kOrdinary2AtlasTileSize) {
            return {};
        }
        const uint64_t maximumArea = layeredAtlasAreaCapPixels(
            sceneWidth, sceneHeight, quality);
        const uint32_t height = ordinary2AlignDown(static_cast<uint32_t>(
            (std::min)(maximumArea / width,
                static_cast<uint64_t>(sceneHeight))));
        return height == 0u ? Ordinary2AtlasExtent{} :
            Ordinary2AtlasExtent{ width, height };
    }

    struct LayeredAtlasRequest {
        TransparentWorkIdentity work;
        Ordinary2ScreenRect screenRect;
        uint32_t packetIndex = 0u;
        int32_t priority = 0;
        TransparencyQuality quality = TransparencyQuality::Ordinary2;
    };

    [[nodiscard]] constexpr bool isLayeredGlassPacket(
        const DrawPacket& packet, TransparencyQuality quality) noexcept {
        return packet.transparencyExecutionMode ==
                TransparencyExecutionMode::Classified &&
            packet.transparency.resolvedClass ==
                TransparencyClass::LayeredGlass &&
            packet.transparency.quality == quality;
    }

    using LayeredRequestCollectionStats = Ordinary2RequestCollectionStats;

    // Fixed-capacity projection shared by the explicit 2/4/8 tiers. The mask
    // lets a backend prepare only resident tiers without scanning the queue
    // once per tier. Capacity pressure retains author priority, stable work,
    // quality, then packet identity deterministically.
    template <size_t MaximumRequests>
    class BoundedLayeredRequestCollector final {
    public:
        void collect(std::span<const DrawPacket> packets,
            const glm::mat4& viewProjection, uint32_t sceneWidth,
            uint32_t sceneHeight, uint32_t activeTierMask) noexcept {
            requestCount_ = 0u;
            stats_ = {};
            stats_.inspectedPacketCount = static_cast<uint32_t>((std::min)(
                packets.size(), static_cast<size_t>(
                    (std::numeric_limits<uint32_t>::max)())));
            for (size_t packetIndex = 0u; packetIndex < packets.size();
                ++packetIndex) {
                const DrawPacket& packet = packets[packetIndex];
                const size_t tierIndex = layeredQualityTierIndex(
                    packet.transparency.quality);
                if (tierIndex == kLayeredQualityTierCount ||
                    (activeTierMask & (1u << tierIndex)) == 0u ||
                    !isLayeredGlassPacket(packet,
                        packet.transparency.quality)) {
                    continue;
                }
                ++stats_.candidatePacketCount;
                const Ordinary2ProjectionResult projected =
                    projectOrdinary2WorldBounds(packet.boundsMinWorld,
                        packet.boundsMaxWorld, packet.transparentWorkFlags,
                        viewProjection, sceneWidth, sceneHeight);
                switch (projected.status) {
                case Ordinary2ProjectionStatus::Accepted: break;
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
                const LayeredAtlasRequest request{
                    .work = transparentWorkIdentity(packet),
                    .screenRect = projected.screenRect,
                    .packetIndex = static_cast<uint32_t>((std::min)(
                        packetIndex, static_cast<size_t>(
                            (std::numeric_limits<uint32_t>::max)()))),
                    .priority = packet.transparency.priority,
                    .quality = packet.transparency.quality,
                };
                if (requestCount_ < MaximumRequests) {
                    requests_[requestCount_++] = request;
                    std::push_heap(requests_.begin(),
                        requests_.begin() + requestCount_, requestBetter);
                    continue;
                }
                ++stats_.requestCapacityFallbackCount;
                if (!requestBetter(request, requests_.front())) continue;
                std::pop_heap(requests_.begin(),
                    requests_.begin() + requestCount_, requestBetter);
                requests_[requestCount_ - 1u] = request;
                std::push_heap(requests_.begin(),
                    requests_.begin() + requestCount_, requestBetter);
            }
        }

        [[nodiscard]] std::span<const LayeredAtlasRequest> requests()
            const noexcept {
            return { requests_.data(), requestCount_ };
        }
        [[nodiscard]] const LayeredRequestCollectionStats& stats()
            const noexcept {
            return stats_;
        }

    private:
        [[nodiscard]] static bool requestBetter(
            const LayeredAtlasRequest& lhs,
            const LayeredAtlasRequest& rhs) noexcept {
            if (lhs.priority != rhs.priority)
                return lhs.priority > rhs.priority;
            if (lhs.work != rhs.work) return lhs.work < rhs.work;
            if (lhs.quality != rhs.quality)
                return static_cast<uint8_t>(lhs.quality) <
                    static_cast<uint8_t>(rhs.quality);
            return lhs.packetIndex < rhs.packetIndex;
        }

        std::array<LayeredAtlasRequest, MaximumRequests> requests_{};
        size_t requestCount_ = 0u;
        LayeredRequestCollectionStats stats_{};
    };

    enum class LayeredAtlasDecisionStatus : uint8_t {
        Accepted,
        InvalidQuality,
        InvalidScreenRect,
        ConflictingWorkQuality,
        TierAtlasUnavailable,
        TierAtlasCapacityExceeded,
        ViewportOffsetExceeded,
    };

    struct LayeredAtlasPlacement {
        Ordinary2ScreenRect screenRect;
        TransparencyQuality quality = TransparencyQuality::Ordinary2;
        uint32_t atlasX = 0u;
        uint32_t atlasY = 0u;
        uint32_t width = 0u;
        uint32_t height = 0u;
        int32_t viewportOffsetX = 0;
        int32_t viewportOffsetY = 0;
        uint32_t packetIndex = 0u;
        uint32_t workTableIndex = 0u;
    };

    struct LayeredAtlasDecision {
        LayeredAtlasDecisionStatus status =
            LayeredAtlasDecisionStatus::InvalidQuality;
        LayeredAtlasPlacement placement;

        [[nodiscard]] constexpr bool accepted() const noexcept {
            return status == LayeredAtlasDecisionStatus::Accepted;
        }
    };

    struct LayeredCaptureDraw {
        uint32_t packetIndex = 0u;
        uint32_t workTableIndex = 0u;
        uint32_t atlasX = 0u;
        uint32_t atlasY = 0u;
        uint32_t width = 0u;
        uint32_t height = 0u;
        int32_t viewportOffsetX = 0;
        int32_t viewportOffsetY = 0;
        TransparencyQuality quality = TransparencyQuality::Ordinary2;
    };

    enum class LayeredCaptureDrawPreparationStatus : uint8_t {
        Prepared,
        Empty,
        DecisionCapacityExceeded,
    };

    struct LayeredCaptureDrawStats {
        uint32_t acceptedDecisionCount = 0u;
        uint32_t preparedDrawCount = 0u;
        uint32_t invalidPacketIndexCount = 0u;
        uint32_t incompatiblePacketCount = 0u;
        uint32_t invalidPlacementCount = 0u;
    };

    template <size_t MaximumDraws>
    class BoundedLayeredCaptureDrawPlan final {
    public:
        LayeredCaptureDrawPreparationStatus prepare(
            std::span<const LayeredAtlasDecision> decisions,
            std::span<const DrawPacket> packets,
            const std::array<Ordinary2AtlasExtent,
                kLayeredQualityTierCount>& residentExtents) noexcept {
            drawCount_ = 0u;
            stats_ = {};
            status_ = LayeredCaptureDrawPreparationStatus::Empty;
            if (decisions.empty()) return status_;
            if (decisions.size() > MaximumDraws) {
                status_ = LayeredCaptureDrawPreparationStatus::
                    DecisionCapacityExceeded;
                return status_;
            }
            for (const LayeredAtlasDecision& decision : decisions) {
                if (!decision.accepted()) continue;
                ++stats_.acceptedDecisionCount;
                const LayeredAtlasPlacement& placement = decision.placement;
                const size_t tierIndex = layeredQualityTierIndex(
                    placement.quality);
                if (placement.packetIndex >= packets.size()) {
                    ++stats_.invalidPacketIndexCount;
                    continue;
                }
                const DrawPacket& packet = packets[placement.packetIndex];
                if (tierIndex == kLayeredQualityTierCount ||
                    !isLayeredGlassPacket(packet, placement.quality)) {
                    ++stats_.incompatiblePacketCount;
                    continue;
                }
                const Ordinary2AtlasExtent extent =
                    residentExtents[tierIndex];
                if (extent.empty() || placement.width == 0u ||
                    placement.height == 0u ||
                    placement.atlasX >= extent.width ||
                    placement.atlasY >= extent.height ||
                    placement.width > extent.width - placement.atlasX ||
                    placement.height > extent.height - placement.atlasY ||
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
                    .quality = placement.quality,
                };
            }
            std::sort(draws_.begin(), draws_.begin() + drawCount_,
                [](const LayeredCaptureDraw& lhs,
                    const LayeredCaptureDraw& rhs) {
                    if (lhs.quality != rhs.quality)
                        return static_cast<uint8_t>(lhs.quality) <
                            static_cast<uint8_t>(rhs.quality);
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
                ? LayeredCaptureDrawPreparationStatus::Empty
                : LayeredCaptureDrawPreparationStatus::Prepared;
            return status_;
        }

        [[nodiscard]] std::span<const LayeredCaptureDraw> draws()
            const noexcept {
            return { draws_.data(), drawCount_ };
        }
        [[nodiscard]] const LayeredCaptureDrawStats& stats() const noexcept {
            return stats_;
        }
        [[nodiscard]] LayeredCaptureDrawPreparationStatus status()
            const noexcept {
            return status_;
        }

    private:
        std::array<LayeredCaptureDraw, MaximumDraws> draws_{};
        size_t drawCount_ = 0u;
        LayeredCaptureDrawStats stats_{};
        LayeredCaptureDrawPreparationStatus status_ =
            LayeredCaptureDrawPreparationStatus::Empty;
    };

    enum class LayeredAtlasPreparationStatus : uint8_t {
        Prepared,
        Empty,
        InvalidSceneExtent,
        RequestCapacityExceeded,
        AtlasUnavailable,
    };

    struct LayeredAtlasTierTopology {
        TransparencyQuality quality = TransparencyQuality::Ordinary2;
        Ordinary2AtlasExtent capacityExtent;
        uint32_t maximumInterfaceCount = 0u;
        bool active = false;

        [[nodiscard]] constexpr uint64_t capacityTexelCount() const noexcept {
            return capacityExtent.area();
        }

        [[nodiscard]] constexpr uint64_t interfaceStorageTexelCount()
            const noexcept {
            return active ? capacityExtent.area() * maximumInterfaceCount : 0u;
        }
    };

    struct LayeredAtlasTierStats {
        uint32_t acceptedPacketCount = 0u;
        uint32_t acceptedIslandCount = 0u;
        uint32_t acceptedWorkCount = 0u;
        uint32_t multiWorkIslandCount = 0u;
        uint32_t overlapMergedWorkCount = 0u;
        uint32_t duplicatePacketCount = 0u;
        uint32_t capacityRejectedPacketCount = 0u;
        uint32_t unavailableRejectedPacketCount = 0u;
        uint32_t viewportOffsetRejectedPacketCount = 0u;
        uint64_t allocatedTexelCount = 0u;
        uint64_t allocatedInterfaceTexelCount = 0u;
    };

    struct LayeredAtlasStats {
        uint32_t requestCount = 0u;
        uint32_t validRequestCount = 0u;
        uint32_t acceptedPacketCount = 0u;
        uint32_t acceptedIslandCount = 0u;
        uint32_t acceptedWorkCount = 0u;
        uint32_t multiWorkIslandCount = 0u;
        uint32_t overlapMergedWorkCount = 0u;
        uint32_t duplicatePacketCount = 0u;
        uint32_t invalidQualityCount = 0u;
        uint32_t invalidRectCount = 0u;
        uint32_t conflictingQualityPacketCount = 0u;
        uint32_t requestCapacityRejectedCount = 0u;
        uint64_t allocatedTexelCount = 0u;
        uint64_t allocatedInterfaceTexelCount = 0u;
        uint32_t activeTierMask = 0u;
        uint32_t maximumActiveInterfaceCount = 0u;
    };

    // Allocation-free M6.6 preparation contract. Each quality has an independent
    // atlas so one explicit Cinematic8 island cannot multiply Ordinary2 storage.
    // Same-tier work whose projected rectangles overlap is merged transitively
    // into one optical island, allowing nested/intersecting shells to share a
    // per-pixel interface stack while disjoint work stays separate. Pack order
    // is author priority followed by stable work identity. Duplicate packets
    // union before overlap grouping; one work identity cannot silently select
    // conflicting quality tiers.
    template <size_t MaximumRequests>
    class BoundedLayeredAtlasPlan final {
    public:
        static_assert(MaximumRequests > 0u);
        static_assert(MaximumRequests <= kOrdinary2MaximumWorkCount);

        LayeredAtlasPreparationStatus prepare(
            std::span<const LayeredAtlasRequest> requests,
            uint32_t sceneWidth, uint32_t sceneHeight) noexcept {
            reset();
            if (requests.empty()) return status_;
            if (sceneWidth == 0u || sceneHeight == 0u) {
                status_ = LayeredAtlasPreparationStatus::InvalidSceneExtent;
                return status_;
            }
            sceneWidthForValidation_ = sceneWidth;
            sceneHeightForValidation_ = sceneHeight;
            initializeTopology(sceneWidth, sceneHeight);
            if (topology_[0].capacityExtent.empty() &&
                topology_[1].capacityExtent.empty() &&
                topology_[2].capacityExtent.empty()) {
                status_ = LayeredAtlasPreparationStatus::AtlasUnavailable;
                return status_;
            }
            if (requests.size() > MaximumRequests) {
                stats_.requestCount = static_cast<uint32_t>((std::min)(
                    requests.size(), static_cast<size_t>(
                        (std::numeric_limits<uint32_t>::max)())));
                stats_.requestCapacityRejectedCount = stats_.requestCount;
                status_ =
                    LayeredAtlasPreparationStatus::RequestCapacityExceeded;
                return status_;
            }

            requestCount_ = requests.size();
            stats_.requestCount = static_cast<uint32_t>(requestCount_);
            for (size_t index = 0u; index < requestCount_; ++index) {
                requestOrder_[index] = static_cast<uint32_t>(index);
                decisions_[index] = {};
            }
            std::sort(requestOrder_.begin(),
                requestOrder_.begin() + requestCount_,
                [&requests](uint32_t lhsIndex, uint32_t rhsIndex) {
                    const LayeredAtlasRequest& lhs = requests[lhsIndex];
                    const LayeredAtlasRequest& rhs = requests[rhsIndex];
                    if (lhs.work != rhs.work) return lhs.work < rhs.work;
                    if (lhs.packetIndex != rhs.packetIndex)
                        return lhs.packetIndex < rhs.packetIndex;
                    return lhsIndex < rhsIndex;
                });

            buildGroups(requests, sceneWidth, sceneHeight);
            buildIslands();
            std::sort(islandOrder_.begin(), islandOrder_.begin() + islandCount_,
                [this](uint32_t lhsIndex, uint32_t rhsIndex) {
                    const Island& lhs = islands_[lhsIndex];
                    const Island& rhs = islands_[rhsIndex];
                    if (lhs.priority != rhs.priority)
                        return lhs.priority > rhs.priority;
                    return lhs.stableWork < rhs.stableWork;
                });

            std::array<Cursor, kLayeredQualityTierCount> cursors{};
            for (size_t ordered = 0u; ordered < islandCount_; ++ordered)
                packIsland(requests, islands_[islandOrder_[ordered]], cursors);
            status_ = LayeredAtlasPreparationStatus::Prepared;
            return status_;
        }

        [[nodiscard]] LayeredAtlasPreparationStatus status() const noexcept {
            return status_;
        }

        [[nodiscard]] std::span<const LayeredAtlasDecision> decisions()
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

        [[nodiscard]] std::span<const LayeredAtlasTierTopology> topology()
            const noexcept {
            return topology_;
        }

        [[nodiscard]] std::span<const LayeredAtlasTierStats> tierStats()
            const noexcept {
            return tierStats_;
        }

        [[nodiscard]] const LayeredAtlasStats& stats() const noexcept {
            return stats_;
        }

    private:
        struct Group {
            TransparentWorkIdentity work;
            Ordinary2ScreenRect screenRect;
            TransparencyQuality quality = TransparencyQuality::Ordinary2;
            int32_t priority = 0;
            uint32_t firstSortedRequest = 0u;
            uint32_t sortedRequestCount = 0u;
            uint32_t validRequestCount = 0u;
            uint32_t islandIndex = 0u;
        };

        struct Island {
            Ordinary2ScreenRect screenRect;
            TransparentWorkIdentity stableWork;
            TransparencyQuality quality = TransparencyQuality::Ordinary2;
            int32_t priority = 0;
            uint32_t workCount = 0u;
            uint32_t packetCount = 0u;
        };

        struct Cursor {
            uint32_t x = 0u;
            uint32_t y = 0u;
            uint32_t rowHeight = 0u;
        };

        void reset() noexcept {
            workTable_.reset();
            requestCount_ = 0u;
            groupCount_ = 0u;
            islandCount_ = 0u;
            topology_ = {};
            tierStats_ = {};
            stats_ = {};
            status_ = LayeredAtlasPreparationStatus::Empty;
        }

        void initializeTopology(uint32_t sceneWidth,
            uint32_t sceneHeight) noexcept {
            constexpr std::array qualities{
                TransparencyQuality::Ordinary2,
                TransparencyQuality::Hero4,
                TransparencyQuality::Cinematic8,
            };
            for (size_t index = 0u; index < qualities.size(); ++index) {
                const LayeredQualityTierContract tier =
                    layeredQualityTierContract(qualities[index]);
                topology_[index] = {
                    .quality = qualities[index],
                    .capacityExtent = layeredAtlasCapacityExtent(
                        sceneWidth, sceneHeight, qualities[index]),
                    .maximumInterfaceCount = tier.maximumInterfaceCount,
                };
            }
        }

        void buildGroups(std::span<const LayeredAtlasRequest> requests,
            uint32_t sceneWidth, uint32_t sceneHeight) noexcept {
            size_t sorted = 0u;
            while (sorted < requestCount_) {
                const size_t first = sorted;
                const TransparentWorkIdentity work =
                    requests[requestOrder_[sorted]].work;
                Group group{};
                group.work = work;
                group.firstSortedRequest = static_cast<uint32_t>(first);
                group.priority = (std::numeric_limits<int32_t>::min)();
                bool hasValidRequest = false;
                bool conflictingQuality = false;
                while (sorted < requestCount_ &&
                    requests[requestOrder_[sorted]].work == work) {
                    const uint32_t requestIndex = requestOrder_[sorted];
                    const LayeredAtlasRequest& request = requests[requestIndex];
                    ++group.sortedRequestCount;
                    const size_t tierIndex =
                        layeredQualityTierIndex(request.quality);
                    if (tierIndex == kLayeredQualityTierCount) {
                        decisions_[requestIndex].status =
                            LayeredAtlasDecisionStatus::InvalidQuality;
                        ++stats_.invalidQualityCount;
                    }
                    else if (!validOrdinary2ScreenRect(request.screenRect,
                            sceneWidth, sceneHeight)) {
                        decisions_[requestIndex].status =
                            LayeredAtlasDecisionStatus::InvalidScreenRect;
                        ++stats_.invalidRectCount;
                    }
                    else {
                        ++group.validRequestCount;
                        ++stats_.validRequestCount;
                        group.priority = (std::max)(group.priority,
                            request.priority);
                        if (!hasValidRequest) {
                            group.quality = request.quality;
                            group.screenRect = request.screenRect;
                            hasValidRequest = true;
                        }
                        else {
                            conflictingQuality = conflictingQuality ||
                                group.quality != request.quality;
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
                if (!hasValidRequest) continue;
                if (conflictingQuality) {
                    forEachValidRequest(requests, group,
                        [&](uint32_t requestIndex,
                            const LayeredAtlasRequest&) {
                            decisions_[requestIndex].status =
                                LayeredAtlasDecisionStatus::
                                    ConflictingWorkQuality;
                        });
                    stats_.conflictingQualityPacketCount +=
                        group.validRequestCount;
                    continue;
                }
                groups_[groupCount_] = group;
                ++groupCount_;
            }
        }

        [[nodiscard]] static constexpr bool screenRectsOverlap(
            Ordinary2ScreenRect lhs, Ordinary2ScreenRect rhs) noexcept {
            return lhs.minX < rhs.maxX && rhs.minX < lhs.maxX &&
                lhs.minY < rhs.maxY && rhs.minY < lhs.maxY;
        }

        [[nodiscard]] uint32_t findIslandRoot(uint32_t groupIndex) noexcept {
            uint32_t root = groupIndex;
            while (islandParents_[root] != root)
                root = islandParents_[root];
            while (islandParents_[groupIndex] != groupIndex) {
                const uint32_t next = islandParents_[groupIndex];
                islandParents_[groupIndex] = root;
                groupIndex = next;
            }
            return root;
        }

        void buildIslands() noexcept {
            constexpr uint32_t Unassigned =
                (std::numeric_limits<uint32_t>::max)();
            for (uint32_t groupIndex = 0u; groupIndex < groupCount_;
                ++groupIndex) {
                islandParents_[groupIndex] = groupIndex;
                rootToIsland_[groupIndex] = Unassigned;
            }
            for (uint32_t lhs = 0u; lhs < groupCount_; ++lhs) {
                for (uint32_t rhs = lhs + 1u; rhs < groupCount_; ++rhs) {
                    if (groups_[lhs].quality != groups_[rhs].quality ||
                        !screenRectsOverlap(groups_[lhs].screenRect,
                            groups_[rhs].screenRect)) {
                        continue;
                    }
                    const uint32_t lhsRoot = findIslandRoot(lhs);
                    const uint32_t rhsRoot = findIslandRoot(rhs);
                    if (lhsRoot == rhsRoot) continue;
                    const uint32_t root = (std::min)(lhsRoot, rhsRoot);
                    const uint32_t child = (std::max)(lhsRoot, rhsRoot);
                    islandParents_[child] = root;
                }
            }

            for (uint32_t groupIndex = 0u; groupIndex < groupCount_;
                ++groupIndex) {
                const uint32_t root = findIslandRoot(groupIndex);
                uint32_t islandIndex = rootToIsland_[root];
                Group& group = groups_[groupIndex];
                if (islandIndex == Unassigned) {
                    islandIndex = static_cast<uint32_t>(islandCount_);
                    rootToIsland_[root] = islandIndex;
                    islands_[islandIndex] = {
                        .screenRect = group.screenRect,
                        .stableWork = group.work,
                        .quality = group.quality,
                        .priority = group.priority,
                        .workCount = 1u,
                        .packetCount = group.validRequestCount,
                    };
                    islandOrder_[islandIndex] = islandIndex;
                    ++islandCount_;
                }
                else {
                    Island& island = islands_[islandIndex];
                    island.screenRect.minX = (std::min)(
                        island.screenRect.minX, group.screenRect.minX);
                    island.screenRect.minY = (std::min)(
                        island.screenRect.minY, group.screenRect.minY);
                    island.screenRect.maxX = (std::max)(
                        island.screenRect.maxX, group.screenRect.maxX);
                    island.screenRect.maxY = (std::max)(
                        island.screenRect.maxY, group.screenRect.maxY);
                    island.priority = (std::max)(
                        island.priority, group.priority);
                    island.stableWork = (std::min)(
                        island.stableWork, group.work);
                    ++island.workCount;
                    island.packetCount += group.validRequestCount;
                }
                group.islandIndex = islandIndex;
            }
        }

        template <typename Callback>
        void forEachIslandGroup(const Island& island,
            Callback&& callback) noexcept {
            const uint32_t islandIndex = static_cast<uint32_t>(
                &island - islands_.data());
            for (uint32_t groupIndex = 0u; groupIndex < groupCount_;
                ++groupIndex) {
                if (groups_[groupIndex].islandIndex == islandIndex)
                    callback(groups_[groupIndex]);
            }
        }

        void rejectIsland(std::span<const LayeredAtlasRequest> requests,
            const Island& island,
            LayeredAtlasDecisionStatus status) noexcept {
            forEachIslandGroup(island, [&](Group& group) {
                rejectGroup(requests, group, status);
            });
        }

        void packIsland(std::span<const LayeredAtlasRequest> requests,
            Island& island,
            std::array<Cursor, kLayeredQualityTierCount>& cursors) noexcept {
            const size_t tierIndex = layeredQualityTierIndex(island.quality);
            LayeredAtlasTierTopology& topology = topology_[tierIndex];
            LayeredAtlasTierStats& tierStats = tierStats_[tierIndex];
            if (topology.capacityExtent.empty()) {
                rejectIsland(requests, island,
                    LayeredAtlasDecisionStatus::TierAtlasUnavailable);
                tierStats.unavailableRejectedPacketCount +=
                    island.packetCount;
                return;
            }

            const uint64_t rawWidth = static_cast<uint64_t>(
                island.screenRect.maxX) - island.screenRect.minX;
            const uint64_t rawHeight = static_cast<uint64_t>(
                island.screenRect.maxY) - island.screenRect.minY;
            const uint64_t width = (rawWidth + kOrdinary2AtlasTileSize - 1u) &
                ~static_cast<uint64_t>(kOrdinary2AtlasTileSize - 1u);
            const uint64_t height =
                (rawHeight + kOrdinary2AtlasTileSize - 1u) &
                ~static_cast<uint64_t>(kOrdinary2AtlasTileSize - 1u);
            Cursor candidate = cursors[tierIndex];
            if (static_cast<uint64_t>(candidate.x) + width >
                topology.capacityExtent.width) {
                candidate.x = 0u;
                candidate.y += candidate.rowHeight;
                candidate.rowHeight = 0u;
            }
            if (width > topology.capacityExtent.width ||
                height > topology.capacityExtent.height ||
                static_cast<uint64_t>(candidate.y) + height >
                    topology.capacityExtent.height) {
                rejectIsland(requests, island,
                    LayeredAtlasDecisionStatus::TierAtlasCapacityExceeded);
                tierStats.capacityRejectedPacketCount +=
                    island.packetCount;
                return;
            }

            const int64_t offsetX = static_cast<int64_t>(
                island.screenRect.minX) - candidate.x;
            const int64_t offsetY = static_cast<int64_t>(
                island.screenRect.minY) - candidate.y;
            if (!validLayeredViewportOffset(offsetX, offsetY)) {
                rejectIsland(requests, island,
                    LayeredAtlasDecisionStatus::ViewportOffsetExceeded);
                tierStats.viewportOffsetRejectedPacketCount +=
                    island.packetCount;
                return;
            }

            forEachIslandGroup(island, [&](Group& group) {
                const LayeredWorkTableInsertResult inserted =
                    workTable_.insert(group.work);
                const LayeredAtlasPlacement base{
                    .screenRect = island.screenRect,
                    .quality = island.quality,
                    .atlasX = candidate.x,
                    .atlasY = candidate.y,
                    .width = static_cast<uint32_t>(width),
                    .height = static_cast<uint32_t>(height),
                    .viewportOffsetX = static_cast<int32_t>(offsetX),
                    .viewportOffsetY = static_cast<int32_t>(offsetY),
                    .workTableIndex = inserted.workTableIndex,
                };
                forEachValidRequest(requests, group,
                    [&](uint32_t requestIndex,
                        const LayeredAtlasRequest& request) {
                        decisions_[requestIndex].status =
                            LayeredAtlasDecisionStatus::Accepted;
                        decisions_[requestIndex].placement = base;
                        decisions_[requestIndex].placement.packetIndex =
                            request.packetIndex;
                    });
                tierStats.duplicatePacketCount +=
                    group.validRequestCount - 1u;
                stats_.duplicatePacketCount +=
                    group.validRequestCount - 1u;
            });

            candidate.x += static_cast<uint32_t>(width);
            candidate.rowHeight = (std::max)(candidate.rowHeight,
                static_cast<uint32_t>(height));
            cursors[tierIndex] = candidate;
            topology.active = true;
            const uint64_t texels = width * height;
            ++tierStats.acceptedIslandCount;
            tierStats.acceptedPacketCount += island.packetCount;
            tierStats.acceptedWorkCount += island.workCount;
            if (island.workCount > 1u) {
                ++tierStats.multiWorkIslandCount;
                tierStats.overlapMergedWorkCount += island.workCount - 1u;
            }
            tierStats.allocatedTexelCount += texels;
            tierStats.allocatedInterfaceTexelCount +=
                texels * topology.maximumInterfaceCount;
            ++stats_.acceptedIslandCount;
            stats_.acceptedPacketCount += island.packetCount;
            stats_.acceptedWorkCount += island.workCount;
            if (island.workCount > 1u) {
                ++stats_.multiWorkIslandCount;
                stats_.overlapMergedWorkCount += island.workCount - 1u;
            }
            stats_.allocatedTexelCount += texels;
            stats_.allocatedInterfaceTexelCount +=
                texels * topology.maximumInterfaceCount;
            stats_.activeTierMask |= 1u << tierIndex;
            stats_.maximumActiveInterfaceCount = (std::max)(
                stats_.maximumActiveInterfaceCount,
                topology.maximumInterfaceCount);
        }

        template <typename Callback>
        void forEachValidRequest(
            std::span<const LayeredAtlasRequest> requests,
            const Group& group, Callback&& callback) noexcept {
            for (uint32_t offset = 0u;
                offset < group.sortedRequestCount; ++offset) {
                const uint32_t requestIndex = requestOrder_[
                    group.firstSortedRequest + offset];
                const LayeredAtlasRequest& request = requests[requestIndex];
                if (layeredQualityTierIndex(request.quality) !=
                        kLayeredQualityTierCount &&
                    validOrdinary2ScreenRect(request.screenRect,
                        sceneWidthForValidation_, sceneHeightForValidation_)) {
                    callback(requestIndex, request);
                }
            }
        }

        void rejectGroup(std::span<const LayeredAtlasRequest> requests,
            const Group& group,
            LayeredAtlasDecisionStatus status) noexcept {
            forEachValidRequest(requests, group,
                [&](uint32_t requestIndex, const LayeredAtlasRequest&) {
                    decisions_[requestIndex].status = status;
                });
        }

        std::array<uint32_t, MaximumRequests> requestOrder_{};
        std::array<uint32_t, MaximumRequests> islandOrder_{};
        std::array<uint32_t, MaximumRequests> islandParents_{};
        std::array<uint32_t, MaximumRequests> rootToIsland_{};
        std::array<Group, MaximumRequests> groups_{};
        std::array<Island, MaximumRequests> islands_{};
        std::array<LayeredAtlasDecision, MaximumRequests> decisions_{};
        BoundedLayeredWorkTable<MaximumRequests> workTable_;
        std::array<LayeredAtlasTierTopology,
            kLayeredQualityTierCount> topology_{};
        std::array<LayeredAtlasTierStats,
            kLayeredQualityTierCount> tierStats_{};
        size_t requestCount_ = 0u;
        size_t groupCount_ = 0u;
        size_t islandCount_ = 0u;
        uint32_t sceneWidthForValidation_ = 0u;
        uint32_t sceneHeightForValidation_ = 0u;
        LayeredAtlasStats stats_{};
        LayeredAtlasPreparationStatus status_ =
            LayeredAtlasPreparationStatus::Empty;
    };

    using LayeredAtlasPlan =
        BoundedLayeredAtlasPlan<kOrdinary2MaximumWorkCount>;
    using LayeredRequestCollector =
        BoundedLayeredRequestCollector<kOrdinary2MaximumWorkCount>;
    using LayeredCaptureDrawPlan =
        BoundedLayeredCaptureDrawPlan<kOrdinary2MaximumWorkCount>;

} // namespace Iridium
