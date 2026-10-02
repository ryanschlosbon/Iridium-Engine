#include "DepthPyramid.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Iridium {
namespace {

    [[nodiscard]] uint64_t texelCount(DepthPyramidExtent extent) noexcept {
        return static_cast<uint64_t>(extent.width) * extent.height;
    }

    [[nodiscard]] float farDepth(DeviceDepthConvention convention) noexcept {
        return convention == DeviceDepthConvention::ForwardZeroToOne ? 1.0f : 0.0f;
    }

    [[nodiscard]] float sanitizeDepth(float depth,
        DeviceDepthConvention convention) noexcept {
        if (!std::isfinite(depth) || depth < 0.0f || depth > 1.0f)
            return farDepth(convention);
        return depth;
    }

    [[nodiscard]] float combineFarthest(float a, float b,
        DeviceDepthConvention convention) noexcept {
        return convention == DeviceDepthConvention::ForwardZeroToOne
            ? (std::max)(a, b) : (std::min)(a, b);
    }

    [[nodiscard]] uint32_t proportionalFloor(uint32_t coordinate,
        uint32_t targetExtent, uint32_t sourceExtent) noexcept {
        return static_cast<uint32_t>((static_cast<uint64_t>(coordinate) *
            targetExtent) / sourceExtent);
    }

    [[nodiscard]] uint32_t proportionalCeil(uint32_t coordinate,
        uint32_t targetExtent, uint32_t sourceExtent) noexcept {
        return static_cast<uint32_t>((static_cast<uint64_t>(coordinate) *
            targetExtent + sourceExtent - 1u) / sourceExtent);
    }

} // namespace

uint32_t depthPyramidMipCount(DepthPyramidExtent extent) noexcept {
    if (!extent.valid()) return 0;
    uint32_t levels = 1;
    while (extent.width > 1 || extent.height > 1) {
        extent.width = (std::max)(extent.width >> 1u, 1u);
        extent.height = (std::max)(extent.height >> 1u, 1u);
        ++levels;
    }
    return levels;
}

DepthPyramidExtent depthPyramidMipExtent(DepthPyramidExtent extent,
    uint32_t mipLevel) noexcept {
    if (!extent.valid()) return {};
    while (mipLevel-- != 0 && (extent.width > 1u || extent.height > 1u)) {
        extent.width = (std::max)(extent.width >> 1u, 1u);
        extent.height = (std::max)(extent.height >> 1u, 1u);
    }
    return extent;
}

DepthPyramidHistoryDecision evaluateDepthPyramidHistory(
    const DepthPyramidHistoryInput& input) noexcept {
    DepthPyramidHistoryDecision result;
    const auto reject = [&](DepthPyramidHistoryRejection reason) {
        result.rejection = reason;
        return result;
    };
    if (!input.enabled)
        return reject(DepthPyramidHistoryRejection::Disabled);
    if (!input.historyAvailable)
        return reject(DepthPyramidHistoryRejection::HistoryUnavailable);
    if (!input.currentExtent.valid() || !input.historyExtent.valid())
        return reject(DepthPyramidHistoryRejection::InvalidExtent);
    if (input.currentExtent != input.historyExtent)
        return reject(DepthPyramidHistoryRejection::ExtentMismatch);
    const auto validOwner = [](const DepthPyramidHistoryOwner& owner) {
        return owner.viewIdentity != 0 && owner.sceneEpoch != 0 && owner.depthContentRevision != 0;
    };
    if (!validOwner(input.currentOwner) || !validOwner(input.historyOwner))
        return reject(DepthPyramidHistoryRejection::InvalidOwnership);
    if (input.currentOwner.viewIdentity != input.historyOwner.viewIdentity)
        return reject(DepthPyramidHistoryRejection::ViewMismatch);
    if (input.currentOwner.sceneEpoch != input.historyOwner.sceneEpoch)
        return reject(DepthPyramidHistoryRejection::SceneMismatch);
    const auto validConvention = [](DeviceDepthConvention convention) {
        return convention == DeviceDepthConvention::ForwardZeroToOne ||
            convention == DeviceDepthConvention::ReverseZeroToOne;
    };
    if (!validConvention(input.currentConvention) || !validConvention(input.historyConvention) ||
        input.currentConvention != input.historyConvention)
        return reject(DepthPyramidHistoryRejection::DepthConventionMismatch);
    if (input.historyFrameSerial == 0 || input.currentFrameSerial <= input.historyFrameSerial ||
        input.currentFrameSerial - input.historyFrameSerial != 1u)
        return reject(DepthPyramidHistoryRejection::StaleHistory);
    if (!input.projectionValid)
        return reject(DepthPyramidHistoryRejection::InvalidProjection);
    if (input.cameraCut || input.currentOwner.resetRevision != input.historyOwner.resetRevision)
        return reject(DepthPyramidHistoryRejection::CameraCut);
    if (input.currentOwner.projectionRevision != input.historyOwner.projectionRevision)
        return reject(DepthPyramidHistoryRejection::ProjectionChanged);
    if (input.currentOwner.depthContentRevision != input.historyOwner.depthContentRevision)
        return reject(DepthPyramidHistoryRejection::DepthContentChanged);
    if (input.rapidMotion)
        return reject(DepthPyramidHistoryRejection::RapidMotion);
    if (input.nearPlaneIntersection)
        return reject(DepthPyramidHistoryRejection::NearPlaneIntersection);
    if (input.newlyResident)
        return reject(DepthPyramidHistoryRejection::NewlyResident);
    if (input.largeTransformChange)
        return reject(DepthPyramidHistoryRejection::LargeTransformChange);
    result.rejection = DepthPyramidHistoryRejection::None;
    result.eligible = true;
    return result;
}

void DepthPyramidHistoryPublicationTracker::reset(uint32_t viewCount,
    uint32_t frameSlotCount, uint64_t imageGeneration) {
    if (viewCount == 0u || viewCount > MaximumViews ||
        frameSlotCount == 0u || frameSlotCount > MaximumFrameSlots ||
        imageGeneration == 0u) {
        throw std::invalid_argument(
            "Invalid depth-pyramid history tracker configuration");
    }
    clear();
    viewCount_ = viewCount;
    frameSlotCount_ = frameSlotCount;
    imageGeneration_ = imageGeneration;
}

void DepthPyramidHistoryPublicationTracker::clear() noexcept {
    published_ = {};
    queued_ = {};
    pending_ = {};
    latestScheduledSerial_ = {};
    viewCount_ = 0u;
    frameSlotCount_ = 0u;
    imageGeneration_ = 0u;
}

void DepthPyramidHistoryPublicationTracker::schedule(uint32_t frameSlot,
    uint32_t view, const DepthPyramidHistoryOwner& owner,
    DepthPyramidExtent extent, DeviceDepthConvention convention,
    uint64_t submissionSerial) {
    if (frameSlot >= frameSlotCount_ || view >= viewCount_ ||
        !extent.valid() || submissionSerial == 0u ||
        imageGeneration_ == 0u) {
        throw std::invalid_argument(
            "Invalid depth-pyramid history publication");
    }
    if (pending_[frameSlot].valid) {
        throw std::logic_error(
            "Depth-pyramid frame slot was rescheduled before completion");
    }
    pending_[frameSlot] = {
        .publication = {
            .owner = owner,
            .extent = extent,
            .convention = convention,
            .submissionSerial = submissionSerial,
            .imageGeneration = imageGeneration_,
            .available = true,
        },
        .view = view,
        .valid = true,
    };
    queued_[view] = pending_[frameSlot].publication;
    latestScheduledSerial_[view] = submissionSerial;
}

void DepthPyramidHistoryPublicationTracker::complete(uint32_t frameSlot,
    uint64_t completedSubmissionSerial) noexcept {
    if (frameSlot >= frameSlotCount_) return;
    const Pending pending = pending_[frameSlot];
    if (!pending.valid ||
        pending.publication.submissionSerial > completedSubmissionSerial) {
        return;
    }
    pending_[frameSlot] = {};
    if (pending.view >= viewCount_ ||
        pending.publication.imageGeneration != imageGeneration_ ||
        latestScheduledSerial_[pending.view] !=
            pending.publication.submissionSerial) {
        return;
    }
    published_[pending.view] = pending.publication;
}

const DepthPyramidHistoryPublication&
DepthPyramidHistoryPublicationTracker::queued(uint32_t view) const {
    if (view >= viewCount_)
        throw std::out_of_range("Depth-pyramid queued-history view index");
    return queued_[view];
}

const DepthPyramidHistoryPublication&
DepthPyramidHistoryPublicationTracker::published(uint32_t view) const {
    if (view >= viewCount_)
        throw std::out_of_range("Depth-pyramid history view index");
    return published_[view];
}

void DepthPyramidReference::build(DepthPyramidExtent extent,
    DeviceDepthConvention convention,
    std::span<const float> baseDeviceDepth) {
    if (!extent.valid())
        throw std::invalid_argument("Depth pyramid extent must be nonzero");
    const uint64_t baseCount = texelCount(extent);
    if (baseCount != baseDeviceDepth.size())
        throw std::invalid_argument("Depth pyramid source size does not match extent");

    const uint32_t levels = depthPyramidMipCount(extent);
    uint64_t totalCount = 0;
    for (uint32_t mipLevel = 0; mipLevel < levels; ++mipLevel)
        totalCount += texelCount(depthPyramidMipExtent(extent, mipLevel));
    if (totalCount > std::numeric_limits<uint32_t>::max())
        throw std::length_error("Depth pyramid reference exceeds addressable storage");

    extent_ = extent;
    convention_ = convention;
    mipOffsets_.clear();
    depth_.clear();
    mipOffsets_.reserve(levels);
    depth_.resize(static_cast<size_t>(totalCount));

    uint32_t offset = 0;
    for (uint32_t mipLevel = 0; mipLevel < levels; ++mipLevel) {
        mipOffsets_.push_back(offset);
        offset += static_cast<uint32_t>(texelCount(
            depthPyramidMipExtent(extent_, mipLevel)));
    }
    for (uint32_t index = 0; index < baseCount; ++index)
        depth_[index] = sanitizeDepth(baseDeviceDepth[index], convention_);

    for (uint32_t mipLevel = 1; mipLevel < levels; ++mipLevel) {
        const DepthPyramidExtent sourceExtent =
            depthPyramidMipExtent(extent_, mipLevel - 1u);
        const DepthPyramidExtent outputExtent =
            depthPyramidMipExtent(extent_, mipLevel);
        const uint32_t sourceOffset = mipOffsets_[mipLevel - 1u];
        const uint32_t outputOffset = mipOffsets_[mipLevel];
        for (uint32_t y = 0; y < outputExtent.height; ++y) {
            const uint32_t sourceY0 = proportionalFloor(y,
                sourceExtent.height, outputExtent.height);
            const uint32_t sourceY1 = proportionalFloor(y + 1u,
                sourceExtent.height, outputExtent.height);
            for (uint32_t x = 0; x < outputExtent.width; ++x) {
                const uint32_t sourceX0 = proportionalFloor(x,
                    sourceExtent.width, outputExtent.width);
                const uint32_t sourceX1 = proportionalFloor(x + 1u,
                    sourceExtent.width, outputExtent.width);
                float reduced = convention_ ==
                        DeviceDepthConvention::ForwardZeroToOne
                    ? 0.0f : 1.0f;
                for (uint32_t sourceY = sourceY0; sourceY < sourceY1; ++sourceY) {
                    for (uint32_t sourceX = sourceX0; sourceX < sourceX1; ++sourceX) {
                        reduced = combineFarthest(reduced,
                            depth_[sourceOffset + sourceY * sourceExtent.width +
                                sourceX], convention_);
                    }
                }
                depth_[outputOffset + y * outputExtent.width + x] = reduced;
            }
        }
    }
}

void DepthPyramidReference::clear() noexcept {
    extent_ = {};
    mipOffsets_.clear();
    depth_.clear();
}

DepthPyramidProjectionResult projectDepthPyramidBounds(
    const DepthPyramidProjectionInput& input) noexcept {
    DepthPyramidProjectionResult result;
    const auto reject = [&](DepthPyramidProjectionRejection reason) {
        result.rejection = reason;
        return result;
    };
    if (!input.extent.valid()) return reject(DepthPyramidProjectionRejection::InvalidExtent);
    if ((input.convention != DeviceDepthConvention::ForwardZeroToOne &&
            input.convention != DeviceDepthConvention::ReverseZeroToOne) ||
        !std::isfinite(input.guardPixels) || input.guardPixels < 1.0f ||
        !std::isfinite(input.minimumFootprintPixels) || input.minimumFootprintPixels < 1.0f ||
        !std::isfinite(input.depthBias) || input.depthBias < 0.0f)
        return reject(DepthPyramidProjectionRejection::InvalidSettings);
    for (unsigned axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(input.minimumWorld[axis]) || !std::isfinite(input.maximumWorld[axis]) ||
            input.minimumWorld[axis] > input.maximumWorld[axis])
            return reject(DepthPyramidProjectionRejection::InvalidBounds);
    }
    for (float value : input.worldToClip)
        if (!std::isfinite(value)) return reject(DepthPyramidProjectionRejection::InvalidProjection);

    double minX = 1.0, minY = 1.0, maxX = -1.0, maxY = -1.0;
    const bool forward = input.convention == DeviceDepthConvention::ForwardZeroToOne;
    double nearest = forward ? 1.0 : 0.0;
    for (unsigned corner = 0; corner < 8; ++corner) {
        std::array<double, 4> world{}, clip{};
        for (unsigned axis = 0; axis < 3; ++axis)
            world[axis] = (corner & (1u << axis)) ? input.maximumWorld[axis] : input.minimumWorld[axis];
        world[3] = 1.0;
        for (unsigned row = 0; row < 4; ++row)
            for (unsigned column = 0; column < 4; ++column)
                clip[row] += static_cast<double>(input.worldToClip[row * 4 + column]) * world[column];
        if (!std::isfinite(clip[3]) || clip[3] <= 0.000001)
            return reject(DepthPyramidProjectionRejection::ClipPlaneIntersection);
        const double x = clip[0] / clip[3], y = clip[1] / clip[3], z = clip[2] / clip[3];
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
            return reject(DepthPyramidProjectionRejection::InvalidProjection);
        // Do not invent clipped extrema for a box crossing either depth plane.
        if (z <= 0.0 || z >= 1.0)
            return reject(DepthPyramidProjectionRejection::ClipPlaneIntersection);
        minX = (std::min)(minX, x); minY = (std::min)(minY, y);
        maxX = (std::max)(maxX, x); maxY = (std::max)(maxY, y);
        nearest = forward ? (std::min)(nearest, z) : (std::max)(nearest, z);
    }
    if (maxX <= -1.0 || minX >= 1.0 || maxY <= -1.0 || minY >= 1.0)
        return reject(DepthPyramidProjectionRejection::OutsideView);
    const double x0 = (std::clamp)(minX, -1.0, 1.0) * .5 + .5;
    const double y0 = (std::clamp)(minY, -1.0, 1.0) * .5 + .5;
    const double x1 = (std::clamp)(maxX, -1.0, 1.0) * .5 + .5;
    const double y1 = (std::clamp)(maxY, -1.0, 1.0) * .5 + .5;
    if ((x1 - x0) * input.extent.width <= input.minimumFootprintPixels ||
        (y1 - y0) * input.extent.height <= input.minimumFootprintPixels)
        return reject(DepthPyramidProjectionRejection::SmallBounds);
    const auto lower = [&](double uv, uint32_t extent) {
        return static_cast<uint32_t>((std::max)(0.0, std::floor(uv * extent - input.guardPixels)));
    };
    const auto upper = [&](double uv, uint32_t extent) {
        return static_cast<uint32_t>((std::min)(static_cast<double>(extent), std::ceil(uv * extent + input.guardPixels)));
    };
    result.query = {lower(x0, input.extent.width), lower(y0, input.extent.height),
        upper(x1, input.extent.width), upper(y1, input.extent.height),
        std::nextafter(static_cast<float>(nearest), forward ? 0.0f : 1.0f), input.depthBias};
    result.rejection = DepthPyramidProjectionRejection::None;
    result.eligible = true;
    return result;
}

std::span<const float> DepthPyramidReference::mip(
    uint32_t mipLevel) const noexcept {
    if (mipLevel >= mipOffsets_.size()) return {};
    const uint32_t begin = mipOffsets_[mipLevel];
    const uint32_t end = mipLevel + 1u < mipOffsets_.size()
        ? mipOffsets_[mipLevel + 1u]
        : static_cast<uint32_t>(depth_.size());
    return std::span<const float>(depth_).subspan(begin, end - begin);
}

DepthPyramidOcclusionResult DepthPyramidReference::test(
    const DepthPyramidOcclusionQuery& query) const noexcept {
    DepthPyramidOcclusionResult result;
    result.farthestOccluderDepth = farDepth(convention_);
    if (!extent_.valid() || mipOffsets_.empty() ||
        query.minimumX >= query.maximumX ||
        query.minimumY >= query.maximumY ||
        query.maximumX > extent_.width || query.maximumY > extent_.height ||
        !std::isfinite(query.nearestDepth) || query.nearestDepth < 0.0f ||
        query.nearestDepth > 1.0f || !std::isfinite(query.depthBias) ||
        query.depthBias < 0.0f)
        return result;

    uint32_t firstX = query.minimumX;
    uint32_t firstY = query.minimumY;
    uint32_t lastX = query.maximumX - 1u;
    uint32_t lastY = query.maximumY - 1u;
    uint32_t selectedMip = 0;
    DepthPyramidExtent selectedExtent = extent_;
    for (uint32_t mipLevel = 0; mipLevel < mipOffsets_.size(); ++mipLevel) {
        const DepthPyramidExtent candidateExtent =
            depthPyramidMipExtent(extent_, mipLevel);
        // Invert each reduction's integer partition, not a direct base-to-mip
        // ratio: rounded boundaries do not compose at odd extents. A source
        // pixel p belongs to ceil((p + 1) * output / source) - 1.
        if (mipLevel != 0u) {
            firstX = proportionalCeil(firstX + 1u, candidateExtent.width,
                selectedExtent.width) - 1u;
            firstY = proportionalCeil(firstY + 1u, candidateExtent.height,
                selectedExtent.height) - 1u;
            lastX = proportionalCeil(lastX + 1u, candidateExtent.width,
                selectedExtent.width) - 1u;
            lastY = proportionalCeil(lastY + 1u, candidateExtent.height,
                selectedExtent.height) - 1u;
        }
        selectedMip = mipLevel;
        selectedExtent = candidateExtent;
        if (lastX - firstX < 2u && lastY - firstY < 2u)
            break;
    }

    float sampled = convention_ == DeviceDepthConvention::ForwardZeroToOne
        ? 0.0f : 1.0f;
    const uint32_t offset = mipOffsets_[selectedMip];
    for (uint32_t y = firstY; y <= lastY; ++y) {
        for (uint32_t x = firstX; x <= lastX; ++x) {
            sampled = combineFarthest(sampled,
                depth_[offset + y * selectedExtent.width + x], convention_);
            ++result.sampledTexels;
        }
    }
    result.mipLevel = selectedMip;
    result.farthestOccluderDepth = sampled;
    result.tested = result.sampledTexels != 0;
    if (result.tested) {
        result.occluded = convention_ ==
                DeviceDepthConvention::ForwardZeroToOne
            ? query.nearestDepth > sampled + query.depthBias
            : query.nearestDepth < sampled - query.depthBias;
    }
    return result;
}

} // namespace Iridium
