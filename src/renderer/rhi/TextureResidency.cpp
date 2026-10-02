#include "renderer/rhi/TextureResidency.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace Iridium {

    namespace {
        uint32_t nextGeneration(uint32_t generation) noexcept {
            generation = generation == RenderHandle<TextureViewTag>::MaxGeneration
                ? 1 : generation + 1;
            return generation == 0 ? 1 : generation;
        }
    }

    TextureViewResidencyTable::TextureViewResidencyTable(
        uint32_t initialCapacity, uint32_t maximumCapacity)
        : maximumCapacity_(std::min(maximumCapacity,
            TextureViewHandle::MaxIndex + 1)) {
        if (initialCapacity < 2 || initialCapacity > maximumCapacity_) {
            throw std::invalid_argument(
                "Texture view residency capacity must be in the range 2..maximumCapacity");
        }
        slots_.resize(initialCapacity);
        slots_[0].state = TextureResidencyState::Fallback;
        for (uint32_t index = initialCapacity; index-- > 1;) {
            available_.push_back(index);
        }
        stats_.capacity = initialCapacity;
        updateCounts();
    }

    void TextureViewResidencyTable::setFallback(uint64_t backendToken) {
        if (backendToken == 0) {
            throw std::invalid_argument("Fallback texture view token must be nonzero");
        }
        slots_[0].backendToken = backendToken;
    }

    bool TextureViewResidencyTable::grow() {
        if (slots_.size() >= maximumCapacity_) {
            return false;
        }
        const uint32_t oldCapacity = static_cast<uint32_t>(slots_.size());
        const uint32_t newCapacity = std::min(maximumCapacity_,
            std::max(oldCapacity + 1, oldCapacity * 2));
        slots_.resize(newCapacity);
        for (uint32_t index = newCapacity; index-- > oldCapacity;) {
            available_.push_back(index);
        }
        stats_.capacity = newCapacity;
        ++stats_.growthCount;
        return true;
    }

    TextureViewHandle TextureViewResidencyTable::allocate(uint64_t backendToken) {
        if (available_.empty() && !grow()) {
            throw std::runtime_error("Texture view residency table exhausted");
        }
        const uint32_t index = available_.back();
        available_.pop_back();
        TextureViewSlot& slot = slots_[index];
        slot.backendToken = backendToken;
        slot.retireAfterSerial = 0;
        slot.state = backendToken == 0
            ? TextureResidencyState::NonResident
            : TextureResidencyState::Resident;
        stats_.highWatermark = std::max(stats_.highWatermark, index + 1);
        updateCounts();
        return TextureViewHandle::fromParts(index, slot.generation);
    }

    bool TextureViewResidencyTable::valid(TextureViewHandle handle) const noexcept {
        const uint32_t index = handle.getIndex();
        return handle.isValid() && index != 0 && index < slots_.size() &&
            slots_[index].generation == handle.getGeneration() &&
            slots_[index].state != TextureResidencyState::Retired;
    }

    bool TextureViewResidencyTable::makeResident(
        TextureViewHandle handle, uint64_t backendToken) {
        if (!valid(handle) || backendToken == 0) {
            ++stats_.staleHandleRejections;
            return false;
        }
        TextureViewSlot& slot = slots_[handle.getIndex()];
        slot.backendToken = backendToken;
        slot.state = TextureResidencyState::Resident;
        updateCounts();
        return true;
    }

    bool TextureViewResidencyTable::evict(TextureViewHandle handle) {
        if (!valid(handle)) {
            ++stats_.staleHandleRejections;
            return false;
        }
        TextureViewSlot& slot = slots_[handle.getIndex()];
        slot.backendToken = 0;
        slot.state = TextureResidencyState::NonResident;
        updateCounts();
        return true;
    }

    bool TextureViewResidencyTable::release(
        TextureViewHandle handle, uint64_t retireAfterSerial) {
        if (!valid(handle)) {
            ++stats_.staleHandleRejections;
            return false;
        }
        TextureViewSlot& slot = slots_[handle.getIndex()];
        slot.backendToken = 0;
        slot.retireAfterSerial = retireAfterSerial;
        slot.state = TextureResidencyState::Retired;
        updateCounts();
        return true;
    }

    void TextureViewResidencyTable::collect(uint64_t completedSerial) {
        for (uint32_t index = 1; index < slots_.size(); ++index) {
            TextureViewSlot& slot = slots_[index];
            if (slot.state != TextureResidencyState::Retired ||
                slot.retireAfterSerial > completedSerial) {
                continue;
            }
            slot.generation = nextGeneration(slot.generation);
            slot.retireAfterSerial = 0;
            slot.state = TextureResidencyState::NonResident;
            available_.push_back(index);
        }
        updateCounts();
    }

    uint32_t TextureViewResidencyTable::shaderIndex(
        TextureViewHandle handle) const noexcept {
        if (!valid(handle)) {
            return 0;
        }
        return slots_[handle.getIndex()].state == TextureResidencyState::Resident
            ? handle.getIndex() : 0;
    }

    uint64_t TextureViewResidencyTable::backendToken(uint32_t index) const noexcept {
        if (index >= slots_.size() ||
            (index != 0 && slots_[index].state != TextureResidencyState::Resident)) {
            return slots_[0].backendToken;
        }
        return slots_[index].backendToken;
    }

    TextureResidencyState TextureViewResidencyTable::state(
        TextureViewHandle handle) const noexcept {
        if (!valid(handle)) {
            return TextureResidencyState::Fallback;
        }
        return slots_[handle.getIndex()].state;
    }

    void TextureViewResidencyTable::updateCounts() noexcept {
        stats_.residentViews = 0;
        stats_.nonResidentViews = 0;
        stats_.retiredViews = 0;
        for (uint32_t index = 1; index < slots_.size(); ++index) {
            switch (slots_[index].state) {
            case TextureResidencyState::Resident: ++stats_.residentViews; break;
            case TextureResidencyState::NonResident: ++stats_.nonResidentViews; break;
            case TextureResidencyState::Retired: ++stats_.retiredViews; break;
            case TextureResidencyState::Fallback: break;
            }
        }
    }

    SamplerRegistry::SamplerRegistry(SamplerDesc fallback) {
        entries_.push_back({
            .handle = SamplerHandle::fromParts(0, 1),
            .desc = fallback,
            .shaderIndex = 0,
            .referenceCount = std::numeric_limits<uint32_t>::max(),
        });
    }

    SamplerRegistryEntry SamplerRegistry::acquire(const SamplerDesc& desc) {
        const auto existing = std::find_if(entries_.begin() + 1, entries_.end(),
            [&](const SamplerRegistryEntry& entry) {
                return entry.referenceCount != 0 && entry.desc == desc;
            });
        if (existing != entries_.end()) {
            ++existing->referenceCount;
            return *existing;
        }
        if (entries_.size() > SamplerHandle::MaxIndex) {
            throw std::runtime_error("Sampler registry exhausted");
        }
        const uint32_t index = static_cast<uint32_t>(entries_.size());
        entries_.push_back({
            .handle = SamplerHandle::fromParts(index, 1),
            .desc = desc,
            .shaderIndex = index,
            .referenceCount = 1,
        });
        return entries_.back();
    }

    bool SamplerRegistry::release(SamplerHandle handle) {
        const uint32_t index = handle.getIndex();
        if (!handle.isValid() || index == 0 || index >= entries_.size()) {
            return false;
        }
        SamplerRegistryEntry& entry = entries_[index];
        if (entry.handle != handle || entry.referenceCount == 0) {
            return false;
        }
        --entry.referenceCount;
        return true;
    }

    uint32_t SamplerRegistry::shaderIndex(SamplerHandle handle) const noexcept {
        const uint32_t index = handle.getIndex();
        if (!handle.isValid() || index >= entries_.size()) {
            return 0;
        }
        const SamplerRegistryEntry& entry = entries_[index];
        return entry.handle == handle && entry.referenceCount != 0 ? entry.shaderIndex : 0;
    }

    const SamplerDesc& SamplerRegistry::descriptor(uint32_t index) const noexcept {
        if (index >= entries_.size() || entries_[index].referenceCount == 0) {
            return entries_[0].desc;
        }
        return entries_[index].desc;
    }

} // namespace Iridium
