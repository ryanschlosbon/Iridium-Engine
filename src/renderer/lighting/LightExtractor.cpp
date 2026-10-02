#include "renderer/lighting/LightExtractor.h"

#include "scene/components/LightComponent.h"
#include "scene/components/RelationshipComponent.h"
#include "scene/components/TransformComponent.h"
#include "scene/lighting/LightPhotometry.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

#include <glm/gtc/matrix_transform.hpp>

namespace Iridium {
    namespace {

        [[nodiscard]] bool finite(glm::vec3 value) noexcept {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                std::isfinite(value.z);
        }

        [[nodiscard]] bool same(const PackedGpuLight& lhs,
            const PackedGpuLight& rhs) noexcept {
            return std::memcmp(&lhs, &rhs, sizeof(PackedGpuLight)) == 0;
        }

        [[nodiscard]] glm::mat4 localRotation(
            const TransformComponent& transform) {
            glm::mat4 result(1.0f);
            result = glm::rotate(result, glm::radians(transform.rotation.z),
                glm::vec3(0.0f, 0.0f, 1.0f));
            result = glm::rotate(result, glm::radians(transform.rotation.y),
                glm::vec3(0.0f, 1.0f, 0.0f));
            return glm::rotate(result, glm::radians(transform.rotation.x),
                glm::vec3(1.0f, 0.0f, 0.0f));
        }

        [[nodiscard]] uint32_t packedMetadata(const LightComponent& light) {
            uint32_t result = static_cast<uint32_t>(light.type);
            if (light.castsShadows) result |= PackedGpuLightCastsShadows;
            result |= static_cast<uint32_t>(light.shadowQuality) <<
                PackedGpuLightShadowQualityShift;
            return result;
        }

    } // namespace

    LightExtractor::LightExtractor(LightExtractionConfig config)
        : config_(config) {
        if (config_.initialCapacity == 0 || config_.maximumCapacity == 0 ||
            config_.initialCapacity > config_.maximumCapacity ||
            config_.maximumCapacity > kMaximumGpuLightCapacity) {
            throw std::invalid_argument(
                "Light extraction capacity policy is invalid");
        }
        records_.resize(config_.initialCapacity);
        recordRevisions_.resize(config_.initialCapacity);
        selectionMetadata_.resize(config_.initialCapacity);
        activeSlots_.reserve(config_.initialCapacity);
        previousActiveSlots_.reserve(config_.initialCapacity);
        changedSlots_.reserve(config_.initialCapacity);
        changedRanges_.reserve(config_.initialCapacity);
        diagnostics_.reserve(32);
        transformChain_.reserve(16);
        candidates_.reserve(config_.initialCapacity);
        newCandidates_.reserve(config_.initialCapacity);
        removedOwners_.reserve(config_.initialCapacity);
        occupiedSlots_.resize(config_.initialCapacity);
        slotsByOwner_.reserve(config_.initialCapacity);
    }

    void LightExtractor::reset() noexcept {
        world_ = nullptr;
        worldEpoch_ = 0;
        nextRevision_ = 0;
        activeListRevision_ = 0;
        std::ranges::fill(records_, PackedGpuLight{});
        std::ranges::fill(recordRevisions_, uint64_t{ 0 });
        std::ranges::fill(selectionMetadata_, LightSelectionMetadata{});
        activeSlots_.clear();
        previousActiveSlots_.clear();
        changedSlots_.clear();
        changedRanges_.clear();
        diagnostics_.clear();
        transformChain_.clear();
        candidates_.clear();
        newCandidates_.clear();
        removedOwners_.clear();
        occupiedSlots_.clear();
        slotsByOwner_.clear();
        stats_ = {};
    }

    void LightExtractor::resetForWorld(const SceneWorld& world) {
        world_ = &world;
        worldEpoch_ = world.stateEpoch();
        for (const auto& [owner, slot] : slotsByOwner_) {
            (void)owner;
            clearRecord(slot);
            selectionMetadata_[slot] = {};
        }
        slotsByOwner_.clear();
        previousActiveSlots_.swap(activeSlots_);
        activeSlots_.clear();
        advanceRevision(activeListRevision_);
    }

    void LightExtractor::advanceRevision(uint64_t& value) noexcept {
        if (++nextRevision_ == 0) ++nextRevision_;
        value = nextRevision_;
    }

    void LightExtractor::ensureCapacity(uint32_t required) {
        if (required <= records_.size()) return;
        uint32_t capacity = static_cast<uint32_t>(records_.size());
        while (capacity < required && capacity < config_.maximumCapacity) {
            capacity = (std::min)(config_.maximumCapacity,
                capacity <= config_.maximumCapacity / 2
                    ? capacity * 2 : config_.maximumCapacity);
        }
        records_.resize(capacity);
        recordRevisions_.resize(capacity);
        selectionMetadata_.resize(capacity);
        activeSlots_.reserve(capacity);
        previousActiveSlots_.reserve(capacity);
        changedSlots_.reserve(capacity);
        changedRanges_.reserve(capacity);
        candidates_.reserve(capacity);
        newCandidates_.reserve(capacity);
        removedOwners_.reserve(capacity);
        occupiedSlots_.resize(capacity);
        slotsByOwner_.reserve(capacity);
    }

    void LightExtractor::writeRecord(uint32_t slot,
        const PackedGpuLight& record) {
        if (slot >= records_.size()) throw std::out_of_range(
            "Light record slot is outside extractor capacity");
        if (same(records_[slot], record)) return;
        records_[slot] = record;
        advanceRevision(recordRevisions_[slot]);
        changedSlots_.push_back(slot);
    }

    void LightExtractor::clearRecord(uint32_t slot) {
        writeRecord(slot, PackedGpuLight{});
    }

    bool LightExtractor::worldEmissionDirection(
        const SceneWorld& world, Entity entity, glm::vec3& direction) {
        const Registry& registry = world.registry();
        const auto* transforms = registry.findPool<TransformComponent>();
        const auto* relationships = registry.findPool<RelationshipComponent>();
        if (!transforms || !transforms->has(entity)) return false;

        transformChain_.clear();
        Entity current = entity;
        for (size_t depth = 0; depth <= registry.aliveCount(); ++depth) {
            transformChain_.push_back(current);
            if (!relationships || !relationships->has(current)) break;
            const Entity parent = relationships->get(current).parent;
            if (parent == NULL_ENTITY || !registry.isAlive(parent) ||
                !transforms->has(parent)) break;
            current = parent;
            if (depth == registry.aliveCount()) return false;
        }

        glm::mat4 rotation(1.0f);
        for (auto iterator = transformChain_.rbegin();
            iterator != transformChain_.rend(); ++iterator) {
            const TransformComponent& transform = transforms->get(*iterator);
            if (!finite(transform.rotation)) return false;
            rotation *= localRotation(transform);
        }
        direction = glm::vec3(rotation *
            glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
        const float lengthSquared = glm::dot(direction, direction);
        if (!finite(direction) || !std::isfinite(lengthSquared) ||
            lengthSquared <= 1.0e-12f) return false;
        direction *= glm::inversesqrt(lengthSquared);
        return finite(direction);
    }

    bool LightExtractor::buildCandidate(const SceneWorld& world,
        Entity entity, Candidate& candidate) {
        const Registry& registry = world.registry();
        const auto owner = world.identities().persistentId(entity);
        if (!owner) {
            diagnostics_.push_back({
                .code = LightExtractionDiagnosticCode::MissingIdentity,
                .propertyPath = "/identity",
                .message = "Light owner has no persistent scene UUID",
            });
            return false;
        }
        candidate.owner = *owner;
        const auto* transforms = registry.findPool<TransformComponent>();
        if (!transforms || !transforms->has(entity)) {
            diagnostics_.push_back({
                .code = LightExtractionDiagnosticCode::MissingTransform,
                .owner = *owner,
                .propertyPath = "/transform",
                .message = "Light owner has no Transform component",
            });
            return false;
        }
        const auto* lights = registry.findPool<LightComponent>();
        if (!lights || !lights->has(entity)) return false;
        const LightComponent& light = lights->get(entity);
        if (light.type == LightType::Area) {
            diagnostics_.push_back({
                .code = LightExtractionDiagnosticCode::UnsupportedArea,
                .owner = *owner,
                .propertyPath = "/type",
                .message = "Area lights are not supported by M5 raster extraction",
            });
            return false;
        }
        const int32_t type = static_cast<int32_t>(light.type);
        const int32_t quality = static_cast<int32_t>(light.shadowQuality);
        if (type < 0 || type > static_cast<int32_t>(LightType::Spot) ||
            quality < 0 || quality > 3 || !finite(light.colorLinearRec709) ||
            glm::any(glm::lessThan(light.colorLinearRec709, glm::vec3(0.0f))) ||
            !std::isfinite(light.illuminanceLux) || light.illuminanceLux < 0.0f ||
            !std::isfinite(light.luminousIntensityCandela) ||
            light.luminousIntensityCandela < 0.0f ||
            !std::isfinite(light.rangeMeters) || light.rangeMeters < 0.0f ||
            !std::isfinite(light.sourceRadiusMeters) ||
            light.sourceRadiusMeters < 0.0f ||
            !std::isfinite(light.innerConeDegrees) ||
            !std::isfinite(light.outerConeDegrees) ||
            light.innerConeDegrees < 0.0f ||
            light.outerConeDegrees < light.innerConeDegrees ||
            light.outerConeDegrees > 90.0f) {
            diagnostics_.push_back({
                .code = LightExtractionDiagnosticCode::InvalidPhysicalValue,
                .owner = *owner,
                .propertyPath = "/",
                .message = "Light contains invalid physical or enum values",
            });
            return false;
        }

        const glm::vec3 position = glm::vec3(
            transforms->get(entity).worldMatrix[3]);
        glm::vec3 direction;
        if (!finite(position) ||
            !worldEmissionDirection(world, entity, direction)) {
            diagnostics_.push_back({
                .code = LightExtractionDiagnosticCode::InvalidTransform,
                .owner = *owner,
                .propertyPath = "/transform",
                .message = "Light world position or scale-independent orientation is invalid",
            });
            return false;
        }
        const glm::vec3 color = normalizedAp1LightChromaticity(
            light.colorLinearRec709);
        if (!finite(color) || color == glm::vec3(0.0f)) {
            diagnostics_.push_back({
                .code = LightExtractionDiagnosticCode::ZeroLuminanceColor,
                .owner = *owner,
                .propertyPath = "/colorLinearRec709",
                .message = "Light color has zero AP1 luminance and is omitted",
            });
            return false;
        }

        const float innerCos = std::cos(glm::radians(
            light.innerConeDegrees));
        const float outerCos = std::cos(glm::radians(
            light.outerConeDegrees));
        const float coneDelta = innerCos - outerCos;
        const float inverseConeDelta = light.type == LightType::Spot &&
            coneDelta > 1.0e-7f ? 1.0f / coneDelta : 0.0f;
        const float intensity = light.type == LightType::Directional
            ? light.illuminanceLux : light.luminousIntensityCandela;
        candidate.record.positionRange = { position, light.rangeMeters };
        candidate.record.directionOuterCos = { direction, outerCos };
        candidate.record.colorIntensity = { color, intensity };
        candidate.record.shapeMetadata = {
            light.sourceRadiusMeters, inverseConeDelta,
            std::bit_cast<float>(packedMetadata(light)),
            std::bit_cast<float>(kInvalidShadowDataSlot),
        };
        candidate.priority = light.priority;
        candidate.castsShadows = light.castsShadows;
        return true;
    }

    void LightExtractor::buildChangedRanges() {
        changedRanges_.clear();
        if (changedSlots_.empty()) return;
        std::ranges::sort(changedSlots_);
        const auto uniqueEnd = std::ranges::unique(changedSlots_).begin();
        changedSlots_.erase(uniqueEnd, changedSlots_.end());
        uint32_t first = changedSlots_.front();
        uint32_t previous = first;
        for (size_t index = 1; index < changedSlots_.size(); ++index) {
            const uint32_t slot = changedSlots_[index];
            if (slot == previous + 1) {
                previous = slot;
                continue;
            }
            changedRanges_.push_back({ first, previous - first + 1 });
            first = previous = slot;
        }
        changedRanges_.push_back({ first, previous - first + 1 });
    }

    LightingFramePacket LightExtractor::extract(const SceneWorld& world) {
        diagnostics_.clear();
        changedSlots_.clear();
        stats_ = {};
        if (world_ != &world || worldEpoch_ != world.stateEpoch()) {
            resetForWorld(world);
        }

        const Registry& registry = world.registry();
        const auto* lights = registry.findPool<LightComponent>();
        candidates_.clear();
        if (lights) {
            stats_.sceneLightCount = static_cast<uint32_t>((std::min)(
                lights->components.size(), static_cast<size_t>(
                    (std::numeric_limits<uint32_t>::max)())));
            candidates_.reserve(lights->components.size());
            for (Entity entity : lights->entities) {
                Candidate candidate;
                if (buildCandidate(world, entity, candidate)) {
                    candidates_.push_back(std::move(candidate));
                }
            }
        }
        occupiedSlots_.assign(records_.size(), uint8_t{ 0 });
        newCandidates_.clear();
        newCandidates_.reserve(candidates_.size());
        for (Candidate& candidate : candidates_) {
            const auto existing = slotsByOwner_.find(candidate.owner);
            if (existing == slotsByOwner_.end()) {
                newCandidates_.push_back(&candidate);
            }
            else {
                occupiedSlots_[existing->second] = 1;
                selectionMetadata_[existing->second] = {
                    candidate.owner, candidate.priority,
                    candidate.castsShadows };
                writeRecord(existing->second, candidate.record);
            }
        }
        removedOwners_.clear();
        removedOwners_.reserve(slotsByOwner_.size());
        for (const auto& [owner, slot] : slotsByOwner_) {
            if (occupiedSlots_[slot] == 0) {
                clearRecord(slot);
                selectionMetadata_[slot] = {};
                removedOwners_.push_back(owner);
            }
        }
        for (SceneEntityUuid owner : removedOwners_) slotsByOwner_.erase(owner);

        const uint32_t existingCount = static_cast<uint32_t>(slotsByOwner_.size());
        const uint64_t desired = static_cast<uint64_t>(existingCount) +
            newCandidates_.size();
        ensureCapacity(static_cast<uint32_t>((std::min)(desired,
            static_cast<uint64_t>(config_.maximumCapacity))));
        const uint32_t available = config_.maximumCapacity - existingCount;
        if (newCandidates_.size() > available) {
            std::ranges::sort(newCandidates_,
                [](const Candidate* lhs, const Candidate* rhs) {
                    if (lhs->priority != rhs->priority)
                        return lhs->priority > rhs->priority;
                    if (lhs->castsShadows != rhs->castsShadows)
                        return lhs->castsShadows > rhs->castsShadows;
                    return lhs->owner < rhs->owner;
                });
            for (size_t index = available; index < newCandidates_.size(); ++index) {
                diagnostics_.push_back({
                    .code = LightExtractionDiagnosticCode::CapacityExceeded,
                    .owner = newCandidates_[index]->owner,
                    .propertyPath = "/",
                    .message = "Light was omitted because extraction capacity is exhausted",
                });
            }
            newCandidates_.resize(available);
        }
        std::ranges::sort(newCandidates_,
            [](const Candidate* lhs, const Candidate* rhs) {
                return lhs->owner < rhs->owner;
            });
        if (occupiedSlots_.size() < records_.size()) {
            occupiedSlots_.resize(records_.size(), uint8_t{ 0 });
        }
        uint32_t nextFree = 0;
        for (const Candidate* candidate : newCandidates_) {
            while (nextFree < occupiedSlots_.size() && occupiedSlots_[nextFree]) ++nextFree;
            if (nextFree >= occupiedSlots_.size()) break;
            slotsByOwner_.emplace(candidate->owner, nextFree);
            occupiedSlots_[nextFree] = 1;
            selectionMetadata_[nextFree] = {
                candidate->owner, candidate->priority,
                candidate->castsShadows };
            writeRecord(nextFree, candidate->record);
        }

        previousActiveSlots_.swap(activeSlots_);
        activeSlots_.clear();
        activeSlots_.reserve(slotsByOwner_.size());
        for (const auto& [owner, slot] : slotsByOwner_) {
            (void)owner;
            activeSlots_.push_back(slot);
        }
        std::ranges::sort(activeSlots_);
        if (activeSlots_ != previousActiveSlots_) {
            advanceRevision(activeListRevision_);
        }
        buildChangedRanges();

        stats_.activeLightCount = static_cast<uint32_t>(activeSlots_.size());
        for (uint32_t slot : activeSlots_) {
            const uint32_t metadata = std::bit_cast<uint32_t>(
                records_[slot].shapeMetadata.z);
            if ((metadata & 3u) == static_cast<uint32_t>(
                    PackedGpuLightType::Directional)) {
                ++stats_.directionalLightCount;
            }
            else ++stats_.localLightCount;
        }
        stats_.changedRecordCount = static_cast<uint32_t>(changedSlots_.size());
        stats_.changedRangeCount = static_cast<uint32_t>(changedRanges_.size());
        stats_.omittedLightCount = stats_.sceneLightCount -
            stats_.activeLightCount;
        stats_.capacity = static_cast<uint32_t>(records_.size());
        stats_.changedRecordBytes = static_cast<uint64_t>(
            stats_.changedRecordCount) * sizeof(PackedGpuLight);
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

    std::optional<uint32_t> LightExtractor::slotFor(
        SceneEntityUuid owner) const {
        const auto found = slotsByOwner_.find(owner);
        return found == slotsByOwner_.end()
            ? std::nullopt : std::optional<uint32_t>(found->second);
    }

} // namespace Iridium
