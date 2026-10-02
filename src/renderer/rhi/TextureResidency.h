#pragma once

#include "renderer/rhi/RenderHandles.h"
#include "renderer/rhi/TextureTypes.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Iridium {

    enum class TextureResidencyState : uint8_t {
        Fallback,
        NonResident,
        Resident,
        Retired,
    };

    struct TextureResidencyStats {
        uint32_t capacity = 0;
        uint32_t residentViews = 0;
        uint32_t nonResidentViews = 0;
        uint32_t retiredViews = 0;
        uint32_t samplerCount = 0;
        uint32_t highWatermark = 0;
        uint64_t growthCount = 0;
        uint64_t staleHandleRejections = 0;
    };

    struct TextureViewSlot {
        uint32_t generation = 1;
        TextureResidencyState state = TextureResidencyState::NonResident;
        uint64_t backendToken = 0;
        uint64_t retireAfterSerial = 0;
    };

    // Backend-neutral ownership model for a shader-visible texture-view array.
    // Slot zero is permanently reserved for the fallback view. A released slot
    // cannot be recycled until the backend reports completion of the retirement
    // serial, so stale in-flight indices never alias a new image.
    class TextureViewResidencyTable {
    public:
        explicit TextureViewResidencyTable(uint32_t initialCapacity = 256,
            uint32_t maximumCapacity = TextureViewHandle::MaxIndex + 1);

        void setFallback(uint64_t backendToken);
        [[nodiscard]] TextureViewHandle allocate(uint64_t backendToken = 0);
        [[nodiscard]] bool makeResident(TextureViewHandle handle, uint64_t backendToken);
        [[nodiscard]] bool evict(TextureViewHandle handle);
        [[nodiscard]] bool release(TextureViewHandle handle, uint64_t retireAfterSerial);
        void collect(uint64_t completedSerial);

        [[nodiscard]] uint32_t shaderIndex(TextureViewHandle handle) const noexcept;
        [[nodiscard]] uint64_t backendToken(uint32_t shaderIndex) const noexcept;
        [[nodiscard]] TextureResidencyState state(TextureViewHandle handle) const noexcept;
        [[nodiscard]] const TextureResidencyStats& stats() const noexcept { return stats_; }
        [[nodiscard]] std::span<const TextureViewSlot> slots() const noexcept {
            return slots_;
        }

    private:
        std::vector<TextureViewSlot> slots_;
        std::vector<uint32_t> available_;
        uint32_t maximumCapacity_ = 0;
        TextureResidencyStats stats_{};

        [[nodiscard]] bool valid(TextureViewHandle handle) const noexcept;
        [[nodiscard]] bool grow();
        void updateCounts() noexcept;
    };

    struct SamplerRegistryEntry {
        SamplerHandle handle;
        SamplerDesc desc;
        uint32_t shaderIndex = 0;
        uint32_t referenceCount = 0;
    };

    // Samplers are immutable and deduplicated by their full explicit descriptor.
    // Slot zero is the fallback sampler and remains valid for the table lifetime.
    class SamplerRegistry {
    public:
        explicit SamplerRegistry(SamplerDesc fallback = {});

        [[nodiscard]] SamplerRegistryEntry acquire(const SamplerDesc& desc);
        [[nodiscard]] bool release(SamplerHandle handle);
        [[nodiscard]] uint32_t shaderIndex(SamplerHandle handle) const noexcept;
        [[nodiscard]] const SamplerDesc& descriptor(uint32_t shaderIndex) const noexcept;
        [[nodiscard]] uint32_t size() const noexcept {
            return static_cast<uint32_t>(entries_.size());
        }

    private:
        std::vector<SamplerRegistryEntry> entries_;
    };

} // namespace Iridium
