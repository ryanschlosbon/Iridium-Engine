#include "qualification/vulkan/VulkanReadbackAnalysis.h"

#include "renderer/color/SceneColor.h"
#include "renderer/transparency/LayeredGlass.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace Iridium {

    namespace {
        struct ReadbackBytes {
            const std::byte* bytes = nullptr;

            [[nodiscard]] uint32_t readUint(size_t base, size_t pixel) const {
                uint32_t value = 0u;
                std::memcpy(&value,
                    bytes + base + pixel * sizeof(uint32_t), sizeof(value));
                return value;
            }
            [[nodiscard]] float readFloat(size_t base, size_t pixel) const {
                float value = 0.0f;
                std::memcpy(&value,
                    bytes + base + pixel * sizeof(float), sizeof(value));
                return value;
            }
            [[nodiscard]] std::array<float, 4> readHalf4(size_t offset) const {
                std::array<uint16_t, 4> half{};
                std::memcpy(half.data(), bytes + offset, 8u);
                return { Color::halfToFloat(half[0]),
                    Color::halfToFloat(half[1]), Color::halfToFloat(half[2]),
                    Color::halfToFloat(half[3]) };
            }
        };

        [[nodiscard]] bool validDepth(float depth) noexcept {
            return std::isfinite(depth) && depth >= 0.0f && depth <= 1.0f;
        }

        [[nodiscard]] bool occupiedLocal(const std::array<float, 4>& local) noexcept {
            return local[0] != 0.0f || local[1] != 0.0f || local[2] != 0.0f ||
                local[3] != 0.0f;
        }

        [[nodiscard]] bool validLocal(const std::array<float, 4>& local) noexcept {
            return std::ranges::all_of(local,
                    [](float value) { return std::isfinite(value); }) &&
                local[0] >= 0.0f && local[1] >= 0.0f && local[2] >= 0.0f &&
                local[3] > 0.0f && local[3] <= 1.0f;
        }

        void requireBytes(std::span<const std::byte> bytes, size_t required,
            const char* what) {
            if (bytes.size() < required)
                throw std::runtime_error(what);
        }
    }

    FrameCapture convertFrameCaptureReadback(const FrameCaptureReadbackInfo& info,
        std::span<const std::byte> bytes) {
        const size_t pixelCount = static_cast<size_t>(info.width) *
            static_cast<size_t>(info.height);
        const bool sceneLinear = isSceneLinearCapturePoint(info.point);
        const size_t outputBytesPerPixel = info.halfFloatSource ? 16 : 4;
        const size_t byteCount = pixelCount * outputBytesPerPixel;
        requireBytes(bytes, pixelCount * (info.halfFloatSource ? 8 : 4),
            "A completed frame capture has invalid readback state.");
        FrameCapture completed{};
        completed.captureId = info.captureId;
        completed.width = info.width;
        completed.height = info.height;
        completed.rowPitchBytes = info.width *
            static_cast<uint32_t>(outputBytesPerPixel);
        completed.pixelFormat = info.pixelFormat;
        completed.colorDomain = sceneLinear
            ? FrameCaptureColorDomain::SceneLinearAcesCg
            : (info.halfFloatSource ? FrameCaptureColorDomain::DisplayLinearHdr
                : FrameCaptureColorDomain::DisplayEncodedSdr);
        completed.pixels.resize(byteCount);
        if (info.halfFloatSource) {
            for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
                uint16_t channels[4]{};
                std::memcpy(channels, bytes.data() + pixel * 8, sizeof(channels));
                float rgba[4] = { Color::halfToFloat(channels[0]),
                    Color::halfToFloat(channels[1]),
                    Color::halfToFloat(channels[2]),
                    Color::halfToFloat(channels[3]) };
                std::memcpy(completed.pixels.data() + pixel * 16,
                    rgba, sizeof(rgba));
            }
        }
        else {
            std::memcpy(completed.pixels.data(), bytes.data(), byteCount);
        }
        return completed;
    }

    Ordinary2CaptureValidationResult analyzeOrdinary2CaptureReadback(
        const Ordinary2ReadbackInfo& info, std::span<const std::byte> bytes) {
        constexpr uint32_t OrientationBit = 0x80000000u;
        constexpr uint32_t WorkMask = kLayeredInterfaceWorkMask;
        Ordinary2CaptureValidationResult result{};
        result.validationId = info.validationId;
        result.atlasWidth = info.width;
        result.atlasHeight = info.height;
        result.expectedDrawCount = info.expectedDrawCount;
        result.workItemCount = info.workItemCount;
        result.inspectedPixelCount =
            static_cast<uint64_t>(info.width) * info.height;
        const size_t pixelCount = static_cast<size_t>(
            result.inspectedPixelCount);
        const size_t imageBytes = pixelCount * sizeof(uint32_t);
        requireBytes(bytes, imageBytes * 4u + pixelCount * 8u,
            "Ordinary2 readback is smaller than its layout");
        const ReadbackBytes source{ bytes.data() };
        float minimumDelta = (std::numeric_limits<float>::max)();
        float maximumDelta = 0.0f;
        float minimumLocalAlpha = (std::numeric_limits<float>::max)();
        float maximumLocalAlpha = 0.0f;
        for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
            const uint32_t entryIdentity = source.readUint(0u, pixel);
            const float entryDepth = source.readFloat(imageBytes, pixel);
            const uint32_t exitIdentity = source.readUint(imageBytes * 2u, pixel);
            const float exitDepth = source.readFloat(imageBytes * 3u, pixel);
            const bool hasEntry = entryIdentity != 0u;
            const bool hasExit = exitIdentity != 0u;
            const uint32_t entryWork = entryIdentity & WorkMask;
            const uint32_t exitWork = exitIdentity & WorkMask;

            if (hasEntry) {
                ++result.entryPixelCount;
                if ((entryIdentity & OrientationBit) != 0u)
                    ++result.invalidOrientationPixelCount;
                if (entryWork == 0u || entryWork > info.workItemCount)
                    ++result.invalidWorkIndexPixelCount;
                if (!validDepth(entryDepth))
                    ++result.invalidDepthPixelCount;
            }
            if (hasExit) {
                ++result.exitPixelCount;
                if ((exitIdentity & OrientationBit) == 0u)
                    ++result.invalidOrientationPixelCount;
                if (exitWork == 0u || exitWork > info.workItemCount)
                    ++result.invalidWorkIndexPixelCount;
                if (!validDepth(exitDepth))
                    ++result.invalidDepthPixelCount;
                if (!hasEntry) {
                    ++result.unpairedExitPixelCount;
                }
                else {
                    ++result.pairedPixelCount;
                    if (entryWork != exitWork)
                        ++result.workMismatchPixelCount;
                    if (validDepth(entryDepth) && validDepth(exitDepth)) {
                        if (!(exitDepth > entryDepth)) {
                            ++result.nonIncreasingDepthPixelCount;
                        }
                        else {
                            const float delta = exitDepth - entryDepth;
                            minimumDelta = (std::min)(minimumDelta, delta);
                            maximumDelta = (std::max)(maximumDelta, delta);
                        }
                    }
                }
            }
            if (hasEntry && !hasExit)
                ++result.entryOnlyPixelCount;

            const std::array<float, 4> local =
                source.readHalf4(imageBytes * 4u + pixel * 8u);
            if (occupiedLocal(local)) {
                ++result.localColorPixelCount;
                if (!validLocal(local)) {
                    ++result.localColorInvalidPixelCount;
                }
                else {
                    minimumLocalAlpha = (std::min)(minimumLocalAlpha, local[3]);
                    maximumLocalAlpha = (std::max)(maximumLocalAlpha, local[3]);
                }
            }
        }
        if (minimumDelta != (std::numeric_limits<float>::max)()) {
            result.minimumPairedDepthDelta = minimumDelta;
            result.maximumPairedDepthDelta = maximumDelta;
        }
        if (minimumLocalAlpha != (std::numeric_limits<float>::max)()) {
            result.minimumLocalAlpha = minimumLocalAlpha;
            result.maximumLocalAlpha = maximumLocalAlpha;
        }
        return result;
    }

    DeepLayeredCaptureValidationResult analyzeDeepLayeredCaptureReadback(
        const DeepLayeredReadbackInfo& info, std::span<const std::byte> bytes) {
        constexpr uint32_t OrientationBit = 0x80000000u;
        constexpr uint32_t WorkMask = kDeepLayeredWorkMask;
        if (info.interfaceCount > kMaximumLayeredInterfaceCount)
            throw std::invalid_argument(
                "Deep layered readback exceeds the interface limit");
        DeepLayeredCaptureValidationResult result{};
        result.validationId = info.validationId;
        result.quality = info.quality;
        result.atlasWidth = info.width;
        result.atlasHeight = info.height;
        result.interfaceCount = info.interfaceCount;
        result.expectedDrawCount = info.expectedDrawCount;
        result.sceneResolveDrawCount = info.sceneResolveDrawCount;
        result.compatibilityForwardDrawCount =
            info.compatibilityForwardDrawCount;
        result.workItemCount = info.workItemCount;
        result.inspectedPixelCount =
            static_cast<uint64_t>(info.width) * info.height;
        const size_t pixelCount = static_cast<size_t>(
            result.inspectedPixelCount);
        const size_t imageBytes = pixelCount * sizeof(uint32_t);
        const size_t localColorOffset = imageBytes * info.interfaceCount * 2u;
        const uint32_t tileWidth = (info.width +
            kDeepLayeredEarlyTerminationTileSize - 1u) /
            kDeepLayeredEarlyTerminationTileSize;
        const uint32_t tileHeight = (info.height +
            kDeepLayeredEarlyTerminationTileSize - 1u) /
            kDeepLayeredEarlyTerminationTileSize;
        const size_t tileImageBytes = static_cast<size_t>(tileWidth) *
            tileHeight * sizeof(uint32_t);
        const size_t tileBaseOffset = localColorOffset + pixelCount * 8u;
        requireBytes(bytes, tileBaseOffset + tileImageBytes * info.interfaceCount,
            "Deep layered readback is smaller than its layout");
        const ReadbackBytes source{ bytes.data() };
        float minimumDelta = (std::numeric_limits<float>::max)();
        float maximumDelta = 0.0f;
        float minimumLocalAlpha = (std::numeric_limits<float>::max)();
        float maximumLocalAlpha = 0.0f;

        for (size_t pixel = 0u; pixel < pixelCount; ++pixel) {
            std::array<uint32_t, kMaximumLayeredInterfaceCount> openWorks{};
            uint32_t openCount = 0u;
            uint32_t observedCount = 0u;
            uint32_t maximumOpenCount = 0u;
            uint32_t pairCount = 0u;
            uint32_t lastIdentity = 0u;
            bool crossingPair = false;
            bool seenEmpty = false;
            bool pixelInvalid = false;
            bool hasPreviousDepth = false;
            float previousDepth = 0.0f;
            for (uint32_t interfaceIndex = 0u;
                interfaceIndex < info.interfaceCount; ++interfaceIndex) {
                const size_t identityOffset = imageBytes * (interfaceIndex * 2u);
                const size_t depthOffset = identityOffset + imageBytes;
                const uint32_t identity = source.readUint(identityOffset, pixel);
                if (identity == 0u) {
                    seenEmpty = true;
                    continue;
                }
                ++result.interfacePixelCounts[interfaceIndex];
                ++observedCount;
                lastIdentity = identity;
                if (seenEmpty) {
                    ++result.interfaceGapPixelCount;
                    pixelInvalid = true;
                }
                const uint32_t work = identity & WorkMask;
                if (work == 0u || work > info.workItemCount) {
                    ++result.invalidWorkIndexPixelCount;
                    pixelInvalid = true;
                }
                const float depth = source.readFloat(depthOffset, pixel);
                if (!validDepth(depth)) {
                    ++result.invalidDepthPixelCount;
                    pixelInvalid = true;
                }
                else if (hasPreviousDepth) {
                    if (!(depth > previousDepth)) {
                        ++result.nonIncreasingDepthPixelCount;
                        pixelInvalid = true;
                    }
                    else {
                        const float delta = depth - previousDepth;
                        minimumDelta = (std::min)(minimumDelta, delta);
                        maximumDelta = (std::max)(maximumDelta, delta);
                    }
                }
                if (validDepth(depth)) {
                    previousDepth = depth;
                    hasPreviousDepth = true;
                }

                const bool exit = (identity & OrientationBit) != 0u;
                if (!exit) {
                    const bool duplicate = std::find(
                        openWorks.begin(), openWorks.begin() + openCount,
                        work) != openWorks.begin() + openCount;
                    if (duplicate || openCount >= openWorks.size()) {
                        ++result.duplicateEntryPixelCount;
                        pixelInvalid = true;
                    }
                    else {
                        openWorks[openCount++] = work;
                        maximumOpenCount = (std::max)(maximumOpenCount,
                            openCount);
                    }
                }
                else {
                    uint32_t match = openCount;
                    while (match > 0u && openWorks[match - 1u] != work) {
                        --match;
                    }
                    if (match == 0u) {
                        ++result.unmatchedExitPixelCount;
                        pixelInvalid = true;
                    }
                    else {
                        const uint32_t matchIndex = match - 1u;
                        // Closing something other than the most recently
                        // opened work proves a valid crossing sequence:
                        // Entry(A), Entry(B), Exit(A), Exit(B).
                        crossingPair |= matchIndex + 1u != openCount;
                        for (uint32_t move = matchIndex + 1u;
                            move < openCount; ++move) {
                            openWorks[move - 1u] = openWorks[move];
                        }
                        --openCount;
                        ++pairCount;
                    }
                }
            }
            result.maximumObservedInterfaceCount = (std::max)(
                result.maximumObservedInterfaceCount, observedCount);
            if (openCount != 0u) {
                if (observedCount == info.interfaceCount) {
                    // A topology-validated closed workload that fills the
                    // tier while volumes remain open is a saturated exact
                    // prefix, not malformed capture. The unmatched entry
                    // and uncaptured entry surfaces are evaluated by the
                    // bounded residual material path.
                    ++result.saturatedResidualPixelCount;
                }
                else {
                    ++result.unclosedEntryPixelCount;
                    pixelInvalid = true;
                }
            }
            const bool paired = !pixelInvalid && pairCount != 0u;
            if (paired) {
                ++result.pairedPixelCount;
                if (observedCount >= 4u && maximumOpenCount >= 2u)
                    ++result.nestedFourInterfacePixelCount;
                if (crossingPair)
                    ++result.crossingPairPixelCount;
                if (observedCount < info.interfaceCount &&
                    deepLayeredOpenCount(lastIdentity) == 0u &&
                    deepLayeredTransmissionQuantized(lastIdentity) <=
                        kDeepLayeredTerminationThresholdQuantized) {
                    ++result.earlyTerminatedPixelCount;
                }
            }

            const std::array<float, 4> local =
                source.readHalf4(localColorOffset + pixel * 8u);
            if (occupiedLocal(local)) {
                ++result.localColorPixelCount;
                if (!validLocal(local) || !paired) {
                    ++result.localColorInvalidPixelCount;
                }
                else {
                    minimumLocalAlpha = (std::min)(minimumLocalAlpha, local[3]);
                    maximumLocalAlpha = (std::max)(maximumLocalAlpha, local[3]);
                }
            }
        }

        for (uint32_t interfaceIndex = 0u;
            interfaceIndex < info.interfaceCount; ++interfaceIndex) {
            if (!deepLayeredTerminationInterface(interfaceIndex,
                    info.interfaceCount)) {
                continue;
            }
            const size_t identityOffset = imageBytes * (interfaceIndex * 2u);
            const size_t maskOffset = tileBaseOffset +
                tileImageBytes * interfaceIndex;
            for (uint32_t tileY = 0u; tileY < tileHeight; ++tileY) {
                for (uint32_t tileX = 0u; tileX < tileWidth; ++tileX) {
                    const size_t tileIndex = static_cast<size_t>(tileY) *
                        tileWidth + tileX;
                    if (source.readUint(maskOffset, tileIndex) == 0u)
                        continue;
                    bool occupied = false;
                    const uint32_t beginX = tileX *
                        kDeepLayeredEarlyTerminationTileSize;
                    const uint32_t beginY = tileY *
                        kDeepLayeredEarlyTerminationTileSize;
                    const uint32_t endX = (std::min)(beginX +
                        kDeepLayeredEarlyTerminationTileSize, info.width);
                    const uint32_t endY = (std::min)(beginY +
                        kDeepLayeredEarlyTerminationTileSize, info.height);
                    for (uint32_t y = beginY; y < endY && !occupied; ++y) {
                        for (uint32_t x = beginX; x < endX; ++x) {
                            const size_t pixel = static_cast<size_t>(y) *
                                info.width + x;
                            if ((source.readUint(identityOffset, pixel) &
                                    kDeepLayeredWorkMask) != 0u) {
                                occupied = true;
                                break;
                            }
                        }
                    }
                    if (occupied) {
                        ++result.terminatedOccupiedTileCounts[interfaceIndex];
                        ++result.terminatedOccupiedTileCount;
                    }
                }
            }
        }
        if (minimumDelta != (std::numeric_limits<float>::max)()) {
            result.minimumDepthDelta = minimumDelta;
            result.maximumDepthDelta = maximumDelta;
        }
        if (minimumLocalAlpha != (std::numeric_limits<float>::max)()) {
            result.minimumLocalAlpha = minimumLocalAlpha;
            result.maximumLocalAlpha = maximumLocalAlpha;
        }
        return result;
    }

    DepthPyramidCaptureValidationResult analyzeDepthPyramidCaptureReadback(
        const DepthPyramidReadbackInfo& info, std::span<const std::byte> bytes) {
        DepthPyramidCaptureValidationResult result{};
        result.validationId = info.validationId;
        result.extent = { info.width, info.height };
        result.mipCount = info.mipCount;
        result.sourceTexelCount = static_cast<uint64_t>(info.width) * info.height;
        requireBytes(bytes, static_cast<size_t>(result.sourceTexelCount) *
                sizeof(float),
            "Depth-pyramid readback is smaller than its source image");
        std::vector<float> source(static_cast<size_t>(result.sourceTexelCount));
        std::memcpy(source.data(), bytes.data(), source.size() * sizeof(float));
        result.invalidSourceTexelCount = static_cast<uint64_t>(
            std::ranges::count_if(source, [](float value) {
                return !std::isfinite(value) || value < 0.0f || value > 1.0f;
            }));
        DepthPyramidReference reference;
        reference.build(result.extent,
            DeviceDepthConvention::ForwardZeroToOne, source);
        size_t byteOffset = source.size() * sizeof(float);
        bool firstMismatch = true;
        for (uint32_t mip = 0; mip < info.mipCount; ++mip) {
            const std::span<const float> expected = reference.mip(mip);
            requireBytes(bytes, byteOffset + expected.size() * sizeof(float),
                "Depth-pyramid readback is smaller than its mip chain");
            for (size_t texel = 0; texel < expected.size(); ++texel) {
                float observed = 0.0f;
                std::memcpy(&observed,
                    bytes.data() + byteOffset + texel * sizeof(float),
                    sizeof(float));
                ++result.pyramidTexelCount;
                if (std::bit_cast<uint32_t>(observed) !=
                    std::bit_cast<uint32_t>(expected[texel])) {
                    ++result.mismatchTexelCount;
                    if (firstMismatch) {
                        result.firstMismatchMip = mip;
                        result.firstMismatchTexel = texel;
                        firstMismatch = false;
                    }
                    if (std::isfinite(observed) &&
                        std::isfinite(expected[texel])) {
                        result.maximumAbsoluteError = (std::max)(
                            result.maximumAbsoluteError,
                            std::abs(observed - expected[texel]));
                    }
                    else {
                        result.maximumAbsoluteError =
                            (std::numeric_limits<float>::infinity)();
                    }
                }
            }
            byteOffset += expected.size() * sizeof(float);
        }
        return result;
    }

} // namespace Iridium
