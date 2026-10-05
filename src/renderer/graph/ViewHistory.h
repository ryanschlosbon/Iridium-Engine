#pragma once

#include <cstdint>

namespace Iridium::RenderGraph {

    // The view a frame's History belongs to. `identity` is a stable view id
    // (camera/viewport); 0 means "no view", under which no history is valid.
    // Bumping `resetRevision` discards history (cuts, teleports, settings
    // changes). The rhi `ViewHistoryContext` is this type (M9 G1).
    struct ViewHistoryContext {
        uint64_t identity = 0;
        uint64_t resetRevision = 0;

        friend constexpr bool operator==(const ViewHistoryContext&,
            const ViewHistoryContext&) = default;
    };

} // namespace Iridium::RenderGraph
