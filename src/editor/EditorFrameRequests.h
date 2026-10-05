#pragma once

// M7R R5a (design section 3.3): what the editor hands the frame, produced once
// per frame by EditorHost::build. The editor never writes runtime
// configuration, the backend or extraction state: the application applies the
// three settings requests (FrameOrchestrator::apply), extraction reads the
// view, and the requested scene extent is applied after endFrame.

#include <optional>

#include "editor/EditorUIState.h"
#include "extraction/EditorViewState.h"
#include "renderer/rhi/ReflectionProbeSettings.h"
#include "renderer/rhi/RhiResourceTypes.h"
#include "renderer/rhi/ShadowSettings.h"

namespace Iridium {

    struct EditorFrameRequests {
        // Output transport, manual exposure, paper white and peak luminance.
        std::optional<EditorOutputSettings> output;
        // Allocation fields (resolutions, pool capacities) are ignored on apply.
        std::optional<ProjectShadowSettings> shadows;
        // Also reconfigures the capture scheduler and the backend.
        std::optional<ProjectReflectionProbeSettings> probes;
        // The editor state after this frame's build.
        EditorViewState view;
        // The editor-requested scene extent, applied after endFrame.
        RenderExtent requestedSceneExtent{};
    };

} // namespace Iridium
