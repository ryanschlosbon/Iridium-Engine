#pragma once

#include "renderer/rhi/LightingTypes.h"

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace Iridium {

    // Produces the exact contiguous record ranges whose source revisions differ
    // for one fence-owned frame context. The caller commits revisions only after
    // the corresponding writes succeed.
    inline void buildLightUploadRanges(
        std::span<const uint64_t> sourceRevisions,
        std::span<const uint64_t> uploadedRevisions,
        std::vector<LightRecordRange>& ranges) {
        if (uploadedRevisions.size() < sourceRevisions.size()) {
            throw std::invalid_argument(
                "Uploaded light revision table is smaller than the source table");
        }
        ranges.clear();
        uint32_t index = 0;
        while (index < sourceRevisions.size()) {
            if (sourceRevisions[index] == uploadedRevisions[index]) {
                ++index;
                continue;
            }
            const uint32_t first = index++;
            while (index < sourceRevisions.size() &&
                sourceRevisions[index] != uploadedRevisions[index]) {
                ++index;
            }
            ranges.push_back({ first, index - first });
        }
    }

} // namespace Iridium
