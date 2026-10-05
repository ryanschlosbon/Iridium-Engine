#include "app/cli/ApplicationCliOptions.h"

#include "app/ApplicationConfig.h"
#include "app/cli/CliValueParsers.h"

#include <stdexcept>
#include <string>

namespace Iridium::AppCli {

    void registerEditorOptions(Cli::CliOptionRegistry& registry,
        ApplicationConfig& config) {
        const std::string_view owner = kEditorOwner;
        ApplicationConfig& c = config;

        addSwitch(registry, owner, "--show-profiler", "Open the editor profiler panel",
            [&c] { c.showProfiler = true; });
        addSwitch(registry, owner, "--show-material-diagnostics",
            "Open the source-to-GPU material inspector",
            [&c] { c.showMaterialDiagnostics = true; });
        addSwitch(registry, owner, "--wireframe",
            "Capture the editor opaque-wireframe diagnostic",
            [&c] { c.forceWireframe = true; });
        addValueOption(registry, owner, "--open-asset-viewer", "GUID",
            "Open a model or material in the isolated editor viewer",
            "--open-asset-viewer requires an asset GUID",
            [&c](std::string_view value) {
                c.editorAssetViewerGuid = AssetGuid::parse(value);
                if (!c.editorAssetViewerGuid || c.editorAssetViewerGuid->isNil()) {
                    throw std::invalid_argument(
                        "--open-asset-viewer requires a non-nil asset GUID");
                }
            },
            true);

        registry.addValidator(std::string(owner), [&c] {
            if (c.forceWireframe && c.debugView != RenderDebugView::Final) {
                throw std::invalid_argument(
                    "--wireframe cannot be combined with a material debug view");
            }
        }, kValidateWireframeDebugView);
    }

} // namespace Iridium::AppCli
