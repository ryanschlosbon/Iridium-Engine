// Test-only reference: extractReflectionProbes and ReflectionProbePublisher as of
// f2cc899 (the M7R R5c.6 baseline), kept verbatim apart from the names (and
// without the probe-selection helpers it does not use) so the replay tests can
// compare the change-driven production path against the full scan byte for
// byte. It is never linked into a production target.

#include "ReferenceReflectionProbe.h"

#include "scene/components/TransformComponent.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace Iridium {
    namespace {

        [[nodiscard]] bool finite(glm::vec3 value) noexcept {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                std::isfinite(value.z);
        }

        [[nodiscard]] float volume(
            const ReflectionProbeComponent& probe) noexcept {
            if (probe.shape == ReflectionProbeShape::Sphere) {
                const float radius = probe.sphereRadiusMeters;
                return 4.0f / 3.0f * std::numbers::pi_v<float> *
                    radius * radius * radius;
            }
            return 8.0f * probe.boxExtentsMeters.x *
                probe.boxExtentsMeters.y * probe.boxExtentsMeters.z;
        }

        [[nodiscard]] bool same(const PackedGpuReflectionProbe& lhs,
            const PackedGpuReflectionProbe& rhs) noexcept {
            return std::memcmp(&lhs, &rhs,
                sizeof(PackedGpuReflectionProbe)) == 0;
        }

        [[nodiscard]] PackedGpuReflectionProbe packedProbe(
            const ReflectionProbeCandidate& candidate,
            uint32_t environmentSlot, uint32_t selectionRank) noexcept {
            PackedGpuReflectionProbe result{};
            result.worldToProbe = candidate.worldToProbe;
            if (candidate.probe.shape == ReflectionProbeShape::Sphere) {
                result.influence = { candidate.probe.sphereRadiusMeters,
                    0.0f, 0.0f, candidate.probe.blendDistanceMeters };
            }
            else {
                result.influence = { candidate.probe.boxExtentsMeters,
                    candidate.probe.blendDistanceMeters };
            }
            result.positionIntensity = {
                glm::vec3(candidate.probeToWorld[3]),
                candidate.probe.intensity };
            uint32_t flags = 0;
            if (candidate.probe.shape == ReflectionProbeShape::Box)
                flags |= PackedGpuReflectionProbeBoxShape;
            if (candidate.probe.parallaxMode ==
                ReflectionProbeParallaxMode::BoxProjection)
                flags |= PackedGpuReflectionProbeBoxProjection;
            result.metadata = { flags, environmentSlot,
                std::bit_cast<uint32_t>(candidate.probe.priority),
                selectionRank };
            return result;
        }

        [[nodiscard]] bool finite(glm::mat4 value) noexcept {
            for (uint32_t column = 0; column < 4; ++column) {
                if (!finite(glm::vec3(value[column])) ||
                    !std::isfinite(value[column].w)) return false;
            }
            return true;
        }

        [[nodiscard]] bool validProbe(
            const ReflectionProbeComponent& value) noexcept {
            const int32_t shape = static_cast<int32_t>(value.shape);
            const int32_t update = static_cast<int32_t>(value.updateMode);
            const int32_t parallax = static_cast<int32_t>(value.parallaxMode);
            const bool resolutionSupported = value.captureResolution == 128 ||
                value.captureResolution == 256 ||
                value.captureResolution == 512 ||
                value.captureResolution == 1024 ||
                value.captureResolution == 2048 ||
                value.captureResolution == 4096;
            return shape >= 0 && shape <= 1 && update >= 0 && update <= 2 &&
                parallax >= 0 && parallax <= 1 &&
                std::isfinite(value.sphereRadiusMeters) &&
                value.sphereRadiusMeters > 0.0f &&
                finite(value.boxExtentsMeters) &&
                glm::all(glm::greaterThan(value.boxExtentsMeters,
                    glm::vec3(0.0f))) &&
                std::isfinite(value.blendDistanceMeters) &&
                value.blendDistanceMeters >= 0.0f &&
                std::isfinite(value.intensity) && value.intensity >= 0.0f &&
                resolutionSupported &&
                std::isfinite(value.captureNearMeters) &&
                value.captureNearMeters > 0.0f &&
                std::isfinite(value.captureFarMeters) &&
                value.captureFarMeters > value.captureNearMeters;
        }

        [[nodiscard]] bool rigidProbeTransform(const glm::mat4& world,
            glm::mat4& probeToWorld, glm::mat4& worldToProbe) noexcept {
            if (!finite(world)) return false;
            glm::vec3 x = glm::vec3(world[0]);
            glm::vec3 y = glm::vec3(world[1]);
            const glm::vec3 sourceZ = glm::vec3(world[2]);
            const float xLength2 = glm::dot(x, x);
            if (!std::isfinite(xLength2) || xLength2 <= 1.0e-12f)
                return false;
            x *= glm::inversesqrt(xLength2);
            y -= x * glm::dot(x, y);
            const float yLength2 = glm::dot(y, y);
            if (!std::isfinite(yLength2) || yLength2 <= 1.0e-12f)
                return false;
            y *= glm::inversesqrt(yLength2);
            glm::vec3 z = glm::cross(x, y);
            if (!finite(z) || glm::dot(z, z) <= 1.0e-12f ||
                !finite(sourceZ)) return false;
            z = glm::normalize(z);
            if (glm::dot(z, sourceZ) < 0.0f) {
                y = -y;
                z = -z;
            }
            const glm::vec3 position = glm::vec3(world[3]);
            if (!finite(position)) return false;
            probeToWorld = glm::mat4(1.0f);
            probeToWorld[0] = glm::vec4(x, 0.0f);
            probeToWorld[1] = glm::vec4(y, 0.0f);
            probeToWorld[2] = glm::vec4(z, 0.0f);
            probeToWorld[3] = glm::vec4(position, 1.0f);
            worldToProbe = glm::inverse(probeToWorld);
            return finite(worldToProbe);
        }

    } // namespace

    ReflectionProbeFramePacket referenceExtractReflectionProbes(
        const SceneWorld& world, ReflectionProbeResidencyFn residency) {
        ReflectionProbeFramePacket result;
        const Registry& registry = world.registry();
        const auto* probes = registry.findPool<ReflectionProbeComponent>();
        if (!probes) return result;
        const auto* transforms = registry.findPool<TransformComponent>();
        for (Entity entity : probes->entities) {
            ++result.stats.sceneProbeCount;
            const auto owner = world.identities().persistentId(entity);
            if (!owner) {
                result.diagnostics.push_back({
                    .code = ReflectionProbeExtractionDiagnosticCode::MissingIdentity,
                    .propertyPath = "/identity",
                    .message = "Reflection probe owner has no persistent scene UUID",
                });
                ++result.stats.omittedCount;
                continue;
            }
            if (!transforms || !transforms->has(entity)) {
                result.diagnostics.push_back({
                    .code = ReflectionProbeExtractionDiagnosticCode::MissingTransform,
                    .owner = *owner,
                    .propertyPath = "/transform",
                    .message = "Reflection probe owner has no Transform component",
                });
                ++result.stats.omittedCount;
                continue;
            }
            const ReflectionProbeComponent& component = probes->get(entity);
            if (!validProbe(component)) {
                result.diagnostics.push_back({
                    .code = ReflectionProbeExtractionDiagnosticCode::InvalidProbe,
                    .owner = *owner,
                    .propertyPath = "/",
                    .message = "Reflection probe settings are invalid",
                });
                ++result.stats.omittedCount;
                continue;
            }
            ReflectionProbeCandidate candidate;
            candidate.owner = *owner;
            candidate.probe = component;
            if (!rigidProbeTransform(transforms->get(entity).worldMatrix,
                    candidate.probeToWorld, candidate.worldToProbe)) {
                result.diagnostics.push_back({
                    .code = ReflectionProbeExtractionDiagnosticCode::InvalidTransform,
                    .owner = *owner,
                    .propertyPath = "/transform",
                    .message = "Reflection probe world transform is invalid",
                });
                ++result.stats.omittedCount;
                continue;
            }
            const AssetGuid requested =
                !component.requestedEnvironmentAssetGuid.isNil()
                ? component.requestedEnvironmentAssetGuid
                : component.environmentAssetGuid;
            candidate.probe.environmentAssetGuid = requested;
            candidate.resident = !requested.isNil() && (residency
                ? residency(requested)
                : component.resolvedEnvironmentAssetGuid == requested);
            result.stats.residentCount += candidate.resident ? 1u : 0u;
            result.candidates.push_back(std::move(candidate));
        }
        std::ranges::sort(result.candidates,
            [](const ReflectionProbeCandidate& lhs,
               const ReflectionProbeCandidate& rhs) {
                return lhs.owner < rhs.owner;
            });
        result.stats.candidateCount = static_cast<uint32_t>(
            result.candidates.size());
        return result;
    }

    ReferenceReflectionProbePublisher::ReferenceReflectionProbePublisher(
        ReflectionProbePublicationConfig config)
        : config_(config) {
        if (config_.initialCapacity == 0 || config_.maximumCapacity == 0 ||
            config_.initialCapacity > config_.maximumCapacity) {
            throw std::invalid_argument(
                "Reflection probe publication capacity is invalid");
        }
        records_.resize(config_.initialCapacity);
        recordRevisions_.resize(config_.initialCapacity);
        selectionMetadata_.resize(config_.initialCapacity);
    }

    void ReferenceReflectionProbePublisher::advanceRevision(uint64_t& value) noexcept {
        ++nextRevision_;
        if (nextRevision_ == 0) ++nextRevision_;
        value = nextRevision_;
    }

    void ReferenceReflectionProbePublisher::reset() noexcept {
        slotsByOwner_.clear();
        std::ranges::fill(records_, PackedGpuReflectionProbe{});
        std::ranges::fill(recordRevisions_, uint64_t{ 0 });
        std::ranges::fill(selectionMetadata_,
            ReflectionProbeSelectionMetadata{});
        activeSlots_.clear();
        previousActiveSlots_.clear();
        changedSlots_.clear();
        changedRanges_.clear();
        occupiedSlots_.clear();
        publishCandidates_.clear();
        newCandidates_.clear();
        removedOwners_.clear();
        nextRevision_ = 0;
        activeListRevision_ = 0;
        stats_ = {};
    }

    void ReferenceReflectionProbePublisher::ensureCapacity(uint32_t required) {
        if (required <= records_.size()) return;
        if (required > config_.maximumCapacity) {
            throw std::overflow_error(
                "Reflection probe publication exceeded its configured maximum");
        }
        uint32_t capacity = static_cast<uint32_t>(records_.size());
        while (capacity < required) {
            const uint64_t doubled = static_cast<uint64_t>(capacity) * 2u;
            capacity = static_cast<uint32_t>((std::min)(
                static_cast<uint64_t>(config_.maximumCapacity),
                (std::max)(doubled, static_cast<uint64_t>(required))));
        }
        records_.resize(capacity);
        recordRevisions_.resize(capacity);
        selectionMetadata_.resize(capacity);
    }

    void ReferenceReflectionProbePublisher::writeRecord(uint32_t slot,
        const PackedGpuReflectionProbe& record) {
        if (slot >= records_.size()) {
            throw std::out_of_range(
                "Reflection probe record slot is outside capacity");
        }
        if (same(records_[slot], record)) return;
        records_[slot] = record;
        advanceRevision(recordRevisions_[slot]);
        changedSlots_.push_back(slot);
    }

    void ReferenceReflectionProbePublisher::clearRecord(uint32_t slot) {
        writeRecord(slot, PackedGpuReflectionProbe{});
    }

    void ReferenceReflectionProbePublisher::buildChangedRanges() {
        changedRanges_.clear();
        if (changedSlots_.empty()) return;
        std::ranges::sort(changedSlots_);
        const auto uniqueEnd = std::ranges::unique(changedSlots_).begin();
        changedSlots_.erase(uniqueEnd, changedSlots_.end());
        uint32_t first = changedSlots_.front();
        uint32_t previous = first;
        for (size_t index = 1; index < changedSlots_.size(); ++index) {
            const uint32_t slot = changedSlots_[index];
            if (slot == previous + 1u) {
                previous = slot;
                continue;
            }
            changedRanges_.push_back({ first, previous - first + 1u });
            first = previous = slot;
        }
        changedRanges_.push_back({ first, previous - first + 1u });
    }

    ReflectionProbeGpuFramePacket ReferenceReflectionProbePublisher::publish(
        std::span<const ReflectionProbeCandidate> candidates,
        const ReflectionProbeEnvironmentSlotFn& environmentSlot) {
        if (!environmentSlot) {
            throw std::invalid_argument(
                "Reflection probe publication requires an environment table resolver");
        }
        stats_ = {};
        stats_.extractedCandidateCount = static_cast<uint32_t>((std::min)(
            candidates.size(), static_cast<size_t>(
                (std::numeric_limits<uint32_t>::max)())));
        changedSlots_.clear();
        publishCandidates_.clear();
        publishCandidates_.reserve((std::min)(candidates.size(),
            static_cast<size_t>(config_.maximumCapacity)));
        for (const ReflectionProbeCandidate& candidate : candidates) {
            if (!candidate.probe.enabled) continue;
            if (!candidate.resident) {
                ++stats_.nonresidentProbeCount;
                continue;
            }
            const std::optional<uint32_t> resolved =
                candidate.runtimeEnvironmentSlot
                ? candidate.runtimeEnvironmentSlot
                : (candidate.probe.environmentAssetGuid.isNil()
                    ? std::optional<uint32_t>{}
                    : environmentSlot(candidate.probe.environmentAssetGuid));
            if (!resolved || *resolved == kInvalidEnvironmentTableSlot) {
                ++stats_.unresolvedEnvironmentCount;
                continue;
            }
            publishCandidates_.push_back({ &candidate, *resolved,
                volume(candidate.probe) });
        }
        std::ranges::sort(publishCandidates_,
            [](const PublishCandidate& lhs, const PublishCandidate& rhs) {
                if (lhs.source->probe.priority != rhs.source->probe.priority) {
                    return lhs.source->probe.priority >
                        rhs.source->probe.priority;
                }
                if (lhs.influenceVolume != rhs.influenceVolume)
                    return lhs.influenceVolume < rhs.influenceVolume;
                return lhs.source->owner < rhs.source->owner;
            });
        if (publishCandidates_.size() > config_.maximumCapacity) {
            stats_.capacityOmittedCount = static_cast<uint32_t>(
                publishCandidates_.size() - config_.maximumCapacity);
            publishCandidates_.resize(config_.maximumCapacity);
        }
        for (uint32_t index = 0; index < publishCandidates_.size(); ++index)
            publishCandidates_[index].selectionRank = index;
        ensureCapacity(static_cast<uint32_t>(publishCandidates_.size()));
        occupiedSlots_.assign(records_.size(), uint8_t{ 0 });

        newCandidates_.clear();
        newCandidates_.reserve(publishCandidates_.size());
        for (PublishCandidate& candidate : publishCandidates_) {
            const auto existing = slotsByOwner_.find(candidate.source->owner);
            if (existing == slotsByOwner_.end()) {
                newCandidates_.push_back(&candidate);
                continue;
            }
            const uint32_t slot = existing->second;
            occupiedSlots_[slot] = 1;
            selectionMetadata_[slot] = { candidate.source->owner,
                candidate.source->probe.priority,
                candidate.influenceVolume };
            writeRecord(slot, packedProbe(*candidate.source,
                candidate.environmentSlot, candidate.selectionRank));
        }

        removedOwners_.clear();
        for (const auto& [owner, slot] : slotsByOwner_) {
            if (occupiedSlots_[slot] != 0) continue;
            clearRecord(slot);
            selectionMetadata_[slot] = {};
            removedOwners_.push_back(owner);
        }
        for (SceneEntityUuid owner : removedOwners_) slotsByOwner_.erase(owner);

        for (PublishCandidate* candidate : newCandidates_) {
            const auto free = std::ranges::find(occupiedSlots_, uint8_t{ 0 });
            if (free == occupiedSlots_.end()) {
                throw std::logic_error(
                    "Prepared reflection probe capacity has no free slot");
            }
            const uint32_t slot = static_cast<uint32_t>(
                std::distance(occupiedSlots_.begin(), free));
            *free = 1;
            slotsByOwner_.emplace(candidate->source->owner, slot);
            selectionMetadata_[slot] = { candidate->source->owner,
                candidate->source->probe.priority,
                candidate->influenceVolume };
            writeRecord(slot, packedProbe(*candidate->source,
                candidate->environmentSlot, candidate->selectionRank));
        }

        previousActiveSlots_ = activeSlots_;
        activeSlots_.clear();
        activeSlots_.reserve(slotsByOwner_.size());
        for (const auto& [owner, slot] : slotsByOwner_) {
            (void)owner;
            activeSlots_.push_back(slot);
        }
        std::ranges::sort(activeSlots_);
        if (activeSlots_ != previousActiveSlots_)
            advanceRevision(activeListRevision_);
        buildChangedRanges();
        stats_.activeProbeCount = static_cast<uint32_t>(activeSlots_.size());
        stats_.changedRecordCount = static_cast<uint32_t>(
            changedSlots_.size());
        stats_.changedRangeCount = static_cast<uint32_t>(
            changedRanges_.size());
        stats_.capacity = static_cast<uint32_t>(records_.size());
        stats_.changedRecordBytes = static_cast<uint64_t>(
            stats_.changedRecordCount) * sizeof(PackedGpuReflectionProbe);
        return {
            .records = records_,
            .recordRevisions = recordRevisions_,
            .activeSlots = activeSlots_,
            .selectionMetadata = selectionMetadata_,
            .changedRanges = changedRanges_,
            .activeListRevision = activeListRevision_,
            .requiredCapacity = static_cast<uint32_t>(records_.size()),
            .stats = stats_,
        };
    }

    std::optional<uint32_t> ReferenceReflectionProbePublisher::slotFor(
        SceneEntityUuid owner) const {
        const auto found = slotsByOwner_.find(owner);
        return found == slotsByOwner_.end()
            ? std::nullopt : std::optional<uint32_t>(found->second);
    }

} // namespace Iridium
