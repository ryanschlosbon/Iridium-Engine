#pragma once

#include <cstdint>

namespace Iridium::RenderGraph {

    // M9 G2: History keeps one physical set per retained view (0 = the scene
    // view, 1 = the asset preview), so alternating views keep their history.
    inline constexpr uint32_t HistoryViewSetCount = 2;

    // Per-pair reset policy. OnCut pairs (TAA colour, depth/velocity history,
    // denoisers) are invalidated by a view's resetRevision; SurviveCut pairs
    // (adapted exposure) ignore it but still follow identity, extent, format
    // and topology.
    enum class HistoryReset : uint8_t {
        OnCut = 0,
        SurviveCut = 1,
    };

    // The view a frame's History belongs to. `identity` is a stable view id
    // (camera/viewport); 0 means "no view", under which no history is valid.
    // Bumping `resetRevision` discards OnCut history (cuts, teleports,
    // settings changes). `historySet` selects the view's physical history set
    // (< HistoryViewSetCount). The rhi `ViewHistoryContext` is this type (M9 G1).
    struct ViewHistoryContext {
        uint64_t identity = 0;
        uint64_t resetRevision = 0;
        uint32_t historySet = 0;

        friend constexpr bool operator==(const ViewHistoryContext&,
            const ViewHistoryContext&) = default;
    };

} // namespace Iridium::RenderGraph
