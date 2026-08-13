#include "renderer/lighting/ReflectionProbeCapture.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

#include <glm/gtc/matrix_transform.hpp>

namespace Iridium {
namespace {

    [[nodiscard]] bool finite(glm::vec3 value) noexcept {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
            std::isfinite(value.z);
    }

    [[nodiscard]] bool supportedResolution(uint32_t value) noexcept {
        return value == 128 || value == 256 || value == 512 ||
            value == 1024 || value == 2048 || value == 4096;
    }

    [[nodiscard]] bool sameRealtimeDependencies(
        const ReflectionProbeCaptureRequest& left,
        const ReflectionProbeCaptureRequest& right) noexcept {
        return left.sceneRevision == right.sceneRevision &&
            left.lightingRevision == right.lightingRevision &&
            left.environmentRevision == right.environmentRevision;
    }

    [[nodiscard]] bool requestPrecedes(
        const ReflectionProbeCaptureRequest& left,
        const ReflectionProbeCaptureRequest& right) noexcept {
        if (left.priority != right.priority)
            return left.priority > right.priority;
        if (left.resolution != right.resolution)
            return left.resolution < right.resolution;
        return left.owner < right.owner;
    }

} // namespace

uint32_t reflectionProbeCaptureMipCount(uint32_t resolution) {
    if (!supportedResolution(resolution))
        throw std::invalid_argument(
            "Reflection-probe capture resolution is unsupported");
    return std::bit_width(resolution);
}

ReflectionProbeCaptureStorageFootprint
reflectionProbeCaptureStorageFootprint(uint32_t resolution) {
    const uint32_t mipCount = reflectionProbeCaptureMipCount(resolution);
    uint64_t mipTexels = 0;
    for (uint32_t mip = 0; mip < mipCount; ++mip) {
        const uint64_t size = static_cast<uint64_t>(resolution) >> mip;
        mipTexels += size * size;
    }
    ReflectionProbeCaptureStorageFootprint result;
    const uint64_t baseTexels = static_cast<uint64_t>(resolution) *
        resolution * kReflectionProbeCaptureFaceCount;
    result.rawRadianceBytes = baseTexels * 8u;
    result.depthBytes = baseTexels * 4u;
    result.prefilteredRadianceBytes = mipTexels *
        kReflectionProbeCaptureFaceCount * 8u;
    result.totalStagingBytes = result.rawRadianceBytes + result.depthBytes +
        result.prefilteredRadianceBytes;
    return result;
}

std::array<ReflectionProbeCaptureFace, kReflectionProbeCaptureFaceCount>
buildReflectionProbeCaptureFaces(glm::vec3 position, float nearPlane,
    float farPlane) {
    if (!finite(position) || !std::isfinite(nearPlane) ||
        !std::isfinite(farPlane) || nearPlane <= 0.0f ||
        farPlane <= nearPlane)
        throw std::invalid_argument(
            "Reflection-probe capture projection is invalid");
    constexpr std::array<glm::vec3, kReflectionProbeCaptureFaceCount>
        Directions{
            glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0),
            glm::vec3(0, 1, 0), glm::vec3(0, -1, 0),
            glm::vec3(0, 0, 1), glm::vec3(0, 0, -1),
        };
    constexpr std::array<glm::vec3, kReflectionProbeCaptureFaceCount> Ups{
        glm::vec3(0, -1, 0), glm::vec3(0, -1, 0),
        glm::vec3(0, 0, 1), glm::vec3(0, 0, -1),
        glm::vec3(0, -1, 0), glm::vec3(0, -1, 0),
    };
    const glm::mat4 projection = glm::perspectiveRH_ZO(
        glm::radians(90.0f), 1.0f, nearPlane, farPlane);
    std::array<ReflectionProbeCaptureFace,
        kReflectionProbeCaptureFaceCount> result{};
    for (uint32_t index = 0; index < result.size(); ++index) {
        result[index] = {
            .faceIndex = index,
            .direction = Directions[index],
            .up = Ups[index],
            .worldToClip = projection * glm::lookAtRH(position,
                position + Directions[index], Ups[index]),
        };
    }
    return result;
}

ReflectionProbeCaptureScheduler::ReflectionProbeCaptureScheduler(
    ReflectionProbeCaptureSchedulerConfig config) : config_(config) {
    validateConfig(config_);
}

void ReflectionProbeCaptureScheduler::validateConfig(
    const ReflectionProbeCaptureSchedulerConfig& config) {
    if (config.maximumRenderedTexels == 0 ||
        config.maximumFacesPerProbePerFrame == 0 ||
        config.maximumFacesPerProbePerFrame >
            kReflectionProbeCaptureFaceCount ||
        config.maximumCapturesInFlight == 0 ||
        config.minimumRealtimeFramesBetweenCaptures == 0)
        throw std::invalid_argument(
            "Reflection-probe capture scheduling policy is invalid");
}

void ReflectionProbeCaptureScheduler::validateRequest(
    const ReflectionProbeCaptureRequest& request) {
    const int32_t mode = static_cast<int32_t>(request.updateMode);
    if (request.owner.isNil() || mode < 0 || mode > 2 ||
        !finite(request.position) || !supportedResolution(request.resolution) ||
        !std::isfinite(request.nearPlane) || request.nearPlane <= 0.0f ||
        !std::isfinite(request.farPlane) ||
        request.farPlane <= request.nearPlane ||
        request.settingsRevision == 0 || request.pipelineRevision == 0)
        throw std::invalid_argument(
            "Reflection-probe capture request is invalid");
}

void ReflectionProbeCaptureScheduler::configure(
    ReflectionProbeCaptureSchedulerConfig config) {
    if (currentSchedule_)
        throw std::logic_error(
            "Reflection-probe capture policy cannot change during a schedule");
    validateConfig(config);
    config_ = config;
}

ReflectionProbeCaptureScheduler::CaptureState*
ReflectionProbeCaptureScheduler::findState(SceneEntityUuid owner) noexcept {
    const auto found = std::ranges::find(states_, owner,
        &CaptureState::owner);
    return found == states_.end() ? nullptr : &*found;
}

const ReflectionProbeCaptureScheduler::CaptureState*
ReflectionProbeCaptureScheduler::findState(
    SceneEntityUuid owner) const noexcept {
    const auto found = std::ranges::find(states_, owner,
        &CaptureState::owner);
    return found == states_.end() ? nullptr : &*found;
}

ReflectionProbeCaptureDirtyReason ReflectionProbeCaptureScheduler::dirtyReason(
    const ReflectionProbeCaptureRequest& request,
    const CaptureState& state) const noexcept {
    if (!state.hasPublished) {
        if (request.updateMode == ReflectionProbeUpdateMode::Baked &&
            request.explicitRequestRevision == 0)
            return ReflectionProbeCaptureDirtyReason::None;
        return request.explicitRequestRevision != 0
            ? ReflectionProbeCaptureDirtyReason::ExplicitRequest
            : ReflectionProbeCaptureDirtyReason::NewProbe;
    }
    const auto& published = state.published;
    if (request.settingsRevision != published.settingsRevision)
        return ReflectionProbeCaptureDirtyReason::SettingsChanged;
    if (request.pipelineRevision != published.pipelineRevision)
        return ReflectionProbeCaptureDirtyReason::PipelineChanged;
    if (request.explicitRequestRevision !=
        published.explicitRequestRevision)
        return ReflectionProbeCaptureDirtyReason::ExplicitRequest;
    if (request.updateMode != ReflectionProbeUpdateMode::Realtime)
        return ReflectionProbeCaptureDirtyReason::None;
    if (request.sceneRevision != published.sceneRevision)
        return ReflectionProbeCaptureDirtyReason::SceneChanged;
    if (request.lightingRevision != published.lightingRevision)
        return ReflectionProbeCaptureDirtyReason::LightingChanged;
    if (request.environmentRevision != published.environmentRevision)
        return ReflectionProbeCaptureDirtyReason::EnvironmentChanged;
    return ReflectionProbeCaptureDirtyReason::None;
}

bool ReflectionProbeCaptureScheduler::pendingIsCompatible(
    const ReflectionProbeCaptureRequest& request,
    const CaptureState& state) const noexcept {
    if (!state.hasPending) return true;
    const auto& pending = state.pending;
    if (request.settingsRevision != pending.settingsRevision ||
        request.pipelineRevision != pending.pipelineRevision ||
        request.explicitRequestRevision !=
            pending.explicitRequestRevision ||
        request.updateMode != pending.updateMode)
        return false;
    return request.updateMode != ReflectionProbeUpdateMode::Realtime ||
        sameRealtimeDependencies(request, pending);
}

uint64_t ReflectionProbeCaptureScheduler::nextTicket() {
    if (nextTicket_ == (std::numeric_limits<uint64_t>::max)())
        throw std::overflow_error(
            "Reflection-probe capture ticket space is exhausted");
    ++nextTicket_;
    return nextTicket_;
}

const ReflectionProbeCaptureSchedule& ReflectionProbeCaptureScheduler::schedule(
    std::span<const ReflectionProbeCaptureRequest> requests) {
    if (currentSchedule_)
        throw std::logic_error(
            "Reflection-probe capture schedule must be completed first");

    std::vector<ReflectionProbeCaptureRequest> ranked(
        requests.begin(), requests.end());
    std::unordered_set<SceneEntityUuid, SceneEntityUuidHash> owners;
    owners.reserve(ranked.size());
    for (const auto& request : ranked) {
        validateRequest(request);
        if (!owners.insert(request.owner).second)
            throw std::invalid_argument(
                "Reflection-probe capture owner is duplicated");
    }
    std::ranges::sort(ranked, requestPrecedes);

    for (size_t index = states_.size(); index-- > 0;) {
        if (owners.contains(states_[index].owner)) continue;
        states_.erase(states_.begin() + static_cast<ptrdiff_t>(index));
    }

    currentSchedule_.emplace();
    auto& result = *currentSchedule_;
    result.entries.reserve(ranked.size());
    result.stats.requests = static_cast<uint32_t>(ranked.size());
    uint64_t remainingTexels = config_.maximumRenderedTexels;
    uint32_t pendingCount = static_cast<uint32_t>(std::ranges::count_if(
        states_, [](const CaptureState& state) { return state.hasPending; }));

    for (const auto& request : ranked) {
        CaptureState* state = findState(request.owner);
        if (state == nullptr) {
            states_.push_back({ .owner = request.owner });
            state = &states_.back();
        }
        if (state->hasPending && !pendingIsCompatible(request, *state)) {
            state->hasPending = false;
            state->awaitingPublication = false;
            state->capturedFaceMask = 0;
            state->pendingTicket = 0;
            state->pendingReason = ReflectionProbeCaptureDirtyReason::None;
            --pendingCount;
            ++result.stats.capturesInvalidated;
        }

        ReflectionProbeCaptureDirtyReason reason = state->hasPending
            ? state->pendingReason : dirtyReason(request, *state);
        const bool cadenceLimited = !state->hasPending && state->hasPublished &&
            request.updateMode == ReflectionProbeUpdateMode::Realtime &&
            (reason == ReflectionProbeCaptureDirtyReason::SceneChanged ||
                reason == ReflectionProbeCaptureDirtyReason::LightingChanged ||
                reason == ReflectionProbeCaptureDirtyReason::EnvironmentChanged) &&
            request.frameIndex >= state->published.frameIndex &&
            request.frameIndex - state->published.frameIndex <
                config_.minimumRealtimeFramesBetweenCaptures;
        if (cadenceLimited) {
            reason = ReflectionProbeCaptureDirtyReason::None;
            ++result.stats.cadenceDeferred;
        }
        ReflectionProbeCaptureScheduleEntry entry{
            .owner = request.owner,
            .dirtyReason = reason,
            .resolution = request.resolution,
            .sampleable = state->hasPublished,
        };
        if (reason == ReflectionProbeCaptureDirtyReason::None) {
            if (state->hasPublished) ++result.stats.cacheHits;
            result.entries.push_back(entry);
            continue;
        }

        ++result.stats.dirty;
        if (!state->hasPending) {
            if (pendingCount >= config_.maximumCapturesInFlight) {
                ++result.stats.capacityDeferred;
                result.entries.push_back(entry);
                continue;
            }
            state->pending = request;
            state->pendingTicket = nextTicket();
            state->capturedFaceMask = 0;
            state->pendingReason = reason;
            state->hasPending = true;
            state->awaitingPublication = false;
            ++pendingCount;
            ++result.stats.capturesStarted;
        }

        entry.captureTicket = state->pendingTicket;
        entry.captureInFlight = true;
        entry.awaitingPublication = state->awaitingPublication;
        entry.capturedFaceMask = state->capturedFaceMask;
        entry.resolution = state->pending.resolution;
        entry.position = state->pending.position;
        entry.nearPlane = state->pending.nearPlane;
        entry.captureSky = state->pending.captureSky;
        entry.updateMode = state->pending.updateMode;
        entry.faces = buildReflectionProbeCaptureFaces(
            state->pending.position, state->pending.nearPlane,
            state->pending.farPlane);
        if (!state->awaitingPublication) {
            const uint64_t faceTexels = static_cast<uint64_t>(
                state->pending.resolution) * state->pending.resolution;
            const uint32_t budgetFaces = static_cast<uint32_t>((std::min)(
                remainingTexels / faceTexels,
                static_cast<uint64_t>(
                    config_.maximumFacesPerProbePerFrame)));
            uint32_t scheduled = 0;
            for (uint32_t face = 0;
                face < kReflectionProbeCaptureFaceCount &&
                    scheduled < budgetFaces; ++face) {
                const uint8_t bit = static_cast<uint8_t>(1u << face);
                if ((state->capturedFaceMask & bit) != 0) continue;
                entry.scheduledFaceMask = static_cast<uint8_t>(
                    entry.scheduledFaceMask | bit);
                ++scheduled;
            }
            const uint64_t scheduledTexels = faceTexels * scheduled;
            remainingTexels -= scheduledTexels;
            result.stats.facesScheduled += scheduled;
            result.stats.renderedTexels += scheduledTexels;
            if (entry.scheduledFaceMask == 0 &&
                state->capturedFaceMask !=
                    kReflectionProbeCaptureCompleteMask)
                ++result.stats.budgetDeferred;
        }
        result.entries.push_back(entry);
    }

    result.stats.capturesInFlight = pendingCount;
    result.stats.awaitingPublication = static_cast<uint32_t>(
        std::ranges::count_if(states_, [](const CaptureState& state) {
            return state.awaitingPublication;
        }));
    return result;
}

void ReflectionProbeCaptureScheduler::markScheduledFacesRendered() {
    if (!currentSchedule_)
        throw std::logic_error(
            "No reflection-probe capture schedule is pending");
    for (const auto& entry : currentSchedule_->entries) {
        if (entry.scheduledFaceMask == 0) continue;
        CaptureState* state = findState(entry.owner);
        if (state == nullptr || !state->hasPending ||
            state->pendingTicket != entry.captureTicket ||
            (state->capturedFaceMask & entry.scheduledFaceMask) != 0)
            throw std::logic_error(
                "Reflection-probe capture completion is inconsistent");
        state->capturedFaceMask = static_cast<uint8_t>(
            state->capturedFaceMask | entry.scheduledFaceMask);
        if (state->capturedFaceMask == kReflectionProbeCaptureCompleteMask)
            state->awaitingPublication = true;
    }
    currentSchedule_.reset();
}

std::span<const ReflectionProbeCapturePublication>
ReflectionProbeCaptureScheduler::publicationsReady() {
    if (currentSchedule_)
        throw std::logic_error(
            "Capture publications are unavailable during a schedule");
    readyPublications_.clear();
    for (const CaptureState& state : states_) {
        if (!state.hasPending || !state.awaitingPublication) continue;
        readyPublications_.push_back({
            .owner = state.owner,
            .captureTicket = state.pendingTicket,
            .capturedRequest = state.pending,
        });
    }
    std::ranges::sort(readyPublications_, {},
        &ReflectionProbeCapturePublication::owner);
    return readyPublications_;
}

void ReflectionProbeCaptureScheduler::markPublished(
    SceneEntityUuid owner, uint64_t captureTicket) {
    if (currentSchedule_)
        throw std::logic_error(
            "Capture publication cannot occur during a schedule");
    CaptureState* state = findState(owner);
    if (state == nullptr || !state->hasPending ||
        !state->awaitingPublication ||
        state->capturedFaceMask != kReflectionProbeCaptureCompleteMask ||
        state->pendingTicket != captureTicket)
        throw std::invalid_argument(
            "Reflection-probe capture publication is not ready");
    state->published = state->pending;
    state->publishedTicket = state->pendingTicket;
    state->hasPublished = true;
    state->hasPending = false;
    state->awaitingPublication = false;
    state->pendingTicket = 0;
    state->capturedFaceMask = 0;
    state->pendingReason = ReflectionProbeCaptureDirtyReason::None;
}

void ReflectionProbeCaptureScheduler::abandon(
    SceneEntityUuid owner, uint64_t captureTicket) {
    if (currentSchedule_)
        throw std::logic_error(
            "Capture abandonment cannot occur during a schedule");
    CaptureState* state = findState(owner);
    if (state == nullptr || !state->hasPending ||
        state->pendingTicket != captureTicket)
        throw std::invalid_argument(
            "Reflection-probe capture abandonment is invalid");
    state->hasPending = false;
    state->awaitingPublication = false;
    state->pendingTicket = 0;
    state->capturedFaceMask = 0;
    state->pendingReason = ReflectionProbeCaptureDirtyReason::None;
}

void ReflectionProbeCaptureScheduler::reset() noexcept {
    states_.clear();
    currentSchedule_.reset();
    readyPublications_.clear();
    nextTicket_ = 0;
}

bool ReflectionProbeCaptureScheduler::hasPublishedCapture(
    SceneEntityUuid owner) const noexcept {
    const CaptureState* state = findState(owner);
    return state != nullptr && state->hasPublished;
}

std::optional<uint64_t> ReflectionProbeCaptureScheduler::publishedTicket(
    SceneEntityUuid owner) const noexcept {
    const CaptureState* state = findState(owner);
    if (state == nullptr || !state->hasPublished) return std::nullopt;
    return state->publishedTicket;
}

} // namespace Iridium
