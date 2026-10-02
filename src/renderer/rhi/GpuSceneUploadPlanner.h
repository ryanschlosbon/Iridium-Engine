#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace Iridium {

    struct GpuSceneRecordRange {
        uint32_t firstRecord = 0;
        uint32_t recordCount = 0;
        auto operator<=>(const GpuSceneRecordRange&) const = default;
    };

    // Revision comparison is per fence-owned frame context. CPU changed lists are
    // insufficient because two contexts may have uploaded different publications.
    inline void buildGpuSceneUploadRanges(
        std::span<const uint64_t> sourceRevisions,
        std::span<const uint64_t> uploadedRevisions,
        std::vector<GpuSceneRecordRange>& ranges) {
        if (uploadedRevisions.size() < sourceRevisions.size())
            throw std::invalid_argument(
                "Uploaded GPU-scene revision table is smaller than source table");
        ranges.clear();
        uint32_t index = 0;
        while (index < sourceRevisions.size()) {
            if (sourceRevisions[index] == uploadedRevisions[index]) {
                ++index;
                continue;
            }
            const uint32_t first = index++;
            while (index < sourceRevisions.size() &&
                sourceRevisions[index] != uploadedRevisions[index]) ++index;
            ranges.push_back({ first, index - first });
        }
    }

} // namespace Iridium
